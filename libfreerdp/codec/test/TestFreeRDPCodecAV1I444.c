#include <freerdp/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <freerdp/codec/av1.h>
#include "../av1_color.h"

static BYTE sample(size_t plane, size_t x, size_t y)
{
	return (BYTE)((x * 37 + y * 71 + plane * 83 + 13) & 255);
}

static BYTE reference_channel(INT32 value)
{
	/* Explicit floor division is independent of the implementation's signed shift. */
	const INT32 result = value < 0 ? -((-value + 255) / 256) : value / 256;
	return (BYTE)(result < 0 ? 0 : result > 255 ? 255 : result);
}

static BOOL reference_pixel(BYTE* dst, UINT32 format, BYTE y, BYTE u, BYTE v)
{
	const INT32 uc = (INT32)u - 128;
	const INT32 vc = (INT32)v - 128;
	const BYTE r = reference_channel(256 * (INT32)y + 403 * vc);
	const BYTE g = reference_channel(256 * (INT32)y - 48 * uc - 120 * vc);
	const BYTE b = reference_channel(256 * (INT32)y + 475 * uc);
	return FreeRDPWriteColorIgnoreAlpha(dst, format, FreeRDPGetColor(format, r, g, b, 0));
}

static BOOL test_conversion(av1_i444_to_rgb_fn convert, UINT32 width, UINT32 height, UINT32 format)
{
	BOOL ok = FALSE;
	BYTE* storage[3] = WINPR_C_ARRAY_INIT;
	const BYTE* planes[3] = WINPR_C_ARRAY_INIT;
	const ptrdiff_t strides[3] = { width + 1, width + 3, width + 5 };
	BYTE* out = nullptr;
	BYTE* expected = nullptr;
	const UINT32 bpp = FreeRDPGetBytesPerPixel(format);
	const UINT32 dstStride = width * bpp + 7;
	const size_t size = 3 + (size_t)(height - 1) * dstStride + (size_t)width * bpp;
	const prim_size_t roi = { width, height };
	for (size_t p = 0; p < 3; p++)
	{
		/* Unaligned planes with no readable padding after the final visible pixel. */
		storage[p] = malloc(1 + (size_t)(height - 1) * (size_t)strides[p] + width);
		if (!storage[p])
			goto out;
		planes[p] = storage[p] + 1;
		for (size_t y = 0; y < height; y++)
			for (size_t x = 0; x < width; x++)
				storage[p][1 + y * (size_t)strides[p] + x] = sample(p, x, y);
	}
	out = malloc(size);
	expected = malloc(size);
	if (!out || !expected)
		goto out;
	memset(out, 0xA5, size);
	memset(expected, 0xA5, size);
	for (size_t y = 0; y < height; y++)
		for (size_t x = 0; x < width; x++)
			if (!reference_pixel(expected + 3 + y * dstStride + x * bpp, format, sample(0, x, y),
			                     sample(1, x, y), sample(2, x, y)))
				goto out;
	if (convert(planes, strides, out + 3, dstStride, format, &roi) != PRIMITIVES_SUCCESS)
		goto out;
	ok = memcmp(out, expected, size) == 0;
out:
	if (!ok)
		fprintf(stderr, "I444 conversion mismatch: %ux%u format=%s\n", width, height,
		        FreeRDPGetColorFormatName(format));
	for (size_t p = 0; p < 3; p++)
		free(storage[p]);
	free(out);
	free(expected);
	return ok;
}

static BOOL test_neighbours(av1_i444_to_rgb_fn convert)
{
	BYTE y[4] = { 128, 128, 128, 128 };
	BYTE u[4] = { 128, 128, 128, 128 };
	BYTE v[4] = { 128, 128, 128, 128 };
	const BYTE* planes[3] = { y, u, v };
	const ptrdiff_t strides[3] = { 2, 2, 2 };
	const prim_size_t roi = { 2, 2 };
	BYTE dst[16];
	BYTE expected[4] = { 0 };
	if (!reference_pixel(expected, PIXEL_FORMAT_BGRX32, 128, 128, 128))
		return FALSE;
	for (BYTE delta = 29; delta <= 31; delta++)
	{
		u[1] = 128 - delta;
		v[2] = 128 + delta;
		memset(dst, 0, sizeof(dst));
		if (convert(planes, strides, dst, 8, PIXEL_FORMAT_BGRX32, &roi) != PRIMITIVES_SUCCESS)
			return FALSE;
		if (memcmp(dst, expected, sizeof(expected)) != 0)
			return FALSE;
	}
	const ptrdiff_t negative[3] = { 2, -2, 2 };
	return convert(planes, negative, dst, 8, PIXEL_FORMAT_BGRX32, &roi) != PRIMITIVES_SUCCESS;
}

