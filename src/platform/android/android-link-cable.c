/* Copyright (c) 2026
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "android-link-cable.h"
#include "android-peer-link.h"

#include <mgba/core/core.h>
#include <mgba/core/timing.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/sio.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SIO_POLL_CYCLES 4096
#define SIO_IDLE_CYCLES 280896

struct mAndroidLinkCable {
	struct GBASIODriver driver;
	struct mPeerLink* link;
	struct mTimingEvent pollEvent;
	int deviceId;
	bool transferActive;
	bool replyPending;
	uint16_t localValue;
	uint16_t heldRemote;
	uint32_t seq;
	uint32_t epoch;
	uint64_t startNs;
};

static struct mAndroidLinkCable* _cable(struct GBASIODriver* driver) {
	return (struct mAndroidLinkCable*) driver;
}

static uint64_t _nowNs(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t) now.tv_sec * 1000000000ULL + (uint64_t) now.tv_nsec;
}

static void _complete(struct mAndroidLinkCable* cable, uint16_t remote, uint32_t cyclesLate) {
	uint16_t data[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
	int id = cable->deviceId;
	if (id < 0 || id > 3)
		id = 0;
	/* Keep this side's own word. 0xFFFF in every slot is what the link
	 * handshake treats as a broken cable on this phone only. */
	data[id] = cable->localValue;
	data[id ^ 1] = remote;
	cable->transferActive = false;
	cable->replyPending = false;
	cable->seq = 0;
	GBASIOMultiplayerFinishTransfer(cable->driver.p, data, cyclesLate);
}

static void _poll(struct mTiming* timing, void* context, uint32_t cyclesLate);

static int32_t _pollDelay(struct mAndroidLinkCable* cable) {
	if (cable->transferActive || cable->replyPending || (cable->link && mPeerLinkIsConnected(cable->link)))
		return SIO_POLL_CYCLES;
	return SIO_IDLE_CYCLES;
}

static void _schedule(struct mAndroidLinkCable* cable, struct mTiming* timing) {
	cable->pollEvent.context = cable;
	cable->pollEvent.callback = _poll;
	cable->pollEvent.name = "Android link";
	cable->pollEvent.priority = 0x80;
	mTimingSchedule(timing, &cable->pollEvent, _pollDelay(cable));
}

static void _poll(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct mAndroidLinkCable* cable = context;
	if (!cable->link || !cable->driver.p)
		return;

	if (cable->transferActive && mPeerLinkEpoch(cable->link) != cable->epoch) {
		_complete(cable, 0xFFFF, cyclesLate);
	} else if (cable->deviceId == 0 && cable->transferActive) {
		uint16_t remote = 0xFFFF;
		enum mPeerSioResult result = mPeerLinkSioPoll(cable->link, cable->localValue, &cable->seq, cable->startNs, &remote);
		if (result == mPEER_SIO_READY)
			_complete(cable, remote, cyclesLate);
		else if (result == mPEER_SIO_RESET || result == mPEER_SIO_OFFLINE)
			_complete(cable, 0xFFFF, cyclesLate);
	} else if (cable->deviceId != 0 && cable->replyPending) {
		if (mPeerLinkSioReply(cable->link, cable->seq, cable->localValue))
			_complete(cable, cable->heldRemote, cyclesLate);
		else if (!mPeerLinkIsConnected(cable->link) || (_nowNs() - cable->startNs) >= MPEER_SIO_HOLD_NS)
			_complete(cable, 0xFFFF, cyclesLate);
	} else if (cable->deviceId != 0) {
		uint32_t seq = 0;
		uint16_t remote = 0xFFFF;
		enum mPeerSioResult result = mPeerLinkSioTake(cable->link, &seq, &remote);
		if (result == mPEER_SIO_READY) {
			cable->localValue = cable->driver.p->p->memory.io[GBA_REG(SIOMLT_SEND)];
			cable->heldRemote = remote;
			cable->seq = seq;
			cable->startNs = _nowNs();
			cable->epoch = mPeerLinkEpoch(cable->link);
			if (mPeerLinkSioReply(cable->link, seq, cable->localValue))
				_complete(cable, remote, cyclesLate);
			else {
				cable->replyPending = true;
				cable->transferActive = true;
			}
		}
	}

	_schedule(cable, timing);
}

