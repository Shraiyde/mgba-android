/* Copyright (c) 2026 Gitsnup
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ANDROID_UI_H
#define ANDROID_UI_H

#include <SDL.h>
#include <stdbool.h>

#include <mgba-util/common.h>

CXX_GUARD_START

// Shows a ROM browser and returns a malloc'd path, or NULL if the user quits.
char* mAndroidPickRom(void);

// Touch overlay during gameplay. Set the window right after creating it, call
// handleEvent for every polled event (it synthesizes keyboard events for
// touches), and draw after the frame blit. `view` is the letterboxed game rect.
void mAndroidOverlaySetWindow(SDL_Window* window);
void mAndroidOverlaySetRomLoaded(bool loaded);
void mAndroidOverlayReset(void);
void mAndroidOverlayHandleEvent(const SDL_Event* event);
void mAndroidOverlayDraw(SDL_Renderer* renderer, const SDL_Rect* view);
void mAndroidOverlaySetDebugStatus(const char* rom, const char* load, const char* frames, const char* thread, const char* video, const char* pace, const char* error);
// Bottom edge, in pixels, of the top utility control row for a window of the
// given size. The game view uses it to sit under the top controls in portrait.
int mAndroidOverlayTopInset(int w, int h);

CXX_GUARD_END

#endif
