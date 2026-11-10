/* Copyright (c) 2026 Gitsnup
 *
 * Android frontend for mGBA, based on the SDL frontend.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "main.h"

#include "android-ui.h"
#include "android-peer-link.h"
#include "android-link-cable.h"
#include "android-peer-jni.h"

#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/input.h>
#include <mgba/core/serialize.h>
#include <mgba/core/thread.h>
#include <mgba/internal/gba/input.h>
#include <mgba/feature/commandline.h>
#include <mgba-util/vfs.h>

#include <SDL.h>
#include <arpa/inet.h>

#define PORT "android"
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
#include <string.h>

static struct mStandardLogger _logger;
static struct mPeerLink* sPeerLink;
static pthread_mutex_t sPeerLinkPointerLock = PTHREAD_MUTEX_INITIALIZER;
static struct mAndroidLinkCable* sLinkCable;

static int mSDLRun(struct mSDLRenderer* renderer, struct mArguments* args);

#define HNS_COOP_MAILBOX_MAGIC 0x504F4F43
#define HNS_COOP_MAILBOX_VERSION 8
#define HNS_COOP_MAILBOX_SIZE 2160
#define HNS_COOP_FLAG_QUEUE_SIZE 128
#define HNS_COOP_VAR_QUEUE_SIZE 128
#define GBA_EWRAM_START 0x02000000
#define GBA_EWRAM_END 0x02040000

static uint32_t sCoopMailboxAddress;
static uint32_t sCoopMailboxNextScan;
static uint32_t sCoopLastMapSent = UINT32_MAX;
static uint32_t sCoopLastSioActive = UINT32_MAX;
static uint32_t sCoopLastConfigFrame = UINT32_MAX;
static uint32_t sCoopLastConfigSequence = UINT32_MAX;
static bool sCoopHostRole;
static atomic_int sPendingPeerState = ATOMIC_VAR_INIT(-1);
static atomic_bool sPendingPeerHostRole = ATOMIC_VAR_INIT(false);

void mAndroidPeerLinkSetConnected(bool host) {
	atomic_store(&sPendingPeerHostRole, host);
	atomic_store(&sPendingPeerState, 1);
}

void mAndroidPeerLinkSetDisconnected(void) {
	atomic_store(&sPendingPeerState, 0);
}

void mAndroidPeerLinkReceiveFrame(const uint8_t* frame, size_t size) {
	pthread_mutex_lock(&sPeerLinkPointerLock);
	if (sPeerLink)
		mPeerLinkReceiveFrame(sPeerLink, frame, size);
	pthread_mutex_unlock(&sPeerLinkPointerLock);
}


static void mSDLAndroidUpdateCoopMailbox(struct mCore* core) {
	if (!core || !core->busRead32 || !core->busRead16 || !core->busWrite32)
		return;

	bool connected = sPeerLink && mPeerLinkIsConnected(sPeerLink);
	// Keep the normal single-player emulator path completely independent of
	// the co-op mailbox. Only discover a game mailbox after a peer connects.
	// If a known session disconnects, continue one-way updates so the game
	// receives its disconnected state.
	if (!connected && !sCoopMailboxAddress)
		return;

	uint32_t frame = core->frameCounter ? core->frameCounter(core) : 0;
	if (!sCoopMailboxAddress && frame >= sCoopMailboxNextScan) {
		sCoopMailboxNextScan = frame + 30;
		uint32_t address;
		for (address = GBA_EWRAM_START; address + HNS_COOP_MAILBOX_SIZE <= GBA_EWRAM_END; address += 4) {
			if (core->busRead32(core, address) == HNS_COOP_MAILBOX_MAGIC
				&& core->busRead16(core, address + 4) == HNS_COOP_MAILBOX_VERSION
				&& core->busRead16(core, address + 6) == HNS_COOP_MAILBOX_SIZE) {
				sCoopMailboxAddress = address;
				break;
			}
		}
	}

	if (sCoopMailboxAddress) {
		core->busWrite32(core, sCoopMailboxAddress + 8, connected ? 1 : 0);
		core->busWrite32(core, sCoopMailboxAddress + 2124, sCoopHostRole ? 1 : 0);
		if (!connected) {
			core->busWrite32(core, sCoopMailboxAddress + 2112, 0);
			core->busWrite32(core, sCoopMailboxAddress + 2120, 0);
			core->busWrite32(core, sCoopMailboxAddress + 2156, 0);
			sCoopLastMapSent = UINT32_MAX;
			sCoopLastSioActive = UINT32_MAX;
		}

		if (connected) {
            if (sCoopHostRole) {
                uint32_t configReady = core->busRead32(core, sCoopMailboxAddress + 2140);
                if (configReady && (configReady != sCoopLastConfigSequence
                    || sCoopLastConfigFrame == UINT32_MAX || frame - sCoopLastConfigFrame >= 60)) {
                    uint32_t config[4] = {
                        htonl(core->busRead32(core, sCoopMailboxAddress + 2128)),
                        htonl(core->busRead32(core, sCoopMailboxAddress + 2132)),
                        htonl(core->busRead32(core, sCoopMailboxAddress + 2136)),
                        htonl(configReady)
                    };
                    if (mPeerLinkSend(sPeerLink, mPEER_MSG_WORLD_CONFIG, config, sizeof(config))) {
                        sCoopLastConfigFrame = frame;
                        sCoopLastConfigSequence = configReady;
                    }
                }
            }
			uint32_t localMapId = core->busRead32(core, sCoopMailboxAddress + 2104);
			uint32_t localSioActive = core->busRead32(core, sCoopMailboxAddress + 2116) ? 1 : 0;
			if (localSioActive != sCoopLastSioActive)
				mPeerLinkSetLocalSioActive(sPeerLink, localSioActive != 0);
			if (localMapId != sCoopLastMapSent || localSioActive != sCoopLastSioActive || frame % 30 == 0) {
				uint32_t networkState[2] = { htonl(localMapId), htonl(localSioActive) };
				if (mPeerLinkSend(sPeerLink, mPEER_MSG_POSITION, networkState, sizeof(networkState))) {
					sCoopLastMapSent = localMapId;
					sCoopLastSioActive = localSioActive;
				}
			}

			uint32_t read = core->busRead32(core, sCoopMailboxAddress + 20);
			uint32_t write = core->busRead32(core, sCoopMailboxAddress + 16);
			unsigned sent = 0;
			while (read != write && sent < 4) {
				uint32_t packed = core->busRead32(core, sCoopMailboxAddress + 32 + (read % HNS_COOP_FLAG_QUEUE_SIZE) * 4);
				uint32_t networkPacked = htonl(packed);
				if (!mPeerLinkSend(sPeerLink, mPEER_MSG_WORLD_FLAG, &networkPacked, sizeof(networkPacked)))
					break;
				++read;
				++sent;
				core->busWrite32(core, sCoopMailboxAddress + 20, read);
			}

			uint32_t varRead = core->busRead32(core, sCoopMailboxAddress + 1060);
			uint32_t varWrite = core->busRead32(core, sCoopMailboxAddress + 1056);
			unsigned varsSent = 0;
			while (varRead != varWrite && varsSent < 4) {
				uint32_t packed = core->busRead32(core, sCoopMailboxAddress + 1072 + (varRead % HNS_COOP_VAR_QUEUE_SIZE) * 4);
				uint32_t networkPacked = htonl(packed);
				if (!mPeerLinkSend(sPeerLink, mPEER_MSG_WORLD_VAR, &networkPacked, sizeof(networkPacked)))
					break;
				++varRead;
				++varsSent;
				core->busWrite32(core, sCoopMailboxAddress + 1060, varRead);
			}

			// Keep advertising readiness until the peer confirms its initial
			// story-state transfer is also complete.
			uint32_t localReady = core->busRead32(core, sCoopMailboxAddress + 2096);
			if (localReady && frame % 60 == 0) {
				uint32_t ready = htonl(1);
				mPeerLinkSend(sPeerLink, mPEER_MSG_WORLD_READY, &ready, sizeof(ready));
			}

			struct mPeerEvent peerEvent;
			while (mPeerLinkPoll(sPeerLink, &peerEvent)) {
				if (peerEvent.type == mPEER_EVENT_ERROR) {
					SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Co-op peer event queue overflowed; some messages were dropped");
					continue;
				}
				if (peerEvent.type != mPEER_EVENT_MESSAGE)
					continue;
                if (peerEvent.messageType == mPEER_MSG_WORLD_CONFIG && !sCoopHostRole && peerEvent.size == 4 * sizeof(uint32_t)) {
                    uint32_t config[4];
                    memcpy(config, peerEvent.data, sizeof(config));
                    core->busWrite32(core, sCoopMailboxAddress + 2144, ntohl(config[0]));
                    core->busWrite32(core, sCoopMailboxAddress + 2148, ntohl(config[1]));
                    core->busWrite32(core, sCoopMailboxAddress + 2152, ntohl(config[2]));
                    uint32_t sequence = ntohl(config[3]);
                    core->busWrite32(core, sCoopMailboxAddress + 2156, sequence);
                    uint32_t ack = htonl(sequence);
                    mPeerLinkSend(sPeerLink, mPEER_MSG_WORLD_CONFIG_ACK, &ack, sizeof(ack));
                    continue;
                }
                if (peerEvent.messageType == mPEER_MSG_WORLD_CONFIG_ACK && sCoopHostRole && peerEvent.size == sizeof(uint32_t)) {
                    uint32_t networkSequence;
                    memcpy(&networkSequence, peerEvent.data, sizeof(networkSequence));
                    core->busWrite32(core, sCoopMailboxAddress + 2156, ntohl(networkSequence));
                    continue;
                }
				if (peerEvent.messageType == mPEER_MSG_POSITION && peerEvent.size == 2 * sizeof(uint32_t)) {
					uint32_t networkState[2];
					memcpy(networkState, peerEvent.data, sizeof(networkState));
					core->busWrite32(core, sCoopMailboxAddress + 2108, ntohl(networkState[0]));
					core->busWrite32(core, sCoopMailboxAddress + 2112, 1);
					core->busWrite32(core, sCoopMailboxAddress + 2120, ntohl(networkState[1]) ? 1 : 0);
					continue;
				}
				if (peerEvent.size != sizeof(uint32_t))
					continue;
				uint32_t networkPacked;
				memcpy(&networkPacked, peerEvent.data, sizeof(networkPacked));
				uint32_t packed = ntohl(networkPacked);
				if (peerEvent.messageType == mPEER_MSG_WORLD_FLAG) {
					uint32_t incomingWrite = core->busRead32(core, sCoopMailboxAddress + 24);
					uint32_t incomingRead = core->busRead32(core, sCoopMailboxAddress + 28);
					if (incomingWrite - incomingRead >= HNS_COOP_FLAG_QUEUE_SIZE)
						continue;
					core->busWrite32(core, sCoopMailboxAddress + 544 + (incomingWrite % HNS_COOP_FLAG_QUEUE_SIZE) * 4, packed);
					core->busWrite32(core, sCoopMailboxAddress + 24, incomingWrite + 1);
				} else if (peerEvent.messageType == mPEER_MSG_WORLD_VAR) {
					uint32_t incomingWrite = core->busRead32(core, sCoopMailboxAddress + 1064);
					uint32_t incomingRead = core->busRead32(core, sCoopMailboxAddress + 1068);
					if (incomingWrite - incomingRead >= HNS_COOP_VAR_QUEUE_SIZE)
						continue;
					core->busWrite32(core, sCoopMailboxAddress + 1584 + (incomingWrite % HNS_COOP_VAR_QUEUE_SIZE) * 4, packed);
					core->busWrite32(core, sCoopMailboxAddress + 1064, incomingWrite + 1);
				} else if (peerEvent.messageType == mPEER_MSG_WORLD_READY && packed != 0) {
					core->busWrite32(core, sCoopMailboxAddress + 2100, 1);
				} else if (peerEvent.messageType == mPEER_MSG_POSITION) {
					core->busWrite32(core, sCoopMailboxAddress + 2108, packed);
					core->busWrite32(core, sCoopMailboxAddress + 2112, 1);
				}
			}
		}
	}
}

static void mSDLAndroidApplyPendingPeerState(void) {
	int state = atomic_exchange(&sPendingPeerState, -1);
	if (state < 0)
		return;
	bool connected = state == 1;
	bool host = atomic_load(&sPendingPeerHostRole);
	sCoopLastMapSent = UINT32_MAX;
	sCoopLastSioActive = UINT32_MAX;
	sCoopLastConfigFrame = UINT32_MAX;
	sCoopLastConfigSequence = UINT32_MAX;
	sCoopHostRole = connected && host;
	if (sPeerLink) {
		mPeerLinkSetHostRole(sPeerLink, host);
		mPeerLinkSetConnected(sPeerLink, connected);
	}
	if (sLinkCable)
		mAndroidLinkCableSetRole(sLinkCable, connected && host);
}

static void mSDLAndroidRunloop(struct mSDLRenderer* renderer, void* user) {
	struct mCoreThread* context = user;
	SDL_Event event;
	bool firstFrameLogged = false;
	bool renderCopyErrorLogged = false;
	Uint32 lastDiagnostics = SDL_GetTicks();

	SDL_Log("Android video loop entered: window=%p renderer=%p texture=%p size=%ux%u output=%p",
		renderer->window, renderer->sdlRenderer, renderer->sdlTex, renderer->width, renderer->height, renderer->outputBuffer);
	while (mCoreThreadIsActive(context)) {
		Uint32 now = SDL_GetTicks();
		if ((Uint32)(now - lastDiagnostics) >= 2000) {
			lastDiagnostics = now;
			SDL_Log("Android emulator heartbeat: frame=%u active=%d crashed=%d output=%p sample0=%u sampleCenter=%u",
				renderer->core->frameCounter ? renderer->core->frameCounter(renderer->core) : 0,
				mCoreThreadIsActive(context), mCoreThreadHasCrashed(context), renderer->outputBuffer,
				renderer->outputBuffer ? (unsigned) renderer->outputBuffer[0] : 0,
				renderer->outputBuffer ? (unsigned) renderer->outputBuffer[(renderer->height / 2) * renderer->outputStride + (renderer->width / 2)] : 0);
		}
		while (SDL_PollEvent(&event)) {
			mAndroidOverlayHandleEvent(&event);
			mSDLHandleEvent(context, &renderer->player, &event);
		}

		if (!mCoreSyncWaitFrameStart(&context->impl->sync)) {
			// The core is waiting on this sync lock; apply mailbox reads/writes here
			// so the game bus is never accessed concurrently with CPU execution.
			mSDLAndroidApplyPendingPeerState();
			mSDLAndroidUpdateCoopMailbox(renderer->core);
			// Keep the Android controls visible even before the GBA core has
			// produced its first frame (or while video sync is waiting).
			// Release the sync mutex before doing SDL work so the core thread
			// can post its first frame without waiting for the UI renderer.
			mCoreSyncWaitFrameEnd(&context->impl->sync);

			SDL_Rect dest;
			dest.w = renderer->viewportHeight * renderer->width / renderer->height;
			dest.h = renderer->viewportHeight;
			if (dest.w > renderer->viewportWidth) {
				dest.w = renderer->viewportWidth;
				dest.h = renderer->viewportWidth * renderer->height / renderer->width;
			}
			dest.x = (renderer->viewportWidth - dest.w) / 2;
			dest.y = (renderer->viewportHeight - dest.h) / 2;

			SDL_SetRenderDrawColor(renderer->sdlRenderer, 0, 0, 0, 255);
			if (SDL_RenderClear(renderer->sdlRenderer) < 0) {
				SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Android idle screen clear failed: %s", SDL_GetError());
			}
			mAndroidOverlayDraw(renderer->sdlRenderer, &dest);
			SDL_RenderPresent(renderer->sdlRenderer);
			SDL_Delay(16);
			continue;
		}

		{
			SDL_UnlockTexture(renderer->sdlTex);

			SDL_Rect dest;
			dest.w = renderer->viewportHeight * renderer->width / renderer->height;
			dest.h = renderer->viewportHeight;
			if (dest.w > renderer->viewportWidth) {
				dest.w = renderer->viewportWidth;
				dest.h = renderer->viewportWidth * renderer->height / renderer->width;
			}
			dest.x = (renderer->viewportWidth - dest.w) / 2;
			dest.y = (renderer->viewportHeight - dest.h) / 2;

			SDL_SetRenderDrawColor(renderer->sdlRenderer, 0, 0, 0, 255);
			if (SDL_RenderClear(renderer->sdlRenderer) < 0) {
				SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Android frame clear failed: %s", SDL_GetError());
			}
			bool frameCopied = SDL_RenderCopy(renderer->sdlRenderer, renderer->sdlTex, 0, &dest) == 0;
			if (!frameCopied && !renderCopyErrorLogged) {
				SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Android game frame copy failed: %s", SDL_GetError());
				renderCopyErrorLogged = true;
			}
			mAndroidOverlayDraw(renderer->sdlRenderer, &dest);
			SDL_RenderPresent(renderer->sdlRenderer);
			if (frameCopied && !firstFrameLogged) {
				firstFrameLogged = true;
				SDL_Log("First Android game frame copied and presented; core frame=%u", renderer->core->frameCounter ? renderer->core->frameCounter(renderer->core) : 0);
			}

			int stride = 0;
			if (SDL_LockTexture(renderer->sdlTex, 0, (void**) &renderer->outputBuffer, &stride) < 0 || !renderer->outputBuffer) {
				SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Android game texture relock failed: %s", SDL_GetError());
				mCoreThreadEnd(context);
				mCoreSyncWaitFrameEnd(&context->impl->sync);
				break;
			}
			renderer->outputStride = stride / BYTES_PER_PIXEL;
			renderer->core->setVideoBuffer(renderer->core, renderer->outputBuffer, renderer->outputStride);
			mSDLAndroidApplyPendingPeerState();
			mSDLAndroidUpdateCoopMailbox(renderer->core);
		}
		mCoreSyncWaitFrameEnd(&context->impl->sync);
	}
	if (!firstFrameLogged) {
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Android video loop exited before presenting its first frame (active=%d, crashed=%d)",
			mCoreThreadIsActive(context), mCoreThreadHasCrashed(context));
	}
}

static void mSDLDeinit(struct mSDLRenderer* renderer) {
	mSDLDeinitEvents(&renderer->events);
	if (renderer->audioInitialized) {
		mSDLDeinitAudio(&renderer->audio);
		renderer->audioInitialized = false;
	}
	SDL_DestroyWindow(renderer->window);
	renderer->deinit(renderer);
	SDL_Quit();
}

int main(int argc, char** argv) {
	struct mSDLRenderer renderer = {0};

	struct mCoreOptions opts = {
		.useBios = true,
		.rewindEnable = true,
		.rewindBufferCapacity = 600,
		.rewindBufferInterval = 1,
		.audioBuffers = 1024,
		.videoSync = false,
		// Video must remain independent of audio-device callback timing.
		// This Android baseline intentionally runs audio asynchronously.
		.audioSync = false,
		.volume = 0x100,
		.logLevel = mLOG_WARN | mLOG_ERROR | mLOG_FATAL,
		.fullscreen = 1,
	};

	struct mArguments args;
	struct mGraphicsOpts graphicsOpts;
	memset(&args, 0, sizeof(args));

	struct mSubParser subparser;

	mSubParserGraphicsInit(&subparser, &graphicsOpts);
	bool parsed = mArgumentsParse(&args, argc, argv, &subparser, 1);
	if (args.showVersion) {
		version(argv[0]);
		mArgumentsDeinit(&args);
		return 0;
	}

	if (!SDL_OK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS))) {
		printf("Could not initialize SDL: %s\n", SDL_GetError());
		mArgumentsDeinit(&args);
		return 1;
	}

	setenv("XDG_CONFIG_HOME", SDL_AndroidGetInternalStoragePath(), 1);

	if (!args.fname) {
		args.fname = mAndroidPickRom();
		if (!args.fname) {
			SDL_Quit();
			mArgumentsDeinit(&args);
			return 0;
		}
	}

	SDL_Log("Resolving ROM core for: %s", args.fname);
	renderer.core = mCoreFind(args.fname);
	if (!renderer.core) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No compatible emulator core found for selected ROM");
		printf("Could not run game. Are you sure the file exists and is a compatible game?\n");
		mArgumentsDeinit(&args);
		SDL_Quit();
		return 1;
	}

	if (!renderer.core->init(renderer.core)) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Emulator core initialization failed");
		mArgumentsDeinit(&args);
		SDL_Quit();
		return 1;
	}

	renderer.core->baseVideoSize(renderer.core, &renderer.width, &renderer.height);
	renderer.ratio = 0;
	if (renderer.ratio == 0) {
		renderer.ratio = 1;
	}
	opts.width = renderer.width * renderer.ratio;
	opts.height = renderer.height * renderer.ratio;

	mInputMapInit(&renderer.core->inputMap, &GBAInputInfo);
	mCoreInitConfig(renderer.core, PORT);
	mArgumentsApply(&args, &subparser, 1, &renderer.core->config);

	mCoreConfigSetDefaultIntValue(&renderer.core->config, "logToStdout", true);
	mCoreConfigLoadDefaults(&renderer.core->config, &opts);
	mCoreLoadConfig(renderer.core);
	mStandardLoggerInit(&_logger);
	mStandardLoggerConfig(&_logger, &renderer.core->config);
	mLogSetDefaultLogger(&_logger.d);

	renderer.viewportWidth = renderer.core->opts.width;
	renderer.viewportHeight = renderer.core->opts.height;
	renderer.player.fullscreen = 1;
	renderer.player.windowUpdated = 0;

	renderer.lockAspectRatio = renderer.core->opts.lockAspectRatio;
	renderer.lockIntegerScaling = renderer.core->opts.lockIntegerScaling;
	renderer.interframeBlending = renderer.core->opts.interframeBlending;
	renderer.filter = renderer.core->opts.resampleVideo;

	mSDLSWCreate(&renderer);
	renderer.runloop = mSDLAndroidRunloop;

	if (!renderer.init(&renderer)) {
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Android renderer initialization failed: %s", SDL_GetError());
		mArgumentsDeinit(&args);
		mCoreConfigDeinit(&renderer.core->config);
		renderer.core->deinit(renderer.core);
		SDL_Quit();
		return 1;
	}
	mAndroidOverlaySetWindow(renderer.window);

	renderer.player.bindings = &renderer.core->inputMap;
	mSDLInitBindingsGBA(&renderer.core->inputMap);
	mSDLInitEvents(&renderer.events);
	mSDLEventsLoadConfig(&renderer.events, mCoreConfigGetInput(&renderer.core->config));
	mSDLAttachPlayer(&renderer.events, &renderer.player, -1);
	mSDLPlayerLoadConfig(&renderer.player, mCoreConfigGetInput(&renderer.core->config));

#if SDL_VERSION_ATLEAST(2, 0, 0)
	renderer.core->setPeripheral(renderer.core, mPERIPH_RUMBLE, &renderer.player.rumble.d.d);
#endif

	pthread_mutex_lock(&sPeerLinkPointerLock);
	sPeerLink = mPeerLinkCreate();
	struct mPeerLink* peerLink = sPeerLink;
	pthread_mutex_unlock(&sPeerLinkPointerLock);
	if (peerLink) {
		mPeerLinkUseJavaTransport(peerLink, true);
		sLinkCable = mAndroidLinkCableCreate(peerLink, true);
		// Java may have preserved an accepted peer session while SDL was paused
		// or the Activity was recreated. Reapply that state after the native link exists.
		mAndroidPeerJniSyncConnection();
	}
	SDL_Log("Android renderer ready; loading selected ROM: %s", args.fname);
	int ret = mSDLRun(&renderer, &args);
	mAndroidOverlaySetWindow(NULL);
	mSDLDetachPlayer(&renderer.events, &renderer.player);
	pthread_mutex_lock(&sPeerLinkPointerLock);
	struct mPeerLink* peerLinkToDestroy = sPeerLink;
	sPeerLink = NULL;
	pthread_mutex_unlock(&sPeerLinkPointerLock);
	if (peerLinkToDestroy)
		mPeerLinkDestroy(peerLinkToDestroy);
	mInputMapDeinit(&renderer.core->inputMap);

	mSDLDeinit(&renderer);
	struct mAndroidLinkCable* linkCable = sLinkCable;
	struct mPeerLink* peerLink = sPeerLink;
	sLinkCable = NULL;
	sPeerLink = NULL;
	mAndroidLinkCableDestroy(linkCable);
	mPeerLinkDestroy(peerLink);
	mStandardLoggerDeinit(&_logger);

	mArgumentsDeinit(&args);
	mCoreConfigFreeOpts(&opts);
	mCoreConfigDeinit(&renderer.core->config);
	renderer.core->deinit(renderer.core);

	return ret;
}

int mSDLRun(struct mSDLRenderer* renderer, struct mArguments* args) {
	struct mCoreThread thread = {
		.core = renderer->core
	};
	sCoopMailboxAddress = 0;
	sCoopMailboxNextScan = 0;
	sCoopLastMapSent = UINT32_MAX;
	sCoopLastSioActive = UINT32_MAX;
	SDL_Log("Loading ROM: %s", args->fname ? args->fname : "(null)");
	if (!mCoreLoadFile(renderer->core, args->fname)) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "mCoreLoadFile failed for selected ROM");
		return 1;
	}
	SDL_Log("ROM loaded successfully: %s", args->fname);
	if (sLinkCable)
		mAndroidLinkCableAttach(renderer->core, sLinkCable);
	mCoreAutoloadSave(renderer->core);
	mArgumentsApplyFileLoads(args, renderer->core);

	renderer->audio.samples = renderer->core->opts.audioBuffers;
	renderer->audio.sampleRate = 44100;
	thread.logger.logger = &_logger.d;

	bool didFail = !mCoreThreadStart(&thread);
	if (didFail) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "mCoreThreadStart failed; emulator core did not start");
	} else {
		SDL_Log("Emulator core thread started successfully");
	}

	if (!didFail) {
		mSDLSetScreensaverSuspendable(&renderer->events, renderer->core->opts.suspendScreensaver);
		mSDLSuspendScreensaver(&renderer->events);
		renderer->audioInitialized = mSDLInitAudio(&renderer->audio, &thread);
		if (!renderer->audioInitialized) {
			// Audio is optional on Android. A missing or unavailable audio
			// device must never prevent video and touch controls from running.
			printf("Audio unavailable; continuing with video only: %s\n", SDL_GetError());
			MutexLock(&thread.impl->sync.audioBufferMutex);
			thread.impl->sync.audioWait = false;
			MutexUnlock(&thread.impl->sync.audioBufferMutex);
		}
		renderer->runloop(renderer, &thread);
		if (renderer->audioInitialized)
			mSDLPauseAudio(&renderer->audio);
		if (mCoreThreadHasCrashed(&thread)) {
			didFail = true;
			printf("The game crashed!\\n");
			mCoreThreadEnd(&thread);
		}
		mSDLResumeScreensaver(&renderer->events);
		mSDLSetScreensaverSuspendable(&renderer->events, false);

		mCoreThreadJoin(&thread);
	} else {
		printf("Could not run game. Are you sure the file exists and is a compatible game?\n");
	}
	renderer->core->unloadROM(renderer->core);
	if (sLinkCable)
		mAndroidLinkCableDetach(renderer->core);

	return didFail;
}
