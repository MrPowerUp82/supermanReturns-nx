#pragma once
#include "sr_native_guest.h"
#include <array>
#include <cstdint>
#include <variant>
#include <vector>

namespace sr::native {
using Serial = uint64_t;
enum class NativeResult { kComplete, kPending, kCancelled, kUnsupported, kInvalid, kFailed };
struct PacketSite {
  uint64_t allocation_epoch = 0;
  uint32_t physical_address = 0;
  bool operator==(const PacketSite&) const = default;
};
struct PacketStamp {
  PacketSite site;
  uint32_t header = 0;
  std::vector<uint32_t> payload;
  bool operator==(const PacketStamp&) const = default;
};
struct Viewport {
  uint32_t x = 0, y = 0, width = 0, height = 0;
  float min_depth = 0, max_depth = 1;
};
struct Rect { int32_t left = 0, top = 0, right = 0, bottom = 0; };
struct ShaderIdentity {
  bool vertex = false;
  std::vector<uint8_t> container;
  bool operator==(const ShaderIdentity&) const = default;
};
struct CapturedState {
  GuestAddress device = 0;
  uint64_t frame = 0, vs_version = 0, ps_version = 0;
  std::array<uint32_t, 256 * 4> vs_constants{}, ps_constants{};
  std::array<uint32_t, 32 * 6> fetch_constants{};
  std::array<uint32_t, 4> vs_bools{}, ps_bools{};
  std::array<uint32_t, 16> vs_loops{}, ps_loops{};
  // Register 0x2000 through 0x23ff, inclusive, indexed by reg - 0x2000.
  std::array<uint32_t, 0x400> render_state{};
  std::array<GuestAddress, 16> stream_buffers{};
  std::array<uint32_t, 16> stream_strides{};
  std::array<GuestAddress, 4> render_targets{};
  GuestAddress depth_stencil = 0, vertex_decl = 0, index_buffer = 0;
  GuestAddress vertex_shader_object = 0, pixel_shader_object = 0;
  std::vector<uint32_t> patched_vs, patched_ps;
  Viewport viewport;
  Rect scissor;
};
enum class CommandKind { kDraw, kClear, kResolve, kSwap };
struct DrawPayload {
  uint32_t primitive = 0, start = 0, count = 0, stride = 0;
  int32_t base_vertex = 0;
  bool indexed = false, index32 = false;
  uint32_t endian = 0;
  std::vector<std::byte> inline_vertices, indices;
};
struct ClearPayload {
  uint32_t flags = 0, stencil = 0;
  std::vector<Rect> rects;
  std::array<float, 4> color{};
  float depth = 1;
};
struct ResolvePayload {
  uint32_t flags = 0, mip = 0, slice = 0;
  GuestAddress destination = 0;
  Rect source;
  int32_t destination_x = 0, destination_y = 0;
  std::array<float, 4> clear_color{};
  float clear_depth = 1;
  uint32_t clear_stencil = 0;
};
struct SwapPayload { GuestAddress front_buffer = 0; uint32_t width = 0, height = 0; };
using CommandPayload=std::variant<DrawPayload, ClearPayload, ResolvePayload, SwapPayload>;
struct NativeCommand {
  Serial serial = 0;
  CommandKind kind = CommandKind::kDraw;
  std::vector<PacketStamp> stamps;
  CapturedState state;
  CommandPayload payload;
};
}  // namespace sr::native
