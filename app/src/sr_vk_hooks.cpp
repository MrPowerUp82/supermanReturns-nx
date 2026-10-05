// Hooks on the game's statically linked XDK Direct3D functions for the native Vulkan renderer.
// Adapted from superman_returns_recomp port/src/native_renderer/native_hooks.cpp (same XDK
// 2.0.3529 addresses, confirmed there against the game's disassembly and a running build).
//
// Every hook calls the recompiled original first, exactly once, because the original flushes the
// draw's dirty state into the command segment that the front end then copies. Without an active
// front end (sr_renderer=xenos, or the black-clear milestone) each hook only calls the original.
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstring>

#include "generated/default/superman_returns_init.h"
#include "pcvk/native_renderer/frontend.h"
#include "pcvk/native_renderer/shader_objects.h"
#include "sr_vk_runtime.h"

namespace {

namespace native = superman_returns::native;

inline uint32_t GuestLoad32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

// DrawVerticesUP calls BeginVertices/EndVertices itself; those inner calls must not be captured
// as separate draws.
thread_local bool t_in_draw_up = false;

// Inline-vertex draw opened by BeginVertices and consumed by EndVertices (per guest thread).
struct PendingInlineDraw {
  bool active = false;
  uint32_t prim = 0, count = 0, stride = 0, data = 0;
};
thread_local PendingInlineDraw t_pending_inline;

std::atomic<uint64_t> g_swap_number{0};

inline void NoteDevice(native::Frontend* frontend, uint32_t device) {
  if (frontend) frontend->NoteGuestDevice(device);
}

// D3DVertexBuffer_Unlock / D3DIndexBuffer_Unlock (r3 = buffer object): the guest rewrote it.
void InvalidateBufferObject(native::Frontend* frontend, uint8_t* base, uint32_t object) {
  if (!frontend || !object) return;
  const uint32_t address = GuestLoad32(base, object + 0x18) & ~3u;
  const uint32_t size = GuestLoad32(base, object + 0x1C) & 0x00FFFFFFu;
  frontend->InvalidateGuestRange(address, size ? size : 0x10000);
}

// The XDK's shader creators receive the finished container (virtualSize/physicalSize set) in
// r3 and return the new object in r3. The container is captured here, before Direct3D patches
// the object's copy at bind time.
void ShaderCreated(uint32_t object, uint32_t source, bool vertex) {
  auto* access = sr::vk::ActiveGuestAccess();
  if (!access || !object) return;
  sr::vk::ShaderObjectRegistry().Remember(object, native::CaptureShaderContainer(*access, source, vertex));
}

}  // namespace

// ---- Draws -------------------------------------------------------------------

// D3DDevice_DrawVertices(dev, PrimType, StartVertex, VertexCount)
REX_HOOK_RAW(sub_820FBBF8) {
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  auto* frontend = sr::vk::ActiveFrontend();
  NoteDevice(frontend, dev);
  __imp__sub_820FBBF8(ctx, base);
  if (frontend) frontend->DrawVertices(base, prim, start, count);
}

// D3DDevice_DrawIndexedVertices(dev, PrimType, BaseVertexIndex, StartIndex, IndexCount)
REX_HOOK_RAW(sub_820FC000) {
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, base_vertex = ctx.r5.u32, start = ctx.r6.u32,
                 count = ctx.r7.u32;
  auto* frontend = sr::vk::ActiveFrontend();
  NoteDevice(frontend, dev);
  __imp__sub_820FC000(ctx, base);
  if (frontend) frontend->DrawIndexedVertices(base, prim, int32_t(base_vertex), start, count);
}

// D3DDevice_DrawVerticesUP(dev, PrimType, VertexCount, pVertexData, Stride): Begin/End based.
REX_HOOK_RAW(sub_820FBBB0) {
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, count = ctx.r5.u32, data = ctx.r6.u32,
                 stride = ctx.r7.u32;
  auto* frontend = sr::vk::ActiveFrontend();
  NoteDevice(frontend, dev);
  const bool previous = t_in_draw_up;
  t_in_draw_up = true;
  __imp__sub_820FBBB0(ctx, base);
  t_in_draw_up = previous;
  if (frontend) frontend->DrawInlineVertices(base, prim, data, count, stride);
}

// D3DDevice_BeginVertices(dev, PrimType, VertexCount, Stride) -> r3 = where the game writes the
// inline vertices (inside the command segment).
REX_HOOK_RAW(sub_820FB6E8) {
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  auto* frontend = sr::vk::ActiveFrontend();
  if (!t_in_draw_up) NoteDevice(frontend, dev);
  __imp__sub_820FB6E8(ctx, base);
  if (frontend && !t_in_draw_up) t_pending_inline = {true, prim, count, stride, ctx.r3.u32};
}

// D3DDevice_EndVertices(dev): the inline vertex data is complete here.
REX_HOOK_RAW(sub_820FBBA0) {
  auto* frontend = sr::vk::ActiveFrontend();
  if (frontend && t_pending_inline.active) {
    const PendingInlineDraw draw = t_pending_inline;
    t_pending_inline = {};
    // Captured before the original: it closes the block and may recycle the data.
    frontend->DrawInlineVertices(base, draw.prim, draw.data, draw.count, draw.stride);
  }
  __imp__sub_820FBBA0(ctx, base);
}

// ---- Resolve, tiling, clear ---------------------------------------------------

