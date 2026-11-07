#include <freerdp/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus/opus.h>
#include <freerdp/codec/dsp.h>

#if defined(WITH_FAAD2) && defined(WITH_FAAC)
#include <faac.h>
#include <neaacdec.h>
#endif

static AUDIO_FORMAT opus_format(UINT32 rate, UINT16 channels)
{
	const AUDIO_FORMAT format = { .wFormatTag = WAVE_FORMAT_OPUS,
		                          .nChannels = channels,
		                          .nSamplesPerSec = rate,
		                          .nAvgBytesPerSec = 12000,
		                          .nBlockAlign = 1,
		                          .wBitsPerSample = 16 };
	return format;
}

static void fill_samples(INT16* pcm, size_t count, size_t offset)
{
	for (size_t x = 0; x < count; x++)
		pcm[x] = (INT16)((int)((x + offset) % 30001) - 15000);
}

static BOOL test_opus(FREERDP_DSP_CONTEXT* dsp, UINT32 rate, UINT16 channels, BOOL variable)
{
	BOOL ok = FALSE;
	int error = OPUS_OK;
	const AUDIO_FORMAT format = opus_format(rate, channels);
	OpusEncoder* encoder =
	    opus_encoder_create((opus_int32)rate, channels, OPUS_APPLICATION_AUDIO, &error);
	OpusDecoder* reference = opus_decoder_create((opus_int32)rate, channels, &error);
	wStream* out = Stream_New(nullptr, 16);
	INT16 pcm[5760 * 2] = WINPR_C_ARRAY_INIT;
	INT16 expected[5760 * 2] = WINPR_C_ARRAY_INIT;
	BYTE packet[4096] = WINPR_C_ARRAY_INIT;
	const UINT32 durations[] = { 8, 4, 16, 8, 1, 24, 2, 8 };
	size_t inputSamples = 0;
	size_t outputSamples = 0;
	if (!encoder || !reference || !out || !freerdp_dsp_supports_format(&format, FALSE) ||
	    !freerdp_dsp_context_reset(dsp, &format, 0))
		goto out;

	for (size_t p = 0; p < 512; p++)
	{
		/* Durations are in 2.5-ms units; exercise a smaller packet followed by a larger one. */
		const UINT32 frames = rate / 400 * (variable ? durations[p % ARRAYSIZE(durations)] : 8);
		fill_samples(pcm, frames * channels, inputSamples * channels);
		const int bytes = opus_encode(encoder, pcm, (int)frames, packet, sizeof(packet));
		if (bytes <= 0)
			goto out;
		const int decoded = opus_decode(reference, packet, bytes, expected, 5760, 0);
		if (decoded != (int)frames)
			goto out;

		Stream_ResetPosition(out);
		Stream_Write_UINT32(out, 0x12345678);
		if (!freerdp_dsp_decode(dsp, &format, packet, (size_t)bytes, out))
			goto out;
		const size_t length = Stream_GetPosition(out) - 4;
		if ((length != frames * channels * sizeof(INT16)) ||
		    (memcmp(Stream_Buffer(out) + 4, expected, length) != 0))
		{
			fprintf(stderr,
			        "Opus output mismatch: rate=%u channels=%u packet=%zu frames=%u bytes=%zu\n",
			        rate, channels, p, frames, length);
			goto out;
		}
		inputSamples += frames;
		outputSamples += length / channels / sizeof(INT16);
	}
	if (inputSamples != outputSamples)
		goto out;
	printf("Opus %u Hz %u channels %s: %zu samples, exact libopus PCM, no backlog\n", rate,
	       channels, variable ? "variable packets" : "20-ms packets", outputSamples);
	ok = TRUE;
out:
	opus_encoder_destroy(encoder);
	opus_decoder_destroy(reference);
	Stream_Free(out, TRUE);
	return ok;
}

