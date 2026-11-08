/* Copyright (c) 2026
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ANDROID_LINK_CABLE_H
#define ANDROID_LINK_CABLE_H

#include <stdbool.h>

struct mCore;
struct mPeerLink;
struct mAndroidLinkCable;

struct mAndroidLinkCable* mAndroidLinkCableCreate(struct mPeerLink* link, bool host);
void mAndroidLinkCableSetRole(struct mAndroidLinkCable* cable, bool host);
void mAndroidLinkCableAttach(struct mCore* core, struct mAndroidLinkCable* cable);
void mAndroidLinkCableDetach(struct mCore* core);
void mAndroidLinkCableDestroy(struct mAndroidLinkCable* cable);

#endif
