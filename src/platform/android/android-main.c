/* Copyright (c) 2026 Gitsnup
 *
 * Android frontend for mGBA, based on the SDL frontend.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#define _POSIX_C_SOURCE 200809L
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
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/feature/commandline.h>
#include <mgba-util/vfs.h>

#include <SDL.h>
#include <arpa/inet.h>

#define PORT "android"
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/stat.h>

static struct mStandardLogger _logger;
static struct mPeerLink* sPeerLink;
static pthread_mutex_t sPeerLinkPointerLock = PTHREAD_MUTEX_INITIALIZER;
static struct mAndroidLinkCable* sLinkCable;

static int mSDLRun(struct mSDLRenderer* renderer, struct mArguments* args);

#define HNS_COOP_MAILBOX_MAGIC 0x504F4F43
#define HNS_COOP_MAILBOX_VERSION 9
#define HNS_COOP_MAILBOX_SIZE 2176
#define HNS_COOP_FLAG_QUEUE_SIZE 128
#define HNS_COOP_VAR_QUEUE_SIZE 128
#define HNS_COOP_OFF_GAME_READY 12
#define HNS_COOP_OFF_LOCAL_STORY_READY 2096
#define HNS_COOP_OFF_LOCAL_X 2160
#define HNS_COOP_OFF_LOCAL_Y 2164
#define HNS_COOP_OFF_REMOTE_X 2168
#define HNS_COOP_OFF_REMOTE_Y 2172
#define GBA_EWRAM_START 0x02000000
#define GBA_EWRAM_END 0x02040000

static uint32_t sCoopMailboxAddress;
static uint32_t sCoopMailboxNextScan;
static uint32_t sCoopLastMapSent = UINT32_MAX;
static uint32_t sCoopLastSioActive = UINT32_MAX;
static uint32_t sCoopLastXSent = UINT32_MAX;
static uint32_t sCoopLastYSent = UINT32_MAX;
static uint32_t sCoopLastConfigFrame = UINT32_MAX;
static uint32_t sCoopLastConfigSequence = UINT32_MAX;
static bool sCoopHostRole;
static uint32_t sCoopPrevLocalSio = UINT32_MAX;
static atomic_int sPendingPeerState = ATOMIC_VAR_INIT(-1);
static atomic_bool sPendingPeerHostRole = ATOMIC_VAR_INIT(false);
static atomic_uint sPeerGeneration;
static uint32_t sAppliedPeerGeneration;
static bool sPeerSessionOpen;

static char sAndroidRomStatus[192] = "ROM: not checked";
static char sAndroidLoadStatus[96] = "LOAD: not attempted";
static char sAndroidLastError[192] = "ERROR: none";
static int sAndroidSwapIntervalSet = -1;
static int sAndroidSwapInterval = -1;
static void* sFrameUploadBuffer;
static size_t sFrameUploadBytes;
static int sAudioOpenedFreq;
static int sAudioOpenedFrames;

static bool mSDLAndroidFrameIsBlack(const struct mSDLRenderer* renderer) {
	if (!renderer || !renderer->outputBuffer || renderer->outputStride < (int) renderer->width)
		return true;
	unsigned y, x;
	for (y = 0; y < renderer->height; ++y) {
		const mColor* row = renderer->outputBuffer + y * renderer->outputStride;
		for (x = 0; x < renderer->width; ++x) {
#ifdef COLOR_16_BIT
# ifdef COLOR_5_6_5
			if (row[x] != 0)
# else
			if ((row[x] & 0x7FFF) != 0)
# endif
#else
			if ((row[x] & 0x00FFFFFF) != 0)
#endif
				return false;
		}
	}
	return true;
}

static void mSDLAndroidUpdateDebugOverlay(struct mSDLRenderer* renderer, unsigned frames, bool frameBlack, bool threadActive, bool threadCrashed, bool presented, double coreFps, double drawFps, double showFps, double waitMs, double uploadMs, double drawMs, double presentMs, double coopMs, bool frameMutexHeld) {
	char frameStatus[192];
	char threadStatus[96];
	char videoStatus[192];
	char paceStatus[192];
	struct GBA* gba = renderer && renderer->core ? (struct GBA*) renderer->core->board : NULL;
	struct ARMCore* cpu = renderer && renderer->core ? renderer->core->cpu : NULL;
	unsigned underruns = 0;
	if (renderer)
		underruns = __atomic_load_n(&renderer->audio.underruns, __ATOMIC_RELAXED);
	snprintf(frameStatus, sizeof(frameStatus), "FPS CORE:%.1f DISTINCT:%.1f DRAW:%.1f | BLACK:%s",
		coreFps, showFps, drawFps, frames ? (frameBlack ? "YES" : "NO") : "WAITING");
	snprintf(paceStatus, sizeof(paceStatus), "PRESENTED:%.1f FPS  UNDERRUNS:%u  AUDIO:%dHz/%d",
		showFps, underruns, sAudioOpenedFreq, sAudioOpenedFrames);
	snprintf(threadStatus, sizeof(threadStatus), "EMULATION THREAD: %s",
		threadCrashed ? "CRASHED" : (threadActive ? "RUNNING" : "STOPPED"));
	if (gba && cpu && gba->sync) {
		static int pending;
		static bool videoWait;
		static uint32_t postCount, startCount, acceptedCount, endCount;
		static bool haveSyncSnapshot;
		static uint64_t lastVideoWaitNs, lastAudioWaitNs, lastAudioWaitCount;
		static uint64_t lastVideoRunWallNs, lastVideoRunCpuNs, lastVideoRunCount;
		static uint32_t lastVideoPostCount;
		static double videoPostWaitMs, audioWaitMs, videoRunWallMs, videoRunCpuMs;
		if (frameMutexHeld || MutexTryLock(&gba->sync->videoFrameMutex) == 0) {
			pending = gba->sync->videoFramePending;
			videoWait = gba->sync->videoFrameWait;
			postCount = gba->sync->videoPostCount;
			startCount = gba->sync->videoStartCount;
			acceptedCount = gba->sync->videoAcceptedCount;
			endCount = gba->sync->videoEndCount;
			uint64_t total = gba->sync->videoPostWaitNs;
			if (total >= lastVideoWaitNs && postCount > lastVideoPostCount)
				videoPostWaitMs = (double) (total - lastVideoWaitNs) / 1000000.0 / (double) (postCount - lastVideoPostCount);
			lastVideoWaitNs = total;
			lastVideoPostCount = postCount;
			uint64_t runCount = gba->sync->videoRunCount;
			uint64_t runWall = gba->sync->videoRunWallNs;
			uint64_t runCpu = gba->sync->videoRunCpuNs;
			if (runCount > lastVideoRunCount) {
				videoRunWallMs = (double) (runWall - lastVideoRunWallNs) / 1000000.0 / (double) (runCount - lastVideoRunCount);
				videoRunCpuMs = (double) (runCpu - lastVideoRunCpuNs) / 1000000.0 / (double) (runCount - lastVideoRunCount);
			}
			lastVideoRunCount = runCount;
			lastVideoRunWallNs = runWall;
			lastVideoRunCpuNs = runCpu;
			if (!frameMutexHeld)
				MutexUnlock(&gba->sync->videoFrameMutex);
			haveSyncSnapshot = true;
		}
		if (MutexTryLock(&gba->sync->audioBufferMutex) == 0) {
			uint64_t total = gba->sync->audioWaitNs;
			uint64_t count = gba->sync->audioWaitCount;
			if (count > lastAudioWaitCount && total >= lastAudioWaitNs)
				audioWaitMs = (double) (total - lastAudioWaitNs) / 1000000.0 / (double) (count - lastAudioWaitCount);
			lastAudioWaitNs = total;
			lastAudioWaitCount = count;
			MutexUnlock(&gba->sync->audioBufferMutex);
		}
		SDL_RendererInfo info = {0};
		bool haveRendererInfo = SDL_GetRendererInfo(renderer->sdlRenderer, &info) == 0;
		const char* rendererName = haveRendererInfo ? info.name : "unknown";
		const char* vsyncStatus = sAndroidSwapIntervalSet == 0
			? (sAndroidSwapInterval == 0 ? "OFF" : "MISMATCH")
			: (sAndroidSwapIntervalSet == -2 ? "NOGL" : "ERROR");
		snprintf(videoStatus, sizeof(videoStatus), "%s W%.1f U%.1f X%.1f P%.1f C%.1f A%.1f VW%.1f R%.1f CPU%.1f VS=%s TV=%d VC=%d Q=%d",
			rendererName ? rendererName : "unknown", waitMs, uploadMs, drawMs, presentMs, coopMs, audioWaitMs, videoPostWaitMs,
			videoRunWallMs, videoRunCpuMs, vsyncStatus, renderer->core->videoLogger != NULL, gba->video.vcount, haveSyncSnapshot ? pending : -1);
		SDL_Log("GBA diagnostic: fpsCore=%.2f fpsDraw=%.2f waitMs=%.3f uploadMs=%.3f presentMs=%.3f coopMs=%.3f renderer=%s pc=%08X cycles=%d nextEvent=%d halted=%d vcount=%d frames=%u event=%s@%u timingRoot=%p cpuBlocked=%d rom=%p romSize=%zu BIOS=%d syncPending=%d syncWait=%d post=%u start=%u accepted=%u end=%u",
			coreFps, drawFps, waitMs, uploadMs, presentMs, coopMs, rendererName ? rendererName : "unknown",
			cpu->gprs[ARM_PC], cpu->cycles, cpu->nextEvent, cpu->halted, gba->video.vcount,
			gba->video.frameCounter, gba->video.event.callback ? "scheduled" : "missing",
			gba->video.event.when, (void*) gba->timing.root, gba->cpuBlocked,
			(void*) gba->memory.rom, gba->memory.romSize, gba->memory.fullBios,
			haveSyncSnapshot ? pending : -1, videoWait, postCount, startCount, acceptedCount, endCount);
	} else {
		snprintf(videoStatus, sizeof(videoStatus), "VIDEO: BUFFER %s | SDL %s",
			renderer && renderer->videoBufferAttached ? "ATTACHED" : "MISSING",
			presented ? "PRESENTED" : "NOT PRESENTED");
	}
	mAndroidOverlaySetDebugStatus(sAndroidRomStatus, sAndroidLoadStatus, frameStatus,
		threadStatus, videoStatus, paceStatus, sAndroidLastError);
}
static void mSDLAndroidShowFailure(struct mSDLRenderer* renderer, const char* error, bool threadCrashed) {
	if (error && error[0])
		snprintf(sAndroidLastError, sizeof(sAndroidLastError), "ERROR: %.180s", error);
	mAndroidOverlaySetDebugStatus(sAndroidRomStatus, sAndroidLoadStatus, "FRAMES: stopped",
		threadCrashed ? "EMULATION THREAD: CRASHED" : "EMULATION THREAD: STOPPED",
		"VIDEO: stopped", "PRESENTED: stopped", sAndroidLastError);
	bool dismissed = false;
	while (!dismissed) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			mAndroidOverlayHandleEvent(&event);
			if (event.type == SDL_QUIT || event.type == SDL_KEYDOWN || event.type == SDL_FINGERDOWN)
				dismissed = true;
		}
		SDL_SetRenderDrawColor(renderer->sdlRenderer, 0, 0, 0, 255);
		SDL_RenderClear(renderer->sdlRenderer);
		mAndroidOverlayDraw(renderer->sdlRenderer, NULL);
		SDL_RenderPresent(renderer->sdlRenderer);
		SDL_Delay(16);
	}
}

void mAndroidPeerLinkSetConnected(bool host) {
	atomic_store(&sPendingPeerHostRole, host);
	atomic_store(&sPendingPeerState, 1);
	atomic_fetch_add(&sPeerGeneration, 1);
}

void mAndroidPeerLinkSetDisconnected(void) {
	atomic_store(&sPendingPeerState, 0);
	atomic_fetch_add(&sPeerGeneration, 1);
}

void mAndroidPeerLinkReceiveFrame(const uint8_t* frame, size_t size) {
	pthread_mutex_lock(&sPeerLinkPointerLock);
	if (sPeerLink)
		mPeerLinkReceiveFrame(sPeerLink, frame, size);
	pthread_mutex_unlock(&sPeerLinkPointerLock);
}


static bool mSDLAndroidCoopHeader(struct mCore* core, uint32_t address) {
	struct GBA* gba = (struct GBA*) core->board;
	const uint32_t* ewram = gba ? gba->memory.wram : NULL;
	size_t word;
	if (!ewram || address < GBA_EWRAM_START || address + 8 > GBA_EWRAM_END)
		return false;
	word = (address - GBA_EWRAM_START) / sizeof(uint32_t);
	return ewram[word] == HNS_COOP_MAILBOX_MAGIC
		&& ewram[word + 1] == ((uint32_t) HNS_COOP_MAILBOX_SIZE << 16 | HNS_COOP_MAILBOX_VERSION);
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
	if (sCoopMailboxAddress && !mSDLAndroidCoopHeader(core, sCoopMailboxAddress)) {
		sCoopMailboxAddress = 0;
		sCoopMailboxNextScan = frame;
		sCoopPrevLocalSio = UINT32_MAX;
	}
	if (!sCoopMailboxAddress && frame >= sCoopMailboxNextScan) {
		sCoopMailboxNextScan = frame + 30;
		/*
		 * The renderer calls this at a frame boundary while the emulation
		 * thread is waiting for frame acknowledgement. Do not perform tens of
		 * thousands of emulated bus transactions here: scan the backing EWRAM
		 * directly. GBA memory is quiescent at this point because the core is
		 * blocked in mCoreSyncPostFrame.
		 */
		struct GBA* gba = (struct GBA*) core->board;
		const uint32_t* ewram = gba ? gba->memory.wram : NULL;
		if (ewram) {
			const size_t lastWord = (GBA_EWRAM_END - GBA_EWRAM_START - HNS_COOP_MAILBOX_SIZE) / sizeof(uint32_t);
			size_t word;
			for (word = 0; word <= lastWord; ++word) {
				if (ewram[word] == HNS_COOP_MAILBOX_MAGIC
					&& ewram[word + 1] == ((uint32_t) HNS_COOP_MAILBOX_SIZE << 16 | HNS_COOP_MAILBOX_VERSION)) {
					sCoopMailboxAddress = GBA_EWRAM_START + (uint32_t) (word * sizeof(uint32_t));
					break;
				}
			}
		}
	}

	if (sCoopMailboxAddress) {
		uint32_t transport = connected ? 1 : 0;
		if (!connected) {
			sCoopPrevLocalSio = UINT32_MAX;
		} else {
			/* gameReady stays 0 until the link handshake has actually produced
			 * the other player. The first warp in that window uses the link
			 * map loader and follows an unspawned guest. Dropping transport
			 * for this one frame makes the ROM close the link before that
			 * loader runs. Later warps, after gameReady, keep the session. */
			uint32_t gameReady = core->busRead32(core, sCoopMailboxAddress + HNS_COOP_OFF_GAME_READY);
			uint32_t localSioNow = core->busRead32(core, sCoopMailboxAddress + 2116) ? 1 : 0;
			if (!gameReady && sCoopPrevLocalSio == 1 && localSioNow == 0)
				transport = 0;
			sCoopPrevLocalSio = localSioNow;
		}
		core->busWrite32(core, sCoopMailboxAddress + 8, transport);
		core->busWrite32(core, sCoopMailboxAddress + 2124, sCoopHostRole ? 1 : 0);
		if (!connected) {
			core->busWrite32(core, sCoopMailboxAddress + 2112, 0);
			core->busWrite32(core, sCoopMailboxAddress + 2120, 0);
			core->busWrite32(core, sCoopMailboxAddress + 2156, 0);
			sCoopLastMapSent = UINT32_MAX;
			sCoopLastSioActive = UINT32_MAX;
			sCoopLastXSent = UINT32_MAX;
			sCoopLastYSent = UINT32_MAX;
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
			uint32_t gameReady = core->busRead32(core, sCoopMailboxAddress + HNS_COOP_OFF_GAME_READY) ? 1 : 0;
			uint32_t localSioActive = core->busRead32(core, sCoopMailboxAddress + 2116) ? 1 : 0;
			uint32_t localX = core->busRead32(core, sCoopMailboxAddress + HNS_COOP_OFF_LOCAL_X);
			uint32_t localY = core->busRead32(core, sCoopMailboxAddress + HNS_COOP_OFF_LOCAL_Y);
			/* The initial GBA link handshake must run before gameReady can be
			 * published. Treat the connected pre-handshake session as SIO-active;
			 * once the handshake completes, honor the ROM's map/battle activity bit. */
			uint32_t sioActive = localSioActive || !gameReady;
			if (sioActive != sCoopLastSioActive)
				mPeerLinkSetLocalSioActive(sPeerLink, sioActive != 0);
			if (localMapId != sCoopLastMapSent || sioActive != sCoopLastSioActive
				|| localX != sCoopLastXSent || localY != sCoopLastYSent || frame % 30 == 0) {
				uint32_t networkState[4] = { htonl(localMapId), htonl(sioActive), htonl(localX), htonl(localY) };
				if (mPeerLinkSend(sPeerLink, mPEER_MSG_POSITION, networkState, sizeof(networkState))) {
					sCoopLastMapSent = localMapId;
					sCoopLastSioActive = sioActive;
					sCoopLastXSent = localX;
					sCoopLastYSent = localY;
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
			uint32_t localReady = core->busRead32(core, sCoopMailboxAddress + HNS_COOP_OFF_LOCAL_STORY_READY);
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
				if (peerEvent.messageType == mPEER_MSG_POSITION && peerEvent.size == 4 * sizeof(uint32_t)) {
					uint32_t networkState[4];
					memcpy(networkState, peerEvent.data, sizeof(networkState));
					core->busWrite32(core, sCoopMailboxAddress + 2108, ntohl(networkState[0]));
					core->busWrite32(core, sCoopMailboxAddress + 2112, 1);
					core->busWrite32(core, sCoopMailboxAddress + 2120, ntohl(networkState[1]) ? 1 : 0);
					core->busWrite32(core, sCoopMailboxAddress + HNS_COOP_OFF_REMOTE_X, ntohl(networkState[2]));
					core->busWrite32(core, sCoopMailboxAddress + HNS_COOP_OFF_REMOTE_Y, ntohl(networkState[3]));
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
	uint32_t generation;
	uint32_t generationAgain;
	int state;
	bool host;
	bool connected;
	do {
		generation = atomic_load(&sPeerGeneration);
		state = atomic_load(&sPendingPeerState);
		host = atomic_load(&sPendingPeerHostRole);
		generationAgain = atomic_load(&sPeerGeneration);
	} while (generation != generationAgain);
	if (generation == sAppliedPeerGeneration)
		return;
	sAppliedPeerGeneration = generation;
	connected = state == 1;
	/* A new peer packet increments the generation even when the connected
	 * flag never looks like it changed. Drop the previous native session
	 * first so the role and both SIO queues belong to this session. */
	if (sPeerSessionOpen && sPeerLink)
		mPeerLinkSetConnected(sPeerLink, false);
	sCoopLastMapSent = UINT32_MAX;
	sCoopLastSioActive = UINT32_MAX;
	sCoopLastXSent = UINT32_MAX;
	sCoopLastYSent = UINT32_MAX;
	sCoopLastConfigFrame = UINT32_MAX;
	sCoopLastConfigSequence = UINT32_MAX;
	sCoopPrevLocalSio = UINT32_MAX;
	sCoopHostRole = connected && host;
	if (sPeerLink) {
		mPeerLinkSetHostRole(sPeerLink, host);
		mPeerLinkSetConnected(sPeerLink, connected);
	}
	if (sLinkCable)
		mAndroidLinkCableSetRole(sLinkCable, connected && host);
	sPeerSessionOpen = connected;
}

static bool mSDLAndroidSWInit(struct mSDLRenderer* renderer) {
	unsigned width, height;
	bool softwareRenderer = false;
	renderer->core->baseVideoSize(renderer->core, &width, &height);
	// Prefer the GLES renderer. The software fallback scales a full-screen
	// framebuffer on the CPU and cannot hold 60 fps on a phone.
	SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
	SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
#if SDL_VERSION_ATLEAST(3, 0, 0)
	renderer->window = SDL_CreateWindow("mGBA", renderer->viewportWidth, renderer->viewportHeight, SDL_WINDOW_FULLSCREEN * renderer->player.fullscreen);
	renderer->sdlRenderer = SDL_CreateRenderer(renderer->window, NULL);
	// Core video synchronization owns frame pacing. Avoid a second blocking
	// VSync wait in SDL_RenderPresent, which can stall the emulation thread's
	// frame acknowledgement and starve the audio producer.
	SDL_SetRenderVSync(renderer->sdlRenderer, 0);
#else
	renderer->window = SDL_CreateWindow("mGBA", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		renderer->viewportWidth, renderer->viewportHeight,
		SDL_WINDOW_FULLSCREEN_DESKTOP * renderer->player.fullscreen);
	if (!renderer->window)
		return false;
	renderer->sdlRenderer = SDL_CreateRenderer(renderer->window, -1,
		SDL_RENDERER_ACCELERATED);
	if (!renderer->sdlRenderer) {
		renderer->sdlRenderer = SDL_CreateRenderer(renderer->window, -1, SDL_RENDERER_SOFTWARE);
		softwareRenderer = true;
	}
#endif
	if (!renderer->window || !renderer->sdlRenderer) {
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Could not create Android SDL renderer: %s", SDL_GetError());
		return false;
	}
	// Explicitly request an unsynchronized EGL buffer swap and retain the
	// return status. SDL's renderer-info flag alone can hide a failed request.
	if (SDL_GL_GetCurrentContext()) {
		sAndroidSwapIntervalSet = SDL_GL_SetSwapInterval(0);
		sAndroidSwapInterval = SDL_GL_GetSwapInterval();
		if (sAndroidSwapIntervalSet != 0) {
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Could not disable Android EGL VSync: %s", SDL_GetError());
		} else {
			SDL_Log("Android EGL swap interval set to 0 (reported=%d)", sAndroidSwapInterval);
		}
	} else {
		sAndroidSwapIntervalSet = -2;
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Android SDL renderer has no current GL context for swap interval verification");
	}
	SDL_GetWindowSize(renderer->window, &renderer->viewportWidth, &renderer->viewportHeight);
	renderer->player.window = renderer->window;

create_android_game_texture:
#ifdef COLOR_16_BIT
#ifdef COLOR_5_6_5
	renderer->sdlTex = SDL_CreateTexture(renderer->sdlRenderer, SDL_PIXELFORMAT_RGB565,
		SDL_TEXTUREACCESS_STREAMING, width, height);
#else
	renderer->sdlTex = SDL_CreateTexture(renderer->sdlRenderer, SDL_PIXELFORMAT_ABGR1555,
		SDL_TEXTUREACCESS_STREAMING, width, height);
#endif
#else
	renderer->sdlTex = SDL_CreateTexture(renderer->sdlRenderer, SDL_PIXELFORMAT_ABGR8888,
		SDL_TEXTUREACCESS_STREAMING, width, height);
#endif
	if (!renderer->sdlTex && !softwareRenderer) {
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Android accelerated texture creation failed; retrying software renderer: %s", SDL_GetError());
		SDL_DestroyRenderer(renderer->sdlRenderer);
		renderer->sdlRenderer = SDL_CreateRenderer(renderer->window, -1, SDL_RENDERER_SOFTWARE);
		softwareRenderer = true;
		if (renderer->sdlRenderer)
			goto create_android_game_texture;
	}
	if (!renderer->sdlTex) {
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Could not create Android game texture: %s", SDL_GetError());
		return false;
	}

	// Keep the core's software framebuffer in ordinary owned memory. Android's
	// streaming-texture lock/unlock cycle is avoided entirely; each completed
	// frame is copied into the SDL texture with SDL_UpdateTexture.
	renderer->outputStride = (int) width;
	renderer->outputBuffer = calloc((size_t) width * height, sizeof(mColor));
	if (!renderer->outputBuffer) {
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Could not allocate Android GBA framebuffer");
		return false;
	}
	renderer->core->setVideoBuffer(renderer->core, renderer->outputBuffer, renderer->outputStride);
	renderer->videoBufferAttached = true;
	sFrameUploadBytes = (size_t) renderer->width * renderer->height * BYTES_PER_PIXEL;
	sFrameUploadBuffer = malloc(sFrameUploadBytes);
	if (!sFrameUploadBuffer) {
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Could not allocate Android frame upload buffer");
		return false;
	}
	return true;
}

static void mSDLAndroidSWDeinit(struct mSDLRenderer* renderer) {
	free(sFrameUploadBuffer);
	sFrameUploadBuffer = NULL;
	sFrameUploadBytes = 0;
	free(renderer->outputBuffer);
	renderer->outputBuffer = NULL;
	renderer->outputStride = 0;
	renderer->videoBufferAttached = false;
}

// 280896 master cycles / 16777216 Hz.
static const uint64_t kGbaFrameNs = 16744614ULL;

static uint32_t mSDLAndroidHashFrame(const uint8_t* pixels, size_t size) {
	uint32_t hash = 2166136261u;
	size_t i;
	for (i = 0; i < size; i += 16) {
		hash ^= pixels[i];
		hash *= 16777619u;
	}
	return hash;
}

static void mSDLAndroidPaceFrame(struct timespec* nextFrame) {
	struct timespec now;
	uint64_t nowNs;
	uint64_t nextNs;
	clock_gettime(CLOCK_MONOTONIC, &now);
	nowNs = (uint64_t) now.tv_sec * 1000000000ULL + (uint64_t) now.tv_nsec;
	nextNs = (uint64_t) nextFrame->tv_sec * 1000000000ULL + (uint64_t) nextFrame->tv_nsec;
	if (nextNs > nowNs + kGbaFrameNs)
		nextNs = nowNs;
	if (nextNs > nowNs) {
		struct timespec abs;
		abs.tv_sec = (time_t) (nextNs / 1000000000ULL);
		abs.tv_nsec = (long) (nextNs % 1000000000ULL);
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, NULL);
		nowNs = nextNs;
	}
	nextNs += kGbaFrameNs;
	nextFrame->tv_sec = (time_t) (nextNs / 1000000000ULL);
	nextFrame->tv_nsec = (long) (nextNs % 1000000000ULL);
}

static void mSDLAndroidRunloop(struct mSDLRenderer* renderer, void* user) {
	struct mCoreThread* context = user;
	SDL_Event event;
	bool firstFrameLogged = false;
	bool renderCopyErrorLogged = false;
	Uint32 lastDiagnostics = SDL_GetTicks();
	bool latestFrameBlack = true;
	bool lastFramePresented = false;
	Uint64 perfFrequency = SDL_GetPerformanceFrequency();
	Uint64 lastPerfSample = SDL_GetPerformanceCounter();
	Uint64 waitTicks = 0, uploadTicks = 0, drawTicks = 0, presentTicks = 0, coopTicks = 0;
	Uint64 lastCoreFrames = 0;
	Uint64 presentedFrames = 0, lastPresentedFrames = 0;
	Uint64 distinctFrames = 0, lastDistinctFrames = 0;
	uint32_t lastFrameHash = 0;
	bool haveFrameHash = false;
	struct timespec nextFrame;
	unsigned safeFrameCount = 0;
	clock_gettime(CLOCK_MONOTONIC, &nextFrame);

	SDL_RendererInfo startupInfo;
	if (SDL_GetRendererInfo(renderer->sdlRenderer, &startupInfo) == 0)
		SDL_Log("Android SDL renderer selected: %s flags=0x%X maxTexture=%dx%d", startupInfo.name, startupInfo.flags, startupInfo.max_texture_width, startupInfo.max_texture_height);
	SDL_Log("Android video loop entered: window=%p renderer=%p texture=%p size=%ux%u output=%p",
		renderer->window, renderer->sdlRenderer, renderer->sdlTex, renderer->width, renderer->height, renderer->outputBuffer);
	while (mCoreThreadIsActive(context)) {
		// One presented GBA frame per 59.7275 Hz slot. Audio no longer clocks
		// the core; this wait is what keeps a fast core from running ahead.
		mSDLAndroidPaceFrame(&nextFrame);
		while (SDL_PollEvent(&event)) {
			mAndroidOverlayHandleEvent(&event);
			mSDLHandleEvent(context, &renderer->player, &event);
		}

		Uint64 stageStart = SDL_GetPerformanceCounter();
		if (!mCoreSyncWaitFrameStart(&context->impl->sync)) {
			waitTicks += SDL_GetPerformanceCounter() - stageStart;
			// Connection state can be applied while idle, but game-bus mailbox
			// access is deferred until a frame boundary below. This path may run
			// while the CPU is executing, so it must not inspect or modify the bus.
			mSDLAndroidApplyPendingPeerState();
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
			// WaitFrameStart already blocks on the frame condition (with a
			// bounded timeout). Do not add a second fixed sleep here.
			continue;
		}

		{
			bool reportDiagnostics = false;
			double coreFps = 0, drawFps = 0, showFps = 0, waitMs = 0, uploadMs = 0, drawMs = 0, presentMs = 0, coopMs = 0;
			bool active = false, crashed = false;
			safeFrameCount = renderer->core->frameCounter ? renderer->core->frameCounter(renderer->core) : 0;
			Uint32 now = SDL_GetTicks();
			if ((Uint32)(now - lastDiagnostics) >= 500) {
				Uint64 sampleNow = SDL_GetPerformanceCounter();
				Uint64 coreDelta = safeFrameCount >= lastCoreFrames ? safeFrameCount - lastCoreFrames : 0;
				Uint64 drawDelta = presentedFrames - lastPresentedFrames;
				Uint64 showDelta = distinctFrames - lastDistinctFrames;
				double seconds = perfFrequency ? (double)(sampleNow - lastPerfSample) / (double)perfFrequency : 0.5;
				coreFps = seconds > 0 ? coreDelta / seconds : 0;
				drawFps = seconds > 0 ? drawDelta / seconds : 0;
				showFps = seconds > 0 ? showDelta / seconds : 0;
				waitMs = perfFrequency ? (double)waitTicks * 1000.0 / perfFrequency / (coreDelta ? coreDelta : 1) : 0;
				uploadMs = perfFrequency ? (double)uploadTicks * 1000.0 / perfFrequency / (drawDelta ? drawDelta : 1) : 0;
				drawMs = perfFrequency ? (double)drawTicks * 1000.0 / perfFrequency / (drawDelta ? drawDelta : 1) : 0;
				presentMs = perfFrequency ? (double)presentTicks * 1000.0 / perfFrequency / (drawDelta ? drawDelta : 1) : 0;
				coopMs = perfFrequency ? (double)coopTicks * 1000.0 / perfFrequency / (drawDelta ? drawDelta : 1) : 0;
				lastDiagnostics = now;
				lastPerfSample = sampleNow;
				lastCoreFrames = safeFrameCount;
				lastPresentedFrames = presentedFrames;
				lastDistinctFrames = distinctFrames;
				active = mCoreThreadIsActive(context);
				crashed = mCoreThreadHasCrashed(context);
				reportDiagnostics = true;
				waitTicks = uploadTicks = drawTicks = presentTicks = coopTicks = 0;
			}
			waitTicks += SDL_GetPerformanceCounter() - stageStart;

			// Co-op mailbox access touches live GBA memory, so finish it while
			// the emulation thread is still blocked on this frame.
			stageStart = SDL_GetPerformanceCounter();
			mSDLAndroidApplyPendingPeerState();
			mSDLAndroidUpdateCoopMailbox(renderer->core);
			coopTicks += SDL_GetPerformanceCounter() - stageStart;

			latestFrameBlack = mSDLAndroidFrameIsBlack(renderer);
			if (sFrameUploadBuffer && renderer->outputBuffer && sFrameUploadBytes) {
				size_t rowBytes = (size_t) renderer->outputStride * BYTES_PER_PIXEL;
				size_t copyBytes = (size_t) renderer->width * BYTES_PER_PIXEL;
				size_t frameBytes = copyBytes * renderer->height;
				unsigned y;
				const uint8_t* src = (const uint8_t*) renderer->outputBuffer;
				uint8_t* dst = sFrameUploadBuffer;
				uint32_t hash;
				for (y = 0; y < renderer->height; ++y) {
					memcpy(dst, src, copyBytes);
					src += rowBytes;
					dst += copyBytes;
				}
				hash = mSDLAndroidHashFrame(sFrameUploadBuffer, frameBytes);
				if (!haveFrameHash || hash != lastFrameHash)
					++distinctFrames;
				lastFrameHash = hash;
				haveFrameHash = true;
			}

			// The core can render the next frame as soon as the pixels are in
			// the private upload buffer. SDL_UpdateTexture may wait on the GPU
			// fence from the previous present; that wait stays on this thread.
			mCoreSyncWaitFrameEnd(&context->impl->sync);
			if (reportDiagnostics) {
				mSDLAndroidUpdateDebugOverlay(renderer, safeFrameCount, latestFrameBlack, active, crashed, lastFramePresented,
					coreFps, drawFps, showFps, waitMs, uploadMs, drawMs, presentMs, coopMs, false);
				SDL_Log("Android emulator heartbeat: frame=%u coreFPS=%.2f drawFPS=%.2f distinctFPS=%.2f wait=%.3fms upload=%.3fms draw=%.3fms present=%.3fms coop=%.3fms audio=%dHz/%d underruns=%u active=%d crashed=%d black=%d",
					safeFrameCount, coreFps, drawFps, showFps, waitMs, uploadMs, drawMs, presentMs, coopMs, sAudioOpenedFreq, sAudioOpenedFrames,
					__atomic_load_n(&renderer->audio.underruns, __ATOMIC_RELAXED), active, crashed, latestFrameBlack);
			}
			stageStart = SDL_GetPerformanceCounter();
			if (!sFrameUploadBuffer || SDL_UpdateTexture(renderer->sdlTex, NULL, sFrameUploadBuffer,
				(int) renderer->width * BYTES_PER_PIXEL) < 0) {
				snprintf(sAndroidLastError, sizeof(sAndroidLastError), "ERROR: SDL_UpdateTexture: %.150s", SDL_GetError());
				SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Android framebuffer upload failed: %s", SDL_GetError());
				lastFramePresented = false;
				mCoreThreadEnd(context);
				break;
			}
			uploadTicks += SDL_GetPerformanceCounter() - stageStart;
			stageStart = SDL_GetPerformanceCounter();

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
			lastFramePresented = frameCopied;
			if (!frameCopied) {
				snprintf(sAndroidLastError, sizeof(sAndroidLastError), "ERROR: SDL_RenderCopy: %.150s", SDL_GetError());
				if (!renderCopyErrorLogged) {
					SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Android game frame copy failed: %s", SDL_GetError());
					renderCopyErrorLogged = true;
				}
			}
			mAndroidOverlayDraw(renderer->sdlRenderer, &dest);
			drawTicks += SDL_GetPerformanceCounter() - stageStart;
			stageStart = SDL_GetPerformanceCounter();
			SDL_RenderPresent(renderer->sdlRenderer);
			presentTicks += SDL_GetPerformanceCounter() - stageStart;
			if (frameCopied)
				++presentedFrames;
			if (frameCopied && !firstFrameLogged) {
				firstFrameLogged = true;
				SDL_Log("First Android game frame copied and presented; core frame=%u", safeFrameCount);
			}

		}
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
	// SDL renderer resources must be released before their owning window.
	// Keep the core framebuffer alive until the renderer-specific deinit runs.
	if (renderer->sdlTex) {
		SDL_DestroyTexture(renderer->sdlTex);
		renderer->sdlTex = NULL;
	}
	if (renderer->sdlRenderer) {
		SDL_DestroyRenderer(renderer->sdlRenderer);
		renderer->sdlRenderer = NULL;
	}
	if (renderer->window) {
		SDL_DestroyWindow(renderer->window);
		renderer->window = NULL;
	}
	renderer->deinit(renderer);
	SDL_Quit();
}

int main(int argc, char** argv) {
	struct mSDLRenderer renderer = {0};

	struct mCoreOptions opts = {
		.useBios = true,
		// Disable per-frame rewind serialization on Android until the
		// emulator's real-time audio/video path is stable.
		.rewindEnable = false,
		.rewindBufferCapacity = 0,
		.rewindBufferInterval = 1,
		.audioBuffers = 1024,
		.videoSync = true,
		// Audio output is attached after the core thread starts. Starting with
		// audio sync enabled can let the CPU thread fill its buffer and wait
		// before SDL has opened the device that drains it. Enable sync only
		// after the audio callback is live.
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

	// Create the GBA core and renderer before asking for a ROM. The app must
	// remain open and usable even when the user has not selected a game yet.
	if (args.fname) {
		SDL_Log("Resolving ROM core for: %s", args.fname);
		renderer.core = mCoreFind(args.fname);
		if (!renderer.core) {
			struct stat romInfo;
			if (stat(args.fname, &romInfo) == 0)
				snprintf(sAndroidRomStatus, sizeof(sAndroidRomStatus), "ROM: FOUND | SIZE: %llu bytes", (unsigned long long) romInfo.st_size);
			else
				snprintf(sAndroidRomStatus, sizeof(sAndroidRomStatus), "ROM: FILE NOT FOUND");
			snprintf(sAndroidLoadStatus, sizeof(sAndroidLoadStatus), "LOAD: WAITING FOR USER");
			snprintf(sAndroidLastError, sizeof(sAndroidLastError), "ERROR: initial file not recognized; choose a ROM");
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Initial file is not a supported ROM; opening the emulator without a game");
			free(args.fname);
			args.fname = NULL;
		}
	}
	if (!renderer.core) {
		renderer.core = mCoreCreate(mPLATFORM_GBA);
		if (!args.fname && strcmp(sAndroidRomStatus, "ROM: not checked") == 0) {
			snprintf(sAndroidRomStatus, sizeof(sAndroidRomStatus), "ROM: NONE SELECTED");
			snprintf(sAndroidLoadStatus, sizeof(sAndroidLoadStatus), "LOAD: WAITING FOR USER");
			snprintf(sAndroidLastError, sizeof(sAndroidLastError), "ERROR: none");
		}
	}
	if (!renderer.core) {
		snprintf(sAndroidLoadStatus, sizeof(sAndroidLoadStatus), "LOAD: FAILED - CORE UNAVAILABLE");
		snprintf(sAndroidLastError, sizeof(sAndroidLastError), "ERROR: mCoreCreate could not create the GBA core");
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Emulator initialization failed", "mGBA could not create the GBA emulator core.", NULL);
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not create GBA core");
		mArgumentsDeinit(&args);
		SDL_Quit();
		return 1;
	}

	if (!renderer.core->init(renderer.core)) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Emulator initialization failed", "mGBA could not initialize the emulator core.", NULL);
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
	// Upstream frontends default to detecting idle loops. A co-op ROM rebuild
	// does not keep the stock Pokémon idle-loop address, so removal never
	// engages unless detection is allowed to find the new one.
	mCoreConfigSetDefaultValue(&renderer.core->config, "idleOptimization", "detect");
	// Diagnostic A/B build: use the core's direct video renderer to isolate
	// the threaded scanline proxy from Android frame pacing.
	mCoreConfigSetDefaultValue(&renderer.core->config, "threadedVideo", "0");
	mCoreConfigLoadDefaults(&renderer.core->config, &opts);
	mCoreLoadConfig(renderer.core);
	// Override any persisted threaded-video preference for this diagnostic
	// build. The core reads this setting when its emulation thread resets.
	mCoreConfigSetValue(&renderer.core->config, "threadedVideo", "0");
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
	renderer.init = mSDLAndroidSWInit;
	renderer.deinit = mSDLAndroidSWDeinit;
	renderer.runloop = mSDLAndroidRunloop;

	if (!renderer.init(&renderer)) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Video initialization failed", SDL_GetError(), NULL);
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

	struct mPeerLink* peerLink;
	pthread_mutex_lock(&sPeerLinkPointerLock);
	sPeerLink = mPeerLinkCreate();
	peerLink = sPeerLink;
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
	mInputMapDeinit(&renderer.core->inputMap);

	mSDLDeinit(&renderer);
	struct mAndroidLinkCable* linkCable = sLinkCable;
	pthread_mutex_lock(&sPeerLinkPointerLock);
	peerLink = sPeerLink;
	sPeerLink = NULL;
	pthread_mutex_unlock(&sPeerLinkPointerLock);
	sLinkCable = NULL;
	mAndroidLinkCableDestroy(linkCable);
	if (peerLink)
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
	sCoopLastXSent = UINT32_MAX;
	sCoopLastYSent = UINT32_MAX;

	// No ROM is a normal startup state. Keep SDL, the renderer, touch controls,
	// and peer-link UI alive while the user chooses a game with the on-screen
	// LOAD ROM control. A cancelled picker simply returns to this idle screen.
	mAndroidOverlaySetDebugStatus(sAndroidRomStatus, sAndroidLoadStatus,
		"FRAMES: no game loaded", "EMULATION THREAD: waiting",
		"VIDEO: renderer ready", "PRESENTED: waiting", sAndroidLastError);
	while (!args->fname) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			mAndroidOverlayHandleEvent(&event);
			if (event.type == SDL_QUIT) {
				return 0;
			}
			if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F14) {
				args->fname = mAndroidPickRom();
				if (args->fname) break;
			}
			if (event.type == SDL_DROPFILE && event.drop.file) {
				args->fname = strdup(event.drop.file);
				SDL_free(event.drop.file);
				if (args->fname) break;
			}
		}
		if (args->fname) break;
		SDL_SetRenderDrawColor(renderer->sdlRenderer, 0, 0, 0, 255);
		SDL_RenderClear(renderer->sdlRenderer);
		mAndroidOverlayDraw(renderer->sdlRenderer, NULL);
		SDL_RenderPresent(renderer->sdlRenderer);
		SDL_Delay(16);
	}

	for (;;) {
		struct stat romInfo;
		if (args->fname && stat(args->fname, &romInfo) == 0) {
			snprintf(sAndroidRomStatus, sizeof(sAndroidRomStatus), "ROM: FOUND | SIZE: %llu bytes", (unsigned long long) romInfo.st_size);
		} else {
			snprintf(sAndroidRomStatus, sizeof(sAndroidRomStatus), "ROM: FILE NOT FOUND | PATH: %.120s", args->fname ? args->fname : "(null)");
		}
		snprintf(sAndroidLoadStatus, sizeof(sAndroidLoadStatus), "LOAD: IN PROGRESS");
		snprintf(sAndroidLastError, sizeof(sAndroidLastError), "ERROR: none");
		mAndroidOverlaySetDebugStatus(sAndroidRomStatus, sAndroidLoadStatus, "FRAMES: waiting",
			"EMULATION THREAD: not started", "VIDEO: checking attachment", "PRESENTED: waiting", sAndroidLastError);
		SDL_Log("Loading ROM: %s", args->fname ? args->fname : "(null)");
		if (mCoreLoadFile(renderer->core, args->fname)) {
			snprintf(sAndroidLoadStatus, sizeof(sAndroidLoadStatus), "LOAD: SUCCESS");
			break;
		}
		snprintf(sAndroidLoadStatus, sizeof(sAndroidLoadStatus), "LOAD: FAILED - SELECT ANOTHER");
		snprintf(sAndroidLastError, sizeof(sAndroidLastError), "ERROR: mCoreLoadFile rejected selected ROM");
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "mCoreLoadFile failed for selected ROM: %s", args->fname ? args->fname : "(null)");
		free(args->fname);
		args->fname = NULL;
		mAndroidOverlaySetDebugStatus(sAndroidRomStatus, sAndroidLoadStatus, "FRAMES: waiting",
			"EMULATION THREAD: not started", "VIDEO: ready for ROM", "PRESENTED: waiting", sAndroidLastError);
		while (!args->fname) {
			SDL_Event event;
			while (SDL_PollEvent(&event)) {
				mAndroidOverlayHandleEvent(&event);
				if (event.type == SDL_QUIT) return 0;
				if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F14) {
					args->fname = mAndroidPickRom();
					if (args->fname) break;
				}
				if (event.type == SDL_DROPFILE && event.drop.file) {
					args->fname = strdup(event.drop.file);
					SDL_free(event.drop.file);
					if (args->fname) break;
				}
			}
			if (args->fname) break;
			SDL_SetRenderDrawColor(renderer->sdlRenderer, 0, 0, 0, 255);
			SDL_RenderClear(renderer->sdlRenderer);
			mAndroidOverlayDraw(renderer->sdlRenderer, NULL);
			SDL_RenderPresent(renderer->sdlRenderer);
			SDL_Delay(16);
		}
	}
	mAndroidOverlaySetRomLoaded(true);
	// Reattach after loadROM: some core loaders reset their video state while
	// initializing the cartridge, so the texture must be installed again.
	if (renderer->outputBuffer && renderer->outputStride >= (int) renderer->width) {
		renderer->core->setVideoBuffer(renderer->core, renderer->outputBuffer, renderer->outputStride);
		renderer->videoBufferAttached = true;
	} else {
		snprintf(sAndroidLastError, sizeof(sAndroidLastError), "ERROR: SDL VIDEO BUFFER NOT AVAILABLE AFTER ROM LOAD");
	}
	SDL_Log("ROM loaded successfully: %s", args->fname);
	if (sLinkCable)
		mAndroidLinkCableAttach(renderer->core, sLinkCable);
	mCoreAutoloadSave(renderer->core);
	mArgumentsApplyFileLoads(args, renderer->core);

	renderer->audio.samples = renderer->core->opts.audioBuffers;
	// Phones run the audio HAL at 48000 Hz. Asking for 44100 makes AAudio
	// reopen at 48000 or makes AudioTrack negotiate a different period than
	// the resampler is pacing.
	renderer->audio.sampleRate = 48000;
	thread.logger.logger = &_logger.d;

	bool didFail = !mCoreThreadStart(&thread);
	if (didFail) {
		snprintf(sAndroidLastError, sizeof(sAndroidLastError), "ERROR: emulator thread failed to start");
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "mCoreThreadStart failed; emulator core did not start");
		mSDLAndroidShowFailure(renderer, "mCoreThreadStart failed; emulator did not start", false);
	} else {
		SDL_Log("Emulator core thread started successfully");
	}

	if (!didFail) {
		mSDLSetScreensaverSuspendable(&renderer->events, renderer->core->opts.suspendScreensaver);
		mSDLSuspendScreensaver(&renderer->events);
		renderer->audioInitialized = mSDLInitAudio(&renderer->audio, &thread);
		sAudioOpenedFreq = renderer->audio.obtainedSpec.freq;
		sAudioOpenedFrames = renderer->audio.obtainedSpec.samples;
		SDL_Log("Android audio device: requested=%uHz/%u frames, opened=%dHz/%d frames, channels=%u",
			renderer->audio.sampleRate, (unsigned) renderer->audio.samples, sAudioOpenedFreq, sAudioOpenedFrames,
			renderer->audio.obtainedSpec.channels);
		if (!renderer->audioInitialized) {
			// No consumer exists, so leave audio sync disabled. Video and
			// emulation must continue independently of audio device failures.
			renderer->core->opts.audioSync = false;
			snprintf(sAndroidLastError, sizeof(sAndroidLastError), "ERROR: AUDIO UNAVAILABLE - VIDEO-ONLY FALLBACK");
			SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Audio initialization failed; continuing video without audio: %s", SDL_GetError());
		} else {
			// The phone measurement was about 1.5 ms of core CPU per frame and
			// about 30 ms blocked in audio sync, with video wait near zero.
			// That device period is not one GBA frame, so audio sync held the
			// game at ~33 fps and the callback padded the rest with silence.
			// The renderer clock below keeps emulation at 60 fps. The callback
			// resamples whatever is buffered and counts an underrun when it
			// has to pad.
			MutexLock(&thread.impl->sync.audioBufferMutex);
			thread.impl->sync.audioWait = false;
			ConditionWake(&thread.impl->sync.audioRequiredCond);
			MutexUnlock(&thread.impl->sync.audioBufferMutex);
			renderer->core->opts.audioSync = false;
			snprintf(sAndroidLastError, sizeof(sAndroidLastError), "ERROR: none");
		}
		renderer->runloop(renderer, &thread);
		if (renderer->audioInitialized)
			mSDLPauseAudio(&renderer->audio);
		if (mCoreThreadHasCrashed(&thread)) {
			didFail = true;
			snprintf(sAndroidLastError, sizeof(sAndroidLastError), "ERROR: emulator thread crashed");
			printf("The game crashed!\\n");
			mCoreThreadEnd(&thread);
			mSDLAndroidShowFailure(renderer, "The emulation thread crashed; see the on-screen status", true);
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
