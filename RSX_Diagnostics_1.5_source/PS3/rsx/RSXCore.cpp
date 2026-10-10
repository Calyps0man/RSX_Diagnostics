// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
#include <malloc.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/nv40.h>
#include <sysutil/video.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include "PS3/rsx/RSXCore.h"
#include "PS3/rsx/RSXShaders.inc"
#include "PS3/frontend/Frontend.h"

extern "C" void __real_rsxSetTransferScaleSurface(gcmContextData *, const gcmTransferScale *, const gcmTransferSurface *);
void PS3ScaledTransfer(gcmContextData *context, const gcmTransferScale *scale, const gcmTransferSurface *surface);
extern "C" s32 rsxContextCallback(gcmContextData *context, u32 count);
extern "C" s32 __real_rsxInit(gcmContextData **context, u32 cmdSize, u32 ioSize, const void *ioAddress);

namespace RSX {

static gcmContextData *g_context = nullptr;
static uint8_t *g_io = nullptr;           // Start of our I/O-mapped region.
static uint32_t g_ioUsed = 0;             // Bump pointer (bytes from g_io).
static uint32_t g_ioMark = 0;
static uint32_t g_ioFirstFree = 0;

// Labels 64..255 are for applications; keep away from the ones samples commonly use (255).
static constexpr u8 LABEL_FENCE = 200;
static constexpr u8 LABEL_GPUWAIT = 201;
static uint32_t g_fenceValue = 0;
static uint32_t g_gpuWaitValue = 0;
static void InitLabels();

static double NowSeconds() {
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return (double)sec + (double)nsec * 1e-9;
}

}  // namespace RSX

// SDL's PSL1GHT driver calls rsxInit with a 64 KB command buffer inside a 1 MB I/O region. A
// renderer needs a bigger command buffer, plus main memory the RSX can read (vertex staging,
// readback). Swap in our own parameters; SDL keeps working the same way on top of it.
extern "C" s32 __wrap_rsxInit(gcmContextData **context, u32 cmdSize, u32 ioSize, const void *ioAddress) {
	using namespace RSX;
	void *io = memalign(1024 * 1024, IO_REGION_SIZE);
	if (io) {
		s32 result = __real_rsxInit(context, COMMAND_BUFFER_SIZE, IO_REGION_SIZE, io);
		if (result == 0) {
			g_io = (uint8_t *)io;
			// The command buffer lives at the start of the region; leave a margin after it.
			g_ioFirstFree = COMMAND_BUFFER_SIZE + 64 * 1024;
			g_ioUsed = g_ioMark = g_ioFirstFree;
			g_context = context ? *context : gGcmContext;
			PS3Frontend::Trace("RSX init: command buffer %u KB, I/O region %u MB (requested %u KB / %u KB)",
				COMMAND_BUFFER_SIZE / 1024, IO_REGION_SIZE / (1024 * 1024), cmdSize / 1024, ioSize / 1024);
			return 0;
		}
		PS3Frontend::Trace("RSX init with enlarged buffers failed (%d); using SDL's request", result);
		free(io);
	}
	s32 result = __real_rsxInit(context, cmdSize, ioSize, ioAddress);
	if (result == 0) {
		g_context = context ? *context : gGcmContext;
		g_io = nullptr;
	}
	return result;
}

