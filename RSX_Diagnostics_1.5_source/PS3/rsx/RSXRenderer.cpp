// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.
#include <algorithm>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <ppu-types.h>
#include <rsx/rsx.h>

#include "ext/xxhash.h"
#include "Common/Math/math_util.h"
#include "Core/MemMap.h"
#include "Core/HLE/sceDisplay.h"
#include "GPU/GPUState.h"
#include "GPU/ge_constants.h"
#include "GPU/Common/TextureDecoder.h"
#include "GPU/Math3D.h"
#include "GPU/Software/FuncId.h"
#include "GPU/Software/Rasterizer.h"
#include "GPU/Software/RasterizerRegCache.h"
#include "GPU/Software/Sampler.h"
#include "GPU/Software/TransformUnit.h"

#include "PS3/frontend/Frontend.h"
#include "PS3/rsx/RSXCore.h"
#include "PS3/rsx/RSXRenderer.h"
#include "PS3/rsx/RSXSelfTest.h"

using namespace Rasterizer;

namespace {

constexpr int PSP_W = 480, PSP_H = 272;
constexpr uint32_t VRAM_MASK = 0x041FFFFF;
constexpr int MAX_BATCH = 6000;  // Vertices; must stay below one vertex ring segment.
constexpr uint32_t TEXTURE_BUDGET = 96u * 1024 * 1024;

static const uint8_t *g_font = nullptr;

inline int BytesPerPixel(GEBufferFormat fmt) { return fmt == GE_FORMAT_8888 ? 4 : 2; }

uint32_t NormalizeAddr(uint32_t addr) {
	if (Memory::IsVRAMAddress(addr)) return addr & VRAM_MASK;
	return addr & 0x3FFFFFFF;
}

uint16_t CompareFunc(int ge) {
	static const uint16_t table[8] = {GCM_NEVER, GCM_ALWAYS, GCM_EQUAL, GCM_NOTEQUAL, GCM_LESS, GCM_LEQUAL, GCM_GREATER, GCM_GEQUAL};
	return table[ge & 7];
}

uint16_t StencilOp(int ge) {
	switch (ge) {
	case GE_STENCILOP_ZERO: return GCM_ZERO;
	case GE_STENCILOP_REPLACE: return GCM_REPLACE;
	case GE_STENCILOP_INVERT: return GCM_INVERT;
	case GE_STENCILOP_INCR: return GCM_INCR;
	case GE_STENCILOP_DECR: return GCM_DECR;
	default: return GCM_KEEP;
	}
}

// 0xBBGGRR (GE register layout) -> floats.
void ColorToFloats(uint32_t bgr, float alpha, float out[4]) {
	out[0] = (bgr & 0xFF) / 255.0f;
	out[1] = ((bgr >> 8) & 0xFF) / 255.0f;
	out[2] = ((bgr >> 16) & 0xFF) / 255.0f;
	out[3] = alpha;
}

uint32_t BgrToArgb(uint32_t bgr) {
	return 0xFF000000 | ((bgr & 0xFF) << 16) | (bgr & 0xFF00) | ((bgr >> 16) & 0xFF);
}

// Cheap timing with the PPU time base (79.8 MHz on the PS3).
static inline uint64_t TimeBase() {
	uint64_t tb;
	__asm__ volatile("mftb %0" : "=r"(tb));
	return tb;
}
struct TbScope {
	uint64_t &acc;
	uint64_t start;
	explicit TbScope(uint64_t &a) : acc(a), start(TimeBase()) {}
	~TbScope() { acc += TimeBase() - start; }
};

struct Surface {
	uint32_t addr = 0;
	uint16_t stride = 0;
	GEBufferFormat fmt = GE_FORMAT_565;
	void *mem = nullptr;
	uint32_t offset = 0, pitch = 0;
	uint16_t w = 0, h = 0;     // Scaled size of the RSX buffer.
	int rows = PSP_H;          // PSP rows in use (for hashing/sync).
	bool gpuDirty = false;     // Rendered on the RSX since the last write-back to VRAM.
	bool synced = false;       // syncHash valid.
	uint64_t syncHash = 0;     // Hash of VRAM right after the last upload/write-back.
	uint32_t lastFrame = 0;
	uint32_t validatedFrame = 0xFFFFFFFF, validatedGen = 0xFFFFFFFF;  // VRAM checked once per frame.
};

struct DepthSurface {
	uint32_t addr = 0;
	void *mem = nullptr;
	uint32_t offset = 0, pitch = 0;
	uint16_t w = 0, h = 0;
	bool fresh = false;  // Not cleared yet.
};

struct TexEntry {
	uint32_t addr = 0;
	uint8_t fmt = 0;
	uint16_t w = 0, h = 0, bufw = 0;
	uint64_t clutHash = 0;
	uint64_t dataHash = 0;
	uint32_t validatedFrame = 0xFFFFFFFF;
	uint32_t validatedGen = 0xFFFFFFFF;
	void *mem = nullptr;
	uint32_t offset = 0, pitch = 0, bytes = 0;
	uint32_t lastFrame = 0;
	uint16_t uvW = 0, uvH = 0;  // PSP texture size for through-mode UVs (0 = w/h).
};

struct HwState {
	int fragment;
	uint8_t tex, texLinMin, texLinMag, clampS, clampT, pad0;
	uint32_t texOffset, texW, texH, texPitch;
	uint32_t uvW, uvH;
	uint8_t blend, depthTest, depthWrite, alphaTest, stencil, logic, flat, pad1;
	uint16_t bsrc, bdst, beq, depthFunc, alphaFunc, sfunc, sfail, szfail, szpass, logicOp;
	uint32_t blendColor, colorMask;
	uint8_t alphaRef, sref, smask, swmask;
	int16_t sx, sy, sw, sh;
	float env[4], fog[4], mul[4];
};

}  // namespace

class RSXRenderer : public HardwareBackend {
public:
	explicit RSXRenderer(const RSXRendererOptions &o) : opt_(o) {}
	bool Init();
	void Shutdown();

	void Triangle(const RasterizerState &state, const VertexData &v0, const VertexData &v1, const VertexData &v2) override;
	void Rect(const RasterizerState &state, const VertexData &v0, const VertexData &v1, bool clear) override;
	void Line(const RasterizerState &state, const VertexData &v0, const VertexData &v1) override;
	void Point(const RasterizerState &state, const VertexData &v0) override;
	void Flush(const char *reason) override;
	bool DrawTriangles(const RasterizerState &state, const FastVertex *verts, int count, bool through) override;
	bool HasPendingWrite(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) override;

	void Present(const std::string &overlay);
	std::string Stats();

private:
	bool EnsureTarget();
	Surface *FindSurface(uint32_t addr, uint16_t stride, GEBufferFormat fmt, int rows, bool create);
	DepthSurface *FindDepth(uint32_t addr, uint16_t minW, uint16_t minH);
	void Upload(Surface &s);
	void Download(Surface &s);
	void DownloadAll();
	uint64_t HashVram(const Surface &s) const;
	bool Validate(Surface &s);
	void ApplyState(const RasterizerState &state, bool clear);
	bool LookupTexture(const RasterizerState &state, TexEntry **out);
	void FlushBatch();
	void PushVertex(const VertexData &v, bool through, float sScale, float tScale, bool proj);
	void BeginPrim(uint32_t type);
	void DeferFree(void *mem);
	void ProcessDeferredFrees(bool force);
	void *AllocLocal(uint32_t size);
	void EvictTextures(uint32_t frameAge);
	void DrawOverlay(const std::string &text, int x, int y);
	void ComputeGameRect();

