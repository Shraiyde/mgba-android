/* Copyright (c) 2026 Gitsnup
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "android-ui.h"

#include <jni.h>
#include <SDL_system.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
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
	if (!env || !activity) return NULL;

	jclass cls = (*env)->GetObjectClass(env, activity);
	jmethodID openPicker = (*env)->GetMethodID(env, cls, "openRomPicker", "()V");
	jmethodID getResult = (*env)->GetMethodID(env, cls, "getPickedRomPath", "()Ljava/lang/String;");
	if (!openPicker || !getResult) {
		(*env)->DeleteLocalRef(env, cls);
		return NULL;
	}
	(*env)->CallVoidMethod(env, activity, openPicker);
	char* result = NULL;
	while (!result) {
		SDL_Delay(100);
		jstring value = (jstring) (*env)->CallObjectMethod(env, activity, getResult);
		if (value) {
			const char* utf = (*env)->GetStringUTFChars(env, value, NULL);
			if (utf && utf[0]) {
				if (strcmp(utf, "__CANCELLED__") == 0) {
					result = NULL;
					(*env)->ReleaseStringUTFChars(env, value, utf);
					(*env)->DeleteLocalRef(env, value);
					break;
				}
				result = strdup(utf);
				(*env)->ReleaseStringUTFChars(env, value, utf);
			}
			(*env)->DeleteLocalRef(env, value);
		}
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
};

static struct {
	bool hidden;
	SDL_Window* window;
	struct {
		Sint64 id;
		SDL_Keycode key;
	} touches[MAX_TOUCHES];
} overlay;

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

static int _layout(const SDL_Rect* view, struct OverlayButton* out) {
	int w, h;
	if (overlay.window) {
		SDL_GetWindowSize(overlay.window, &w, &h);
	} else {
		return 0;
	}
	bool landscape = w >= h;
	int cell = (landscape ? h : w) / 12;
	if (cell < 40) {
		cell = 40;
	}
	if (cell > 90) {
		cell = 90;
	}
	int r = cell; // dpad radius in cells
	int n = 0;

	int dpx = landscape ? cell * 4 : w / 4;
	int dpy = landscape ? h - cell * 5 : h - cell * 5;
	if (view && dpy < view->y + view->h + cell * 3) {
		dpy = h - cell * 4;
	}
	// D-pad cross (up, down, left, right)
	out[n].rect.x = dpx - cell / 2;
	out[n].rect.y = dpy - cell - cell / 2;
	out[n].rect.w = cell;
	out[n].rect.h = cell;
	out[n].key = SDLK_UP;
	out[n].label = "^";
	++n;
	out[n].rect.x = dpx - cell / 2;
	out[n].rect.y = dpy + cell / 2;
	out[n].rect.w = cell;
	out[n].rect.h = cell;
	out[n].key = SDLK_DOWN;
	out[n].label = "v";
	++n;
	out[n].rect.x = dpx - cell - cell / 2;
	out[n].rect.y = dpy - cell / 2;
	out[n].rect.w = cell;
	out[n].rect.h = cell;
	out[n].key = SDLK_LEFT;
	out[n].label = "<";
	++n;
	out[n].rect.x = dpx + cell / 2;
	out[n].rect.y = dpy - cell / 2;
	out[n].rect.w = cell;
	out[n].rect.h = cell;
	out[n].key = SDLK_RIGHT;
	out[n].label = ">";
	++n;

	// A / B
	int abx = landscape ? w - cell * 4 : w * 3 / 4;
	int aby = dpy;
	out[n].rect.x = abx + cell / 2;
	out[n].rect.y = aby - cell - cell / 2;
	out[n].rect.w = cell;
	out[n].rect.h = cell;
	out[n].key = SDLK_x;
	out[n].label = "A";
	++n;
	out[n].rect.x = abx - cell - cell / 2;
	out[n].rect.y = aby + cell / 2;
	out[n].rect.w = cell;
	out[n].rect.h = cell;
	out[n].key = SDLK_z;
	out[n].label = "B";
	++n;

	// L / R
	out[n].rect.x = cell / 2;
	out[n].rect.y = 8;
	out[n].rect.w = cell * 2;
	out[n].rect.h = cell;
	out[n].key = SDLK_a;
	out[n].label = "L";
	++n;
	out[n].rect.x = w - cell * 2 - cell / 2;
	out[n].rect.y = 8;
	out[n].rect.w = cell * 2;
	out[n].rect.h = cell;
	out[n].key = SDLK_s;
	out[n].label = "R";
	++n;

	// Start / Select
	int sy = dpy + cell * 2;
	if (sy + cell > h) {
		sy = h - cell;
	}
	out[n].rect.x = w / 2 - cell * 2;
	out[n].rect.y = sy;
	out[n].rect.w = cell * 2;
	out[n].rect.h = cell / 2;
	out[n].key = SDLK_RETURN;
	out[n].label = "START";
	++n;
	out[n].rect.x = w / 2 + cell * 0.5f;
	out[n].rect.y = sy;
	out[n].rect.w = cell * 2;
	out[n].rect.h = cell / 2;
	out[n].key = SDLK_BACKSPACE;
	out[n].label = "SELECT";
	++n;

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

void mAndroidOverlaySetWindow(SDL_Window* window) {
	overlay.window = window;
}

void mAndroidOverlayReset(void) {
	int i;
	for (i = 0; i < MAX_TOUCHES; ++i) {
		overlay.touches[i].id = 0;
	}
}

void mAndroidOverlayHandleEvent(const SDL_Event* event) {
	if (event->type != SDL_FINGERDOWN && event->type != SDL_FINGERUP && event->type != SDL_FINGERMOTION) {
		return;
	}
	int w, h;
	if (overlay.window) {
		SDL_GetWindowSize(overlay.window, &w, &h);
	} else {
		return;
	}
	Sint64 id = event->tfinger.fingerId;
	int x = (int) (event->tfinger.x * w);
	int y = (int) (event->tfinger.y * h);

	if (event->type == SDL_FINGERDOWN) {
		const SDL_Rect* toggle = _toggleRect();
		if (x >= toggle->x && x < toggle->x + toggle->w && y >= toggle->y && y < toggle->y + toggle->h) {
			overlay.hidden = !overlay.hidden;
			return;
		}
		if (overlay.hidden) {
			return;
		}
		struct OverlayButton buttons[12];
		int n = _layout(NULL, buttons);
		int i;
		for (i = 0; i < n; ++i) {
			if (x >= buttons[i].rect.x && x < buttons[i].rect.x + buttons[i].rect.w &&
			    y >= buttons[i].rect.y && y < buttons[i].rect.y + buttons[i].rect.h) {
				int slot;
				for (slot = 0; slot < MAX_TOUCHES; ++slot) {
					if (!overlay.touches[slot].id) {
						break;
					}
				}
				if (slot < MAX_TOUCHES) {
					overlay.touches[slot].id = id;
					overlay.touches[slot].key = buttons[i].key;
					_pushKey(buttons[i].key, true);
				}
				return;
			}
		}
		return;
	}

	// FINGERUP / FINGERMOTION: release or slide between buttons
	struct OverlayButton buttons[12];
	int n = overlay.hidden ? 0 : _layout(NULL, buttons);
	int i;
	for (i = 0; i < MAX_TOUCHES; ++i) {
		if (overlay.touches[i].id != id) {
			continue;
		}
		const struct OverlayButton* target = NULL;
		int j;
		for (j = 0; j < n; ++j) {
			if (x >= buttons[j].rect.x && x < buttons[j].rect.x + buttons[j].rect.w &&
			    y >= buttons[j].rect.y && y < buttons[j].rect.y + buttons[j].rect.h) {
				target = &buttons[j];
				break;
			}
		}
		if (event->type == SDL_FINGERUP) {
			_pushKey(overlay.touches[i].key, false);
			overlay.touches[i].id = 0;
		} else if (!target || target->key != overlay.touches[i].key) {
			_pushKey(overlay.touches[i].key, false);
			overlay.touches[i].id = 0;
			if (target) {
				overlay.touches[i].id = id;
				overlay.touches[i].key = target->key;
				_pushKey(target->key, true);
			}
		}
	}
}

void mAndroidOverlayDraw(SDL_Renderer* renderer, const SDL_Rect* view) {
	if (!overlay.window) {
		return;
	}
	const SDL_Rect* toggle = _toggleRect();
	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(renderer, 200, 200, 200, 32);
	SDL_RenderFillRect(renderer, toggle);

	if (overlay.hidden) {
		return;
	}
	struct OverlayButton buttons[12];
	int n = _layout(view, buttons);
	int i;
	for (i = 0; i < n; ++i) {
		SDL_SetRenderDrawColor(renderer, 235, 235, 235, 48);
		SDL_RenderFillRect(renderer, &buttons[i].rect);
		SDL_SetRenderDrawColor(renderer, 255, 255, 255, 96);
		SDL_RenderDrawRect(renderer, &buttons[i].rect);
	}

	// Labels, drawn via a small overlay surface
	int w, h;
	SDL_GetWindowSize(overlay.window, &w, &h);
	SDL_Surface* surf = _makeSurface(w, h);
	Uint32 white = SDL_MapRGBA(surf->format, 0xFF, 0xFF, 0xFF, 0xC0);
	for (i = 0; i < n; ++i) {
		int tx = buttons[i].rect.x + (buttons[i].rect.w - 8 * 2) / 2;
		int ty = buttons[i].rect.y + (buttons[i].rect.h - 8 * 2) / 2;
		_blitText(surf, buttons[i].label, tx, ty, 2, white);
	}
	SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
	SDL_FreeSurface(surf);
	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
	SDL_RenderCopy(renderer, tex, NULL, NULL);
	SDL_DestroyTexture(tex);
}