namespace RSX {

static volatile const char *g_step = "idle";
static volatile double g_stepStart = 0.0;
static volatile bool g_watchdogRunning = false;

std::string GpuStateText() {
	char text[200];
	gcmControlRegister *ctrl = gcmGetControlRegister();
	snprintf(text, sizeof(text), "put %08x get %08x ref %08x, fence label %u (sent %u), wait label %u (sent %u)",
		ctrl ? ctrl->put : 0, ctrl ? ctrl->get : 0, ctrl ? ctrl->ref : 0,
		*(volatile u32 *)gcmGetLabelAddress(LABEL_FENCE), g_fenceValue,
		*(volatile u32 *)gcmGetLabelAddress(LABEL_GPUWAIT), g_gpuWaitValue);
	return text;
}

void Step(const char *name) {
	g_step = name;
	g_stepStart = NowSeconds();
	PS3Frontend::Trace("RSX step: %s", name);
}

void StepQuiet(const char *name) {
	g_step = name;
	g_stepStart = NowSeconds();
}

static void GraphicsException(const u32 value) {
	PS3Frontend::Trace("RSX GRAPHICS EXCEPTION 0x%08x during step '%s'; %s", value, (const char *)g_step, GpuStateText().c_str());
}

static void WatchdogThread(void *) {
	const char *reported = nullptr;
	while (g_watchdogRunning) {
		usleep(500 * 1000);
		const char *step = (const char *)g_step;
		const double age = NowSeconds() - g_stepStart;
		if (step && strcmp(step, "idle") != 0 && age > 3.0 && step != reported) {
			PS3Frontend::Trace("WATCHDOG: still in RSX step '%s' after %.1f s; %s", step, age, GpuStateText().c_str());
			reported = step;
		}
	}
}

void InitDiagnostics() {
	if (g_watchdogRunning || !g_context) return;
	InitLabels();
	gcmSetGraphicsHandler(GraphicsException);
	g_watchdogRunning = true;
	sys_ppu_thread_t id;
	if (sysThreadCreate(&id, WatchdogThread, nullptr, 1000, 0x4000, 0, (char *)"rsx_watchdog") != 0) {
		g_watchdogRunning = false;
		PS3Frontend::Trace("RSX watchdog thread could not start");
	} else {
		PS3Frontend::Trace("RSX diagnostics: watchdog and exception handler installed; %s", GpuStateText().c_str());
	}
}

bool Ready() { return g_context != nullptr; }
_gcmCtxData *Context() { return g_context; }

std::string Describe() {
	gcmConfiguration config{};
	gcmGetConfiguration(&config);
	char text[512];
	snprintf(text, sizeof(text), "%u MB video memory, I/O %u MB, core %u MHz, memory %u MHz",
		config.localSize / (1024 * 1024), config.ioSize / (1024 * 1024), config.coreFreq / 1000000, config.memoryFreq / 1000000);
	return text;
}

void *LocalAlloc(uint32_t size, uint32_t align) { return rsxMemalign(align, size); }
void LocalFree(void *ptr) { if (ptr) rsxFree(ptr); }
uint32_t LargestFreeLocal() {
	uint32_t lo = 0, hi = 256u * 1024 * 1024;
	while (hi - lo > 1024 * 1024) {
		const uint32_t mid = lo + (hi - lo) / 2;
		void *p = LocalAlloc(mid, 1024);
		if (p) { LocalFree(p); lo = mid; } else hi = mid;
	}
	return lo;
}

uint32_t LocalOffset(const void *ptr) {
	u32 offset = 0;
	rsxAddressToOffset((void *)ptr, &offset);
	return offset;
}

void *IoAlloc(uint32_t size, uint32_t align) {
	if (!g_io) return nullptr;
	uint32_t start = (g_ioUsed + align - 1) & ~(align - 1);
	if (start + size > IO_REGION_SIZE) return nullptr;
	g_ioUsed = start + size;
	return g_io + start;
}
uint32_t IoOffset(const void *ptr) {
	u32 offset = 0;
	gcmAddressToOffset((void *)ptr, &offset);
	return offset;
}
void IoMark() { g_ioMark = g_ioUsed; }
void IoRelease() { g_ioUsed = g_ioMark; }

void Flush() {
	__sync_synchronize();
	rsxFlushBuffer(g_context);
}

// Label memory starts with whatever was left there (Frontend 07 found 322420947), so fences
// compared against it looked finished before the RSX had done anything and every readback was
// empty. Our labels are zeroed before first use; counting then starts at 1.
static void InitLabels() {
	static bool done = false;
	if (done) return;
	done = true;
	*(volatile u32 *)gcmGetLabelAddress(LABEL_FENCE) = 0;
	*(volatile u32 *)gcmGetLabelAddress(LABEL_GPUWAIT) = 0;
	__sync_synchronize();
	g_fenceValue = 0;
	g_gpuWaitValue = 0;
}

uint32_t Fence() {
	InitLabels();
	++g_fenceValue;
	if (g_fenceValue == 0) g_fenceValue = 1;
	rsxSetWriteBackendLabel(g_context, LABEL_FENCE, g_fenceValue);
	return g_fenceValue;
}

bool FenceDone(uint32_t value) {
	volatile u32 *label = (volatile u32 *)gcmGetLabelAddress(LABEL_FENCE);
	return (int32_t)(*label - value) >= 0;
}

bool WaitFence(uint32_t value, double timeoutSeconds) {
	if (FenceDone(value)) return true;
	Flush();
	double start = NowSeconds();
	while (!FenceDone(value)) {
		if (NowSeconds() - start > timeoutSeconds) {
			PS3Frontend::Trace("RSX fence %u timed out (label %u)", value, *(volatile u32 *)gcmGetLabelAddress(LABEL_FENCE));
			return false;
		}
		usleep(20);
	}
	return true;
}

bool Finish(double timeoutSeconds) {
	return WaitFence(Fence(), timeoutSeconds);
}

void GpuWaitForRendering() {
	InitLabels();
	++g_gpuWaitValue;
	if (g_gpuWaitValue == 0) g_gpuWaitValue = 1;
	rsxSetWriteBackendLabel(g_context, LABEL_GPUWAIT, g_gpuWaitValue);
	rsxSetWaitLabel(g_context, LABEL_GPUWAIT, g_gpuWaitValue);
	rsxSetWaitForIdle(g_context);
}

// ---------------------------------------------------------------------------------------------
// Programs

static rsxVertexProgram *g_vp = nullptr;
static void *g_vpUcode = nullptr;
static uint32_t g_fpOffset[RSX_FRAGMENT_PROGRAM_COUNT];
static void *g_fpPtr[RSX_FRAGMENT_PROGRAM_COUNT];
static bool g_programsLoaded = false;
static int g_boundFragment = -1;

bool LoadPrograms() {
	if (g_programsLoaded) return true;
	g_vp = (rsxVertexProgram *)rsx_vp_main;
	u32 size = 0;
	rsxVertexProgramGetUCode(g_vp, &g_vpUcode, &size);
	if (!g_vpUcode || size == 0) {
		PS3Frontend::Trace("RSX: vertex program has no microcode");
		return false;
	}
	for (int i = 0; i < RSX_FRAGMENT_PROGRAM_COUNT; ++i) {
		g_fpOffset[i] = 0;
		const rsxFragmentProgram *fp = (const rsxFragmentProgram *)rsx_fp_table[i];
		if (!fp) continue;
		void *ucode = nullptr;
		u32 fpSize = 0;
		rsxFragmentProgramGetUCode(fp, &ucode, &fpSize);
		void *copy = LocalAlloc(fpSize, 64);
		if (!copy) {
			PS3Frontend::Trace("RSX: out of local memory for fragment program %d", i);
			return false;
		}
		memcpy(copy, ucode, fpSize);
		g_fpOffset[i] = LocalOffset(copy);
		g_fpPtr[i] = copy;
	}
	g_programsLoaded = true;
	return true;
}

void BindVertexProgram() {
	rsxLoadVertexProgram(g_context, g_vp, g_vpUcode);
	g_boundFragment = -1;
}

int FragmentIndex(bool textured, int texFunc, bool rgba, bool doubling, bool fog, bool blendMul) {
	if (texFunc > 4) texFunc = 4;
	if (!textured) {
		texFunc = 0;
		rgba = false;
	}
	return 1 + ((((textured ? 1 : 0) * 5 + texFunc) * 2 + (rgba ? 1 : 0)) * 2 + (doubling ? 1 : 0)) * 4 + (fog ? 2 : 0) + (blendMul ? 1 : 0);
}

void BindFragmentProgram(int index) {
	if (index < 0 || index >= RSX_FRAGMENT_PROGRAM_COUNT || !rsx_fp_table[index]) index = FRAGMENT_CLEAR;
	if (index == g_boundFragment) return;
	rsxLoadFragmentProgramLocation(g_context, (const rsxFragmentProgram *)rsx_fp_table[index], g_fpOffset[index], GCM_LOCATION_RSX);
	g_boundFragment = index;
}

void SetVertexConstants(const float env[4], const float fog[4], const float mul[4]) {
	float block[12];
	memcpy(block, env, 16);
	memcpy(block + 4, fog, 16);
	memcpy(block + 8, mul, 16);
	rsxLoadVertexProgramParameterBlock(g_context, g_vp->const_start, 3, block);
}

// ---------------------------------------------------------------------------------------------
// Targets and state

void SetTarget(const Target &t) {
	gcmSurface sf{};
	sf.colorFormat = GCM_SURFACE_A8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = t.colorInMain ? GCM_LOCATION_CELL : GCM_LOCATION_RSX;
	sf.colorOffset[0] = t.colorOffset;
	sf.colorPitch[0] = t.colorPitch;
	for (int i = 1; i < 4; ++i) {
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i] = 0;
		sf.colorPitch[i] = 64;
	}
	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = t.hasDepth ? t.depthOffset : 0;
	sf.depthPitch = t.hasDepth ? t.depthPitch : 64;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = t.width;
	sf.height = t.height;
	sf.x = 0;
	sf.y = 0;
	rsxSetSurface(g_context, &sf);
	for (int i = 0; i < 8; ++i)
		rsxSetViewportClip(g_context, i, t.width, t.height);
}

