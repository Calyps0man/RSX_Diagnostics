// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// Experimental hardware renderer: the software GPU still decodes display lists, transforms, lights
// and clips; every resulting primitive is drawn by the RSX into its own copies of the PSP's
// framebuffers, optionally at 2x or 3x resolution. PSP VRAM stays the source of truth between
// sync points: framebuffers are uploaded from VRAM when the CPU changed them, and written back
// to VRAM when something reads them (block transfers, render-to-texture, CLUT loads, and every
// frame unless "VRAM sync: minimal" is chosen).
#pragma once

#include <cstdint>
#include <string>

#include "GPU/Software/HardwareBackend.h"

class RSXRenderer;

struct RSXRendererOptions {
	int scale = 1;              // Internal resolution multiplier, 1..3.
	bool syncEveryFrame = true; // Write rendered framebuffers back to PSP VRAM every frame.
	bool bilinear = true;       // Output scaling filter.
	int screenMode = 0;         // PS3Frontend::ScreenMode.
	bool batched = true;        // Fast batched path for whole draws (skips the per-triangle software path).
};

// Null if the RSX could not be set up (the reason is in launch.log).
RSXRenderer *RSXRendererCreate(const RSXRendererOptions &options);
void RSXRendererDestroy(RSXRenderer *renderer);
// Shows the PSP's current display framebuffer, with an optional text line on top.
void RSXRendererPresent(RSXRenderer *renderer, const std::string &overlay);
std::string RSXRendererStats(RSXRenderer *renderer);
Rasterizer::HardwareBackend *RSXRendererBackend(RSXRenderer *renderer);
// 192x144 font atlas (12x24 glyphs for ASCII 32..126, 16 per row), one byte per pixel (0 = clear).
void RSXRendererSetFont(const uint8_t *atlas);
