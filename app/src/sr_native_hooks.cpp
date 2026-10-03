// Independently authored hooks against verified Superman XDK/PPC ABI facts.
// Capture is opt-in and executes no GPU work. Each original is called exactly once.
#include "sr_native_bridge.h"
#include "sr_shader_registry.h"
#include "generated/default/superman_returns_init.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <atomic>
#include <cstring>
#include <memory>

REXCVAR_DEFINE_BOOL(sr_native_capture, false, "Superman Returns",
    "Observe immutable D3D operations and exact PM4 stamps; does not enable native draws")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace sr::native {
namespace {
rex::memory::Memory* LiveMemory() {
  auto* kernel=rex::system::kernel_state(); return kernel ? kernel->memory() : nullptr;
}
bool ValidGuest(uint32_t address,uint32_t size,bool write) {
  auto* memory=LiveMemory();
  if (!memory || !size || uint64_t(address)+size>(uint64_t{1}<<32)) return false;
  auto* heap=memory->LookupHeap(address);
  if (!heap || address<heap->heap_base() ||
      uint64_t(address)+size>uint64_t(heap->heap_base())+heap->heap_size() ||
      memory->LookupHeap(address+size-1)!=heap) return false;
  const uint32_t need=rex::memory::kMemoryProtectRead | (write ? rex::memory::kMemoryProtectWrite : 0);
  const uint64_t end=uint64_t(address)+size;
  uint64_t cursor=address;
  while (cursor<end) {
    rex::memory::HeapAllocationInfo info{};
    const uint32_t page=heap->page_size();
    if (!page || !heap->QueryRegionInfo(uint32_t(cursor),&info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit) || (info.protect & need)!=need)
      return false;
    const uint64_t region_end=(cursor & ~uint64_t(page-1))+info.region_size;
    if (region_end<=cursor) return false;
    cursor=region_end;
  }
  return true;
}
struct HookContext {
  GuestMemory memory{{ValidGuest,
    [](uint32_t a,std::span<std::byte> out) {
      if (!ValidGuest(a,uint32_t(out.size()),false)) return false;
      std::memcpy(out.data(),LiveMemory()->TranslateVirtual<const std::byte*>(a),out.size());return true;
    },
    [](uint32_t a,std::span<const std::byte> in) {
      if (!ValidGuest(a,uint32_t(in.size()),true)) return false;
      std::memcpy(LiveMemory()->TranslateVirtual<std::byte*>(a),in.data(),in.size());return true;
    }}};
  NativeCaptureBridge bridge{memory,[](uint32_t a,uint32_t& physical) {
    auto* memory=LiveMemory(); if (!memory) return false;
    physical=memory->GetPhysicalAddress(a);return physical<0x20000000;
  }};
  CaptureCall inline_call;
  DrawPayload inline_draw;
  uint32_t inline_pointer=0;
  bool inline_open=false, draw_up=false;
};
HookContext& Hooks() {
  thread_local HookContext context;
  context.bridge.SetEnabled(REXCVAR_GET(sr_native_capture));
  return context;
}
std::atomic<uint64_t> g_calls{0},g_failures{0};
CaptureCall Start(HookContext& h,CommandKind kind,uint32_t device) {
  auto call=h.bridge.BeginCall(kind,device);
  if (REXCVAR_GET(sr_native_capture) && call.id<=512)
    REXLOG_INFO("[sr-capture] begin id={} depth={} kind={} device={:08X} result={}",
      call.id,call.depth,uint32_t(kind),device,uint32_t(call.result));
  return call;
}
void RecordCommand(const NativeCommand& cmd,uint64_t scope_id,bool checkpoint) {
  const uint64_t sequence=++g_calls;
  // Bounded boot trace. Ownership has transferred to cmd; guest may overwrite it.
  if (sequence<=512) {
    REXLOG_INFO("[sr-capture] call={} scope={} checkpoint={} kind={} device={:08X} frame={} stamps={} vs={:08X} ps={:08X}",
      sequence,scope_id,checkpoint,uint32_t(cmd.kind),cmd.state.device,cmd.state.frame,cmd.stamps.size(),
      cmd.state.vertex_shader_object,cmd.state.pixel_shader_object);
    for (const auto& stamp:cmd.stamps) {
      if ((stamp.header>>30)!=3) continue;
      const auto opcode=(stamp.header>>8)&0x7f;
      if (opcode!=0x22 && opcode!=0x36 && opcode!=0x64) continue;
      REXLOG_INFO("[sr-capture] packet call={} epoch={} physical={:08X} header={:08X} words={}",
        sequence,stamp.site.allocation_epoch,stamp.site.physical_address,stamp.header,stamp.payload.size());
      for (size_t i=0;i<stamp.payload.size();++i)
        REXLOG_INFO("[sr-capture] word call={} index={} value={:08X}",sequence,i,stamp.payload[i]);
    }
  }
}
void Finish(HookContext& h,CaptureCall& call,NativeCommand& cmd) {
  const auto scope_id=call.id;
  const auto result=h.bridge.EndCall(call,h.memory,cmd);
  if (result==NativeResult::kComplete) RecordCommand(cmd,scope_id,false);
  else if (result!=NativeResult::kCancelled && result!=NativeResult::kPending && ++g_failures<=32)
    REXLOG_WARN("[sr-capture] scope={} kind={} device={:08X} result={}",
      scope_id,uint32_t(call.kind),call.device,uint32_t(result));
}
void BeforeWait(HookContext& h) {
  NativeCommand snapshot;
  const auto result=h.bridge.CaptureBeforeWait(h.memory,snapshot);
  if (result==NativeResult::kComplete) RecordCommand(snapshot,h.bridge.ScopeId(),true);
}
void ShaderCreated(uint32_t object,uint32_t source,bool vertex) {
  if (!object) return;
  auto& h=Hooks(); uint32_t flags,virtual_size,physical_size;
  if (source>UINT32_MAX-12 || !h.memory.ReadU32(source,flags) || !h.memory.ReadU32(source+4,virtual_size) ||
      !h.memory.ReadU32(source+8,physical_size) || flags!=(vertex ? 0x102a1101u : 0x102a1100u) ||
      uint64_t(virtual_size)+physical_size>65536 || virtual_size<36) {
    RuntimeShaders().ForgetD3DObject(object); return;
  }
  std::vector<std::byte> bytes;
  if (!h.memory.Copy(source,virtual_size+physical_size,bytes)) {
    RuntimeShaders().ForgetD3DObject(object); return;
  }
  ShaderIdentity identity; identity.vertex=vertex;
  identity.container.resize(bytes.size()); std::memcpy(identity.container.data(),bytes.data(),bytes.size());
  RuntimeShaders().RememberD3DObject(object,identity);
}
} // namespace
} // namespace sr::native

