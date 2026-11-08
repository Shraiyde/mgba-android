/* Copyright (c) 2026
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "android-link-cable.h"
#include "android-peer-link.h"

#include <mgba/core/core.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/sio.h>

#include <stdlib.h>
#include <string.h>

struct mAndroidLinkCable {
	struct GBASIODriver driver;
	struct mPeerLink* link;
	int deviceId;
};

static struct mAndroidLinkCable* _cable(struct GBASIODriver* driver) {
	return (struct mAndroidLinkCable*) driver;
}

static bool _init(struct GBASIODriver* driver) {
	return _cable(driver)->link != NULL;
}

static void _deinit(struct GBASIODriver* driver) {
	UNUSED(driver);
}

static void _reset(struct GBASIODriver* driver) {
	UNUSED(driver);
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
	UNUSED(driver);
	return true;
}

static void _finishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
	struct mAndroidLinkCable* cable = _cable(driver);
	uint16_t local = cable->driver.p->p->memory.io[GBA_REG(SIOMLT_SEND)];
	uint16_t remote = 0xFFFF;
	data[0] = data[1] = data[2] = data[3] = 0xFFFF;

	if (!mPeerLinkExchangeU16(cable->link, local, &remote))
		return;

	data[cable->deviceId] = local;
	data[cable->deviceId ^ 1] = remote;
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
