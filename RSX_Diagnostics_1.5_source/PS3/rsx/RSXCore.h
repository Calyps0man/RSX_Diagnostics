// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
//
// Thin layer over PSL1GHT's RSX library shared by the self-test and the hardware renderer:
// memory (RSX local + I/O-mapped main memory), fences, the embedded shader set, render targets,
// vertex ring, image transfers and our own display buffers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct _gcmCtxData;

namespace RSX {

// Main-memory region mapped for the RSX by our enlarged rsxInit (see RSXCore.cpp).
static constexpr uint32_t COMMAND_BUFFER_SIZE = 1024 * 1024;
static constexpr uint32_t IO_REGION_SIZE = 16 * 1024 * 1024;

bool Ready();
// Diagnostics: the current step is logged before it runs. A watchdog thread reports a step that
// takes over 3 seconds together with the GPU's command pointers and our fence labels, and an RSX
// exception handler logs the error code, so a freeze leaves its location in launch.log.
void InitDiagnostics();
void Step(const char *name);
void StepQuiet(const char *name);  // Same, without the log line (per-frame paths).
std::string GpuStateText();
_gcmCtxData *Context();
// Lines describing the RSX configuration (memory sizes, clocks, IO mapping).
std::string Describe();

// RSX local memory (fast for the RSX; CPU writes are fine, CPU reads are very slow).
void *LocalAlloc(uint32_t size, uint32_t align = 64);
void LocalFree(void *ptr);
// Size of the largest block LocalAlloc could return right now (1 MB granularity).
uint32_t LargestFreeLocal();
uint32_t LocalOffset(const void *ptr);
// Main memory the RSX can read and write (CPU reads are fast). Bump allocator; FreeAllIo() resets it.
void *IoAlloc(uint32_t size, uint32_t align = 128);
uint32_t IoOffset(const void *ptr);
void IoMark();      // Remember the allocation point...
void IoRelease();   // ...and release everything allocated after it.

// Fences. Fence() queues a backend label write (completes after all earlier rendering, including
// pixels written). Wait returns false on timeout (never hangs forever).
uint32_t Fence();
bool FenceDone(uint32_t value);
bool WaitFence(uint32_t value, double timeoutSeconds = 2.0);
bool Finish(double timeoutSeconds = 2.0);  // Fence + wait.
// GPU-side: later commands wait until earlier rendering finished (render -> transfer/texture).
void GpuWaitForRendering();
void Flush();  // Kick the command buffer (with a memory barrier first).

// Shader set.
bool LoadPrograms();
void BindVertexProgram();
// Fragment program index; see make-shaders.py.
int FragmentIndex(bool textured, int texFunc, bool rgba, bool doubling, bool fog, bool blendMul);
static constexpr int FRAGMENT_CLEAR = 0;
static constexpr int FRAGMENT_BURN = 161;  // Stress test load (heavy ALU).
void BindFragmentProgram(int index);
// c[0] env color, c[1] fog color, c[2] blend multiplier, each RGBA in 0..1.
void SetVertexConstants(const float env[4], const float fog[4], const float mul[4]);

// Render target: A8R8G8B8 color plus Z24S8 depth, both in RSX local memory.
struct Target {
	uint32_t colorOffset = 0;
	uint32_t colorPitch = 0;
	uint32_t depthOffset = 0;
	uint32_t depthPitch = 0;
	uint16_t width = 0;
	uint16_t height = 0;
	bool colorInMain = false;  // Color buffer in I/O-mapped main memory (self-test).
	bool hasDepth = true;
};
void SetTarget(const Target &target);
// Viewport mapping drawing pixels 1:1 to the target, z in 0..1. Also sets scissor to the target.
void SetPixelViewport(uint16_t width, uint16_t height);
void SetScissor(int x, int y, int w, int h);
void ClearTarget(uint32_t argb, bool color, bool depthStencil, uint32_t depthStencilValue = 0xFFFFFF00);
void SetDefaultState();  // No blend/depth/alpha/stencil/cull, full color mask, smooth shading.
// Re-uploads fragment programs and drops cached bindings (vertex arrays, programs, caches).
void ResetDrawState();

// Vertex format consumed by the vertex program.
struct Vertex {
	float x, y, z, w;   // Clip space (already multiplied by w).
	uint8_t c0[4];      // R, G, B, A.
	uint8_t c1[4];      // Secondary color R, G, B, (unused).
	float s, t, r, q;   // r unused (0); q divides s,t (TXP).
	float fog;          // 1 = no fog.
	float pad;
};
static_assert(sizeof(Vertex) == 48, "RSX vertex layout");
// Space for count vertices in the ring buffer. Returns nullptr only if count is absurd.
Vertex *AllocVertices(int count, uint32_t *firstIndex);
void DrawVertices(uint32_t primType, uint32_t firstIndex, uint32_t count);
// Call once per frame: rotates ring segments behind fences.
void EndFrame();

// Textures: A8R8G8B8 linear in local memory.
void BindTexture(int unit, uint32_t offset, uint32_t width, uint32_t height, uint32_t pitch,
	bool linearFilter, bool clampS, bool clampT, bool inMain = false, bool linearMag = false);
void InvalidateTextureCache();

// Image transfers (A8R8G8B8). Coordinates in pixels. linear selects bilinear scaling.
enum class Mem { Local, Main };
void ScaleImage(Mem srcMem, uint32_t srcOffset, uint32_t srcPitch, int srcW, int srcH,
	Mem dstMem, uint32_t dstOffset, uint32_t dstPitch, int dstX, int dstY, int dstW, int dstH, bool linear);
// Plain copy of rows (no format change).
void CopyRows(Mem srcMem, uint32_t srcOffset, uint32_t srcPitch, Mem dstMem, uint32_t dstOffset, uint32_t dstPitch,
	uint32_t bytesPerRow, uint32_t rows);

// Our own pair of display buffers (ids 2 and 3; SDL keeps 0 and 1 for the menus).
bool InitDisplay();
int DisplayWidth();
int DisplayHeight();
uint32_t BackBufferOffset();
// CPU pointer to the back buffer (CPU drawing without the 3D pipeline).
void *BackBufferPtr();
// Waits (with a timeout) until the last queued flip has happened. False on timeout.
bool WaitFlip(double timeoutSeconds = 0.25);
uint32_t DisplayPitch();
void Present();  // Flip to the back buffer; waits only if the previous flip is still pending.
void ResetDisplay();  // Before handing the screen back to SDL.

}  // namespace RSX
