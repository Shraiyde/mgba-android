/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#define _POSIX_C_SOURCE 200809L
#include "sdl-audio.h"

#include <mgba/core/core.h>
#include <mgba/core/thread.h>

#include <time.h>

mLOG_DEFINE_CATEGORY(SDL_AUDIO, "SDL Audio", "platform.sdl.audio");

#if SDL_VERSION_ATLEAST(3, 0, 0)
static void _mSDLAudioCallback(void* context, SDL_AudioStream* stream, int additionalLen, int totalLen);
#else
static void _mSDLAudioCallback(void* context, Uint8* data, int len);
#endif

bool mSDLInitAudio(struct mSDLAudio* context, struct mCoreThread* threadContext) {
#if defined(_WIN32) && SDL_VERSION_ATLEAST(2, 0, 8) && !SDL_VERSION_ATLEAST(3, 0, 0)
	if (!getenv("SDL_AUDIODRIVER")) {
		_putenv_s("SDL_AUDIODRIVER", "directsound");
	}
#endif
	if (!SDL_OK(SDL_InitSubSystem(SDL_INIT_AUDIO))) {
		mLOG(SDL_AUDIO, ERROR, "Could not initialize SDL sound system: %s", SDL_GetError());
		return false;
	}

	memset(&context->desiredSpec, 0, sizeof(context->desiredSpec));
	memset(&context->obtainedSpec, 0, sizeof(context->obtainedSpec));
	context->desiredSpec.freq = context->sampleRate;
	context->desiredSpec.channels = 2;
#if SDL_VERSION_ATLEAST(3, 0, 0)
	context->desiredSpec.format = SDL_AUDIO_S16;
	char hint[21];
	snprintf(hint, sizeof(hint), "%" PRIz "u", context->samples);
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, hint);
	context->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &context->desiredSpec, _mSDLAudioCallback, context);
	if (!context->stream) {
#else
	context->desiredSpec.callback = _mSDLAudioCallback;
	context->desiredSpec.userdata = context;
	context->desiredSpec.samples = context->samples;
	context->desiredSpec.format = AUDIO_S16SYS;
#if SDL_VERSION_ATLEAST(2, 0, 0)
	context->deviceId = SDL_OpenAudioDevice(0, 0, &context->desiredSpec, &context->obtainedSpec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (context->deviceId == 0) {
#else
	if (SDL_OpenAudio(&context->desiredSpec, &context->obtainedSpec) < 0) {
#endif
#endif
		mLOG(SDL_AUDIO, ERROR, "Could not open SDL sound system");
		return false;
	}
	context->core = NULL;

#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_GetAudioStreamFormat(context->stream, &context->obtainedSpec, NULL);
#endif
	if (context->obtainedSpec.channels == 0) {
		// This should never happen, but let's make sure
#if SDL_VERSION_ATLEAST(3, 0, 0)
		SDL_DestroyAudioStream(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
		SDL_PauseAudioDevice(context->deviceId, 1);
		SDL_CloseAudioDevice(context->deviceId);
#else
		SDL_PauseAudio(1);
		SDL_CloseAudio();
#endif
		return false;
	}

	// Size the output ring to the buffer the device actually callbacks with.
	// Android's AudioTrack backend raises this above the requested 1024 frames
	// to satisfy AudioTrack.getMinBufferSize; filling only the request and
	// zeroing the rest both drops out and stretches each callback to a
	// fraction of realtime.
	unsigned bufferFrames = context->obtainedSpec.samples ? context->obtainedSpec.samples : context->samples;
	context->nextCallbackNs = 0;
	context->underruns = 0;
	context->underrunFrames = 0;
	mAudioBufferInit(&context->buffer, bufferFrames, context->obtainedSpec.channels);
	mAudioResamplerInit(&context->resampler, mINTERPOLATOR_SINC);
	mAudioResamplerSetDestination(&context->resampler, &context->buffer, context->obtainedSpec.freq);

	if (threadContext) {
		context->core = threadContext->core;
		context->sync = &threadContext->impl->sync;

		mSDLResumeAudio(context);
	}

	return true;
}

void mSDLDeinitAudio(struct mSDLAudio* context) {
	UNUSED(context);
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_DestroyAudioStream(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_PauseAudioDevice(context->deviceId, 1);
	SDL_CloseAudioDevice(context->deviceId);
#else
	SDL_PauseAudio(1);
	SDL_CloseAudio();
#endif
	mAudioBufferDeinit(&context->buffer);
	mAudioResamplerDeinit(&context->resampler);
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void mSDLPauseAudio(struct mSDLAudio* context) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_PauseAudioStreamDevice(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_PauseAudioDevice(context->deviceId, 1);
#else
	UNUSED(context);
	SDL_PauseAudio(1);
#endif
}

void mSDLResumeAudio(struct mSDLAudio* context) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
	SDL_ResumeAudioStreamDevice(context->stream);
#elif SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_PauseAudioDevice(context->deviceId, 0);
#else
	UNUSED(context);
	SDL_PauseAudio(0);
#endif
}

static void _mSDLPaceCallback(struct mSDLAudio* audioContext) {
	unsigned frames = audioContext->obtainedSpec.samples ? audioContext->obtainedSpec.samples : (unsigned) audioContext->samples;
	unsigned freq = audioContext->obtainedSpec.freq ? (unsigned) audioContext->obtainedSpec.freq : audioContext->sampleRate;
	struct timespec now;
	uint64_t nowNs;
	uint64_t periodNs;
	if (!frames || !freq)
		return;
	// One callback's worth of audio. If the driver already blocked in its
	// write, this deadline is in the past and we do not sleep again.
	periodNs = (uint64_t) frames * 1000000000ull / freq;
	clock_gettime(CLOCK_MONOTONIC, &now);
	nowNs = (uint64_t) now.tv_sec * 1000000000ull + (uint64_t) now.tv_nsec;
	if (audioContext->nextCallbackNs > nowNs) {
		struct timespec abs;
		abs.tv_sec = (time_t) (audioContext->nextCallbackNs / 1000000000ull);
		abs.tv_nsec = (long) (audioContext->nextCallbackNs % 1000000000ull);
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, NULL);
		nowNs = audioContext->nextCallbackNs;
	}
	audioContext->nextCallbackNs = nowNs + periodNs;
}

#if SDL_VERSION_ATLEAST(3, 0, 0)
static void _mSDLAudioCallback(void* context, SDL_AudioStream* stream, int additionalLen, int len) {
	UNUSED(additionalLen);
#else
static void _mSDLAudioCallback(void* context, Uint8* data, int len) {
#endif
	struct mSDLAudio* audioContext = context;
	if (!context || !audioContext->core) {
#if SDL_VERSION_ATLEAST(3, 0, 0)
		return;
#else
		memset(data, 0, len);
		return;
#endif
	}
	// Pace before touching the core. A driver that queues without blocking
	// would otherwise run this callback in a burst and pad the rest with silence.
	_mSDLPaceCallback(audioContext);
	struct mAudioBuffer* buffer = audioContext->core->getAudioBuffer(audioContext->core);
	unsigned sampleRate = audioContext->core->audioSampleRate(audioContext->core);
	double fauxClock = 1;
	if (audioContext->sync) {
		if (audioContext->sync->fpsTarget > 0 && audioContext->core) {
			fauxClock = mCoreCalculateFramerateRatio(audioContext->core, audioContext->sync->fpsTarget);
		}
		mCoreSyncLockAudio(audioContext->sync);
		// Pace the core to the callback the device is actually running, not
		// the 1024-frame request. One full callback of real samples then
		// takes the same wall time as the audio it contains.
		unsigned deviceFrames = audioContext->obtainedSpec.samples ? audioContext->obtainedSpec.samples : audioContext->samples;
		audioContext->sync->audioHighWater = deviceFrames + audioContext->resampler.highWaterMark + audioContext->resampler.lowWaterMark + (deviceFrames >> 6);
		double deviceFreq = audioContext->obtainedSpec.freq ? (double) audioContext->obtainedSpec.freq : 1.0;
		double inputPerOutput = (double) sampleRate / (fauxClock * deviceFreq);
		if (inputPerOutput > 0.0) {
			audioContext->sync->audioHighWater = (size_t) ((double) audioContext->sync->audioHighWater * inputPerOutput + 0.5);
		}

		if (audioContext->resampler.source) {
			size_t destSize = mAudioBufferCapacity(audioContext->resampler.source);
			if (audioContext->sync->audioHighWater >= destSize) {
				audioContext->sync->audioHighWater = destSize;
			}
		}
	}
	mAudioResamplerSetSource(&audioContext->resampler, buffer, sampleRate / fauxClock, true);
	mAudioResamplerProcess(&audioContext->resampler);
	if (audioContext->sync) {
		mCoreSyncConsumeAudio(audioContext->sync);
	}
#if SDL_VERSION_ATLEAST(3, 0, 0)
	int16_t data[2048];
	while (len > 0) {
		int thisRead = sizeof(data);
		if (len < thisRead) {
			thisRead = len;
		}
		int available = mAudioBufferRead(&audioContext->buffer, data, thisRead / 4) * 4;
		if (!available) {
			break;
		}
		SDL_PutAudioStreamData(stream, data, available);
		len -= available;
	}
#else
	if (audioContext->obtainedSpec.channels)
		len /= 2 * audioContext->obtainedSpec.channels;
	int available = mAudioBufferRead(&audioContext->buffer, (int16_t*) data, len);

	if (available < len) {
		__atomic_fetch_add(&audioContext->underruns, 1u, __ATOMIC_RELAXED);
		__atomic_fetch_add(&audioContext->underrunFrames, (unsigned) (len - available), __ATOMIC_RELAXED);
		memset(((short*) data) + audioContext->obtainedSpec.channels * available, 0, (len - available) * audioContext->obtainedSpec.channels * sizeof(short));
	}
#endif
}