/* Lossless AV1, profile 1, 8-bit BT.709/full-range, 17x3 pixels.
 * The native Y/U/V samples are exactly sample(plane, x, y) above. Generated with
 * libaom-av1, lossless=1, crf=0, b:v=0 from full-range samples, without RGB conversion.
 * A fixed bitstream tests the actual decoder without depending on an encoder or FFmpeg. */
static const BYTE i444_lossless[] = {
	0x12, 0x00, 0x0a, 0x0c, 0x20, 0x00, 0x00, 0x02, 0x0c, 0x23, 0x6b, 0xe6, 0x81, 0x01, 0x00, 0xc8,
	0x32, 0xa3, 0x02, 0x10, 0x00, 0x80, 0x00, 0xf0, 0xbb, 0xb5, 0x0e, 0x50, 0x49, 0x6c, 0xf3, 0xcf,
	0xee, 0x04, 0x7e, 0xed, 0x0c, 0xd7, 0xb0, 0x8a, 0xb9, 0x42, 0xda, 0x5b, 0x43, 0x3a, 0xa1, 0x34,
	0xab, 0xe1, 0x8c, 0xbb, 0x1a, 0xd8, 0xc1, 0x4f, 0xdc, 0xd3, 0x08, 0xd9, 0x71, 0xeb, 0x6c, 0x15,
	0x57, 0xd7, 0xe3, 0x5e, 0x7f, 0xff, 0xff, 0xfe, 0xe1, 0x83, 0xe1, 0xc3, 0x36, 0x6b, 0xdb, 0x4c,
	0xf8, 0x53, 0xc4, 0x6f, 0xc0, 0x8e, 0x37, 0x1a, 0xcb, 0x0a, 0x81, 0x38, 0x93, 0x80, 0xa8, 0x13,
	0x90, 0x44, 0x9f, 0xff, 0xff, 0xce, 0x58, 0x6d, 0x93, 0x46, 0x2a, 0x66, 0x11, 0x71, 0xc1, 0x4a,
	0xc0, 0x8c, 0x30, 0x06, 0x46, 0x17, 0xe0, 0xa5, 0x60, 0x46, 0x04, 0x60, 0x46, 0x04, 0x60, 0x46,
	0x2b, 0x65, 0x43, 0xcd, 0xa8, 0xcb, 0x57, 0xca, 0xc8, 0x39, 0xb6, 0x1c, 0xf8, 0x26, 0x62, 0x2f,
	0x35, 0x6d, 0x6a, 0x90, 0xb8, 0x2f, 0xf1, 0x2b, 0xa9, 0xb1, 0x0f, 0xfc, 0xa0, 0xac, 0xc9, 0xc8,
	0x95, 0xf6, 0x43, 0xdf, 0x44, 0x8f, 0x6f, 0x27, 0xc0, 0xe8, 0x17, 0xc3, 0x95, 0x06, 0x83, 0x95,
	0xc9, 0x7b, 0x1b, 0x7c, 0x0d, 0x94, 0x2d, 0xfb, 0x97, 0x58, 0xb9, 0x0b, 0x58, 0x91, 0x52, 0x48,
	0xdf, 0x1d, 0xb9, 0x52, 0x45, 0x49, 0x2f, 0x00, 0xc7, 0x81, 0xcf, 0xfa, 0xd3, 0x7d, 0xfe, 0x17,
	0xc9, 0x49, 0x13, 0x16, 0x29, 0x55, 0xf3, 0xc1, 0xab, 0x41, 0x9b, 0xa2, 0xfb, 0x3f, 0x8e, 0xd5,
	0xe1, 0x84, 0x36, 0x5e, 0x17, 0x5a, 0xff, 0x2a, 0x37, 0x28, 0xb6, 0x00, 0xd5, 0xd6, 0xda, 0x9b,
	0x3b, 0xcd, 0x41, 0x69, 0x9a, 0x87, 0x80, 0x7c, 0x15, 0x50, 0x19, 0xa4, 0xa3, 0x56, 0xa7, 0x51,
	0xab, 0x53, 0xa8, 0xd4, 0x8d, 0x48, 0xd4, 0x8d, 0x48, 0xd5, 0x99, 0x91, 0x97, 0x8c, 0xc6, 0xdd,
	0xdb, 0xe9, 0xff, 0xa3, 0xd8, 0xd4, 0x11, 0xff, 0xc5, 0x7a, 0x67, 0x22, 0x01, 0xaf, 0x8a, 0x67,
	0x30, 0x87, 0xce, 0xa9, 0x9c, 0x50, 0xdc, 0xd7, 0x74, 0x08, 0xd7, 0xbd, 0x2c, 0xc2, 0xdf, 0x80,
	0x8c, 0x0a, 0x8c, 0x0a, 0x90, 0x80,
};

