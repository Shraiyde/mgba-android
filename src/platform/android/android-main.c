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
#include <string.h>

static struct mStandardLogger _logger;
static struct mPeerLink* sPeerLink;
static struct mAndroidLinkCable* sLinkCable;

static int mSDLRun(struct mSDLRenderer* renderer, struct mArguments* args);

#define HNS_COOP_MAILBOX_MAGIC 0x504F4F43
#define HNS_COOP_MAILBOX_VERSION 3
#define HNS_COOP_MAILBOX_SIZE 2096
#define HNS_COOP_FLAG_QUEUE_SIZE 128
#define HNS_COOP_VAR_QUEUE_SIZE 128
#define GBA_EWRAM_START 0x02000000
#define GBA_EWRAM_END 0x02040000

static uint32_t sCoopMailboxAddress;
static uint32_t sCoopMailboxNextScan;

void mAndroidPeerLinkSetConnected(bool host) {
	if (sPeerLink) {
		mPeerLinkSetHostRole(sPeerLink, host);
		mPeerLinkSetConnected(sPeerLink, true);
	}
	if (sLinkCable)
		mAndroidLinkCableSetRole(sLinkCable, host);
}

void mAndroidPeerLinkSetDisconnected(void) {
	if (sPeerLink)
		mPeerLinkSetConnected(sPeerLink, false);
}

void mAndroidPeerLinkReceiveFrame(const uint8_t* frame, size_t size) {
	if (sPeerLink)
		mPeerLinkReceiveFrame(sPeerLink, frame, size);
}


static void mSDLAndroidUpdateCoopMailbox(struct mCore* core) {
	if (!core || !core->busRead32 || !core->busRead16 || !core->busWrite32)
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
		bool connected = mPeerLinkIsConnected(sPeerLink);
		core->busWrite32(core, sCoopMailboxAddress + 8, connected ? 1 : 0);

		if (connected) {
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

			struct mPeerEvent peerEvent;
			while (mPeerLinkPoll(sPeerLink, &peerEvent)) {
				if (peerEvent.type != mPEER_EVENT_MESSAGE || peerEvent.size != sizeof(uint32_t))
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
				}
			}
		}
	}
}

static void mSDLAndroidRunloop(struct mSDLRenderer* renderer, void* user) {
	struct mCoreThread* context = user;
	SDL_Event event;

	while (mCoreThreadIsActive(context)) {
		while (SDL_PollEvent(&event)) {
			mAndroidOverlayHandleEvent(&event);
			mSDLHandleEvent(context, &renderer->player, &event);
		}

		if (mCoreSyncWaitFrameStart(&context->impl->sync)) {
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
			SDL_RenderClear(renderer->sdlRenderer);
			SDL_RenderCopy(renderer->sdlRenderer, renderer->sdlTex, 0, &dest);
			mAndroidOverlayDraw(renderer->sdlRenderer, &dest);
			SDL_RenderPresent(renderer->sdlRenderer);

			int stride;
			SDL_LockTexture(renderer->sdlTex, 0, (void**) &renderer->outputBuffer, &stride);
			renderer->core->setVideoBuffer(renderer->core, renderer->outputBuffer, stride / BYTES_PER_PIXEL);
		}
		mCoreSyncWaitFrameEnd(&context->impl->sync);
		mSDLAndroidUpdateCoopMailbox(renderer->core);
	}
}

static void mSDLDeinit(struct mSDLRenderer* renderer) {
	mSDLDeinitEvents(&renderer->events);
	mSDLDeinitAudio(&renderer->audio);
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
		.audioSync = true,
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

	renderer.core = mCoreFind(args.fname);
	if (!renderer.core) {
		printf("Could not run game. Are you sure the file exists and is a compatible game?\n");
		mArgumentsDeinit(&args);
		SDL_Quit();
		return 1;
	}

	if (!renderer.core->init(renderer.core)) {
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

	sPeerLink = mPeerLinkCreate();
	if (sPeerLink) {
		mPeerLinkUseJavaTransport(sPeerLink, true);
		sLinkCable = mAndroidLinkCableCreate(sPeerLink, true);
	}
	int ret = mSDLRun(&renderer, &args);
	mSDLDetachPlayer(&renderer.events, &renderer.player);
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
	if (!mCoreLoadFile(renderer->core, args->fname)) {
		return 1;
	}
	if (sLinkCable)
		mAndroidLinkCableAttach(renderer->core, sLinkCable);
	mAndroidPeerJniShowDialog();
	mCoreAutoloadSave(renderer->core);
	mArgumentsApplyFileLoads(args, renderer->core);

	renderer->audio.samples = renderer->core->opts.audioBuffers;
	renderer->audio.sampleRate = 44100;
	thread.logger.logger = &_logger.d;

	bool didFail = !mCoreThreadStart(&thread);

	if (!didFail) {
		mSDLSetScreensaverSuspendable(&renderer->events, renderer->core->opts.suspendScreensaver);
		mSDLSuspendScreensaver(&renderer->events);
		if (mSDLInitAudio(&renderer->audio, &thread)) {
			renderer->runloop(renderer, &thread);
			mSDLPauseAudio(&renderer->audio);
			if (mCoreThreadHasCrashed(&thread)) {
				didFail = true;
				printf("The game crashed!\n");
				mCoreThreadEnd(&thread);
			}
		} else {
			didFail = true;
			printf("Could not initialize audio.\n");
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