static bool _init(struct GBASIODriver* driver) {
	struct mAndroidLinkCable* cable = _cable(driver);
	if (cable->driver.p)
		_schedule(cable, &cable->driver.p->p->timing);
	return cable->link != NULL;
}

static void _deinit(struct GBASIODriver* driver) {
	struct mAndroidLinkCable* cable = _cable(driver);
	if (cable->driver.p)
		mTimingDeschedule(&cable->driver.p->p->timing, &cable->pollEvent);
}

static void _reset(struct GBASIODriver* driver) {
	struct mAndroidLinkCable* cable = _cable(driver);
	cable->transferActive = false;
	cable->replyPending = false;
	cable->seq = 0;
	if (cable->driver.p) {
		mTimingDeschedule(&cable->driver.p->p->timing, &cable->pollEvent);
		_schedule(cable, &cable->driver.p->p->timing);
	}
}

static uint32_t _driverId(const struct GBASIODriver* driver) {
	UNUSED(driver);
	return 0x4D474241;
}

static void _setMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	UNUSED(mode);
}

static bool _handlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	return mode == GBA_SIO_MULTI;
}

static int _connectedDevices(struct GBASIODriver* driver) {
	return mPeerLinkIsConnected(_cable(driver)->link) ? 1 : 0;
}

static int _deviceId(struct GBASIODriver* driver) {
	return _cable(driver)->deviceId;
}

static bool _start(struct GBASIODriver* driver) {
	struct mAndroidLinkCable* cable = _cable(driver);
	/* Slaves do not clock the wire. Their IRQ is raised when the master's
	 * word arrives, from the poll below. Returning true here lets mGBA's
	 * normal timer finish only if a slave transfer is started locally. */
	if (cable->deviceId != 0)
		return true;
	if (cable->transferActive)
		return false;
	if (!cable->link || !mPeerLinkIsConnected(cable->link))
		return true;
	cable->localValue = cable->driver.p->p->memory.io[GBA_REG(SIOMLT_SEND)];
	cable->seq = 0;
	cable->startNs = _nowNs();
	cable->epoch = mPeerLinkEpoch(cable->link);
	cable->transferActive = true;
	/* Completion is owned by _poll. A false return keeps the core thread
	 * out of the old 10 second condition wait. */
	return false;
}

static void _finishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
	struct mAndroidLinkCable* cable = _cable(driver);
	uint16_t local = cable->driver.p->p->memory.io[GBA_REG(SIOMLT_SEND)];
	int id = cable->deviceId;
	/* Offline or slave-timer path. Do not block, and do not replace our
	 * own send register with 0xFFFF. */
	data[0] = data[1] = data[2] = data[3] = 0xFFFF;
	if (id < 0 || id > 3)
		id = 0;
	data[id] = local;
}

struct mAndroidLinkCable* mAndroidLinkCableCreate(struct mPeerLink* link, bool host) {
	struct mAndroidLinkCable* cable = calloc(1, sizeof(*cable));
	if (!cable)
		return NULL;

	cable->link = link;
	cable->deviceId = host ? 0 : 1;
	cable->driver.init = _init;
	cable->driver.deinit = _deinit;
	cable->driver.reset = _reset;
	cable->driver.driverId = _driverId;
	cable->driver.setMode = _setMode;
	cable->driver.handlesMode = _handlesMode;
	cable->driver.connectedDevices = _connectedDevices;
	cable->driver.deviceId = _deviceId;
	cable->driver.start = _start;
	cable->driver.finishMultiplayer = _finishMultiplayer;
	return cable;
}

void mAndroidLinkCableSetRole(struct mAndroidLinkCable* cable, bool host) {
	if (cable)
		cable->deviceId = host ? 0 : 1;
}

void mAndroidLinkCableAttach(struct mCore* core, struct mAndroidLinkCable* cable) {
	if (core && cable)
		core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &cable->driver);
}

void mAndroidLinkCableDetach(struct mCore* core) {
	if (core)
		core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, NULL);
}

void mAndroidLinkCableDestroy(struct mAndroidLinkCable* cable) {
	free(cable);
}