static BOOL test_decoder(UINT32 format)
{
#if defined(WITH_LIBAOM) || defined(WITH_DAV1D)
#if defined(WITH_LIBYUV)
	/* The separate libyuv fast path does not use the AVC444 primitive under test. */
	if ((format == PIXEL_FORMAT_BGRA32) || (format == PIXEL_FORMAT_BGRX32))
		return TRUE;
#endif
	BOOL ok = FALSE;
	FREERDP_AV1_CONTEXT* dec = freerdp_av1_context_new(FALSE);
	const UINT32 width = 17, height = 3, dstWidth = 32, dstHeight = 16;
	const UINT32 bpp = FreeRDPGetBytesPerPixel(format);
	const UINT32 stride = dstWidth * bpp + 16;
	BYTE* dst = malloc((size_t)stride * dstHeight);
	BYTE* expected = malloc((size_t)stride * dstHeight);
	if (!dec || !dst || !expected)
		goto out;
	memset(expected, 0xA5, (size_t)stride * dstHeight);
	for (size_t y = 0; y < height; y++)
		for (size_t x = 0; x < width; x++)
			if (!reference_pixel(expected + y * stride + x * bpp, format, sample(0, x, y),
			                     sample(1, x, y), sample(2, x, y)))
				goto out;
	const RECTANGLE_16 rect = { 0, 0, width, height };
	for (size_t round = 0; round < 4; round++)
	{
		if (!freerdp_av1_context_reset(dec, dstWidth, dstHeight))
			goto out;
		memset(dst, 0xA5, (size_t)stride * dstHeight);
		if (freerdp_av1_decompress(dec, i444_lossless, sizeof(i444_lossless), dst, format, stride,
		                           dstWidth, dstHeight, &rect, 1) < 0)
			goto out;
		if (memcmp(dst, expected, (size_t)stride * dstHeight) != 0)
		{
			fprintf(stderr, "Decoded native AV1 I444 colours/padding mismatch: %s\n",
			        FreeRDPGetColorFormatName(format));
			for (size_t i = 0; i < (size_t)stride * dstHeight; i++)
			{
				if (dst[i] != expected[i])
				{
					fprintf(stderr, "First differing byte: %zu actual=%u expected=%u\n", i, dst[i],
					        expected[i]);
					break;
				}
			}
			goto out;
		}
	}
	ok = TRUE;
out:
	freerdp_av1_context_free(dec);
	free(expected);
	free(dst);
	return ok;
#else
	WINPR_UNUSED(format);
	WINPR_UNUSED(i444_lossless);
	return TRUE;
#endif
}

int TestFreeRDPCodecAV1I444(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);
	const UINT32 widths[] = { 1, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 2304 };
	const UINT32 formats[] = {
		PIXEL_FORMAT_BGRX32, PIXEL_FORMAT_BGRA32,         PIXEL_FORMAT_RGBX32,
		PIXEL_FORMAT_RGBA32, PIXEL_FORMAT_XRGB32,         PIXEL_FORMAT_ARGB32,
		PIXEL_FORMAT_XBGR32, PIXEL_FORMAT_ABGR32,         PIXEL_FORMAT_RGB24,
		PIXEL_FORMAT_BGR24,  PIXEL_FORMAT_RGB16,          PIXEL_FORMAT_BGR16,
		PIXEL_FORMAT_RGB15,  PIXEL_FORMAT_BGR15,          PIXEL_FORMAT_ARGB15,
		PIXEL_FORMAT_ABGR15, PIXEL_FORMAT_BGRX32_DEPTH30, PIXEL_FORMAT_RGBX32_DEPTH30,
	};
	const av1_i444_to_rgb_fn converters[] = { av1_i444_to_rgb8, av1_i444_converter() };
	for (size_t c = 0; c < ARRAYSIZE(converters); c++)
	{
		if (!test_neighbours(converters[c]))
			return -1;
		for (size_t f = 0; f < ARRAYSIZE(formats); f++)
			for (size_t w = 0; w < ARRAYSIZE(widths); w++)
				for (UINT32 h = 1; h <= 3; h++)
					if (!test_conversion(converters[c], widths[w], h, formats[f]))
						return -1;
	}
	for (size_t f = 0; f < ARRAYSIZE(formats); f++)
		if (!test_decoder(formats[f]))
			return -1;
	return 0;
}