static BOOL test_pcm(FREERDP_DSP_CONTEXT* dsp)
{
	AUDIO_FORMAT format = opus_format(44100, 2);
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nBlockAlign = 4;
	format.nAvgBytesPerSec = 44100 * 4;
	BYTE pcm[1764] = WINPR_C_ARRAY_INIT;
	wStream* out = Stream_New(nullptr, 1);
	BOOL ok = out && freerdp_dsp_context_reset(dsp, &format, 0) &&
	          freerdp_dsp_decode(dsp, &format, pcm, sizeof(pcm), out) &&
	          (Stream_GetPosition(out) == sizeof(pcm)) &&
	          (memcmp(pcm, Stream_Buffer(out), sizeof(pcm)) == 0);
	Stream_Free(out, TRUE);
	return ok;
}

#if defined(WITH_FAAD2) && defined(WITH_FAAC)
static BOOL test_aac(FREERDP_DSP_CONTEXT* dsp, UINT32 rate, UINT16 channels)
{
	BOOL ok = FALSE;
	AUDIO_FORMAT format = opus_format(rate, channels);
	format.wFormatTag = WAVE_FORMAT_AAC_MS;
	unsigned long inputSamples = 0;
	unsigned long maxBytes = 0;
	faacEncHandle encoder = faacEncOpen(rate, channels, &inputSamples, &maxBytes);
	NeAACDecHandle reference = NeAACDecOpen();
	wStream* out = Stream_New(nullptr, 1);
	wStream* expectedPCM = Stream_New(nullptr, 1);
	INT16* pcm = nullptr;
	BYTE* packet = nullptr;
	size_t decodedSamples = 0;
	BOOL initialized = FALSE;
	if (!encoder || !reference || !out || !expectedPCM)
		goto out;
	faacEncConfigurationPtr config = faacEncGetCurrentConfiguration(encoder);
	config->inputFormat = FAAC_INPUT_16BIT;
	config->outputFormat = 1;
	config->aacObjectType = LOW;
	config->mpegVersion = MPEG4;
	config->bitRate = 64000;
	if (!faacEncSetConfiguration(encoder, config))
		goto out;
	pcm = calloc(inputSamples, sizeof(INT16));
	packet = malloc(3 * maxBytes);
	if (!pcm || !packet || !freerdp_dsp_supports_format(&format, FALSE) ||
	    !freerdp_dsp_context_reset(dsp, &format, 0))
		goto out;

	for (size_t p = 0; p < 64; p++)
	{
		fill_samples(pcm, inputSamples, p * inputSamples);
		const int bytes = faacEncEncode(encoder, (int32_t*)pcm, (unsigned int)inputSamples, packet,
		                                (unsigned int)maxBytes);
		if (bytes < 0)
			goto out;
		if (bytes == 0)
			continue;
		if (!initialized)
		{
			unsigned long actualRate = 0;
			unsigned char actualChannels = 0;
			if ((NeAACDecInit(reference, packet, (unsigned long)bytes, &actualRate,
			                  &actualChannels) != 0) ||
			    (actualRate != rate) ||
			    ((actualChannels != channels) && !((channels == 1) && (actualChannels == 2))))
			{
				fprintf(stderr, "AAC initialization: actual rate=%lu channels=%u\n", actualRate,
				        actualChannels);
				goto out;
			}
			initialized = TRUE;
		}
		const size_t frames = 1 + p % 3;
		const size_t packetSize = frames * (size_t)bytes;
		for (size_t x = 1; x < frames; x++)
			memcpy(packet + x * (size_t)bytes, packet, (size_t)bytes);
		Stream_ResetPosition(expectedPCM);
		for (size_t offset = 0; offset < packetSize;)
		{
			NeAACDecFrameInfo info = WINPR_C_ARRAY_INIT;
			const INT16* samples = NeAACDecDecode(reference, &info, packet + offset,
			                                      (unsigned long)(packetSize - offset));
			if (info.error || !info.bytesconsumed || (info.bytesconsumed > packetSize - offset))
				goto out;
			offset += info.bytesconsumed;
			if (!Stream_EnsureRemainingCapacity(expectedPCM, info.samples * sizeof(INT16)))
				goto out;
			if ((channels == 1) && (info.channels == 2))
			{
				for (size_t x = 0; x < info.samples / 2; x++)
					Stream_Write_INT16(expectedPCM,
					                   (INT16)(((INT32)samples[2 * x] + samples[2 * x + 1]) / 2));
			}
			else if (info.samples)
				Stream_Write(expectedPCM, samples, info.samples * sizeof(INT16));
		}
		Stream_ResetPosition(out);
		const size_t expectedBytes = Stream_GetPosition(expectedPCM);
		const BOOL decoded = freerdp_dsp_decode(dsp, &format, packet, packetSize, out);
		if (!decoded || (Stream_GetPosition(out) != expectedBytes) ||
		    (memcmp(Stream_Buffer(out), Stream_Buffer(expectedPCM), expectedBytes) != 0))
		{
			fprintf(stderr, "AAC PCM packet=%zu success=%d expected bytes=%zu actual bytes=%zu\n",
			        p, decoded, expectedBytes, Stream_GetPosition(out));
			goto out;
		}
		decodedSamples += expectedBytes / sizeof(INT16);
	}
	if (!decodedSamples)
		goto out;
	printf("AAC %u Hz %u channels: %zu samples, exact FAAD2 PCM\n", rate, channels,
	       decodedSamples / channels);
	ok = TRUE;
out:
	if (!ok)
		fprintf(stderr, "AAC decode failed: rate=%u channels=%u\n", rate, channels);
	if (encoder)
		faacEncClose(encoder);
	if (reference)
		NeAACDecClose(reference);
	Stream_Free(out, TRUE);
	Stream_Free(expectedPCM, TRUE);
	free(pcm);
	free(packet);
	return ok;
}
#endif