	RSXRendererOptions opt_;
	int scale_ = 1;
	// Owned through pointers so Surface* stays valid while the lists change.
	std::vector<std::unique_ptr<Surface>> surfaces_;
	std::vector<std::unique_ptr<DepthSurface>> depths_;
	std::unordered_map<uint64_t, TexEntry> textures_;
	uint32_t textureBytes_ = 0;
	Surface *target_ = nullptr;
	DepthSurface *depth_ = nullptr;
	bool targetValid_ = false;
	uint32_t targetKey_[4]{};
	uint16_t targetW_ = 0, targetH_ = 0;
	HwState cur_{};
	bool stateValid_ = false;
	// ApplyState memo: the same RasterizerState (the binner makes a new one whenever anything in it
	// changes) with the same scissor in the same frame needs no work at all.
	const RasterizerState *memoState_ = nullptr;
	bool memoClear_ = false;
	uint32_t memoFrame_ = 0, memoGen_ = 0, memoScissor_[3]{};
	std::vector<RSX::Vertex> batch_;
	uint32_t batchType_ = GCM_TYPE_TRIANGLES;
	uint32_t *staging_ = nullptr;   // Main memory, 1024x512 ARGB.
	uint32_t *overlayBuf_ = nullptr;
	std::vector<std::pair<uint32_t, void *>> deferred_;
	uint32_t frame_ = 1;
	uint32_t vramGen_ = 1;
	int clearFrames_ = 2;
	int gameX_ = 0, gameY_ = 0, gameW_ = 0, gameH_ = 0;
	RSXCalibration cal_;
	bool identityTex_ = true;
	// Render to texture straight from our RSX copy of a framebuffer.
	TexEntry rtt_;
	void *rttCopy_ = nullptr;   // Copy of the current target when a draw samples it.
	uint32_t rttCopyBytes_ = 0;
	unsigned rttHits_ = 0, slowDecodes_ = 0, batchedDraws_ = 0;
	bool LookupRenderTarget(uint32_t addr, GETextureFormat fmt, int w, int h, int bufw, bool swizzle, TexEntry **out);
	bool FastDecode(const SamplerID &id, const u8 *texptr, int bufw, int w, int h, uint32_t bytes, uint32_t *dst, uint32_t dstPitch);
	// Stats since the last Stats() call.
	unsigned rttOffsetHits_ = 0, overlapLogs_ = 0;
	unsigned uploads_ = 0, downloads_ = 0, texDecodes_ = 0, draws_ = 0, prims_ = 0;
	uint64_t tbBackend_ = 0, tbDecode_ = 0, tbDownload_ = 0, tbUpload_ = 0, tbHash_ = 0;
};

// ---------------------------------------------------------------------------------------------

bool RSXRenderer::Init() {
	RSX::Step("renderer: init");
	scale_ = std::max(1, std::min(opt_.scale, 3));
	if (!RSX::Ready()) {
		PS3Frontend::Trace("RSX renderer: no RSX context");
		return false;
	}
	if (!RSX::LoadPrograms() || !RSX::InitDisplay()) return false;
	RSX::Step("renderer: calibration");
	cal_ = RSXCalibrate();
	RSX::StepQuiet("idle");
	PS3Frontend::Trace("RSX renderer calibration %s", cal_.summary.c_str());
	if (!cal_.ok) {
		// Keep the defaults the self-test expects; the log line says what didn't check out.
		RSXCalibration defaults;
		defaults.summary = cal_.summary;
		cal_ = defaults;
	}
	identityTex_ = cal_.texOrder[0] == 0 && cal_.texOrder[1] == 1 && cal_.texOrder[2] == 2 && cal_.texOrder[3] == 3;
	RSX::IoMark();
	staging_ = (uint32_t *)RSX::IoAlloc(1024 * 512 * 4, 1024);
	overlayBuf_ = (uint32_t *)RSX::IoAlloc(1024 * 24 * 4, 1024);
	if (!staging_ || !overlayBuf_) {
		PS3Frontend::Trace("RSX renderer: no main-memory staging (I/O region not mapped?)");
		return false;
	}
	batch_.reserve(MAX_BATCH + 8);
	ComputeGameRect();
	RSX::Finish();
	PS3Frontend::Trace("RSX renderer ready: %dx internal resolution, VRAM sync %s, output %dx%d at %d,%d",
		scale_, opt_.syncEveryFrame ? "every frame" : "minimal", gameW_, gameH_, gameX_, gameY_);
	return true;
}

void RSXRenderer::Shutdown() {
	FlushBatch();
	if (opt_.syncEveryFrame) DownloadAll();
	RSX::ResetDisplay();
	ProcessDeferredFrees(true);
	for (auto &s : surfaces_) RSX::LocalFree(s->mem);
	for (auto &d : depths_) RSX::LocalFree(d->mem);
	for (auto &t : textures_) RSX::LocalFree(t.second.mem);
	surfaces_.clear();
	depths_.clear();
	textures_.clear();
	RSX::IoRelease();
}

void RSXRenderer::ComputeGameRect() {
	const int w = RSX::DisplayWidth(), h = RSX::DisplayHeight();
	int rw = w, rh = h;
	switch ((PS3Frontend::ScreenMode)opt_.screenMode) {
	case PS3Frontend::ScreenMode::STRETCH:
		break;
	case PS3Frontend::ScreenMode::PIXEL_2X:
		rw = std::min(w, PSP_W * 2); rh = std::min(h, PSP_H * 2);
		break;
	default:
		rh = h; rw = (h * PSP_W + PSP_H / 2) / PSP_H;
		if (rw > w) { rw = w; rh = (w * PSP_H + PSP_W / 2) / PSP_W; }
		if (opt_.screenMode == (int)PS3Frontend::ScreenMode::FIT_90) { rw = rw * 9 / 10; rh = rh * 9 / 10; }
		break;
	}
	gameX_ = (w - rw) / 2; gameY_ = (h - rh) / 2; gameW_ = rw; gameH_ = rh;
}

void *RSXRenderer::AllocLocal(uint32_t size) {
	void *mem = RSX::LocalAlloc(size, 128);
	if (!mem) {
		// Free what we can: old textures, then everything already retired.
		EvictTextures(0);
		ProcessDeferredFrees(true);
		mem = RSX::LocalAlloc(size, 128);
		if (!mem) PS3Frontend::Trace("RSX renderer: out of RSX memory (%u bytes)", size);
	}
	return mem;
}

void RSXRenderer::DeferFree(void *mem) {
	if (mem) deferred_.push_back({RSX::Fence(), mem});
}

void RSXRenderer::ProcessDeferredFrees(bool force) {
	if (force) RSX::Finish();
	size_t kept = 0;
	for (auto &item : deferred_) {
		if (force || RSX::FenceDone(item.first)) RSX::LocalFree(item.second);
		else deferred_[kept++] = item;
	}
	deferred_.resize(kept);
}

