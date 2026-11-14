/* Copyright (c) 2026 Gitsnup
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "android-ui.h"
#include "android-peer-jni.h"

#include <jni.h>
#include <SDL_system.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "font8x8_basic.h"

#define FONT_SCALE 3
#define ROW_HEIGHT (8 * FONT_SCALE + 6)

static const char* SEARCH_DIRS[] = {
	"", // filled in with the app-specific external storage path
	"/sdcard/ROMS",
	"/sdcard/roms",
	"/sdcard/Download",
};
#define SEARCH_DIRS_COUNT ((int) (sizeof(SEARCH_DIRS) / sizeof(SEARCH_DIRS[0])))

static const char* const ROM_EXTENSIONS[] = {
	".gba", ".agb", ".gb", ".gbc", ".sgb", ".cgb", ".zip",
};
#define ROM_EXTENSIONS_COUNT ((int) (sizeof(ROM_EXTENSIONS) / sizeof(ROM_EXTENSIONS[0])))

// --- Font blitting -----------------------------------------------------------

static void _blitText(SDL_Surface* surf, const char* text, int x, int y, int scale, Uint32 color) {
	if (surf->format->format != SDL_PIXELFORMAT_ABGR8888) {
		return;
	}
	Uint32* pixels = surf->pixels;
	int stride = surf->pitch / sizeof(Uint32);
	const char* p;
	for (p = text; *p && y < surf->h; ++p, x += 8 * scale) {
		if (x + 8 * scale <= 0 || x >= surf->w) {
			continue;
		}
		const char* glyph = font8x8_basic[(unsigned char) *p & 0x7F];
		int row, col, sx, sy;
		for (row = 0; row < 8; ++row) {
			for (col = 0; col < 8; ++col) {
				if (!((glyph[row] >> col) & 1)) {
					continue;
				}
				for (sy = 0; sy < scale; ++sy) {
					int py = y + row * scale + sy;
					if (py < 0 || py >= surf->h) {
						continue;
					}
					for (sx = 0; sx < scale; ++sx) {
						int px = x + col * scale + sx;
						if (px < 0 || px >= surf->w) {
							continue;
						}
						pixels[py * stride + px] = color;
					}
				}
			}
		}
	}
}

static SDL_Surface* _makeSurface(int w, int h) {
	return SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
}

// --- ROM scanning ------------------------------------------------------------

struct RomEntry {
	char* path;
	char* name;
};

struct RomList {
	struct RomEntry* entries;
	size_t count;
	size_t capacity;
};

static void _addRom(struct RomList* list, const char* path) {
	const char* dot = strrchr(path, '.');
	if (!dot) {
		return;
	}
	size_t i;
	bool match = false;
	for (i = 0; i < ROM_EXTENSIONS_COUNT; ++i) {
		if (strcasecmp(dot, ROM_EXTENSIONS[i]) == 0) {
			match = true;
			break;
		}
	}
	if (!match) {
		return;
	}
	if (list->count == list->capacity) {
		list->capacity = list->capacity ? list->capacity * 2 : 32;
		list->entries = realloc(list->entries, list->capacity * sizeof(*list->entries));
	}
	const char* slash = strrchr(path, '/');
	struct RomEntry* e = &list->entries[list->count++];
	e->path = strdup(path);
	e->name = strdup(slash ? slash + 1 : path);
}

static bool _isDir(const char* path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void _scanDir(struct RomList* list, const char* base, int depth) {
	if (depth > 4) {
		return;
	}
	DIR* dir = opendir(base);
	if (!dir) {
		return;
	}
	struct dirent* entry;
	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		char child[1024];
		snprintf(child, sizeof(child), "%s/%s", base, entry->d_name);
		if (_isDir(child)) {
			_scanDir(list, child, depth + 1);
		} else {
			_addRom(list, child);
		}
	}
	closedir(dir);
}

static int _romCompare(const void* a, const void* b) {
	const struct RomEntry* ea = a;
	const struct RomEntry* eb = b;
	return strcasecmp(ea->name, eb->name);
}

static void _scanRoms(struct RomList* list, char* appDir, size_t appDirLen) {
	const char* ext = SDL_AndroidGetExternalStoragePath();
	if (!ext || !ext[0]) {
		ext = SDL_AndroidGetInternalStoragePath();
	}
	snprintf(appDir, appDirLen, "%s", ext ? ext : "");
	SEARCH_DIRS[0] = appDir;

	int i;
	for (i = 0; i < SEARCH_DIRS_COUNT; ++i) {
		_scanDir(list, SEARCH_DIRS[i], 0);
	}
	if (list->count > 1) {
		qsort(list->entries, list->count, sizeof(*list->entries), _romCompare);
	}
}

static void _freeRoms(struct RomList* list) {
	size_t i;
	for (i = 0; i < list->count; ++i) {
		free(list->entries[i].path);
		free(list->entries[i].name);
	}
	free(list->entries);
	list->entries = NULL;
	list->count = list->capacity = 0;
}

// --- ROM picker --------------------------------------------------------------

char* mAndroidPickRom(void) {
	JNIEnv* env = (JNIEnv*) SDL_AndroidGetJNIEnv();
	jobject activity = SDL_AndroidGetActivity();
	if (!env || !activity) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ROM picker unavailable: Android activity or JNI environment is missing");
		return NULL;
	}

	jclass cls = (*env)->GetObjectClass(env, activity);
	if (!cls) return NULL;
	jmethodID openPicker = (*env)->GetMethodID(env, cls, "openRomPicker", "()V");
	jmethodID getResult = (*env)->GetMethodID(env, cls, "getPickedRomPath", "()Ljava/lang/String;");
	if ((*env)->ExceptionCheck(env) || !openPicker || !getResult) {
		(*env)->ExceptionClear(env);
		(*env)->DeleteLocalRef(env, cls);
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ROM picker JNI methods could not be resolved");
		return NULL;
	}

	(*env)->CallVoidMethod(env, activity, openPicker);
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionDescribe(env);
		(*env)->ExceptionClear(env);
		(*env)->DeleteLocalRef(env, cls);
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Android failed to launch the ROM document picker");
		return NULL;
	}

	// Never leave the SDL thread polling forever if Android destroys the
	// Activity, the picker fails to return, or its result callback is lost.
	const Uint32 started = SDL_GetTicks();
	const Uint32 timeoutMs = 120000;
	char* result = NULL;
	bool finished = false;
	while (!finished && (Uint32)(SDL_GetTicks() - started) < timeoutMs) {
		SDL_Delay(100);
		jstring value = (jstring) (*env)->CallObjectMethod(env, activity, getResult);
		if ((*env)->ExceptionCheck(env)) {
			(*env)->ExceptionDescribe(env);
			(*env)->ExceptionClear(env);
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Android ROM picker result query failed");
			break;
		}
		if (!value) continue;

		const char* utf = (*env)->GetStringUTFChars(env, value, NULL);
		if (utf && utf[0]) {
			if (strcmp(utf, "__CANCELLED__") == 0) {
				finished = true;
			} else if (strncmp(utf, "__ERROR__:", 10) == 0) {
				SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ROM import failed: %s", utf + 10);
				finished = true;
			} else {
				result = strdup(utf);
				finished = true;
			}
			(*env)->ReleaseStringUTFChars(env, value, utf);
		}
		(*env)->DeleteLocalRef(env, value);
	}
	if (!finished) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "ROM picker timed out after %u ms; returning to emulator idle screen", timeoutMs);
	}
	(*env)->DeleteLocalRef(env, cls);
	return result;
}

// --- Touch overlay -----------------------------------------------------------

#define MAX_TOUCHES 8
#define TOGGLE_SIZE 40

struct OverlayButton {
	SDL_Rect rect;
	SDL_Keycode key;
	const char* label;
	bool round;
};

static struct {
	bool hidden;
	SDL_Window* window;
	struct {
		Sint64 id;
		SDL_Keycode key;
		bool joystick;
	} touches[MAX_TOUCHES];
    SDL_Rect lastView;
    bool hasLastView;
    bool touchesInitialized;
    bool romLoaded;
} overlay;

static struct {
	SDL_Renderer* renderer;
	SDL_Surface* surface;
	SDL_Texture* texture;
	int width;
	int height;
} overlayCache;

 
// Retained as a no-op for callers that still publish diagnostic snapshots.
void mAndroidOverlaySetDebugStatus(const char* rom, const char* load, const char* frames, const char* thread, const char* video, const char* pace, const char* error) {
	(void) rom; (void) load; (void) frames; (void) thread;
	(void) video; (void) pace; (void) error;
}

static void _freeOverlayCache(void) {
	if (overlayCache.texture) {
		SDL_DestroyTexture(overlayCache.texture);
		overlayCache.texture = NULL;
	}
	if (overlayCache.surface) {
		SDL_FreeSurface(overlayCache.surface);
		overlayCache.surface = NULL;
	}
	overlayCache.renderer = NULL;
	overlayCache.width = overlayCache.height = 0;
}

static void _pushKey(SDL_Keycode key, bool down) {
	SDL_Event event;
	SDL_memset(&event, 0, sizeof(event));
	event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	event.key.keysym.sym = key;
	event.key.keysym.scancode = SDL_GetScancodeFromKey(key);
	event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	event.key.repeat = 0;
	SDL_PushEvent(&event);
}

#define MAX_OVERLAY_BUTTONS 8
#define JOYSTICK_DEADZONE 0.22f

static int _unit(int w, int h) {
	bool landscape = w >= h;
	int unit = (landscape ? h : w) / 8;
	if (unit < 42) unit = 42;
	if (unit > 88) unit = 88;
	return unit;
}

int mAndroidOverlayTopInset(int w, int h) {
	// Bottom edge of the top utility row (L/R, LINK, START/SELECT) so the
	// game view can sit directly underneath it in portrait.
	if (w <= 0 || h <= 0) return 0;
	int unit = _unit(w, h);
	int topY = 10;
	int gap = unit / 3;
	return topY + unit + gap + unit / 2;
}

static int _layout(const SDL_Rect* view, struct OverlayButton* out, SDL_Rect* stick, int* stickRadius) {
	int w, h;
	if (!overlay.window) return 0;
	SDL_GetWindowSize(overlay.window, &w, &h);
	if (w <= 0 || h <= 0) return 0;
	bool landscape = w >= h;
	int unit = _unit(w, h);
	int n = 0;
	int radius = unit * 2;
	int sx = landscape ? unit * 3 : w / 4;
	int sy;
	if (landscape) {
		sy = h - radius - unit / 2;
	} else {
		// Portrait: float the thumb controls between the game view and the
		// bottom edge instead of crowding either end.
		sy = h * 5 / 8;
	}
	if (view && sy - radius < view->y + view->h + unit / 2)
		sy = landscape ? h - radius - 8 : view->y + view->h + unit / 2 + radius;
	stick->x = sx - radius;
	stick->y = sy - radius;
	stick->w = radius * 2;
	stick->h = radius * 2;
	*stickRadius = radius;

	int abRadius = unit / 2;
	int abx = landscape ? w - unit * 3 : w * 3 / 4;
	int aby = sy;
	out[n++] = (struct OverlayButton) {{abx + unit / 2, aby - abRadius, abRadius * 2, abRadius * 2}, SDLK_x, "A", true};
	out[n++] = (struct OverlayButton) {{abx - abRadius * 2 - unit / 2, aby + unit / 5, abRadius * 2, abRadius * 2}, SDLK_z, "B", true};

	int topY = 10;
	int shoulderW = unit * 3 / 2;
	int shoulderH = unit * 3 / 4;
	out[n++] = (struct OverlayButton) {{unit / 3, topY, shoulderW, shoulderH}, SDLK_a, "L", false};
	out[n++] = (struct OverlayButton) {{w - shoulderW - unit / 3, topY, shoulderW, shoulderH}, SDLK_s, "R", false};

	int smallW = unit * 2;
	int smallH = unit / 2;
	int gap = unit / 3;
	int controlsY = topY + unit + gap;
	out[n++] = (struct OverlayButton) {{w / 2 - smallW - gap / 2, controlsY, smallW, smallH}, SDLK_RETURN, "START", false};
	out[n++] = (struct OverlayButton) {{w / 2 + gap / 2, controlsY, smallW, smallH}, SDLK_BACKSPACE, "SELECT", false};
	if (!overlay.romLoaded)
		out[n++] = (struct OverlayButton) {{w / 2 - unit * 3 / 2, h / 2 - unit * 3 / 8, unit * 3, unit * 3 / 4}, SDLK_F14, "LOAD ROM", false};
	out[n++] = (struct OverlayButton) {{w / 2 - unit, topY, unit * 2, unit * 3 / 4}, SDLK_F13, "LINK", false};
	return n;
}
static const SDL_Rect* _toggleRect(void) {
	static SDL_Rect rect;
	int w, h;
	if (overlay.window) {
		SDL_GetWindowSize(overlay.window, &w, &h);
	} else {
		rect.x = rect.y = rect.w = rect.h = 0;
		return &rect;
	}
	rect.x = w - TOGGLE_SIZE - 8;
	rect.y = 8;
	rect.w = TOGGLE_SIZE;
	rect.h = TOGGLE_SIZE;
	return &rect;
}

void mAndroidOverlaySetRomLoaded(bool loaded) {
	if (overlay.romLoaded != loaded) {
		overlay.romLoaded = loaded;
		_freeOverlayCache();
	}
}

void mAndroidOverlaySetWindow(SDL_Window* window) {
	if (overlay.window != window) {
		_freeOverlayCache();
		if (window) {
			// A new SDL window means a new emulator session. Reset UI state
			// that may have survived an Activity recreation in native statics.
			overlay.romLoaded = false;
			overlay.hidden = false;
			overlay.hasLastView = false;
		}
	}
	overlay.window = window;
	if (!overlay.touchesInitialized) {
		int i;
		for (i = 0; i < MAX_TOUCHES; ++i)
			overlay.touches[i].id = -1;
		overlay.touchesInitialized = true;
	} else {
		mAndroidOverlayReset();
	}
}

void mAndroidOverlayReset(void) {
	int i;
	for (i = 0; i < MAX_TOUCHES; ++i) {
		if (overlay.touches[i].id != -1) {
			if (overlay.touches[i].key != SDLK_UNKNOWN) _pushKey(overlay.touches[i].key, false);
			overlay.touches[i].id = -1;
			overlay.touches[i].joystick = false;
		}
	}
}

void mAndroidOverlayHandleEvent(const SDL_Event* event) {
	if ((event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) || event->type == SDL_APP_WILLENTERBACKGROUND) {
		mAndroidOverlayReset();
		return;
	}
	if (event->type != SDL_FINGERDOWN && event->type != SDL_FINGERUP && event->type != SDL_FINGERMOTION) return;
	int w, h;
	if (!overlay.window) return;
	SDL_GetWindowSize(overlay.window, &w, &h);
	if (w <= 0 || h <= 0) return;
	Sint64 id = event->tfinger.fingerId;
	int x = (int) (event->tfinger.x * w);
	int y = (int) (event->tfinger.y * h);
	if (event->type == SDL_FINGERDOWN) {
		const SDL_Rect* toggle = _toggleRect();
		if (x >= toggle->x && x < toggle->x + toggle->w && y >= toggle->y && y < toggle->y + toggle->h) {
			overlay.hidden = !overlay.hidden;
			if (overlay.hidden) mAndroidOverlayReset();
			return;
		}
		if (overlay.hidden) return;
		struct OverlayButton buttons[MAX_OVERLAY_BUTTONS];
		SDL_Rect stick;
		int stickRadius = 0;
		int n = _layout(overlay.hasLastView ? &overlay.lastView : NULL, buttons, &stick, &stickRadius);
		int dx = x - (stick.x + stickRadius), dy = y - (stick.y + stickRadius);
		if (dx * dx + dy * dy <= stickRadius * stickRadius) {
			int slot;
			for (slot = 0; slot < MAX_TOUCHES && overlay.touches[slot].id != -1; ++slot) {}
			if (slot < MAX_TOUCHES) {
				overlay.touches[slot].id = id;
				overlay.touches[slot].joystick = true;
				overlay.touches[slot].key = SDLK_UNKNOWN;
				float nx = (float) dx / stickRadius, ny = (float) dy / stickRadius;
				if (nx * nx + ny * ny >= JOYSTICK_DEADZONE * JOYSTICK_DEADZONE) {
					SDL_Keycode key = abs(dx) > abs(dy) ? (dx < 0 ? SDLK_LEFT : SDLK_RIGHT) : (dy < 0 ? SDLK_UP : SDLK_DOWN);
					overlay.touches[slot].key = key;
					_pushKey(key, true);
				}
			}
			return;
		}
		int i;
		for (i = 0; i < n; ++i) {
			if (x >= buttons[i].rect.x && x < buttons[i].rect.x + buttons[i].rect.w && y >= buttons[i].rect.y && y < buttons[i].rect.y + buttons[i].rect.h) {
				if (buttons[i].key == SDLK_F13) { mAndroidPeerJniShowDialog(); return; }
				int slot;
				for (slot = 0; slot < MAX_TOUCHES && overlay.touches[slot].id != -1; ++slot) {}
				if (slot < MAX_TOUCHES) {
					overlay.touches[slot].id = id;
					overlay.touches[slot].key = buttons[i].key;
					overlay.touches[slot].joystick = false;
					_pushKey(buttons[i].key, true);
				}
				return;
			}
		}
		return;
	}
	struct OverlayButton buttons[MAX_OVERLAY_BUTTONS];
	SDL_Rect stick;
	int stickRadius = 0;
	int n = overlay.hidden ? 0 : _layout(overlay.hasLastView ? &overlay.lastView : NULL, buttons, &stick, &stickRadius);
	int i;
	for (i = 0; i < MAX_TOUCHES; ++i) {
		if (overlay.touches[i].id != id) continue;
		if (overlay.touches[i].joystick) {
			int dx = x - (stick.x + stickRadius), dy = y - (stick.y + stickRadius);
			SDL_Keycode next = SDLK_UNKNOWN;
			// Keep ownership of this finger after it leaves the stick's hit area.
			// SDL continues delivering motion for the active finger until release.
			if (event->type != SDL_FINGERUP &&
				dx * dx + dy * dy >= (int) (stickRadius * stickRadius * JOYSTICK_DEADZONE * JOYSTICK_DEADZONE))
				next = abs(dx) > abs(dy) ? (dx < 0 ? SDLK_LEFT : SDLK_RIGHT) : (dy < 0 ? SDLK_UP : SDLK_DOWN);
			if (next != overlay.touches[i].key) {
				if (overlay.touches[i].key != SDLK_UNKNOWN) _pushKey(overlay.touches[i].key, false);
				if (next != SDLK_UNKNOWN) _pushKey(next, true);
				overlay.touches[i].key = next;
			}
			if (event->type == SDL_FINGERUP) overlay.touches[i].id = -1;
			continue;
		}
		const struct OverlayButton* target = NULL;
		int j;
		for (j = 0; j < n; ++j) {
			if (x >= buttons[j].rect.x && x < buttons[j].rect.x + buttons[j].rect.w && y >= buttons[j].rect.y && y < buttons[j].rect.y + buttons[j].rect.h) { target = &buttons[j]; break; }
		}
		if (event->type == SDL_FINGERUP) {
			_pushKey(overlay.touches[i].key, false);
			overlay.touches[i].id = -1;
		} else if (!target || target->key != overlay.touches[i].key) {
			_pushKey(overlay.touches[i].key, false);
			overlay.touches[i].id = -1;
			if (target) { overlay.touches[i].id = id; overlay.touches[i].key = target->key; _pushKey(target->key, true); }
		}
	}
}
static void _fillCircle(SDL_Renderer* renderer, int cx, int cy, int radius) {
	int y;
	for (y = -radius; y <= radius; ++y) {
		int x = (int) sqrt((double) radius * radius - (double) y * y);
		SDL_RenderDrawLine(renderer, cx - x, cy + y, cx + x, cy + y);
	}
}

static void _drawCircle(SDL_Renderer* renderer, int cx, int cy, int radius) {
	int x = radius, y = 0, error = 1 - radius;
	while (x >= y) {
		SDL_RenderDrawLine(renderer, cx - x, cy + y, cx + x, cy + y);
		SDL_RenderDrawLine(renderer, cx - x, cy - y, cx + x, cy - y);
		SDL_RenderDrawLine(renderer, cx - y, cy + x, cx + y, cy + x);
		SDL_RenderDrawLine(renderer, cx - y, cy - x, cx + y, cy - x);
		++y;
		if (error < 0) error += 2 * y + 1;
		else { --x; error += 2 * (y - x) + 1; }
	}
}

void mAndroidOverlayDraw(SDL_Renderer* renderer, const SDL_Rect* view) {
	if (!overlay.window || !renderer) return;
	const SDL_Rect* toggle = _toggleRect();
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(renderer, 200, 200, 200, 150);
	SDL_RenderFillRect(renderer, toggle);
	if (overlay.hidden) return;
	if (view) { overlay.lastView = *view; overlay.hasLastView = true; }
	else overlay.hasLastView = false;
	struct OverlayButton buttons[MAX_OVERLAY_BUTTONS];
	SDL_Rect stick; int stickRadius = 0;
	int n = _layout(view, buttons, &stick, &stickRadius);
	int cx = stick.x + stickRadius, cy = stick.y + stickRadius;
	SDL_SetRenderDrawColor(renderer, 30, 35, 45, 105);
	_fillCircle(renderer, cx, cy, stickRadius);
	SDL_SetRenderDrawColor(renderer, 235, 240, 250, 190);
	_drawCircle(renderer, cx, cy, stickRadius);
	_drawCircle(renderer, cx, cy, stickRadius * 2 / 3);
	SDL_RenderDrawLine(renderer, cx - stickRadius / 2, cy, cx + stickRadius / 2, cy);
	SDL_RenderDrawLine(renderer, cx, cy - stickRadius / 2, cx, cy + stickRadius / 2);
	SDL_SetRenderDrawColor(renderer, 235, 235, 235, 125);
	int i;
	for (i = 0; i < n; ++i) {
		if (buttons[i].round) {
			int bx = buttons[i].rect.x + buttons[i].rect.w / 2, by = buttons[i].rect.y + buttons[i].rect.h / 2;
			_fillCircle(renderer, bx, by, buttons[i].rect.w / 2);
			SDL_SetRenderDrawColor(renderer, 255, 255, 255, 210);
			_drawCircle(renderer, bx, by, buttons[i].rect.w / 2);
		} else {
			SDL_RenderFillRect(renderer, &buttons[i].rect);
			SDL_SetRenderDrawColor(renderer, 255, 255, 255, 210);
			SDL_RenderDrawRect(renderer, &buttons[i].rect);
		}
		SDL_SetRenderDrawColor(renderer, 235, 235, 235, 125);
	}
	int w, h; SDL_GetWindowSize(overlay.window, &w, &h);
	if (w <= 0 || h <= 0) return;
	if (overlayCache.renderer != renderer || overlayCache.width != w || overlayCache.height != h) {
		_freeOverlayCache();
		overlayCache.renderer = renderer; overlayCache.width = w; overlayCache.height = h;
		overlayCache.surface = _makeSurface(w, h);
		if (overlayCache.surface) overlayCache.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
		if (!overlayCache.surface || !overlayCache.texture) { SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Touch overlay allocation failed: %s", SDL_GetError()); _freeOverlayCache(); return; }
		SDL_SetTextureBlendMode(overlayCache.texture, SDL_BLENDMODE_BLEND);
	}
	SDL_Surface* surf = overlayCache.surface;
	Uint32 transparent = SDL_MapRGBA(surf->format, 0, 0, 0, 0);
	SDL_FillRect(surf, NULL, transparent);
	Uint32 white = SDL_MapRGBA(surf->format, 0xFF, 0xFF, 0xFF, 0xF0);
	for (i = 0; i < n; ++i) {
		int tx = buttons[i].rect.x + (buttons[i].rect.w - (int) strlen(buttons[i].label) * 16) / 2;
		int ty = buttons[i].rect.y + (buttons[i].rect.h - 16) / 2;
		_blitText(surf, buttons[i].label, tx, ty, 2, white);
	}
	_blitText(surf, "4-WAY", cx - 20, cy - 8, 1, white);
	if (SDL_UpdateTexture(overlayCache.texture, NULL, surf->pixels, surf->pitch) < 0) { SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Touch overlay texture update failed: %s", SDL_GetError()); _freeOverlayCache(); return; }
	if (SDL_RenderCopy(renderer, overlayCache.texture, NULL, NULL) < 0) SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Touch overlay draw failed: %s", SDL_GetError());
}