static BOOL test_encoder_reset(void)
{
	BOOL ok = FALSE;
	FREERDP_DSP_CONTEXT* encoder = freerdp_dsp_context_new(TRUE);
	AUDIO_FORMAT target = opus_format(48000, 2);
	AUDIO_FORMAT input = target;
	input.wFormatTag = WAVE_FORMAT_PCM;
	input.nBlockAlign = 4;
	input.nAvgBytesPerSec = 48000 * 4;
	INT16 pcm[960 * 2] = WINPR_C_ARRAY_INIT;
	wStream* out = Stream_New(nullptr, 4096);
	if (!encoder || !out)
		goto out;
	for (size_t x = 0; x < 64; x++)
	{
		Stream_ResetPosition(out);
		if (!freerdp_dsp_context_reset(encoder, &target, 0) ||
		    !freerdp_dsp_encode(encoder, &input, (const BYTE*)pcm, sizeof(pcm), out) ||
		    (Stream_GetPosition(out) == 0))
			goto out;
	}
	ok = TRUE;
out:
	Stream_Free(out, TRUE);
	freerdp_dsp_context_free(encoder);
	return ok;
}

int TestFreeRDPCodecDSP(int argc, char* argv[])
{
	WINPR_UNUSED(argc);
	WINPR_UNUSED(argv);
	BOOL ok = FALSE;
	FREERDP_DSP_CONTEXT* dsp = freerdp_dsp_context_new(FALSE);
	const UINT32 rates[] = { 8000, 12000, 16000, 24000, 48000 };
	if (!dsp)
		goto out;
	for (size_t r = 0; r < ARRAYSIZE(rates); r++)
		for (UINT16 c = 1; c <= 2; c++)
			if (!test_opus(dsp, rates[r], c, FALSE) || !test_opus(dsp, rates[r], c, TRUE) ||
			    !test_pcm(dsp))
				goto out;
	for (size_t x = 0; x < 64; x++)
	{
		AUDIO_FORMAT format = opus_format((x & 1) ? 48000 : 16000, (UINT16)(1 + x % 2));
		if (!freerdp_dsp_context_reset(dsp, &format, 0) || !test_pcm(dsp))
			goto out;
	}
#if defined(WITH_FAAD2) && defined(WITH_FAAC)
	for (size_t x = 0; x < 8; x++)
		for (UINT16 c = 1; c <= 2; c++)
			if (!test_aac(dsp, (x & 1) ? 48000 : 44100, c) || !test_pcm(dsp))
				goto out;
#endif
	if (!test_encoder_reset())
		goto out;
	ok = TRUE;
out:
	freerdp_dsp_context_free(dsp);
	return ok ? 0 : -1;
}
