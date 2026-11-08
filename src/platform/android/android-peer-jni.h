/* Copyright (c) 2026
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ANDROID_PEER_JNI_H
#define ANDROID_PEER_JNI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool mAndroidPeerJniSendFrame(const uint8_t* frame, size_t size);
void mAndroidPeerJniShowDialog(void);
void mAndroidPeerLinkSetConnected(bool host);
void mAndroidPeerLinkSetDisconnected(void);
void mAndroidPeerLinkReceiveFrame(const uint8_t* frame, size_t size);

#endif
