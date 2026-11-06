/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Native AV1 I444 conversion (without AVC444 chroma reconstruction)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FREERDP_CODEC_AV1_COLOR_H
#define FREERDP_CODEC_AV1_COLOR_H

#include <stddef.h>
#include <stdint.h>

#include <freerdp/config.h>
#include <freerdp/codec/color.h>
#include <freerdp/primitives.h>
#include <winpr/sysinfo.h>

#if defined(WITH_SIMD) && \
    (defined(__SSE2__) || defined(_M_AMD64) || (defined(_M_IX86_FP) && (_M_IX86_FP >= 2)))
#define AV1_I444_SSE2
#include <emmintrin.h>
#endif

typedef pstatus_t (*av1_i444_to_rgb_fn)(const BYTE* const planes[3], const ptrdiff_t strides[3],
                                        BYTE* dst, UINT32 dstStride, UINT32 format,
                                        const prim_size_t* roi);

static inline BYTE av1_i444_clip(INT32 value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return (BYTE)value;
}

static inline BOOL av1_i444_write_pixel(BYTE* dst, UINT32 format, BYTE y, BYTE u, BYTE v)
{
	/* Preserve the existing 8-bit BT.709 full-range integer matrix. */
	const INT32 uc = (INT32)u - 128;
	const INT32 vc = (INT32)v - 128;
	const BYTE r = av1_i444_clip((256 * (INT32)y + 403 * vc) >> 8);
	const BYTE g = av1_i444_clip((256 * (INT32)y - 48 * uc - 120 * vc) >> 8);
	const BYTE b = av1_i444_clip((256 * (INT32)y + 475 * uc) >> 8);
	if ((format == PIXEL_FORMAT_BGRA32) || (format == PIXEL_FORMAT_BGRX32))
	{
		dst[0] = b;
		dst[1] = g;
		dst[2] = r;
		return TRUE;
	}
	return FreeRDPWriteColorIgnoreAlpha(dst, format, FreeRDPGetColor(format, r, g, b, 0));
}

static inline BOOL av1_i444_valid(const BYTE* const planes[3], const ptrdiff_t strides[3],
                                  BYTE* dst, UINT32 dstStride, UINT32 format,
                                  const prim_size_t* roi)
{
	if (!planes || !strides || !dst || !roi || !roi->width || !roi->height)
		return FALSE;
	const UINT32 bpp = FreeRDPGetBytesPerPixel(format);
	if ((bpp == 0) || (bpp > 4) || (roi->width > dstStride / bpp))
		return FALSE;
	const size_t rows = (size_t)roi->height - 1;
	const size_t rowBytes = (size_t)roi->width * bpp;
	if (rows > (SIZE_MAX - rowBytes) / dstStride)
		return FALSE;
	for (size_t i = 0; i < 3; i++)
	{
		if (!planes[i] || (strides[i] <= 0) || ((size_t)strides[i] < roi->width))
			return FALSE;
		if (rows > (SIZE_MAX - roi->width) / (size_t)strides[i])
			return FALSE;
	}
	return TRUE;
}

static inline pstatus_t av1_i444_to_rgb8(const BYTE* const planes[3], const ptrdiff_t strides[3],
                                         BYTE* dst, UINT32 dstStride, UINT32 format,
                                         const prim_size_t* roi)
{
	if (!av1_i444_valid(planes, strides, dst, dstStride, format, roi))
		return -1;
	const UINT32 bpp = FreeRDPGetBytesPerPixel(format);
	for (size_t y = 0; y < roi->height; y++)
	{
		const BYTE* py = planes[0] + y * (size_t)strides[0];
		const BYTE* pu = planes[1] + y * (size_t)strides[1];
		const BYTE* pv = planes[2] + y * (size_t)strides[2];
		BYTE* out = dst + y * dstStride;
		for (size_t x = 0; x < roi->width; x++)
		{
			if (!av1_i444_write_pixel(out + x * bpp, format, py[x], pu[x], pv[x]))
				return -1;
		}
	}
	return PRIMITIVES_SUCCESS;
}

