#include "sr_native_capture.h"
#include "sr_native_mirror.h"
#include "sr_native_profile.h"
#include <bit>

namespace sr::native {
NativeResult StateCapture::Snapshot(const GuestMemory& memory,GuestAddress device,uint64_t frame,
                                    const StateMirror& mirror,CapturedState& out) const {
  std::vector<std::byte> bytes;
  if(!device || !memory.Copy(device,profile::kDevice.size,bytes)) return NativeResult::kInvalid;
  const auto load=[&](uint32_t offset) {
    uint32_t value=0;
    for(uint32_t i=0;i<4;++i) value=(value<<8)|std::to_integer<uint32_t>(bytes[offset+i]);
    return value;
  };
  CapturedState state;
  state.device=device; state.frame=frame;
  state.vs_version=mirror.VsVersion(); state.ps_version=mirror.PsVersion();
  const auto copy=[&](auto& dest,uint32_t offset,uint32_t first) {
    for(uint32_t i=0;i<dest.size();++i) dest[i]=mirror.Written(first+i)?mirror.Register(first+i):load(offset+i*4);
  };
  const auto& d=profile::kDevice;
  copy(state.vs_constants,d.vs_constants,0x4000); copy(state.ps_constants,d.ps_constants,0x4400);
  copy(state.fetch_constants,d.fetch_constants,0x4800);
  copy(state.vs_bools,d.vs_bools,0x4900); copy(state.ps_bools,d.ps_bools,0x4904);
  copy(state.vs_loops,d.vs_loops,0x4908); copy(state.ps_loops,d.ps_loops,0x4918);
  for(const auto range:profile::kRegisterShadow)
    for(uint32_t i=0;i<range.count;++i)
      state.render_state[range.first-0x2000+i]=mirror.Written(range.first+i)?mirror.Register(range.first+i):load(range.offset+i*4);
  for(uint32_t i=0;i<state.render_state.size();++i)
    if(mirror.Written(0x2000+i)) state.render_state[i]=mirror.Register(0x2000+i);
  for(uint32_t i=0;i<16;++i) {
    state.stream_buffers[i]=load(d.stream_buffers+i*4);
    // The XDK packs strides as byte-sized dword counts, not sixteen u32s.
    state.stream_strides[i]=std::to_integer<uint32_t>(bytes[d.stream_strides+i])*4;
  }
  for(uint32_t i=0;i<4;++i) state.render_targets[i]=load(d.render_targets+i*4);
  state.depth_stencil=load(d.depth_stencil); state.vertex_decl=load(d.vertex_decl);
  state.index_buffer=load(d.index_buffer);
  state.vertex_shader_object=load(d.shader_a); state.pixel_shader_object=load(d.shader_b);
  state.patched_vs=mirror.ShaderCode(true); state.patched_ps=mirror.ShaderCode(false);
  state.viewport={load(d.viewport),load(d.viewport+4),load(d.viewport+8),load(d.viewport+12),
      std::bit_cast<float>(load(d.viewport+16)),std::bit_cast<float>(load(d.viewport+20))};
  // Guest scissor registers are finalized by the pipeline mapper; preserve the raw values.
  const uint32_t tl=state.render_state[0x0e],br=state.render_state[0x0f];
  state.scissor={int32_t(tl&0x7fff),int32_t((tl>>16)&0x7fff),int32_t(br&0x7fff),int32_t((br>>16)&0x7fff)};
  out=std::move(state);
  return NativeResult::kComplete;
}
}  // namespace sr::native