void SetPixelViewport(uint16_t width, uint16_t height) {
	// The renderer sends normalized device coordinates: x_ndc = 2*px/width - 1, y_ndc = 2*py/height - 1,
	// z_ndc = 2*z - 1. This maps them back to pixels with y growing downwards and z in 0..1.
	float scale[4] = {width * 0.5f, height * 0.5f, 0.5f, 0.0f};
	float offset[4] = {width * 0.5f, height * 0.5f, 0.5f, 0.0f};
	rsxSetViewport(g_context, 0, 0, width, height, 0.0f, 1.0f, scale, offset);
	rsxSetScissor(g_context, 0, 0, width, height);
}

void SetScissor(int x, int y, int w, int h) {
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (w < 0) w = 0;
	if (h < 0) h = 0;
	rsxSetScissor(g_context, (u16)x, (u16)y, (u16)w, (u16)h);
}

void ClearTarget(uint32_t argb, bool color, bool depthStencil, uint32_t depthStencilValue) {
	u32 mask = 0;
	if (color) mask |= GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A;
	if (depthStencil) mask |= GCM_CLEAR_Z | GCM_CLEAR_S;
	rsxSetClearColor(g_context, argb);
	rsxSetClearDepthStencil(g_context, depthStencilValue);
	rsxClearSurface(g_context, mask);
}