#if defined(AV1_I444_SSE2)
static inline __m128i av1_i444_channel(__m128i ylo, __m128i yhi, __m128i uvlo, __m128i uvhi,
                                       __m128i coefficients)
{
	const __m128i lo = _mm_srai_epi32(_mm_add_epi32(ylo, _mm_madd_epi16(uvlo, coefficients)), 8);
	const __m128i hi = _mm_srai_epi32(_mm_add_epi32(yhi, _mm_madd_epi16(uvhi, coefficients)), 8);
	return _mm_packus_epi16(_mm_packs_epi32(lo, hi), _mm_setzero_si128());
}

static inline pstatus_t av1_i444_to_rgb8_sse2(const BYTE* const planes[3],
                                              const ptrdiff_t strides[3], BYTE* dst,
                                              UINT32 dstStride, UINT32 format,
                                              const prim_size_t* roi)
{
	if ((format != PIXEL_FORMAT_BGRA32) && (format != PIXEL_FORMAT_BGRX32))
		return av1_i444_to_rgb8(planes, strides, dst, dstStride, format, roi);
	if (!av1_i444_valid(planes, strides, dst, dstStride, format, roi))
		return -1;
	const __m128i zero = _mm_setzero_si128();
	const __m128i center = _mm_set1_epi16(128);
	const __m128i cr = _mm_set_epi16(403, 0, 403, 0, 403, 0, 403, 0);
	const __m128i cg = _mm_set_epi16(-120, -48, -120, -48, -120, -48, -120, -48);
	const __m128i cb = _mm_set_epi16(0, 475, 0, 475, 0, 475, 0, 475);
	const __m128i alpha = _mm_set1_epi32((INT32)0xFF000000);
	for (size_t y = 0; y < roi->height; y++)
	{
		const BYTE* py = planes[0] + y * (size_t)strides[0];
		const BYTE* pu = planes[1] + y * (size_t)strides[1];
		const BYTE* pv = planes[2] + y * (size_t)strides[2];
		BYTE* out = dst + y * dstStride;
		size_t x = 0;
		for (; x + 8 <= roi->width; x += 8)
		{
			/* Eight samples only: no dependence on decoder padding or a second row. */
			const __m128i yy = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(py + x)), zero);
			const __m128i uu = _mm_sub_epi16(
			    _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(pu + x)), zero), center);
			const __m128i vv = _mm_sub_epi16(
			    _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(pv + x)), zero), center);
			const __m128i ylo = _mm_slli_epi32(_mm_unpacklo_epi16(yy, zero), 8);
			const __m128i yhi = _mm_slli_epi32(_mm_unpackhi_epi16(yy, zero), 8);
			const __m128i uvlo = _mm_unpacklo_epi16(uu, vv);
			const __m128i uvhi = _mm_unpackhi_epi16(uu, vv);
			const __m128i r = av1_i444_channel(ylo, yhi, uvlo, uvhi, cr);
			const __m128i g = av1_i444_channel(ylo, yhi, uvlo, uvhi, cg);
			const __m128i b = av1_i444_channel(ylo, yhi, uvlo, uvhi, cb);
			const __m128i bg = _mm_unpacklo_epi8(b, g);
			const __m128i rx = _mm_unpacklo_epi8(r, zero);
			BYTE* p = out + x * 4;
			const __m128i first =
			    _mm_or_si128(_mm_unpacklo_epi16(bg, rx),
			                 _mm_and_si128(_mm_loadu_si128((const __m128i*)p), alpha));
			const __m128i second =
			    _mm_or_si128(_mm_unpackhi_epi16(bg, rx),
			                 _mm_and_si128(_mm_loadu_si128((const __m128i*)(p + 16)), alpha));
			_mm_storeu_si128((__m128i*)p, first);
			_mm_storeu_si128((__m128i*)(p + 16), second);
		}
		for (; x < roi->width; x++)
		{
			if (!av1_i444_write_pixel(out + x * 4, format, py[x], pu[x], pv[x]))
				return -1;
		}
	}
	return PRIMITIVES_SUCCESS;
}
#endif

static inline av1_i444_to_rgb_fn av1_i444_converter(void)
{
#if defined(AV1_I444_SSE2)
	if (IsProcessorFeaturePresent(PF_SSE2_INSTRUCTIONS_AVAILABLE))
		return av1_i444_to_rgb8_sse2;
#endif
	return av1_i444_to_rgb8;
}

#endif /* FREERDP_CODEC_AV1_COLOR_H */
