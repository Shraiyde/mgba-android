/* Copyright (c) 2026
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ANDROID_PEER_LINK_H
#define ANDROID_PEER_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MPEER_DEFAULT_PORT 47621
#define MPEER_PIN_LENGTH 4
#define MPEER_MAX_PAYLOAD 4096

enum mPeerMessageType {
	mPEER_MSG_POSITION = 16,
	mPEER_MSG_WORLD_FLAG = 17,
	mPEER_MSG_WORLD_VAR = 24,
	mPEER_MSG_WORLD_READY = 25,
	mPEER_MSG_BATTLE_STATE = 18,
	mPEER_MSG_BATTLE_ACTION = 19,
	mPEER_MSG_BATTLE_RESULT = 20,
	mPEER_MSG_KEEPALIVE = 21,
	mPEER_MSG_CHAT = 22,
	mPEER_MSG_SIO_MULTI = 23,
	mPEER_MSG_WORLD_CONFIG = 26,
	mPEER_MSG_WORLD_CONFIG_ACK = 27
};

enum mPeerEventType {
	mPEER_EVENT_NONE,
	mPEER_EVENT_CONNECTED,
	mPEER_EVENT_DISCONNECTED,
	mPEER_EVENT_MESSAGE,
	mPEER_EVENT_ERROR
};

struct mPeerEvent {
	enum mPeerEventType type;
	uint16_t messageType;
	uint32_t size;
	uint8_t data[MPEER_MAX_PAYLOAD];
	int error;
};

struct mPeerLink;

struct mPeerLink* mPeerLinkCreate(void);
void mPeerLinkDestroy(struct mPeerLink* link);

/* Raw TCP host/join remain available for local development. The Android
 * frontend uses PeerLinkManager for PIN rendezvous, WebRTC, and relay fallback. */
bool mPeerLinkHost(struct mPeerLink* link, uint16_t port, const char* pin);
bool mPeerLinkJoin(struct mPeerLink* link, const char* address, uint16_t port, const char* pin);
bool mPeerLinkSend(struct mPeerLink* link, uint16_t type, const void* data, uint32_t size);
bool mPeerLinkPoll(struct mPeerLink* link, struct mPeerEvent* event);
bool mPeerLinkIsConnected(struct mPeerLink* link);
bool mPeerLinkExchangeU16(struct mPeerLink* link, uint16_t localValue, uint16_t* remoteValue);
void mPeerLinkStop(struct mPeerLink* link);
void mPeerLinkUseJavaTransport(struct mPeerLink* link, bool enabled);
void mPeerLinkSetConnected(struct mPeerLink* link, bool connected);
void mPeerLinkSetLocalSioActive(struct mPeerLink* link, bool active);
void mPeerLinkSetHostRole(struct mPeerLink* link, bool host);
bool mPeerLinkReceiveFrame(struct mPeerLink* link, const uint8_t* frame, size_t size);

#endif
