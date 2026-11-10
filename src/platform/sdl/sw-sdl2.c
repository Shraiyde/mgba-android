/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "main.h"

#include <mgba/core/core.h>
#include <mgba/core/thread.h>
#include <mgba/core/version.h>

static bool mSDLSWInit(struct mSDLRenderer* renderer);
static void mSDLSWRunloop(struct mSDLRenderer* renderer, void* user);
static void mSDLSWDeinit(struct mSDLRenderer* renderer);

void mSDLSWCreate(struct mSDLRenderer* renderer) {
	renderer->init = mSDLSWInit;
	renderer->deinit = mSDLSWDeinit;
	renderer->runloop = mSDLSWRunloop;
}

bool mSDLSWInit(struct mSDLRenderer* renderer) {
	unsigned width, height;
	bool softwareRenderer = false;
	renderer->core->baseVideoSize(renderer->core, &width, &height);
#if SDL_VERSION_ATLEAST(3, 0, 0)
	renderer->window = SDL_CreateWindow(projectName, renderer->viewportWidth, renderer->viewportHeight, SDL_WINDOW_FULLSCREEN * renderer->player.fullscreen);
	renderer->sdlRenderer = SDL_CreateRenderer(renderer->window, NULL);
	SDL_SetRenderVSync(renderer->sdlRenderer, 1);
#else
	renderer->window = SDL_CreateWindow(projectName, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, renderer->viewportWidth, renderer->viewportHeight, SDL_WINDOW_FULLSCREEN_DESKTOP * renderer->player.fullscreen);
	if (!renderer->window) {
		printf("Could not create SDL window: %s\\n", SDL_GetError());
		return false;
	}
	renderer->sdlRenderer = SDL_CreateRenderer(renderer->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!renderer->sdlRenderer) {
		// Some Android devices expose no accelerated SDL renderer. Keep the
		// emulator playable with SDL's software renderer instead.
		renderer->sdlRenderer = SDL_CreateRenderer(renderer->window, -1, SDL_RENDERER_SOFTWARE);
		softwareRenderer = true;
	}
#endif
	if (!renderer->window || !renderer->sdlRenderer) {
		printf("Could not create SDL video renderer: %s\\n", SDL_GetError());
		if (renderer->sdlRenderer) SDL_DestroyRenderer(renderer->sdlRenderer);
		if (renderer->window) SDL_DestroyWindow(renderer->window);
		renderer->sdlRenderer = NULL;
		renderer->window = NULL;
		return false;
	}
	SDL_GetWindowSize(renderer->window, &renderer->viewportWidth, &renderer->viewportHeight);
	renderer->player.window = renderer->window;
create_game_texture:
	renderer->outputBuffer = NULL;
#ifdef COLOR_16_BIT
#ifdef COLOR_5_6_5
	renderer->sdlTex = SDL_CreateTexture(renderer->sdlRenderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, width, height);
#else
	renderer->sdlTex = SDL_CreateTexture(renderer->sdlRenderer, SDL_PIXELFORMAT_ABGR1555, SDL_TEXTUREACCESS_STREAMING, width, height);
#endif
#else
	renderer->sdlTex = SDL_CreateTexture(renderer->sdlRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, width, height);
#endif

	if (!renderer->sdlTex && !softwareRenderer) {
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Accelerated game texture creation failed; retrying with software renderer: %s", SDL_GetError());
		SDL_DestroyRenderer(renderer->sdlRenderer);
		renderer->sdlRenderer = SDL_CreateRenderer(renderer->window, -1, SDL_RENDERER_SOFTWARE);
		softwareRenderer = true;
		if (!renderer->sdlRenderer) {
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Could not create software SDL renderer: %s", SDL_GetError());
			SDL_DestroyWindow(renderer->window);
			renderer->window = NULL;
			return false;
		}
		goto create_game_texture;
	}
	if (!renderer->sdlTex) {
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Could not create SDL game texture: %s", SDL_GetError());
		SDL_DestroyRenderer(renderer->sdlRenderer);
		SDL_DestroyWindow(renderer->window);
		renderer->sdlRenderer = NULL;
		renderer->window = NULL;
		return false;
	}

	int stride = 0;
	if (SDL_LockTexture(renderer->sdlTex, 0, (void**) &renderer->outputBuffer, &stride) < 0 || !renderer->outputBuffer) {
		if (!softwareRenderer) {
			SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Accelerated game texture lock failed; retrying with software renderer: %s", SDL_GetError());
			SDL_DestroyTexture(renderer->sdlTex);
			renderer->sdlTex = NULL;
			SDL_DestroyRenderer(renderer->sdlRenderer);
			renderer->sdlRenderer = SDL_CreateRenderer(renderer->window, -1, SDL_RENDERER_SOFTWARE);
			softwareRenderer = true;
			if (renderer->sdlRenderer) {
				goto create_game_texture;
			}
		}
		SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Could not lock SDL game texture: %s", SDL_GetError());
		if (renderer->sdlTex) SDL_DestroyTexture(renderer->sdlTex);
		if (renderer->sdlRenderer) SDL_DestroyRenderer(renderer->sdlRenderer);
		SDL_DestroyWindow(renderer->window);
		renderer->sdlTex = NULL;
		renderer->sdlRenderer = NULL;
		renderer->window = NULL;
		return false;
	}
	renderer->outputStride = stride / BYTES_PER_PIXEL;
	renderer->core->setVideoBuffer(renderer->core, renderer->outputBuffer, renderer->outputStride);
	renderer->videoBufferAttached = true;

	return true;
}

void mSDLSWRunloop(struct mSDLRenderer* renderer, void* user) {
	struct mCoreThread* context = user;
	SDL_Event event;

	while (mCoreThreadIsActive(context)) {
		while (SDL_PollEvent(&event)) {
			mSDLHandleEvent(context, &renderer->player, &event);
		}

		if (mCoreSyncWaitFrameStart(&context->impl->sync)) {
			SDL_UnlockTexture(renderer->sdlTex);
			SDL_RenderCopy(renderer->sdlRenderer, renderer->sdlTex, 0, 0);
			SDL_RenderPresent(renderer->sdlRenderer);
			int stride;
			SDL_LockTexture(renderer->sdlTex, 0, (void**) &renderer->outputBuffer, &stride);
			renderer->core->setVideoBuffer(renderer->core, renderer->outputBuffer, stride / BYTES_PER_PIXEL);
		}
		mCoreSyncWaitFrameEnd(&context->impl->sync);
	}
}

void mSDLSWDeinit(struct mSDLRenderer* renderer) {
	if (renderer->ratio > 1) {
		free(renderer->outputBuffer);
	}
}