void SetDefaultState() {
	rsxSetColorMask(g_context, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(g_context, 0);
	rsxSetBlendEnable(g_context, GCM_FALSE);
	rsxSetDepthTestEnable(g_context, GCM_FALSE);
	rsxSetDepthWriteEnable(g_context, GCM_FALSE);
	rsxSetAlphaTestEnable(g_context, GCM_FALSE);
	rsxSetStencilTestEnable(g_context, GCM_FALSE);
	rsxSetCullFaceEnable(g_context, GCM_FALSE);
	rsxSetLogicOpEnable(g_context, GCM_FALSE);
	rsxSetDitherEnable(g_context, GCM_FALSE);
	rsxSetShadeModel(g_context, GCM_SHADE_MODEL_SMOOTH);
	rsxSetFrontFace(g_context, GCM_FRONTFACE_CCW);
	rsxSetZMinMaxControl(g_context, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	rsxSetUserClipPlaneControl(g_context, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
		GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
}

// ---------------------------------------------------------------------------------------------
// Vertex ring: 8 segments of 1 MB in local memory. A segment is reused only after the fence
// written when the renderer last left it has passed.

static constexpr uint32_t RING_SEGMENTS = 8;
static constexpr uint32_t RING_SEGMENT_BYTES = 1024 * 1024;
static constexpr uint32_t RING_SEGMENT_VERTS = RING_SEGMENT_BYTES / sizeof(Vertex);
static Vertex *g_ring = nullptr;
static uint32_t g_ringOffset = 0;
static uint32_t g_ringSegment = 0;
static uint32_t g_ringPos = 0;  // Vertices used in the current segment.
static uint32_t g_ringFence[RING_SEGMENTS]{};
static bool g_ringBound = false;

static void BindRing() {
	const u32 stride = sizeof(Vertex);
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_POS, 0, g_ringOffset + offsetof(Vertex, x), stride, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_COLOR0, 0, g_ringOffset + offsetof(Vertex, c0), stride, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_COLOR1, 0, g_ringOffset + offsetof(Vertex, c1), stride, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_TEX0, 0, g_ringOffset + offsetof(Vertex, s), stride, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(g_context, GCM_VERTEX_ATTRIB_TEX1, 0, g_ringOffset + offsetof(Vertex, fog), stride, 1, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	g_ringBound = true;
}

Vertex *AllocVertices(int count, uint32_t *firstIndex) {
	if (count <= 0 || (uint32_t)count > RING_SEGMENT_VERTS) return nullptr;
	if (!g_ring) {
		g_ring = (Vertex *)LocalAlloc(RING_SEGMENTS * RING_SEGMENT_BYTES, 128);
		if (!g_ring) return nullptr;
		g_ringOffset = LocalOffset(g_ring);
		g_ringSegment = 0;
		g_ringPos = 0;
	}
	if (!g_ringBound) BindRing();
	if (g_ringPos + count > RING_SEGMENT_VERTS) {
		g_ringFence[g_ringSegment] = Fence();
		g_ringSegment = (g_ringSegment + 1) % RING_SEGMENTS;
		g_ringPos = 0;
		if (g_ringFence[g_ringSegment])
			WaitFence(g_ringFence[g_ringSegment]);
	}
	uint32_t first = g_ringSegment * RING_SEGMENT_VERTS + g_ringPos;
	g_ringPos += count;
	*firstIndex = first;
	return g_ring + first;
}

void DrawVertices(uint32_t primType, uint32_t firstIndex, uint32_t count) {
	if (!count) return;
	if (!g_ringBound) BindRing();
	rsxDrawVertexArray(g_context, primType, firstIndex, count);
}

// Re-copies the fragment program microcode into its RSX memory (in case a test overwrote it)
// and forgets all cached bindings, so the next draw starts from a known state.
void ResetDrawState() {
	if (g_programsLoaded) {
		for (int i = 0; i < RSX_FRAGMENT_PROGRAM_COUNT; ++i) {
			const rsxFragmentProgram *fp = (const rsxFragmentProgram *)rsx_fp_table[i];
			if (!fp || !g_fpPtr[i]) continue;
			void *ucode = nullptr;
			u32 fpSize = 0;
			rsxFragmentProgramGetUCode(fp, &ucode, &fpSize);
			memcpy(g_fpPtr[i], ucode, fpSize);
		}
		__sync_synchronize();
	}
	g_boundFragment = -1;
	g_ringBound = false;
	if (g_context) {
		rsxInvalidateVertexCache(g_context);
		rsxInvalidateTextureCache(g_context, GCM_INVALIDATE_TEXTURE);
	}
}

void EndFrame() {
	// Nothing queued per frame beyond what AllocVertices handles; the vertex program binding
	// survives. Re-bind attributes next frame in case another user (self-test) changed them.
	g_ringBound = false;
}

// ---------------------------------------------------------------------------------------------
// Textures

void BindTexture(int unit, uint32_t offset, uint32_t width, uint32_t height, uint32_t pitch,
	bool linearFilter, bool clampS, bool clampT, bool inMain, bool linearMag) {
	gcmTexture tex{};
	tex.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	tex.mipmap = 1;
	tex.dimension = GCM_TEXTURE_DIMS_2D;
	tex.cubemap = GCM_FALSE;
	tex.remap = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
	tex.width = (u16)width;
	tex.height = (u16)height;
	tex.depth = 1;
	tex.location = inMain ? GCM_LOCATION_CELL : GCM_LOCATION_RSX;
	tex.pitch = pitch;
	tex.offset = offset;
	rsxLoadTexture(g_context, (u8)unit, &tex);
	rsxTextureControl(g_context, (u8)unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	// linearFilter selects minification; magnification is linear if either asks for it.
	const u8 minFilter = linearFilter ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST;
	const u8 magFilter = (linearFilter || linearMag) ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST;
	rsxTextureFilter(g_context, (u8)unit, 0, minFilter, magFilter, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(g_context, (u8)unit, clampS ? GCM_TEXTURE_CLAMP_TO_EDGE : GCM_TEXTURE_REPEAT,
		clampT ? GCM_TEXTURE_CLAMP_TO_EDGE : GCM_TEXTURE_REPEAT, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

void InvalidateTextureCache() {
	rsxInvalidateTextureCache(g_context, GCM_INVALIDATE_TEXTURE);
}

// ---------------------------------------------------------------------------------------------
// Transfers

static u8 TransferMode(Mem src, Mem dst) {
	if (src == Mem::Local)
		return dst == Mem::Local ? GCM_TRANSFER_LOCAL_TO_LOCAL : GCM_TRANSFER_LOCAL_TO_MAIN;
	return dst == Mem::Local ? GCM_TRANSFER_MAIN_TO_LOCAL : GCM_TRANSFER_MAIN_TO_MAIN;
}

void ScaleImage(Mem srcMem, uint32_t srcOffset, uint32_t srcPitch, int srcW, int srcH,
	Mem dstMem, uint32_t dstOffset, uint32_t dstPitch, int dstX, int dstY, int dstW, int dstH, bool linear) {
	if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
	rsxSetTransferScaleMode(g_context, TransferMode(srcMem, dstMem), GCM_TRANSFER_SURFACE);
	gcmTransferScale scale{};
	scale.conversion = GCM_TRANSFER_CONVERSION_TRUNCATE;
	scale.format = GCM_TRANSFER_SCALE_FORMAT_A8R8G8B8;
	scale.operation = GCM_TRANSFER_OPERATION_SRCCOPY;
	scale.clipX = (u16)dstX;
	scale.clipY = (u16)dstY;
	scale.clipW = (u16)dstW;
	scale.clipH = (u16)dstH;
	scale.outX = (u16)dstX;
	scale.outY = (u16)dstY;
	scale.outW = (u16)dstW;
	scale.outH = (u16)dstH;
	scale.ratioX = rsxGetFixedSint32((float)srcW / (float)dstW);
	scale.ratioY = rsxGetFixedSint32((float)srcH / (float)dstH);
	scale.inX = 0;
	scale.inY = 0;
	scale.inW = (u16)srcW;
	scale.inH = (u16)srcH;
	scale.offset = srcOffset;
	scale.pitch = (u16)srcPitch;
	scale.origin = linear ? GCM_TRANSFER_ORIGIN_CENTER : GCM_TRANSFER_ORIGIN_CORNER;
	scale.interp = linear ? GCM_TRANSFER_INTERPOLATOR_LINEAR : GCM_TRANSFER_INTERPOLATOR_NEAREST;
	gcmTransferSurface surface{};
	surface.format = GCM_TRANSFER_SURFACE_FORMAT_A8R8G8B8;
	surface.pitch = (u16)dstPitch;
	surface.offset = dstOffset;
	PS3ScaledTransfer(g_context, &scale, &surface);
}

// Room for count words in the command buffer (the library's own wrap-around callback).
static bool Reserve(u32 count) {
	if (g_context->current + count > g_context->end)
		return rsxContextCallback(g_context, count) == 0;
	return true;
}

void CopyRows(Mem srcMem, uint32_t srcOffset, uint32_t srcPitch, Mem dstMem, uint32_t dstOffset, uint32_t dstPitch,
	uint32_t bytesPerRow, uint32_t rows) {
	// Not rsxSetTransferData: PSL1GHT's version compares its unsigned pitch against a negative
	// limit, so it always takes its per-row path, where "while (--rowCount >= 0)" on an unsigned
	// count never ends. It fills the command buffer forever - the Frontend 06 freeze.
	if (!bytesPerRow || !rows) return;
	if (srcPitch == bytesPerRow && dstPitch == bytesPerRow && (bytesPerRow * rows) % 4096 == 0) {
		// Contiguous: copy as 4 KB lines, which keeps pitches inside the engine's limits.
		rows = bytesPerRow * rows / 4096;
		bytesPerRow = srcPitch = dstPitch = 4096;
	}
	if (srcPitch >= 32768 || dstPitch >= 32768) {
		// Too wide for one transfer: one line at a time.
		for (uint32_t i = 0; i < rows; ++i)
			CopyRows(srcMem, srcOffset + i * srcPitch, bytesPerRow, dstMem, dstOffset + i * dstPitch, bytesPerRow, bytesPerRow, 1);
		return;
	}
	if (!Reserve(3)) return;
	u32 *cmd = g_context->current;
	cmd[0] = (2u << 18) | (1u << 13) | NV_MEMORY_TO_MEMORY_FORMAT_DMA_BUFFER_IN;
	cmd[1] = srcMem == Mem::Main ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
	cmd[2] = dstMem == Mem::Main ? GCM_DMA_MEMORY_HOST_BUFFER : GCM_DMA_MEMORY_FRAME_BUFFER;
	g_context->current += 3;
	const uint32_t MAX_ROWS = 0x7FF;
	while (rows > 0) {
		const uint32_t n = rows > MAX_ROWS ? MAX_ROWS : rows;
		if (!Reserve(9)) return;
		cmd = g_context->current;
		cmd[0] = (8u << 18) | (1u << 13) | NV_MEMORY_TO_MEMORY_FORMAT_OFFSET_IN;
		cmd[1] = srcOffset;
		cmd[2] = dstOffset;
		cmd[3] = srcPitch;
		cmd[4] = dstPitch;
		cmd[5] = bytesPerRow;
		cmd[6] = n;
		cmd[7] = (1u << 8) | 1u;  // Byte formats in/out.
		cmd[8] = 0;               // Buffer notify.
		g_context->current += 9;
		srcOffset += n * srcPitch;
		dstOffset += n * dstPitch;
		rows -= n;
	}
}

// ---------------------------------------------------------------------------------------------
// Display

static int g_displayW = 0, g_displayH = 0;
static uint32_t g_displayPitch = 0;
static void *g_displayBuffer[2]{};
static uint32_t g_displayOffset[2]{};
static int g_back = 0;
static bool g_firstFlip = true;
static constexpr int DISPLAY_ID_BASE = 2;

bool InitDisplay() {
	if (g_displayBuffer[0]) return true;
	videoState state{};
	videoResolution res{};
	if (videoGetState(0, 0, &state) != 0 || videoGetResolution(state.displayMode.resolution, &res) != 0) {
		PS3Frontend::Trace("RSX: could not read the video mode");
		return false;
	}
	g_displayW = res.width;
	g_displayH = res.height;
	g_displayPitch = g_displayW * 4;
	for (int i = 0; i < 2; ++i) {
		g_displayBuffer[i] = LocalAlloc(g_displayPitch * g_displayH, 64);
		if (!g_displayBuffer[i]) {
			PS3Frontend::Trace("RSX: out of local memory for display buffers");
			return false;
		}
		memset(g_displayBuffer[i], 0, g_displayPitch * g_displayH);
		g_displayOffset[i] = LocalOffset(g_displayBuffer[i]);
		gcmSetDisplayBuffer(DISPLAY_ID_BASE + i, g_displayOffset[i], g_displayPitch, g_displayW, g_displayH);
	}
	g_back = 0;
	g_firstFlip = true;
	PS3Frontend::Trace("RSX display buffers: %dx%d", g_displayW, g_displayH);
	return true;
}

int DisplayWidth() { return g_displayW; }
int DisplayHeight() { return g_displayH; }
uint32_t BackBufferOffset() { return g_displayOffset[g_back]; }
void *BackBufferPtr() { return g_displayBuffer[g_back]; }
bool WaitFlip(double timeoutSeconds) {
	if (g_firstFlip)
		return true;
	const double start = NowSeconds();
	while (gcmGetFlipStatus() != 0) {
		if (NowSeconds() - start > timeoutSeconds)
			return false;
		usleep(200);
	}
	return true;
}
uint32_t DisplayPitch() { return g_displayPitch; }

void Present() {
	if (!g_firstFlip) {
		// The previous flip must have happened before we queue another; otherwise we'd draw into a
		// buffer that's still on screen. Only waits when we're producing frames faster than vsync.
		double start = NowSeconds();
		while (gcmGetFlipStatus() != 0) {
			if (NowSeconds() - start > 0.5) break;
			usleep(100);
		}
	}
	gcmResetFlipStatus();
	gcmSetFlip(g_context, DISPLAY_ID_BASE + g_back);
	Flush();
	gcmSetWaitFlip(g_context);
	g_back ^= 1;
	g_firstFlip = false;
}

void ResetDisplay() {
	// Let the last flip complete so SDL's own flip logic starts from a clean status.
	double start = NowSeconds();
	while (!g_firstFlip && gcmGetFlipStatus() != 0 && NowSeconds() - start < 0.5)
		usleep(100);
	Finish();
	g_firstFlip = true;
}

}  // namespace RSX