void RSXRenderer::EvictTextures(uint32_t frameAge) {
	for (auto it = textures_.begin(); it != textures_.end();) {
		if (frame_ - it->second.lastFrame > frameAge) {
			DeferFree(it->second.mem);
			textureBytes_ -= it->second.bytes;
			it = textures_.erase(it);
		} else {
			++it;
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Framebuffers

uint64_t RSXRenderer::HashVram(const Surface &s) const {
	const uint32_t bytes = s.stride * BytesPerPixel(s.fmt) * s.rows;
	if (!Memory::IsValidRange(s.addr, bytes)) return 0;
	return XXH3_64bits(Memory::GetPointerUnchecked(s.addr), bytes);
}

Surface *RSXRenderer::FindSurface(uint32_t addr, uint16_t stride, GEBufferFormat fmt, int rows, bool create) {
	for (size_t i = 0; i < surfaces_.size(); ++i) {
		Surface &s = *surfaces_[i];
		if (s.addr != addr) continue;
		if (s.stride == stride && s.fmt == fmt) {
			if (rows > s.rows) {
				s.rows = std::min(rows, (int)(s.h / scale_));
				s.synced = false;
			}
			return &s;
		}
		// Same address, different layout: write back what we have, then replace it.
		if (s.gpuDirty) Download(s);
		FlushBatch();
		if (target_ == &s) {
			target_ = nullptr;
			targetValid_ = false;
		}
		DeferFree(s.mem);
		surfaces_[i] = std::move(surfaces_.back());
		surfaces_.pop_back();
		break;
	}
	if (!create) return nullptr;
	std::unique_ptr<Surface> owned(new Surface());
	Surface &s = *owned;
	s.addr = addr;
	s.stride = stride;
	s.fmt = fmt;
	const int width = std::max<int>(1, std::min<int>(stride, 1024));
	s.w = (uint16_t)(width * scale_);
	s.h = (uint16_t)(512 * scale_);
	s.pitch = (s.w * 4 + 63) & ~63;
	s.rows = std::max(1, std::min(rows, 512));
	s.mem = AllocLocal(s.pitch * s.h);
	if (!s.mem) return nullptr;
	s.offset = RSX::LocalOffset(s.mem);
	surfaces_.push_back(std::move(owned));
	PS3Frontend::Trace("RSX renderer: new framebuffer %08x stride %d format %d (%dx%d)", addr, stride, (int)fmt, s.w, s.h);
	return surfaces_.back().get();
}

DepthSurface *RSXRenderer::FindDepth(uint32_t addr, uint16_t minW, uint16_t minH) {
	for (size_t i = 0; i < depths_.size(); ++i) {
		DepthSurface &d = *depths_[i];
		if (d.addr != addr) continue;
		if (d.w >= minW && d.h >= minH) return &d;
		if (depth_ == &d) depth_ = nullptr;
		DeferFree(d.mem);
		depths_[i] = std::move(depths_.back());
		depths_.pop_back();
		break;
	}
	std::unique_ptr<DepthSurface> owned(new DepthSurface());
	DepthSurface &d = *owned;
	d.addr = addr;
	d.w = std::max<uint16_t>(minW, 512 * scale_);
	d.h = std::max<uint16_t>(minH, 512 * scale_);
	d.pitch = (d.w * 4 + 63) & ~63;
	d.mem = AllocLocal(d.pitch * d.h);
	if (!d.mem) return nullptr;
	d.offset = RSX::LocalOffset(d.mem);
	d.fresh = true;
	depths_.push_back(std::move(owned));
	return depths_.back().get();
}

void RSXRenderer::Upload(Surface &s) {
	TbScope tb(tbUpload_);
	RSX::StepQuiet("renderer: upload VRAM to RSX");
	const int bpp = BytesPerPixel(s.fmt);
	const int width = std::min<int>(s.stride, 1024);
	if (!Memory::IsValidRange(s.addr, s.stride * bpp * s.rows)) return;
	FlushBatch();
	// The RSX may still be reading the staging buffer for an earlier transfer.
	RSX::Finish();
	const uint8_t *src = Memory::GetPointerUnchecked(s.addr);
	for (int y = 0; y < s.rows; ++y) {
		const uint8_t *row = src + y * s.stride * bpp;
		uint32_t *out = staging_ + y * 1024;
		switch (s.fmt) {
		case GE_FORMAT_565:
			for (int x = 0; x < width; ++x) {
				uint32_t v = row[x * 2] | (row[x * 2 + 1] << 8);
				uint32_t r = v & 31, g = (v >> 5) & 63, b = (v >> 11) & 31;
				out[x] = 0xFF000000 | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
			}
			break;
		case GE_FORMAT_5551:
			for (int x = 0; x < width; ++x) {
				uint32_t v = row[x * 2] | (row[x * 2 + 1] << 8);
				uint32_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
				out[x] = ((v & 0x8000) ? 0xFF000000 : 0) | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
			}
			break;
		case GE_FORMAT_4444:
			for (int x = 0; x < width; ++x) {
				uint32_t v = row[x * 2] | (row[x * 2 + 1] << 8);
				out[x] = (((v >> 12) & 15) * 17 << 24) | ((v & 15) * 17 << 16) | (((v >> 4) & 15) * 17 << 8) | ((v >> 8) & 15) * 17;
			}
			break;
		default:
			for (int x = 0; x < width; ++x)
				out[x] = (row[x * 4 + 3] << 24) | (row[x * 4] << 16) | (row[x * 4 + 1] << 8) | row[x * 4 + 2];
			break;
		}
	}
	__sync_synchronize();
	RSX::ScaleImage(RSX::Mem::Main, RSX::IoOffset(staging_), 1024 * 4, width, s.rows,
		RSX::Mem::Local, s.offset, s.pitch, 0, 0, width * scale_, s.rows * scale_, false);
	RSX::GpuWaitForRendering();
	s.gpuDirty = false;
	uploads_++;
}

void RSXRenderer::Download(Surface &s) {
	TbScope tb(tbDownload_);
	RSX::StepQuiet("renderer: write back RSX to VRAM");
	const int bpp = BytesPerPixel(s.fmt);
	const int width = std::min<int>(s.stride, 1024);
	if (!Memory::IsValidRange(s.addr, s.stride * bpp * s.rows)) {
		s.gpuDirty = false;
		return;
	}
	FlushBatch();
	RSX::GpuWaitForRendering();
	RSX::ScaleImage(RSX::Mem::Local, s.offset, s.pitch, width * scale_, s.rows * scale_,
		RSX::Mem::Main, RSX::IoOffset(staging_), 1024 * 4, 0, 0, width, s.rows, false);
	if (!RSX::Finish()) {
		PS3Frontend::Trace("RSX renderer: write-back of %08x timed out", s.addr);
		return;
	}
	uint8_t *dst = Memory::GetPointerWriteUnchecked(s.addr);
	for (int y = 0; y < s.rows; ++y) {
		const uint32_t *in = staging_ + y * 1024;
		uint8_t *row = dst + y * s.stride * bpp;
		switch (s.fmt) {
		case GE_FORMAT_565:
			for (int x = 0; x < width; ++x) {
				uint32_t c = in[x];
				uint32_t v = ((c >> 19) & 31) | (((c >> 10) & 63) << 5) | (((c >> 3) & 31) << 11);
				row[x * 2] = (uint8_t)v; row[x * 2 + 1] = (uint8_t)(v >> 8);
			}
			break;
		case GE_FORMAT_5551:
			for (int x = 0; x < width; ++x) {
				uint32_t c = in[x];
				uint32_t v = ((c >> 19) & 31) | (((c >> 11) & 31) << 5) | (((c >> 3) & 31) << 10) | ((c >> 31) << 15);
				row[x * 2] = (uint8_t)v; row[x * 2 + 1] = (uint8_t)(v >> 8);
			}
			break;
		case GE_FORMAT_4444:
			for (int x = 0; x < width; ++x) {
				uint32_t c = in[x];
				uint32_t v = ((c >> 20) & 15) | (((c >> 12) & 15) << 4) | (((c >> 4) & 15) << 8) | ((c >> 28) << 12);
				row[x * 2] = (uint8_t)v; row[x * 2 + 1] = (uint8_t)(v >> 8);
			}
			break;
		default:
			for (int x = 0; x < width; ++x) {
				uint32_t c = in[x];
				row[x * 4] = (uint8_t)(c >> 16); row[x * 4 + 1] = (uint8_t)(c >> 8);
				row[x * 4 + 2] = (uint8_t)c; row[x * 4 + 3] = (uint8_t)(c >> 24);
			}
			break;
		}
	}
	s.gpuDirty = false;
	s.syncHash = HashVram(s);
	s.synced = true;
	vramGen_++;
	downloads_++;
}

void RSXRenderer::DownloadAll() {
	for (auto &s : surfaces_)
		if (s->gpuDirty) Download(*s);
}

// Makes the surface's RSX copy match VRAM if the CPU (or a transfer) changed VRAM since we last
// synced. Returns false if the surface couldn't be used.
bool RSXRenderer::Validate(Surface &s) {
	if (s.gpuDirty) return true;  // Our copy is newer than VRAM.
	// Hashing a framebuffer costs ~0.3 ms on the PPU; once per frame (or after a write-back) is enough.
	if (s.synced && s.validatedFrame == frame_ && s.validatedGen == vramGen_) return true;
	s.validatedFrame = frame_;
	s.validatedGen = vramGen_;
	const uint64_t hash = HashVram(s);
	if (!s.synced || hash != s.syncHash) {
		Upload(s);
		s.syncHash = hash;
		s.synced = true;
	}
	return true;
}

bool RSXRenderer::EnsureTarget() {
	RSX::StepQuiet("renderer: bind framebuffer");
	const uint32_t key[4] = {NormalizeAddr(gstate.getFrameBufAddress()), (uint32_t)gstate.FrameBufStride(),
		(uint32_t)gstate.FrameBufFormat(), NormalizeAddr(gstate.getDepthBufAddress())};
	const int rows = std::max(PSP_H, std::min(511, std::max(gstate.getScissorY2(), std::min(gstate.getRegionY2(), 511))) + 1);
	if (targetValid_ && memcmp(key, targetKey_, sizeof(key)) == 0 && (!target_ || rows <= target_->rows))
		return target_ != nullptr && depth_ != nullptr;
	FlushBatch();
	memcpy(targetKey_, key, sizeof(key));
	targetValid_ = true;
	stateValid_ = false;
	target_ = FindSurface(key[0], (uint16_t)key[1], (GEBufferFormat)key[2], rows, true);
	if (!target_) return false;
	Validate(*target_);
	depth_ = FindDepth(key[3], target_->w, target_->h);
	if (!depth_) { target_ = nullptr; return false; }
	RSX::Target t;
	t.colorOffset = target_->offset;
	t.colorPitch = target_->pitch;
	t.depthOffset = depth_->offset;
	t.depthPitch = depth_->pitch;
	t.width = targetW_ = target_->w;
	t.height = targetH_ = target_->h;
	RSX::SetTarget(t);
	RSX::SetPixelViewport(targetW_, targetH_);
	RSX::SetDefaultState();
	RSX::BindVertexProgram();
	if (depth_->fresh) {
		// Zeroed like fresh PSP VRAM; games clear depth themselves before relying on it.
		RSX::ClearTarget(0, false, true, 0);
		depth_->fresh = false;
	}
	return true;
}

bool RSXRenderer::HasPendingWrite(uint32_t start, uint32_t stride, uint32_t w, uint32_t h) {
	start = NormalizeAddr(start);
	const uint32_t end = start + stride * (h ? h - 1 : 0) + w;
	for (const auto &ptr : surfaces_) {
		const Surface &s = *ptr;
		if (!s.gpuDirty && !(target_ == &s && !batch_.empty())) continue;
		const uint32_t sEnd = s.addr + s.stride * BytesPerPixel(s.fmt) * s.rows;
		if (start < sEnd && end > s.addr) return true;
	}
	return false;
}

void RSXRenderer::Flush(const char *reason) {
	FlushBatch();
	// Reasons where the software GPU is about to read PSP memory the RSX may have drawn into.
	// Texture reads ("tex", "selfrender") are handled by LookupTexture: framebuffers are sampled
	// on the RSX directly, and only other overlapping reads download.
	static const char *const readers[] = {"blockxfer", "blockxfer_wrap", "loadclut", "debug", "output"};
	for (const char *r : readers) {
		if (strcmp(reason, r) == 0) {
			DownloadAll();
			return;
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Textures

bool RSXRenderer::LookupTexture(const RasterizerState &state, TexEntry **out) {
	RSX::StepQuiet("renderer: texture");
	const SamplerID &id = state.samplerID;
	const GETextureFormat fmt = id.TexFmt();
	const uint32_t addr = NormalizeAddr(state.texaddr[0]);
	const int w = id.cached.sizes[0].w, h = id.cached.sizes[0].h;
	const int bufw = state.texbufw[0];
	const u8 *texptr = state.texptr[0];
	if (!texptr || w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
	uint32_t bytes = (uint32_t)(textureBitsPerPixel[fmt] * (uint64_t)bufw * h / 8);
	if (bytes == 0 || !Memory::IsValidRange(addr, bytes)) {
		bytes = Memory::ClampValidSizeAt(addr, bytes);
		if (bytes == 0) return false;
	}

	if (LookupRenderTarget(addr, fmt, w, h, bufw, id.swizzle, out))
		return true;

	// Render to texture in a format we can't sample directly: make sure pixels the RSX drew are in
	// PSP memory before reading them.
	for (auto &ptr : surfaces_) {
		Surface &s = *ptr;
		if (!s.gpuDirty) continue;
		const uint32_t sEnd = s.addr + s.stride * BytesPerPixel(s.fmt) * s.rows;
		if (addr < sEnd && addr + bytes > s.addr) {
			if (overlapLogs_ < 24) {
				overlapLogs_++;
				PS3Frontend::Trace("RSX renderer: texture %08x fmt %d %dx%d bufw %d swz %d overlaps framebuffer %08x fmt %d stride %d rows %d - written back",
					addr, (int)fmt, w, h, bufw, id.swizzle ? 1 : 0, s.addr, (int)s.fmt, s.stride, s.rows);
			}
			Download(s);
		}
	}

	uint64_t clutHash = 0;
	const bool isClut = fmt >= GE_TFMT_CLUT4 && fmt <= GE_TFMT_CLUT32;
	if (isClut && id.cached.clut)
		clutHash = XXH3_64bits(id.cached.clut, 1024);
	const uint64_t key = XXH3_64bits(&addr, 4) ^ ((uint64_t)fmt << 56) ^ ((uint64_t)w << 44) ^ ((uint64_t)h << 32) ^
		((uint64_t)bufw << 16) ^ (clutHash * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)id.swizzle << 63);
	TexEntry &e = textures_[key];
	e.lastFrame = frame_;
	if (e.mem && e.validatedFrame == frame_ && e.validatedGen == vramGen_) {
		*out = &e;
		return true;
	}
	uint64_t dataHash;
	{
		TbScope th(tbHash_);
		dataHash = XXH3_64bits(Memory::GetPointerUnchecked(addr), bytes);
	}
	e.validatedFrame = frame_;
	e.validatedGen = vramGen_;
	if (e.mem && dataHash == e.dataHash && e.addr == addr) {
		*out = &e;
		return true;
	}

	// (Re)decode with the software sampler's own fetch function: every PSP format, swizzle and
	// CLUT mode comes out exactly as the software renderer would read it.
	Sampler::FetchFunc fetch = Sampler::GetFetchFunc(id, nullptr);
	if (!fetch) {
		Sampler::FlushJit();
		fetch = Sampler::GetFetchFunc(id, nullptr);
		if (!fetch) return false;
	}
	TbScope td(tbDecode_);
	const uint32_t pitch = ((uint32_t)w * 4 + 63) & ~63u;
	void *mem = AllocLocal(pitch * h);
	if (!mem) return false;
	uint32_t *dst = (uint32_t *)mem;
	const bool fast = identityTex_ && FastDecode(id, texptr, bufw, w, h, bytes, dst, pitch / 4);
	if (!fast)
		slowDecodes_++;
	for (int y = 0; y < h && !fast; ++y) {
		uint32_t *row = dst + y * (pitch / 4);
		for (int x = 0; x < w; ++x) {
			const uint32_t rgba = Math3D::Vec4<int>(fetch(x, y, texptr, bufw, 0, id)).ToRGBA();
			const uint32_t argb = (rgba & 0xFF000000) | ((rgba & 0xFF) << 16) | (rgba & 0xFF00) | ((rgba >> 16) & 0xFF);
			if (identityTex_) {
				row[x] = argb;
			} else {
				// Output byte j (A,R,G,B from the top) is read from memory byte texOrder[j].
				uint8_t *bytes = (uint8_t *)&row[x];
				for (int j = 0; j < 4; ++j) bytes[cal_.texOrder[j]] = (uint8_t)(argb >> (24 - 8 * j));
			}
		}
	}
	__sync_synchronize();
	if (e.mem) {
		DeferFree(e.mem);
		textureBytes_ -= e.bytes;
	}
	e.mem = mem;
	e.offset = RSX::LocalOffset(mem);
	e.pitch = pitch;
	e.bytes = pitch * h;
	e.addr = addr;
	e.fmt = fmt;
	e.w = (uint16_t)w;
	e.h = (uint16_t)h;
	e.bufw = (uint16_t)bufw;
	e.clutHash = clutHash;
	e.dataHash = dataHash;
	textureBytes_ += e.bytes;
	RSX::InvalidateTextureCache();
	texDecodes_++;
	if (textureBytes_ > TEXTURE_BUDGET) EvictTextures(60);
	*out = &textures_[key];
	return true;
}

// A texture at (or inside) a framebuffer we hold, in the same format and stride: sample our RSX
// copy. A texture starting inside the framebuffer (a sub-rectangle, common for post effects) or
// the framebuffer being drawn to is first copied by the RSX into a separate buffer.
bool RSXRenderer::LookupRenderTarget(uint32_t addr, GETextureFormat fmt, int w, int h, int bufw, bool swizzle, TexEntry **out) {
	if (swizzle || fmt > GE_TFMT_8888)
		return false;
	for (auto &ptr : surfaces_) {
		Surface &s = *ptr;
		if ((int)s.fmt != (int)fmt || s.stride != bufw)
			continue;
		const uint32_t bpp = BytesPerPixel(s.fmt);
		const uint32_t rowBytes = s.stride * bpp;
		if (addr < s.addr || addr >= s.addr + rowBytes * s.rows)
			continue;
		const uint32_t byteOff = addr - s.addr;
		if (byteOff % bpp)
			continue;
		const int y0 = (int)(byteOff / rowBytes);
		const int x0 = (int)((byteOff % rowBytes) / bpp);
		const int surfW = s.w / scale_, surfH = s.h / scale_;
		const int copyW = std::min(w, surfW - x0), copyH = std::min(h, std::min(surfH, (int)s.rows) - y0);
		if (copyW <= 0 || copyH <= 0)
			continue;
		Validate(s);
		uint32_t offset = s.offset;
		uint32_t pitch = s.pitch;
		if (&s == target_ || byteOff != 0) {
			// Copy the part the texture covers.
			const uint32_t bytes = s.pitch * (uint32_t)(h * scale_);
			if (!rttCopy_ || rttCopyBytes_ < bytes) {
				if (rttCopy_) DeferFree(rttCopy_);
				rttCopy_ = AllocLocal(bytes);
				rttCopyBytes_ = rttCopy_ ? bytes : 0;
			}
			if (!rttCopy_)
				return false;
			FlushBatch();
			RSX::GpuWaitForRendering();
			const uint32_t src = s.offset + (uint32_t)(y0 * scale_) * s.pitch + (uint32_t)(x0 * scale_) * 4;
			RSX::ScaleImage(RSX::Mem::Local, src, s.pitch, copyW * scale_, copyH * scale_, RSX::Mem::Local,
				RSX::LocalOffset(rttCopy_), s.pitch, 0, 0, copyW * scale_, copyH * scale_, false);
			RSX::GpuWaitForRendering();
			offset = RSX::LocalOffset(rttCopy_);
			if (byteOff != 0) rttOffsetHits_++;
		} else if (s.gpuDirty) {
			// Rendered earlier: the texture unit must see the finished pixels.
			FlushBatch();
			RSX::GpuWaitForRendering();
		}
		RSX::InvalidateTextureCache();
		rtt_.offset = offset;
		rtt_.pitch = pitch;
		rtt_.w = (uint16_t)(w * scale_);
		rtt_.h = (uint16_t)(h * scale_);
		rtt_.uvW = (uint16_t)w;
		rtt_.uvH = (uint16_t)h;
		rtt_.addr = addr;
		rttHits_++;
		*out = &rtt_;
		return true;
	}
	return false;
}

// Direct decoders for the common formats (the per-texel sampler call is far too slow on the PPU).
// Output: native 0xAARRGGBB words.
static inline uint32_t TexelOffset(uint32_t xbytes, uint32_t y, uint32_t rowBytes, bool swizzle) {
	if (!swizzle)
		return y * rowBytes + xbytes;
	return ((y >> 3) * (rowBytes >> 4) + (xbytes >> 4)) * 128 + (y & 7) * 16 + (xbytes & 15);
}

static inline uint32_t Conv565(uint32_t v) {
	const uint32_t r = v & 31, g = (v >> 5) & 63, b = (v >> 11) & 31;
	return 0xFF000000 | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}
static inline uint32_t Conv5551(uint32_t v) {
	const uint32_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
	return ((v & 0x8000) ? 0xFF000000 : 0) | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2));
}
static inline uint32_t Conv4444(uint32_t v) {
	return (((v >> 12) & 15) * 17 << 24) | ((v & 15) * 17 << 16) | (((v >> 4) & 15) * 17 << 8) | ((v >> 8) & 15) * 17;
}
static inline uint32_t Conv8888(const u8 *p) {
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

bool RSXRenderer::FastDecode(const SamplerID &id, const u8 *src, int bufw, int w, int h, uint32_t bytes, uint32_t *dst, uint32_t dstPitch) {
	const GETextureFormat fmt = id.TexFmt();
	static const int bitsPerTexel[8] = {16, 16, 16, 32, 4, 8, 16, 32};
	if (fmt > GE_TFMT_CLUT32)
		return false;
	const uint32_t rowBytes = (uint32_t)bufw * bitsPerTexel[fmt] / 8;
	if (id.swizzle && (rowBytes & 15))
		return false;
	const bool swz = id.swizzle;

	// Converted palette for the CLUT formats, indexed after the clut shift/mask/offset.
	uint32_t pal[512];
	if (fmt >= GE_TFMT_CLUT4) {
		if (!id.cached.clut)
			return false;
		const int entries = id.ClutFmt() == GE_CMODE_32BIT_ABGR8888 ? 256 : 512;
		for (int i = 0; i < entries; ++i) {
			switch (id.ClutFmt()) {
			case GE_CMODE_16BIT_BGR5650: pal[i] = Conv565(id.cached.clut16[i]); break;
			case GE_CMODE_16BIT_ABGR5551: pal[i] = Conv5551(id.cached.clut16[i]); break;
			case GE_CMODE_16BIT_ABGR4444: pal[i] = Conv4444(id.cached.clut16[i]); break;
			default: pal[i] = Conv8888((const u8 *)&id.cached.clut32[i]); break;
			}
		}
		if (entries == 256)
			memcpy(pal + 256, pal, 256 * 4);
	}
	auto clutIndex = [&](uint32_t index) -> uint32_t {
		// As the software sampler's TransformClutIndex.
		if (id.hasClutShift || id.hasClutMask || id.hasClutOffset) {
			const uint8_t shift = (id.cached.clutFormat >> 2) & 0x1F;
			const uint8_t mask = (id.cached.clutFormat >> 8) & 0xFF;
			const uint16_t offset = ((id.cached.clutFormat >> 16) & 0x1F) << 4;
			const uint16_t offsetMask = id.ClutFmt() == GE_CMODE_32BIT_ABGR8888 ? 0xFF : 0x1FF;
			return (((index >> shift) & mask) | (offset & offsetMask)) & 511;
		}
		return index & 0xFF;
	};
	uint32_t small[256];
	if (fmt == GE_TFMT_CLUT4 || fmt == GE_TFMT_CLUT8) {
		for (int i = 0; i < 256; ++i)
			small[i] = pal[clutIndex(i)];
	}

	for (int y = 0; y < h; ++y) {
		uint32_t *row = dst + y * dstPitch;
		for (int x = 0; x < w; ++x) {
			uint32_t c = 0;
			switch (fmt) {
			case GE_TFMT_5650: case GE_TFMT_5551: case GE_TFMT_4444: {
				const uint32_t o = TexelOffset(x * 2, y, rowBytes, swz);
				if (o + 2 > bytes) break;
				const uint32_t v = src[o] | (src[o + 1] << 8);
				c = fmt == GE_TFMT_5650 ? Conv565(v) : (fmt == GE_TFMT_5551 ? Conv5551(v) : Conv4444(v));
				break;
			}
			case GE_TFMT_8888: {
				const uint32_t o = TexelOffset(x * 4, y, rowBytes, swz);
				if (o + 4 > bytes) break;
				c = Conv8888(src + o);
				break;
			}
			case GE_TFMT_CLUT4: {
				const uint32_t o = TexelOffset(x >> 1, y, rowBytes, swz);
				if (o >= bytes) break;
				c = small[(src[o] >> ((x & 1) * 4)) & 15];
				break;
			}
			case GE_TFMT_CLUT8: {
				const uint32_t o = TexelOffset(x, y, rowBytes, swz);
				if (o >= bytes) break;
				c = small[src[o]];
				break;
			}
			case GE_TFMT_CLUT16: {
				const uint32_t o = TexelOffset(x * 2, y, rowBytes, swz);
				if (o + 2 > bytes) break;
				c = pal[clutIndex(src[o] | (src[o + 1] << 8))];
				break;
			}
			default: {
				const uint32_t o = TexelOffset(x * 4, y, rowBytes, swz);
				if (o + 4 > bytes) break;
				c = pal[clutIndex(src[o] | (src[o + 1] << 8) | (src[o + 2] << 16) | ((uint32_t)src[o + 3] << 24))];
				break;
			}
			}
			row[x] = c;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// State

void RSXRenderer::ApplyState(const RasterizerState &state, bool clear) {
	const uint32_t scissorRegs[3] = {gstate.scissor1, gstate.scissor2, gstate.region2};
	if (stateValid_ && memoState_ == &state && memoClear_ == clear && memoFrame_ == frame_ && memoGen_ == vramGen_ &&
		memcmp(scissorRegs, memoScissor_, sizeof(scissorRegs)) == 0)
		return;
	memoState_ = &state;
	memoClear_ = clear;
	memoFrame_ = frame_;
	memoGen_ = vramGen_;
	memcpy(memoScissor_, scissorRegs, sizeof(scissorRegs));
	const PixelFuncID &p = state.pixelID;
	const SamplerID &sid = state.samplerID;
	HwState s;
	memset(&s, 0, sizeof(s));

	const int scissorX1 = gstate.getScissorX1(), scissorY1 = gstate.getScissorY1();
	const int scissorX2 = std::min(gstate.getScissorX2(), gstate.getRegionX2());
	const int scissorY2 = std::min(gstate.getScissorY2(), gstate.getRegionY2());
	s.sx = (int16_t)(scissorX1 * scale_);
	s.sy = (int16_t)(scissorY1 * scale_);
	s.sw = (int16_t)std::max(0, (scissorX2 - scissorX1 + 1) * scale_);
	s.sh = (int16_t)std::max(0, (scissorY2 - scissorY1 + 1) * scale_);
	s.mul[0] = s.mul[1] = s.mul[2] = s.mul[3] = 1.0f;

	if (clear) {
		s.fragment = RSX::FRAGMENT_CLEAR;
		s.depthTest = 1;
		s.depthFunc = GCM_ALWAYS;
		s.depthWrite = p.DepthClear();
		s.colorMask = (p.ColorClear() ? (GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B) : 0) |
			(p.StencilClear() ? GCM_COLOR_MASK_A : 0);
		if (p.StencilClear()) {
			s.stencil = 1;
			s.sfunc = GCM_ALWAYS;
			s.sref = 0;  // Replaced per draw from the vertex alpha below.
			s.smask = 0xFF;
			s.swmask = 0xFF;
			s.sfail = s.szfail = s.szpass = GCM_REPLACE;
		}
	} else {
		// Texture.
		TexEntry *tex = nullptr;
		bool textured = state.enableTextures && LookupTexture(state, &tex);
		if (textured) {
			s.tex = 1;
			s.texOffset = tex->offset;
			s.texW = tex->w;
			s.texH = tex->h;
			s.texPitch = tex->pitch;
			s.uvW = tex->uvW ? tex->uvW : tex->w;
			s.uvH = tex->uvH ? tex->uvH : tex->h;
			s.texLinMin = state.minFilt;
			s.texLinMag = state.magFilt;
			s.clampS = sid.clampS;
			s.clampT = sid.clampT;
			ColorToFloats(sid.cached.texBlendColor, 1.0f, s.env);
		}

		// Blending.
		bool blendMul = false;
		if (p.alphaBlend) {
			s.blend = 1;
			switch (p.AlphaBlendEq()) {
			case GE_BLENDMODE_MUL_AND_SUBTRACT: s.beq = GCM_FUNC_SUBTRACT; break;
			case GE_BLENDMODE_MUL_AND_SUBTRACT_REVERSE: s.beq = GCM_FUNC_REVERSE_SUBTRACT; break;
			case GE_BLENDMODE_MIN: s.beq = GCM_FUNC_MIN; break;
			case GE_BLENDMODE_MAX: s.beq = GCM_FUNC_MAX; break;
			case GE_BLENDMODE_ABSDIFF: s.beq = GCM_FUNC_SUBTRACT; break;  // No RSX equivalent; closest.
			default: s.beq = GCM_FUNC_ADD; break;
			}
			auto factor = [&](PixelBlendFactor f, bool isSrc) -> uint16_t {
				switch (f) {
				case PixelBlendFactor::OTHERCOLOR: return isSrc ? GCM_DST_COLOR : GCM_SRC_COLOR;
				case PixelBlendFactor::INVOTHERCOLOR: return isSrc ? GCM_ONE_MINUS_DST_COLOR : GCM_ONE_MINUS_SRC_COLOR;
				case PixelBlendFactor::SRCALPHA: case PixelBlendFactor::DOUBLESRCALPHA: return GCM_SRC_ALPHA;
				case PixelBlendFactor::INVSRCALPHA: case PixelBlendFactor::DOUBLEINVSRCALPHA: return GCM_ONE_MINUS_SRC_ALPHA;
				case PixelBlendFactor::DSTALPHA: case PixelBlendFactor::DOUBLEDSTALPHA: return GCM_DST_ALPHA;
				case PixelBlendFactor::INVDSTALPHA: case PixelBlendFactor::DOUBLEINVDSTALPHA: return GCM_ONE_MINUS_DST_ALPHA;
				case PixelBlendFactor::ZERO: return GCM_ZERO;
				case PixelBlendFactor::ONE: return GCM_ONE;
				case PixelBlendFactor::FIX:
				default:
					return GCM_CONSTANT_COLOR;
				}
			};
			const bool minmax = p.AlphaBlendEq() == GE_BLENDMODE_MIN || p.AlphaBlendEq() == GE_BLENDMODE_MAX;
			if (minmax) {
				s.bsrc = GCM_ONE;
				s.bdst = GCM_ONE;
			} else {
				s.bsrc = factor(p.AlphaBlendSrc(), true);
				s.bdst = factor(p.AlphaBlendDst(), false);
				if (p.AlphaBlendSrc() == PixelBlendFactor::FIX) {
					// Exact: multiply the source in the shader, keep the constant color for the other side.
					s.bsrc = GCM_ONE;
					blendMul = true;
					ColorToFloats(gstate.getFixA(), 1.0f, s.mul);
				}
				if (p.AlphaBlendDst() == PixelBlendFactor::FIX)
					s.blendColor = BgrToArgb(gstate.getFixB());
			}
		}

		const bool fog = p.applyFog;
		if (fog) ColorToFloats(p.cached.fogColor, 1.0f, s.fog);
		s.fragment = RSX::FragmentIndex(textured, sid.TexFunc(), sid.useTextureAlpha, sid.useColorDoubling, fog, blendMul);

		// Depth.
		s.depthTest = 1;
		s.depthFunc = CompareFunc(p.DepthTestFunc());
		s.depthWrite = p.depthWrite;

		// Alpha test.
		if (p.AlphaTestFunc() != GE_COMP_ALWAYS) {
			s.alphaTest = 1;
			s.alphaFunc = CompareFunc(p.AlphaTestFunc());
			s.alphaRef = p.alphaTestRef;
		}

		// Stencil (the PSP keeps it in framebuffer alpha; the RSX copy keeps it in the depth buffer).
		if (p.stencilTest) {
			s.stencil = 1;
			s.sfunc = CompareFunc(p.StencilTestFunc());
			s.sref = (uint8_t)gstate.getStencilTestRef();
			s.smask = (uint8_t)gstate.getStencilTestMask();
			s.swmask = (uint8_t)~gstate.getStencilWriteMask();
			s.sfail = StencilOp(p.SFail());
			s.szfail = StencilOp(p.ZFail());
			s.szpass = StencilOp(p.ZPass());
		}

		// Color write mask: a channel is written unless all of its bits are masked.
		const uint32_t mask = gstate.getColorMask();
		s.colorMask = ((mask & 0x0000FF) != 0x0000FF ? GCM_COLOR_MASK_R : 0) |
			((mask & 0x00FF00) != 0x00FF00 ? GCM_COLOR_MASK_G : 0) |
			((mask & 0xFF0000) != 0xFF0000 ? GCM_COLOR_MASK_B : 0) |
			(((mask >> 24) != 0xFF && gstate.FrameBufFormat() != GE_FORMAT_565) ? GCM_COLOR_MASK_A : 0);

		if (p.applyLogicOp) {
			s.logic = 1;
			s.logicOp = (uint16_t)(0x1500 + (gstate.getLogicOp() & 15));
		}
		s.flat = !state.shadeGouraud;
	}

	if (stateValid_ && memcmp(&s, &cur_, sizeof(s)) == 0) return;
	FlushBatch();
	gcmContextData *ctx = RSX::Context();
	const bool full = !stateValid_;
	auto changed = [&](const void *a, const void *b, size_t n) { return full || memcmp(a, b, n) != 0; };

	if (changed(&s.fragment, &cur_.fragment, sizeof(s.fragment)))
		RSX::BindFragmentProgram(s.fragment);
	if (s.tex && (full || !cur_.tex || s.texOffset != cur_.texOffset || s.texW != cur_.texW || s.texH != cur_.texH ||
			s.texPitch != cur_.texPitch || s.texLinMin != cur_.texLinMin || s.texLinMag != cur_.texLinMag ||
			s.clampS != cur_.clampS || s.clampT != cur_.clampT))
		RSX::BindTexture(0, s.texOffset, s.texW, s.texH, s.texPitch, s.texLinMin, s.clampS, s.clampT, false, s.texLinMag);
	if (changed(s.env, cur_.env, sizeof(float) * 12))
		RSX::SetVertexConstants(s.env, s.fog, s.mul);
	if (changed(&s.blend, &cur_.blend, 1) || changed(&s.bsrc, &cur_.bsrc, 6) || changed(&s.blendColor, &cur_.blendColor, 4)) {
		rsxSetBlendEnable(ctx, s.blend);
		if (s.blend) {
			rsxSetBlendFunc(ctx, s.bsrc, s.bdst, GCM_ONE, GCM_ZERO);
			rsxSetBlendEquation(ctx, s.beq, GCM_FUNC_ADD);
			rsxSetBlendColor(ctx, s.blendColor, 0);
		}
	}
	if (changed(&s.depthTest, &cur_.depthTest, 1) || changed(&s.depthFunc, &cur_.depthFunc, 2) || changed(&s.depthWrite, &cur_.depthWrite, 1)) {
		rsxSetDepthTestEnable(ctx, s.depthTest);
		rsxSetDepthFunc(ctx, s.depthFunc);
		rsxSetDepthWriteEnable(ctx, s.depthWrite);
	}
	if (changed(&s.alphaTest, &cur_.alphaTest, 1) || changed(&s.alphaFunc, &cur_.alphaFunc, 2) || changed(&s.alphaRef, &cur_.alphaRef, 1)) {
		rsxSetAlphaTestEnable(ctx, s.alphaTest);
		if (s.alphaTest) rsxSetAlphaFunc(ctx, s.alphaFunc, s.alphaRef);
	}
	if (changed(&s.stencil, &cur_.stencil, 1) || changed(&s.sfunc, &cur_.sfunc, 2) || changed(&s.sfail, &cur_.sfail, 6) ||
		changed(&s.sref, &cur_.sref, 3)) {
		rsxSetStencilTestEnable(ctx, s.stencil);
		if (s.stencil) {
			rsxSetStencilFunc(ctx, s.sfunc, s.sref, s.smask);
			rsxSetStencilMask(ctx, s.swmask);
			rsxSetStencilOp(ctx, s.sfail, s.szfail, s.szpass);
		}
	}
	if (changed(&s.colorMask, &cur_.colorMask, 4))
		rsxSetColorMask(ctx, s.colorMask);
	if (changed(&s.logic, &cur_.logic, 1) || changed(&s.logicOp, &cur_.logicOp, 2)) {
		rsxSetLogicOpEnable(ctx, s.logic);
		if (s.logic) rsxSetLogicOp(ctx, s.logicOp);
	}
	if (changed(&s.flat, &cur_.flat, 1))
		rsxSetShadeModel(ctx, s.flat ? GCM_SHADE_MODEL_FLAT : GCM_SHADE_MODEL_SMOOTH);
	if (changed(&s.sx, &cur_.sx, 8))
		RSX::SetScissor(s.sx, s.sy, s.sw, s.sh);
	cur_ = s;
	stateValid_ = true;
}

// ---------------------------------------------------------------------------------------------
// Primitives

void RSXRenderer::BeginPrim(uint32_t type) {
	if (type != batchType_ || batch_.size() + 6 > MAX_BATCH) {
		FlushBatch();
		batchType_ = type;
	}
}

void RSXRenderer::FlushBatch() {
	RSX::StepQuiet("renderer: draw batch");
	if (batch_.empty()) return;
	uint32_t first = 0;
	RSX::Vertex *dst = RSX::AllocVertices((int)batch_.size(), &first);
	if (dst) {
		memcpy(dst, batch_.data(), batch_.size() * sizeof(RSX::Vertex));
		__sync_synchronize();
		RSX::DrawVertices(batchType_, first, (uint32_t)batch_.size());
		draws_++;
	}
	batch_.clear();
}

void RSXRenderer::PushVertex(const VertexData &v, bool through, float sScale, float tScale, bool proj) {
	RSX::Vertex o;
	const float w = through || !(v.clipw > 0.0f) ? 1.0f : v.clipw;
	const float px = v.screenpos.x * (1.0f / SCREEN_SCALE_FACTOR) * scale_;
	const float py = v.screenpos.y * (1.0f / SCREEN_SCALE_FACTOR) * scale_;
	o.x = (2.0f * px / targetW_ - 1.0f) * w;
	o.y = (cal_.flipY ? 1.0f - 2.0f * py / targetH_ : 2.0f * py / targetH_ - 1.0f) * w;
	o.z = (2.0f * (v.screenpos.z * (1.0f / 65535.0f)) - 1.0f) * w;
	o.w = w;
	const uint32_t c0 = v.color0, c1 = v.color1 & 0x00FFFFFF;
	for (int i = 0; i < 4; ++i) {
		// color0/color1 hold R in the low byte; colorOrder says which channel each byte must carry.
		o.c0[i] = (uint8_t)(c0 >> (8 * cal_.colorOrder[i]));
		o.c1[i] = (uint8_t)(c1 >> (8 * cal_.colorOrder[i]));
	}
	o.s = v.texturecoords.s() * sScale;
	o.t = v.texturecoords.t() * tScale;
	o.r = 0.0f;
	o.q = proj ? v.texturecoords.q() : 1.0f;
	o.fog = v.fogdepth;
	o.pad = 0.0f;
	batch_.push_back(o);
}

void RSXRenderer::Triangle(const RasterizerState &state, const VertexData &v0, const VertexData &v1, const VertexData &v2) {
	TbScope tb(tbBackend_);
	if (!EnsureTarget()) return;
	ApplyState(state, false);
	BeginPrim(GCM_TYPE_TRIANGLES);
	const bool through = state.throughMode;
	const float ss = through && cur_.tex ? 1.0f / cur_.uvW : 1.0f;
	const float ts = through && cur_.tex ? 1.0f / cur_.uvH : 1.0f;
	const bool proj = state.textureProj && !through;
	PushVertex(v0, through, ss, ts, proj);
	PushVertex(v1, through, ss, ts, proj);
	PushVertex(v2, through, ss, ts, proj);
	target_->gpuDirty = true;
	prims_++;
}

// Batched path: PSP screen space (homogeneous) -> our NDC, same mapping as PushVertex.
bool RSXRenderer::DrawTriangles(const RasterizerState &state, const FastVertex *verts, int count, bool through) {
	if (!opt_.batched) return false;
	TbScope tb(tbBackend_);
	if (!EnsureTarget()) return true;  // Nothing to draw into; like the regular path, drop it.
	ApplyState(state, false);
	const float ss = through && cur_.tex ? 1.0f / cur_.uvW : 1.0f;
	const float ts = through && cur_.tex ? 1.0f / cur_.uvH : 1.0f;
	const bool proj = state.textureProj && !through;
	const float kx = 2.0f * scale_ / targetW_, ky = 2.0f * scale_ / targetH_, kz = 2.0f / 65535.0f;
	for (int i = 0; i + 2 < count; i += 3) {
		BeginPrim(GCM_TYPE_TRIANGLES);
		for (int j = 0; j < 3; ++j) {
			const FastVertex &v = verts[i + j];
			RSX::Vertex o;
			o.x = kx * v.x - v.w;
			o.y = cal_.flipY ? v.w - ky * v.y : ky * v.y - v.w;
			o.z = kz * v.z - v.w;
			o.w = v.w;
			const uint32_t c0 = v.color0, c1 = v.color1 & 0x00FFFFFF;
			for (int k = 0; k < 4; ++k) {
				o.c0[k] = (uint8_t)(c0 >> (8 * cal_.colorOrder[k]));
				o.c1[k] = (uint8_t)(c1 >> (8 * cal_.colorOrder[k]));
			}
			o.s = v.s * ss;
			o.t = v.t * ts;
			o.r = 0.0f;
			o.q = proj ? v.q : 1.0f;
			o.fog = v.fog;
			o.pad = 0.0f;
			batch_.push_back(o);
		}
		prims_++;
	}
	target_->gpuDirty = true;
	batchedDraws_++;
	return true;
}

void RSXRenderer::Rect(const RasterizerState &state, const VertexData &v0, const VertexData &v1, bool clear) {
	TbScope tb(tbBackend_);
	if (!EnsureTarget()) return;
	ApplyState(state, clear);
	if (clear && cur_.stencil) {
		// Clear mode writes the clear color's alpha as the stencil value.
		const uint8_t ref = (uint8_t)(v1.color0 >> 24);
		if (ref != cur_.sref) {
			FlushBatch();
			rsxSetStencilFunc(RSX::Context(), GCM_ALWAYS, ref, 0xFF);
			cur_.sref = ref;
		}
	}
	BeginPrim(GCM_TYPE_TRIANGLES);
	const bool through = state.throughMode;
	const float ss = !clear && through && cur_.tex ? 1.0f / cur_.uvW : 1.0f;
	const float ts = !clear && through && cur_.tex ? 1.0f / cur_.uvH : 1.0f;
	// Corners take position and texture coordinates from both vertices; everything else from v1.
	VertexData c[4] = {v1, v1, v1, v1};
	c[0].screenpos.x = v0.screenpos.x; c[0].screenpos.y = v0.screenpos.y;
	c[0].texturecoords = Vec3Packedf(v0.texturecoords.s(), v0.texturecoords.t(), 1.0f);
	c[1].screenpos.x = v1.screenpos.x; c[1].screenpos.y = v0.screenpos.y;
	c[1].texturecoords = Vec3Packedf(v1.texturecoords.s(), v0.texturecoords.t(), 1.0f);
	c[2].screenpos.x = v0.screenpos.x; c[2].screenpos.y = v1.screenpos.y;
	c[2].texturecoords = Vec3Packedf(v0.texturecoords.s(), v1.texturecoords.t(), 1.0f);
	c[3].texturecoords = Vec3Packedf(v1.texturecoords.s(), v1.texturecoords.t(), 1.0f);
	const int order[6] = {0, 1, 2, 1, 3, 2};
	for (int i : order)
		PushVertex(c[i], true, ss, ts, false);
	target_->gpuDirty = true;
	prims_++;
}

void RSXRenderer::Line(const RasterizerState &state, const VertexData &v0, const VertexData &v1) {
	TbScope tb(tbBackend_);
	if (!EnsureTarget()) return;
	ApplyState(state, false);
	BeginPrim(GCM_TYPE_LINES);
	const bool through = state.throughMode;
	const float ss = through && cur_.tex ? 1.0f / cur_.uvW : 1.0f;
	const float ts = through && cur_.tex ? 1.0f / cur_.uvH : 1.0f;
	PushVertex(v0, through, ss, ts, false);
	PushVertex(v1, through, ss, ts, false);
	target_->gpuDirty = true;
	prims_++;
}

void RSXRenderer::Point(const RasterizerState &state, const VertexData &v0) {
	TbScope tb(tbBackend_);
	if (!EnsureTarget()) return;
	ApplyState(state, false);
	if (batchType_ != GCM_TYPE_POINTS) {
		FlushBatch();
		rsxSetPointSize(RSX::Context(), (float)scale_);
	}
	BeginPrim(GCM_TYPE_POINTS);
	const bool through = state.throughMode;
	const float ss = through && cur_.tex ? 1.0f / cur_.uvW : 1.0f;
	const float ts = through && cur_.tex ? 1.0f / cur_.uvH : 1.0f;
	PushVertex(v0, through, ss, ts, false);
	target_->gpuDirty = true;
	prims_++;
}

// ---------------------------------------------------------------------------------------------
// Presentation

void RSXRenderer::DrawOverlay(const std::string &text, int x, int y) {
	if (!g_font || text.empty()) return;
	const int chars = std::min<int>((int)text.size(), 1024 / 12 - 1);
	const int w = chars * 12 + 8, h = 24;
	// The RSX may still be copying the previous frame's overlay; one frame of latency is plenty.
	for (int row = 0; row < h; ++row) {
		uint32_t *out = overlayBuf_ + row * 1024;
		for (int i = 0; i < w; ++i) out[i] = 0xC0101820;
	}
	for (int i = 0; i < chars; ++i) {
		unsigned c = (unsigned char)text[i];
		if (c < 32 || c > 126) c = '?';
		c -= 32;
		const int gx = (c % 16) * 12, gy = (c / 16) * 24;
		for (int row = 0; row < 24; ++row) {
			const uint8_t *glyph = g_font + (gy + row) * 192 + gx;
			uint32_t *out = overlayBuf_ + row * 1024 + 4 + i * 12;
			for (int col = 0; col < 12; ++col)
				if (glyph[col]) out[col] = 0xFFFFFFFF;
		}
	}
	__sync_synchronize();
	RSX::ScaleImage(RSX::Mem::Main, RSX::IoOffset(overlayBuf_), 1024 * 4, w, h,
		RSX::Mem::Local, RSX::BackBufferOffset(), RSX::DisplayPitch(), x, y, w, h, false);
}

void RSXRenderer::Present(const std::string &overlay) {
	RSX::StepQuiet("renderer: present");
	FlushBatch();
	PSPPointer<u8> topaddr;
	u32 linesize = 512;
	GEBufferFormat fmt = GE_FORMAT_565;
	__DisplayGetFramebuf(&topaddr, &linesize, &fmt, 0);
	Surface *s = nullptr;
	if (topaddr.ptr && linesize > 0 && linesize <= 1024)
		s = FindSurface(NormalizeAddr(topaddr.ptr), (uint16_t)linesize, fmt, PSP_H, true);
	if (s) {
		Validate(*s);
		s->lastFrame = frame_;
	}

	RSX::GpuWaitForRendering();
	RSX::Target t;
	t.colorOffset = RSX::BackBufferOffset();
	t.colorPitch = RSX::DisplayPitch();
	t.width = (uint16_t)RSX::DisplayWidth();
	t.height = (uint16_t)RSX::DisplayHeight();
	t.hasDepth = false;
	RSX::SetTarget(t);
	RSX::SetPixelViewport(t.width, t.height);
	RSX::SetDefaultState();
	if (clearFrames_ > 0) {
		RSX::ClearTarget(0xFF000000, true, false);
		clearFrames_--;
		RSX::GpuWaitForRendering();
	}
	if (s) {
		const int srcW = std::min(PSP_W, (int)s->stride) * scale_;
		RSX::ScaleImage(RSX::Mem::Local, s->offset, s->pitch, srcW, PSP_H * scale_,
			RSX::Mem::Local, RSX::BackBufferOffset(), RSX::DisplayPitch(), gameX_, gameY_, gameW_, gameH_, opt_.bilinear);
	}
	DrawOverlay(overlay, gameX_ + 8, gameY_ + 6);
	RSX::StepQuiet("renderer: flip");
	RSX::Present();
	RSX::EndFrame();

	if (opt_.syncEveryFrame) DownloadAll();

	ProcessDeferredFrees(false);
	if ((frame_ & 255) == 0) EvictTextures(600);
	// Retired framebuffers nobody drew into or displayed for a long time.
	for (size_t i = 0; i < surfaces_.size();) {
		Surface &old = *surfaces_[i];
		if (!old.gpuDirty && &old != target_ && frame_ - old.lastFrame > 1200) {
			DeferFree(old.mem);
			surfaces_[i] = std::move(surfaces_.back());
			surfaces_.pop_back();
		} else {
			++i;
		}
	}
	if (target_) target_->lastFrame = frame_;
	frame_++;
	// Next frame starts by checking VRAM against our copies again.
	targetValid_ = false;
	stateValid_ = false;
	RSX::StepQuiet("idle");
}

std::string RSXRenderer::Stats() {
	char text[256];
	snprintf(text, sizeof(text), "RSX %ux: %u prims, %u draws, %u tex decodes (%u slow), %u fb-as-texture (%u inside), %u batched draws, %u up, %u down, %u fb, %u tex (%u MB)",
		scale_, prims_, draws_, texDecodes_, slowDecodes_, rttHits_, rttOffsetHits_, batchedDraws_, uploads_, downloads_, (unsigned)surfaces_.size(), (unsigned)textures_.size(),
		textureBytes_ / (1024 * 1024));
	const double ms = 1000.0 / 79800000.0;
	char times[160];
	snprintf(times, sizeof(times), "; time in renderer %.0f ms (texture decode %.0f, texture hashing %.0f, write-back %.0f, upload %.0f)",
		tbBackend_ * ms, tbDecode_ * ms, tbHash_ * ms, tbDownload_ * ms, tbUpload_ * ms);
	tbBackend_ = tbDecode_ = tbDownload_ = tbUpload_ = tbHash_ = 0;
	prims_ = draws_ = texDecodes_ = uploads_ = downloads_ = rttHits_ = rttOffsetHits_ = slowDecodes_ = batchedDraws_ = 0;
	return std::string(text) + times;
	return text;
}

// ---------------------------------------------------------------------------------------------

RSXRenderer *RSXRendererCreate(const RSXRendererOptions &options) {
	RSXRenderer *r = new RSXRenderer(options);
	if (!r->Init()) {
		delete r;
		return nullptr;
	}
	return r;
}

void RSXRendererDestroy(RSXRenderer *renderer) {
	if (!renderer) return;
	renderer->Shutdown();
	delete renderer;
}

void RSXRendererPresent(RSXRenderer *renderer, const std::string &overlay) {
	if (renderer) renderer->Present(overlay);
}

std::string RSXRendererStats(RSXRenderer *renderer) {
	return renderer ? renderer->Stats() : std::string();
}

Rasterizer::HardwareBackend *RSXRendererBackend(RSXRenderer *renderer) {
	return renderer;
}

void RSXRendererSetFont(const uint8_t *atlas) {
	g_font = atlas;
}