// D3DDevice_Resolve(dev, Flags, pSrcRect, pDestTexture, pDestPoint, Level, Slice, pClearColor,
// ClearZ, ClearStencil, pParameters)
REX_HOOK_RAW(sub_8210C5F8) {
  const uint32_t dev = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, dest = ctx.r6.u32,
                 point = ctx.r7.u32, level = ctx.r8.u32, slice = ctx.r9.u32;
  const uint32_t clear_color = (flags & 0x100) ? ctx.r10.u32 : 0;
  const float clear_z = float(ctx.f1.f64);
  // The original reads ClearStencil at entry-SP+92 (new SP+460 after its 368-byte prologue).
  const uint32_t clear_stencil = (flags & 0x200) ? GuestLoad32(base, ctx.r1.u32 + 92) : 0;
  auto* frontend = sr::vk::ActiveFrontend();
  NoteDevice(frontend, dev);
  // The original first: its RB_COPY_* registers land in the command stream the front end copies.
  __imp__sub_8210C5F8(ctx, base);
  if (frontend)
    frontend->Resolve(base, flags, rect, dest, point, clear_color, clear_z, clear_stencil, level, slice);
}

// D3DDevice_BeginTiling(dev, Flags, Count, pTileRects, pClearColor, ClearZ, ClearStencil)
REX_HOOK_RAW(sub_8210D588) {
  const uint32_t dev = ctx.r3.u32, count = ctx.r5.u32, rects = ctx.r6.u32, color = ctx.r7.u32,
                 stencil = ctx.r8.u32;
  const float clear_z = float(ctx.f1.f64);
  auto* frontend = sr::vk::ActiveFrontend();
  NoteDevice(frontend, dev);
  if (frontend) frontend->BeginTiling(base, count, rects, color, clear_z, stencil);
  __imp__sub_8210D588(ctx, base);
}

// D3DDevice_EndTiling: its per-tile resolves go through the Resolve hook.
REX_HOOK_RAW(sub_8210DA98) {
  auto* frontend = sr::vk::ActiveFrontend();
  NoteDevice(frontend, ctx.r3.u32);
  __imp__sub_8210DA98(ctx, base);
  if (frontend) frontend->EndTiling(base);
}

// The shared float4 clear entry also catches direct engine calls that bypass the public
// D3DCOLOR wrapper. A null rectangle clears the whole surface.
REX_HOOK_RAW(sub_82101A58) {
  const uint32_t dev = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, color_ptr = ctx.r6.u32,
                 stencil = ctx.r8.u32;
  const float z = float(ctx.f1.f64);
  auto* frontend = sr::vk::ActiveFrontend();
  float color[4] = {};
  if (frontend && color_ptr && (flags & 0xF)) {
    if (const uint8_t* source = sr::vk::ActiveGuestAccess()->Readable(color_ptr, 16)) {
      for (uint32_t i = 0; i < 4; ++i) {
        uint32_t bits;
        std::memcpy(&bits, source + 4 * i, 4);
        bits = __builtin_bswap32(bits);
        std::memcpy(&color[i], &bits, 4);
      }
    }
  }
  NoteDevice(frontend, dev);
  __imp__sub_82101A58(ctx, base);
  if (frontend) frontend->Clear(base, rect ? 1u : 0u, rect, flags, color, z, stencil);
}

// ---- Command segment ----------------------------------------------------------

// XDK command segment switch (segment full / kickoff): the front end copies the tail of the old
// segment and resynchronizes on the new one.
REX_HOOK_RAW(sub_820FD8C0) {
  const uint32_t dev = ctx.r3.u32;
  auto* frontend = sr::vk::ActiveFrontend();
  NoteDevice(frontend, dev);
  if (frontend) frontend->SyncRing(base, dev);
  __imp__sub_820FD8C0(ctx, base);
  if (frontend) frontend->ResyncRing(base, dev);
}

// Large command segment allocation (after the segment switch could not satisfy it).
REX_HOOK_RAW(sub_820FCF90) {
  const uint32_t dev = ctx.r3.u32;
  auto* frontend = sr::vk::ActiveFrontend();
  NoteDevice(frontend, dev);
  if (frontend) frontend->SyncRing(base, dev);
  __imp__sub_820FCF90(ctx, base);
  if (frontend) frontend->ResyncRing(base, dev);
}

REX_HOOK_RAW(sub_820F4000) {
  const uint32_t object = ctx.r3.u32;
  auto* frontend = sr::vk::ActiveFrontend();
  __imp__sub_820F4000(ctx, base);
  InvalidateBufferObject(frontend, base, object);
}

REX_HOOK_RAW(sub_820F4150) {
  const uint32_t object = ctx.r3.u32;
  auto* frontend = sr::vk::ActiveFrontend();
  __imp__sub_820F4150(ctx, base);
  InvalidateBufferObject(frontend, base, object);
}

// ---- Frame boundary ------------------------------------------------------------

// D3DDevice_Swap(dev, pFrontBuffer, pParameters). The original still runs so the guest's
// swap/vblank/fence semantics are untouched; the native frame is presented right after.
REX_HOOK_RAW(sub_82112050) {
  const uint32_t dev = ctx.r3.u32, front_buffer = ctx.r4.u32;
  auto* frontend = sr::vk::ActiveFrontend();
  NoteDevice(frontend, dev);
  __imp__sub_82112050(ctx, base);
  if (frontend) frontend->OnSwap(base, front_buffer, ++g_swap_number);
}

// ---- Shader objects -------------------------------------------------------------

// sub_820F5148 creates a vertex shader object and sub_820F4D90 a pixel shader object from a
// finished container (r3), returning the object in r3.
REX_HOOK_RAW(sub_820F5148) {
  const uint32_t source = ctx.r3.u32;
  __imp__sub_820F5148(ctx, base);
  ShaderCreated(ctx.r3.u32, source, true);
}

REX_HOOK_RAW(sub_820F4D90) {
  const uint32_t source = ctx.r3.u32;
  __imp__sub_820F4D90(ctx, base);
  ShaderCreated(ctx.r3.u32, source, false);
}
