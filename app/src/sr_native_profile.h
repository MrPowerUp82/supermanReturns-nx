#pragma once
// Factual Superman XDK 2.0.3529 layout verified in the local PC reference
// 257feabc050e03c287fdf6238bf55876e5b59d81 (game_profile.h) and NX generated
// symbols for the supported XEX. This is a new declaration of game facts,
// not imported native-kit implementation. Never substitute Conan/NFSMW offsets.
#include <array>
#include <cstdint>
#include <string_view>

namespace sr::native::profile {
inline constexpr std::string_view kXexSha256 =
    "c8f243acd99de9a91f5ae4f409721c0e954e3d5eb96861419d3da07b8106db2b";
struct DeviceLayout {
  uint32_t fetch_constants = 0x400;
  uint32_t vs_constants = 0x700, ps_constants = 0x1700;
  uint32_t vs_bools = 0x2700, ps_bools = 0x2710;
  uint32_t vs_loops = 0x2720, ps_loops = 0x2760;
  uint32_t register_shadow = 0x2800, vertex_decl = 0x2d10;
  uint32_t index_buffer = 0x2f84, render_targets = 0x2f88, depth_stencil = 0x2f98;
  uint32_t stream_buffers = 0x2f9c, stream_strides = 0x2fe0, textures = 0x2ff0;
  uint32_t viewport = 0x3058, shader_a = 0x3080, shader_b = 0x3084;
  uint32_t ring_write = 0x28, ring_limit = 0x2c;
  uint32_t fence_current = 10780, fence_completed_ptr = 10768;
  uint32_t size = 0x5700;
};
inline constexpr DeviceLayout kDevice{};
struct RegisterShadowRange { uint32_t first, count, offset; };
inline constexpr std::array<RegisterShadowRange, 7> kRegisterShadow{{
    {0x2000, 16, 0x2800}, {0x2100, 21, 0x284c}, {0x2180, 5, 0x28a0},
    {0x2200, 12, 0x28b4}, {0x2280, 21, 0x28e4}, {0x2300, 38, 0x2938},
    {0x2380, 8, 0x29d0}}};
inline constexpr uint32_t kDrawVertices = 0x820fbbf8;
inline constexpr uint32_t kDrawIndexedVertices = 0x820fc000;
inline constexpr uint32_t kDrawVerticesUP = 0x820fbbb0;
inline constexpr uint32_t kBeginVertices = 0x820fb6e8, kEndVertices = 0x820fbba0;
inline constexpr uint32_t kClear = 0x82101a58, kResolve = 0x8210c5f8;
inline constexpr uint32_t kBeginTiling = 0x8210d588, kEndTiling = 0x8210da98;
inline constexpr uint32_t kRingMakeSpace = 0x820fd8c0, kRingAllocLarge = 0x820fcf90;
inline constexpr uint32_t kReserveInlineConstants = 0x82113010, kLoadShaderLiterals = 0x82108470;
inline constexpr uint32_t kVertexBufferUnlock = 0x820f4000, kIndexBufferUnlock = 0x820f4150;
inline constexpr uint32_t kCreateVertexShader = 0x820f5148, kCreatePixelShader = 0x820f4d90;
inline constexpr uint32_t kSwap = 0x82112050;
inline constexpr uint32_t kBlockOnFence = 0x820fcb30, kPollGpuProgress = 0x820f33e8;
}  // namespace sr::native::profile
