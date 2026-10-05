// Guest-thread front end (pcvk/native_renderer/frontend.*): a synthetic guest memory image
// stands in for the console, the sink collects the decoded packets.
#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>

#include "pcvk/graphics/guest/render_packet.h"
#include "pcvk/native_renderer/frontend.h"
#include "pcvk/graphics/shaders/container_key.h"
#include "pcvk/native_renderer/game_profile.h"
#include "pcvk/native_renderer/shader_objects.h"
#include "fake_guest.h"
#include "test_main.h"

namespace {
using namespace superman_returns;
namespace guest = graphics::guest;
namespace profile = native::profile;

using fake::FakeGuest;
using fake::kDevice;using fake::kRing;using fake::kDecl;using fake::kVertexBuffer;

struct Collector {
  std::mutex mutex;
  std::vector<guest::RenderPacket> packets;
  bool Take(guest::RenderPacket&& p, std::string&) {
    std::lock_guard<std::mutex> lock(mutex);
    packets.push_back(std::move(p));
    return true;
  }
};

struct Harness {
  FakeGuest guest;
  Collector collector;
  native::Frontend frontend;
  uint32_t ring_write = kRing;

  Harness()
      : frontend(guest, [] {
          native::FrontendOptions o;
          o.worker_lag = false;  // every swap waits for the worker
          return o;
        }()) {
    frontend.SetLoggers({}, [](const std::string& s) { std::printf("  frontend warning: %s\n", s.c_str()); });
    frontend.Start([this](guest::RenderPacket&& p, std::string& e) { return collector.Take(std::move(p), e); });
    frontend.NoteGuestDevice(kDevice);
    // dev+0x28 holds the address of the last dword written into the command segment.
    guest.Put32(kDevice + profile::kDevice.ring_write, kRing - 4);
    frontend.SyncRing(guest.mem.data(), kDevice);  // establishes the ring cursor
  }
  // Appends PM4 dwords as the XDK would and advances the device's write pointer.
  void WriteRing(std::initializer_list<uint32_t> words) {
    for (uint32_t w : words) {
      guest.Put32(ring_write, w);
      ring_write += 4;
    }
    guest.Put32(kDevice + profile::kDevice.ring_write, ring_write - 4);
  }
  uint8_t* base() { return guest.mem.data(); }
  template <typename T>
  const T* Find(size_t& from) {
    std::lock_guard<std::mutex> lock(collector.mutex);
    for (; from < collector.packets.size(); ++from)
      if (auto* p = std::get_if<T>(&collector.packets[from])) return p;
    return nullptr;
  }
};

// One float3 position vertex stream with a three-vertex triangle.
void SetUpTriangle(Harness& h) {
  auto& g = h.guest;
  g.Put32(kDevice + profile::kDevice.vertex_decl, kDecl);
  g.Put32(kDecl + 0x18, 1);
  const uint32_t entry = kDecl + 0x34;
  g.mem[entry] = 0;
  g.mem[entry + 1] = 0;  // stream 0
  g.mem[entry + 2] = 0;
  g.mem[entry + 3] = 0;  // offset 0
  g.Put32(entry + 4, 0x2a23b9);  // float3
  g.mem[entry + 9] = 0;          // usage POSITION
  g.mem[entry + 10] = 0;         // usage index 0
  const float verts[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  for (int i = 0; i < 9; ++i) g.PutPhysical32(kVertexBuffer + 4 * i, std::bit_cast<uint32_t>(verts[i]));
  const uint32_t fetch0 = kDevice + profile::kDevice.fetch_constants + 0x2F8;
  g.Put32(fetch0, kVertexBuffer | 2);  // physical address, endian bits in the low two
  g.Put32(fetch0 + 4, (36 / 4) << 2);  // size in dwords
  g.mem[kDevice + profile::kDevice.stream_strides] = 12 / 4;
}

SR_TEST(frontend_orders_clear_resolve_and_swap_and_owns_the_pixels_state) {
  Harness h;
  const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  h.frontend.Clear(h.base(), 0, 0, 0x3F, color, 0.5f, 7);
  h.frontend.Resolve(h.base(), 0x10, 0, 0, 0, 0, 1.0f, 0, 0, 0);
  h.frontend.OnSwap(h.base(), 0, 1);
  size_t at = 0;
  const auto* clear = h.Find<guest::ClearPacket>(at);
  SR_CHECK(clear != nullptr);
  if (clear) {
    SR_CHECK_EQ(clear->flags, 0x3Fu);
    SR_CHECK_EQ(clear->stencil, 7u);
    SR_CHECK(clear->color[0] == 0.25f && clear->color[3] == 1.0f);
    SR_CHECK(clear->depth == 0.5f);
  }
  size_t resolve_at = at + 1;
  const auto* resolve = h.Find<guest::ResolvePacket>(resolve_at);
  SR_CHECK(resolve != nullptr);
  if (resolve) SR_CHECK_EQ(resolve->flags, 0x10u);
  size_t swap_at = resolve_at + 1;
  const auto* swap = h.Find<guest::SwapPacket>(swap_at);
  SR_CHECK(swap != nullptr);
  SR_CHECK(resolve_at > at && swap_at > resolve_at);
  SR_CHECK_EQ(h.frontend.stats().decode_failures.load(), 0u);
  SR_CHECK_EQ(h.frontend.stats().swaps.load(), 1u);
}

SR_TEST(frontend_draw_captures_and_swaps_vertices_then_reuses_the_clean_buffer) {
  Harness h;
  SetUpTriangle(h);
  h.frontend.DrawVertices(h.base(), 4, 0, 3);
  h.frontend.DrawVertices(h.base(), 4, 0, 3);
  h.frontend.InvalidateGuestRange(kVertexBuffer, 36);
  h.frontend.DrawVertices(h.base(), 4, 0, 3);
  h.frontend.OnSwap(h.base(), 0, 1);
  size_t at = 0;
  const auto* first = h.Find<guest::DrawPacket>(at);
  SR_CHECK(first != nullptr);
  if (!first) return;
  SR_CHECK_EQ(first->count, 3u);
  SR_CHECK_EQ(first->streams.size(), size_t(1));
  if (first->streams.size() == 1) {
    // First use uploads the whole buffer (action 2), byte-swapped to host order.
    SR_CHECK_EQ(int(first->streams[0].update.plan.action), 2);
    SR_CHECK_EQ(first->streams[0].update.bytes.size(), size_t(36));
    float x1;
    std::memcpy(&x1, first->streams[0].update.bytes.data() + 12, 4);
    SR_CHECK(x1 == 1.0f);
  }
  size_t second_at = at + 1;
  const auto* second = h.Find<guest::DrawPacket>(second_at);
  SR_CHECK(second != nullptr);
  if (second && second->streams.size() == 1) {
    SR_CHECK_EQ(int(second->streams[0].update.plan.action), 0);  // clean: nothing re-captured
    SR_CHECK(second->streams[0].update.bytes.empty());
  }
  size_t third_at = second_at + 1;
  const auto* third = h.Find<guest::DrawPacket>(third_at);
  SR_CHECK(third != nullptr);
  if (third && third->streams.size() == 1)
    SR_CHECK(third->streams[0].update.plan.action != 0);  // Unlock invalidated it
  SR_CHECK_EQ(h.frontend.stats().decode_failures.load(), 0u);
}

SR_TEST(frontend_replays_ring_constants_for_the_draw_that_follows) {
  Harness h;
  SetUpTriangle(h);
  // Type-0 packet: write one vertex shader constant (c0) through register 0x4000.
  h.WriteRing({0x00030000u | 0x4000u, std::bit_cast<uint32_t>(2.0f), std::bit_cast<uint32_t>(3.0f),
               std::bit_cast<uint32_t>(4.0f), std::bit_cast<uint32_t>(5.0f)});
  h.frontend.DrawVertices(h.base(), 4, 0, 3);
  h.frontend.OnSwap(h.base(), 0, 1);
  size_t at = 0;
  const auto* draw = h.Find<guest::DrawPacket>(at);
  SR_CHECK(draw != nullptr);
  if (draw) {
    SR_CHECK(std::bit_cast<float>(draw->constants.vs[0]) == 2.0f);
    SR_CHECK(std::bit_cast<float>(draw->constants.vs[3]) == 5.0f);
  }
}

SR_TEST(frontend_drops_a_command_whose_memory_is_unreadable_instead_of_crashing) {
  Harness h;
  SetUpTriangle(h);
  // A vertex buffer far outside the physical window cannot be captured.
  const uint32_t fetch0 = kDevice + profile::kDevice.fetch_constants + 0x2F8;
  h.guest.Put32(fetch0, 0x1FFFFFF0u);
  h.guest.Put32(fetch0 + 4, (4096 / 4) << 2);
  h.frontend.DrawVertices(h.base(), 4, 0, 3);
  h.frontend.OnSwap(h.base(), 0, 1);
  size_t at = 0;
  SR_CHECK(h.Find<guest::DrawPacket>(at) == nullptr);
  SR_CHECK(h.frontend.stats().capture_failures.load() >= 1);
  size_t swap_at = 0;
  SR_CHECK(h.Find<guest::SwapPacket>(swap_at) != nullptr);  // the frame still presents
}

SR_TEST(frontend_stop_is_idempotent_and_ignores_later_calls) {
  Harness h;
  h.frontend.Stop();
  h.frontend.Stop();
  const float color[4] = {};
  h.frontend.Clear(h.base(), 0, 0, 0xF, color, 1.0f, 0);
  h.frontend.OnSwap(h.base(), 0, 2);
  SR_CHECK(!h.frontend.running());
}

}  // namespace

namespace {
using superman_returns::native::CaptureShaderContainer;
using superman_returns::native::ShaderObjects;
std::vector<uint8_t> FabricatedContainer(bool vertex, bool instanced = false) {
  std::vector<uint8_t> v(96);
  auto put = [&](size_t at, uint32_t x) { for (int i = 0; i < 4; ++i) v[at + i] = uint8_t(x >> (24 - 8 * i)); };
  put(0, 0x102a1100 | uint32_t(vertex));
  put(4, 72);
  put(8, 24);
  if (instanced) std::memcpy(v.data() + 40, "instance_data", 14);
  return v;
}
}  // namespace

SR_TEST(shader_objects_capture_exact_container_and_replace_reused_addresses) {
  FakeGuest g;
  auto vs = FabricatedContainer(true, true), ps = FabricatedContainer(false);
  std::memcpy(g.mem.data() + 0x2000, vs.data(), vs.size());
  std::memcpy(g.mem.data() + 0x3000, ps.data(), ps.size());
  auto a = CaptureShaderContainer(g, 0x2000, true);
  SR_CHECK(a != nullptr);
  if (a) {
    SR_CHECK(a->vertex && a->dynamic_vertex_fetch);
    SR_CHECK_EQ(a->container.size(), size_t(96));
    SR_CHECK(a->container == vs);
    SR_CHECK_EQ(a->hash, superman_returns::graphics::shaders::ContainerKey(vs));
  }
  SR_CHECK(CaptureShaderContainer(g, 0x2000, false) == nullptr);  // stage must agree with the creator
  SR_CHECK(CaptureShaderContainer(g, 0x5000, true) == nullptr);   // not a container
  SR_CHECK(CaptureShaderContainer(g, 0xFFFFFFFFu, true) == nullptr);
  auto b = CaptureShaderContainer(g, 0x3000, false);
  SR_CHECK(b && !b->vertex && !b->dynamic_vertex_fetch);
  ShaderObjects objects;
  objects.Remember(0x700, a);
  SR_CHECK(objects.Find(0x700) == a);
  objects.Remember(0x700, b);  // the XDK reused the address for another shader
  SR_CHECK(objects.Find(0x700) == b);
  objects.Remember(0x700, nullptr);
  SR_CHECK(objects.Find(0x700) == nullptr);
  SR_CHECK(objects.Find(0) == nullptr);
}

SR_TEST(frontend_worker_runs_on_a_large_stack) {
  // The decoder and the driver need far more than the 128 KB Horizon gives a default thread.
  std::atomic<bool> ok{false};
  superman_returns::native::WorkerThread thread(
      [&] {
        volatile char big[1 << 20];
        for (size_t i = 0; i < sizeof(big); i += 4096) big[i] = char(i);
        ok = big[4096] == char(4096);
      },
      8u << 20);
  SR_CHECK(thread.started());
  thread.Join();
  SR_CHECK(ok.load());
}