REX_HOOK_RAW(sub_820FBBF8) {
  auto& h=sr::native::Hooks(); auto call=sr::native::Start(h,sr::native::CommandKind::kDraw,ctx.r3.u32);
  sr::native::NativeCommand cmd; sr::native::DrawPayload draw;
  draw.primitive=ctx.r4.u32;draw.start=ctx.r5.u32;draw.count=ctx.r6.u32;cmd.payload=std::move(draw);
  h.bridge.SetPayload(call,cmd.payload);
  __imp__sub_820FBBF8(ctx,base); sr::native::Finish(h,call,cmd);
}
REX_HOOK_RAW(sub_820FC000) {
  auto& h=sr::native::Hooks(); auto call=sr::native::Start(h,sr::native::CommandKind::kDraw,ctx.r3.u32);
  sr::native::NativeCommand cmd; sr::native::DrawPayload draw;
  draw.indexed=true;draw.primitive=ctx.r4.u32;draw.base_vertex=ctx.r5.s32;
  draw.start=ctx.r6.u32;draw.count=ctx.r7.u32;cmd.payload=std::move(draw);
  h.bridge.SetPayload(call,cmd.payload);
  __imp__sub_820FC000(ctx,base); sr::native::Finish(h,call,cmd);
}
REX_HOOK_RAW(sub_820FBBB0) {
  auto& h=sr::native::Hooks(); auto call=sr::native::Start(h,sr::native::CommandKind::kDraw,ctx.r3.u32);
  sr::native::NativeCommand cmd; sr::native::DrawPayload draw;
  draw.primitive=ctx.r4.u32;draw.count=ctx.r5.u32;draw.stride=ctx.r7.u32;
  const uint64_t size=uint64_t(draw.count)*draw.stride;
  const bool copied=call.result==sr::native::NativeResult::kCancelled ||
    (size<=8*1024*1024 && h.memory.Copy(ctx.r6.u32,uint32_t(size),draw.inline_vertices));
  cmd.payload=std::move(draw); const bool previous=h.draw_up;h.draw_up=true;
  if (!copied) call.result=sr::native::NativeResult::kInvalid;
  h.bridge.SetPayload(call,cmd.payload);
  __imp__sub_820FBBB0(ctx,base);h.draw_up=previous;
  if (!copied) call.result=sr::native::NativeResult::kInvalid;
  sr::native::Finish(h,call,cmd);
}
REX_HOOK_RAW(sub_820FB6E8) {
  auto& h=sr::native::Hooks();
  if (!h.draw_up && !h.inline_open) {
    h.inline_call=sr::native::Start(h,sr::native::CommandKind::kDraw,ctx.r3.u32);
    h.inline_draw={};h.inline_draw.primitive=ctx.r4.u32;
    h.inline_draw.count=ctx.r5.u32;h.inline_draw.stride=ctx.r6.u32;h.inline_open=true;
    h.bridge.SetPayload(h.inline_call,h.inline_draw);
  }
  __imp__sub_820FB6E8(ctx,base);
  if (!h.draw_up && h.inline_open) {
    h.inline_pointer=ctx.r3.u32;
    if (!h.inline_pointer) {
      h.inline_open=false;h.inline_call.result=sr::native::NativeResult::kInvalid;
      sr::native::NativeCommand cmd;cmd.payload=h.inline_draw;
      sr::native::Finish(h,h.inline_call,cmd);
    }
  }
}
REX_HOOK_RAW(sub_820FBBA0) {
  auto& h=sr::native::Hooks();
  __imp__sub_820FBBA0(ctx,base);
  if (!h.draw_up && h.inline_open) {
    h.inline_open=false; const uint64_t size=uint64_t(h.inline_draw.count)*h.inline_draw.stride;
    if (h.inline_call.result!=sr::native::NativeResult::kCancelled &&
        (size>8*1024*1024 || !h.memory.Copy(h.inline_pointer,uint32_t(size),h.inline_draw.inline_vertices)))
      h.inline_call.result=sr::native::NativeResult::kInvalid;
    sr::native::NativeCommand cmd;cmd.payload=std::move(h.inline_draw);
    sr::native::Finish(h,h.inline_call,cmd);
  }
}
REX_HOOK_RAW(sub_82101A58) {
  auto& h=sr::native::Hooks(); auto call=sr::native::Start(h,sr::native::CommandKind::kClear,ctx.r3.u32);
  sr::native::ClearPayload clear;clear.flags=ctx.r4.u32;
  const bool copied=call.result==sr::native::NativeResult::kCancelled ||
    sr::native::ReadClearArguments(h.memory,ctx.r5.u32,(clear.flags & 0xf) ? ctx.r6.u32 : 0,
      ctx.r8.u32,float(ctx.f1.f64),clear);
  sr::native::NativeCommand cmd;cmd.payload=std::move(clear);
  if (!copied) call.result=sr::native::NativeResult::kInvalid;
  h.bridge.SetPayload(call,cmd.payload);
  __imp__sub_82101A58(ctx,base);
  if (!copied) call.result=sr::native::NativeResult::kInvalid;
  sr::native::Finish(h,call,cmd);
}
REX_HOOK_RAW(sub_8210C5F8) {
  auto& h=sr::native::Hooks(); auto call=sr::native::Start(h,sr::native::CommandKind::kResolve,ctx.r3.u32);
  sr::native::ResolvePayload resolve;resolve.flags=ctx.r4.u32;resolve.destination=ctx.r6.u32;
  resolve.mip=ctx.r8.u32;resolve.slice=ctx.r9.u32;resolve.clear_depth=float(ctx.f1.f64);
  // Original loads this at new-SP+460 after a 368-byte frame: entry-SP+92.
  const bool stencil_ok=call.result==sr::native::NativeResult::kCancelled || !(resolve.flags & 0x200) ||
    (ctx.r1.u32<=UINT32_MAX-96 && h.memory.ReadU32(ctx.r1.u32+92,resolve.clear_stencil));
  sr::native::ClearPayload args;
  const bool copied=call.result==sr::native::NativeResult::kCancelled ||
    sr::native::ReadClearArguments(h.memory,ctx.r5.u32,(resolve.flags & 0x100) ? ctx.r10.u32 : 0,
      0,resolve.clear_depth,args);
  if (!args.rects.empty()) resolve.source=args.rects[0];
  resolve.clear_color=args.color;
  uint32_t x=0,y=0;
  const uint32_t point=ctx.r7.u32;
  const bool point_ok=call.result==sr::native::NativeResult::kCancelled || !point ||
    (point<=UINT32_MAX-8 && h.memory.ReadU32(point,x) && h.memory.ReadU32(point+4,y));
  resolve.destination_x=int32_t(x);resolve.destination_y=int32_t(y);
  sr::native::NativeCommand cmd;cmd.payload=resolve;
  if (!copied || !point_ok || !stencil_ok) call.result=sr::native::NativeResult::kInvalid;
  h.bridge.SetPayload(call,cmd.payload);
  __imp__sub_8210C5F8(ctx,base);
  if (!copied || !point_ok || !stencil_ok) call.result=sr::native::NativeResult::kInvalid;
  sr::native::Finish(h,call,cmd);
}
REX_HOOK_RAW(sub_82112050) {
  auto& h=sr::native::Hooks();auto call=sr::native::Start(h,sr::native::CommandKind::kSwap,ctx.r3.u32);
  sr::native::NativeCommand cmd;sr::native::SwapPayload swap;swap.front_buffer=ctx.r4.u32;cmd.payload=swap;
  h.bridge.SetPayload(call,cmd.payload);
  __imp__sub_82112050(ctx,base);sr::native::Finish(h,call,cmd);
}
#define SR_SEGMENT_CAPTURE(address) \
  REX_HOOK_RAW(sub_##address) { \
    auto& h=sr::native::Hooks();const uint32_t device=ctx.r3.u32; \
    h.bridge.BeforeSegmentChange(device); sr::native::BeforeWait(h); __imp__sub_##address(ctx,base); \
    h.bridge.AfterSegmentChange(device); \
  }
SR_SEGMENT_CAPTURE(820FD8C0)
SR_SEGMENT_CAPTURE(820FCF90)

REX_HOOK_RAW(sub_820FCB30) {
  if (REXCVAR_GET(sr_native_capture)) {
    auto& h=sr::native::Hooks();
    const auto result=h.bridge.BeforeSegmentChange(ctx.r3.u32);
    sr::native::BeforeWait(h);
    static std::atomic<uint32_t> reports{0};
    if (++reports<=32) REXLOG_INFO("[sr-capture] fence device={:08X} value={:08X} mode={} sync={}",
      ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,uint32_t(result));
  }
  __imp__sub_820FCB30(ctx,base);
}

REX_HOOK_RAW(sub_820F5148) {
  const uint32_t source=ctx.r3.u32;__imp__sub_820F5148(ctx,base);
  sr::native::ShaderCreated(ctx.r3.u32,source,true);
}
REX_HOOK_RAW(sub_820F4D90) {
  const uint32_t source=ctx.r3.u32;__imp__sub_820F4D90(ctx,base);
  sr::native::ShaderCreated(ctx.r3.u32,source,false);
}
