// See frontend.h. Ported from superman_returns_recomp native_renderer.cpp
// (BeginCmd .. OnSwap, PlanBuffer, PlanStreams, CaptureTextures); the D3D12 renderer, the
// write-watch tracking and the debugging cvars were left out.
#include "frontend.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>

#if defined(_WIN32)
#include <thread>
#else
#include <pthread.h>
#endif

#include "../graphics/guest/draw_state.h"
#include "../graphics/guest/pm4_capture.h"
#include "../graphics/guest/texture_layout.h"
#include "game_profile.h"
#include "texture_binding.h"

namespace superman_returns::native {
namespace {

namespace guest = graphics::guest;
constexpr const profile::DeviceLayout& kDev = profile::kDevice;
constexpr uint32_t kDevVertexFetch0 = kDev.fetch_constants + 0x2F8;
// Xenos primitive types the front end distinguishes (rex::graphics::xenos::PrimitiveType).
constexpr uint32_t kPrimLineStrip = 0x03, kPrimTriangleStrip = 0x06, kPrimRectangleList = 0x08,
                   kPrimQuadList = 0x0D;
constexpr uint32_t kRegPaSuScModeCntl = 0x2205, kRegMultiPrimIbResetIndex = 0x2103;
constexpr uint32_t kPhysicalAlias = 0xA0000000u;

inline uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}
inline uint32_t Load16(const uint8_t* base, uint32_t addr) {
  uint16_t v;
  std::memcpy(&v, base + addr, 2);
  return __builtin_bswap16(v);
}
inline uint32_t Load8(const uint8_t* base, uint32_t addr) { return base[addr]; }

uint32_t RegShadowOffset(uint32_t reg_index) {
  for (const auto& r : profile::kRegisterShadow) {
    if (reg_index >= r.first && reg_index < r.first + r.count) {
      return r.offset + 4 * (reg_index - r.first);
    }
  }
  return 0;
}
uint32_t LoadReg(const uint8_t* base, uint32_t dev, uint32_t reg_index) {
  const uint32_t off = RegShadowOffset(reg_index);
  return off ? Load32(base, dev + off) : 0;
}

// Guest addresses appear both as physical (fetch constants) and as the 0xA0000000+
// physical views (texture objects, surfaces): normalize to physical (low 29 bits, plus
// 4 KB for the 0xE0000000 view, the XDK convention).
inline uint32_t GuestPhysical(uint32_t address) {
  return (address & 0x1FFFFFFFu) + (address >= 0xE0000000u ? 0x1000u : 0u);
}

}  // namespace

struct WorkerThread::Impl {
  std::function<void()> body;
#if defined(_WIN32)
  std::thread thread;
#else
  pthread_t thread{};
#endif
};

WorkerThread::WorkerThread(std::function<void()> body, size_t stack_bytes) : impl_(new Impl) {
  impl_->body = std::move(body);
#if defined(_WIN32)
  (void)stack_bytes;  // the default 1 MB is enough on Windows
  impl_->thread = std::thread([impl = impl_.get()] { impl->body(); });
  started_ = true;
#else
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) return;
  pthread_attr_setstacksize(&attributes, stack_bytes);
  started_ = pthread_create(
                 &impl_->thread, &attributes,
                 [](void* argument) -> void* {
                   static_cast<Impl*>(argument)->body();
                   return nullptr;
                 },
                 impl_.get()) == 0;
  pthread_attr_destroy(&attributes);
#endif
}

void WorkerThread::Join() {
  if (!started_) return;
  started_ = false;
#if defined(_WIN32)
  if (impl_->thread.joinable()) impl_->thread.join();
#else
  pthread_join(impl_->thread, nullptr);
#endif
}

WorkerThread::~WorkerThread() { Join(); }

Frontend::Frontend(GuestAccess& access, FrontendOptions options)
    : access_(access), options_(options) {}

Frontend::~Frontend() { Stop(); }

void Frontend::SetLoggers(Logger info, Logger warn) {
  info_ = std::move(info);
  warn_ = std::move(warn);
}
void Frontend::SetShaderLookup(ShaderLookup lookup) { shader_lookup_ = std::move(lookup); }
void Frontend::SetGammaSource(GammaSource source) { gamma_source_ = std::move(source); }
void Frontend::Info(const std::string& text) const {
  if (info_) info_(text);
}
void Frontend::Warn(const std::string& text) const {
  if (warn_) warn_(text);
}

bool Frontend::Start(PacketSink sink, std::function<void()> cancel) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  if (started_ || stop_.load() || !sink) return false;
  sink_ = std::move(sink);
  cancel_sink_ = std::move(cancel);
  worker_ = std::make_unique<WorkerThread>([this] { WorkerMain(); }, 8u << 20);
  if (!worker_->started()) {
    worker_.reset();
    return false;
  }
  started_ = true;
  return true;
}

void Frontend::Stop() {
  if (!started_) return;
  if (!stop_.exchange(true) && cancel_sink_) cancel_sink_();
  queue_cv_.notify_all();
  done_cv_.notify_all();
  if (worker_) worker_->Join();
}

void Frontend::NoteGuestDevice(uint32_t device) {
  if (device) device_.store(device, std::memory_order_relaxed);
}

std::shared_ptr<const guest::ShaderCapture> Frontend::Shader(uint32_t object) {
  return object && shader_lookup_ ? shader_lookup_(object) : nullptr;
}

// ---------------------------------------------------------------------------
// Capture primitives
// ---------------------------------------------------------------------------

void Frontend::BeginCmd(Op op) {
  if (!batch_) batch_ = std::make_unique<WorkBatch>();
  cur_ = WorkCmd{};
  cur_.op = op;
  cur_.command_serial = ++capture_serial_;
  cur_.tiling_active = capture_tiling_active_;
  cur_.packet_check = true;
  cur_.pass = 0;
  cur_.range_first = uint32_t(batch_->ranges.size());
  cur_.stream_first = uint32_t(batch_->streams.size());
  cur_ok_ = true;
}

bool Frontend::CaptureBytes(uint32_t address, uint32_t length) {
  if (!length) return true;
  const uint8_t* source = access_.Readable(address, length);
  if (!source) {
    // The range is not committed memory. The command is dropped by EndCmd rather than
    // letting a wild read crash the guest process or ship garbage to the GPU.
    cur_ok_ = false;
    return false;
  }
  const size_t offset = batch_->bytes.size();
  batch_->bytes.resize(offset + length);
  std::memcpy(batch_->bytes.data() + offset, source, length);
  batch_->ranges.push_back({address, length, uint32_t(offset)});
  return true;
}

void Frontend::CaptureRing(uint8_t* base, uint32_t dev) {
  if (!dev) return;
  // dev+0x28: last dword written into the current XDK command segment.
  const uint32_t current = Load32(base, dev + kDev.ring_write) + 4;
  if (ring_last_ && current >= ring_last_ && current - ring_last_ <= (1u << 20)) {
    const uint32_t n = current - ring_last_;
    const uint8_t* source = n ? access_.Readable(ring_last_, n) : nullptr;
    if (n && !source) {
      cur_ok_ = false;
    } else if (n) {
      if (!cur_.ring_bytes) cur_.ring_offset = uint32_t(batch_->bytes.size());
      if (cur_.ring_offset + cur_.ring_bytes == batch_->bytes.size()) {
        batch_->bytes.resize(batch_->bytes.size() + n);
        std::memcpy(batch_->bytes.data() + cur_.ring_offset + cur_.ring_bytes, source, n);
        cur_.ring_bytes += n;
      }
    }
  } else if (ring_last_) {
    ++stats_.ring_resyncs;
  }
  ring_last_ = current;
}

void Frontend::CaptureDevice(uint8_t* base, uint32_t dev) {
  cur_.device = dev;
  CaptureBytes(dev + kDev.fetch_constants, 32 * 24);
  CaptureBytes(dev + kDev.vs_bools, kDev.register_shadow - kDev.vs_bools);
  for (const auto& range : profile::kRegisterShadow) CaptureBytes(dev + range.offset, range.count * 4);
  const uint32_t vs = Load32(base, dev + kDev.shader_a), ps = Load32(base, dev + kDev.shader_b);
  cur_.vertex_shader = Shader(vs);
  cur_.pixel_shader = Shader(ps);
  CaptureBytes(dev, 8);
  CaptureBytes(dev + kDev.fetch_constants + 0x280, 0x80);  // vertex fetch slots
  CaptureBytes(dev + kDev.register_shadow, 0x1D0);
  CaptureBytes(dev + kDev.vertex_decl - 4, 8);
  CaptureBytes(dev + kDev.index_buffer - 0xC, 0x120);  // pointers .. viewport
  for (uint32_t i = 0; i < 4; ++i) {
    if (const uint32_t rt = Load32(base, dev + kDev.render_targets + 4 * i)) CaptureBytes(rt, 0x28);
  }
  if (const uint32_t ds = Load32(base, dev + kDev.depth_stencil)) CaptureBytes(ds, 0x28);
  if (const uint32_t decl = Load32(base, dev + kDev.vertex_decl)) {
    const uint32_t n = std::min(Load32(base, decl + 0x18), 64u);
    CaptureBytes(decl, 0x34 + 12 * n);
  }
}

void Frontend::CaptureTextures(uint8_t* base) {
  for (uint32_t slot = 0; slot < 32; ++slot) {
    std::array<uint32_t, 6> fetch{};
    for (uint32_t i = 0; i < 6; ++i) {
      const uint32_t reg = Pm4Mirror::kFetchConstantBase + slot * 6 + i;
      fetch[i] = capture_mirror_.written(reg)
                     ? capture_mirror_.reg(reg)
                     : Load32(base, cur_.device + kDev.fetch_constants + slot * 24 + i * 4);
    }
    if (!IsTextureBound(fetch[0])) continue;
    auto& entry = captured_textures_[fetch];
    bool dirty = !entry.snapshot;
    if (entry.snapshot && entry.checked_frame != front_frame_) {
      uint64_t total = 0;
      for (const auto& range : entry.snapshot->ranges) total += range.length;
      // The guest writes movie and UI textures without any hook and Horizon has no write
      // watch: small textures are verified every frame, big ones now and then.
      if (total <= options_.texture_per_frame_max_bytes ||
          front_frame_ - std::min(front_frame_, entry.checked_frame) >=
              options_.large_texture_recheck_frames)
        dirty = true;
      else
        dirty = false;
    }
    if (dirty) {
      std::string error;
      std::vector<guest::TextureRange> ranges;
      if (!guest::DescribeTextureRanges(fetch, ranges, error)) {
        cur_.texture_errors.emplace_back(slot, std::move(error));
        stats_.texture_failures++;
        continue;
      }
      entry.checked_frame = front_frame_;
      uint64_t hash = 0xcbf29ce484222325ull;
      for (const auto& range : ranges) {
        const uint8_t* bytes = access_.Readable(kPhysicalAlias + range.address, range.length);
        if (!bytes) {
          error = "Texture memory is not readable";
          break;
        }
        hash = XXH3_64bits_withSeed(bytes, range.length, hash);
      }
      if (!error.empty()) {
        cur_.texture_errors.emplace_back(slot, std::move(error));
        stats_.texture_failures++;
        continue;
      }
      if (!entry.snapshot || hash != entry.content_hash) {
        if (!guest::CaptureTexture(
                fetch, cur_.command_serial,
                [this](uint32_t address, uint32_t length) -> std::span<const uint8_t> {
                  const uint8_t* source = access_.Readable(address, length);
                  return source ? std::span<const uint8_t>(source, length)
                                : std::span<const uint8_t>{};
                },
                entry.snapshot, error)) {
          cur_.texture_errors.emplace_back(slot, std::move(error));
          stats_.texture_failures++;
          continue;
        }
        // Hash exactly the owned bytes: a loader may write between the check and the copy.
        hash = 0xcbf29ce484222325ull;
        for (const auto& range : entry.snapshot->ranges) {
          auto bytes = entry.snapshot->memory.Read(kPhysicalAlias + range.address, range.length);
          hash = XXH3_64bits_withSeed(bytes.data(), bytes.size(), hash);
        }
        entry.content_hash = hash;
      }
    }
    cur_.textures[slot] = entry.snapshot;
  }
  // Commands keep ownership even when the cache is pruned.
  if (captured_textures_.size() > 2048) {
    for (auto it = captured_textures_.begin(); it != captured_textures_.end();) {
      if (it->second.checked_frame + 120 < front_frame_)
        it = captured_textures_.erase(it);
      else
        ++it;
    }
  }
}

void Frontend::EndCmd(uint8_t* base) {
  if (stop_.load()) {
    cur_ = WorkCmd{};
    return;
  }
  const auto copy_draws_before = capture_mirror_.copy_draws;
  if (cur_ok_ && cur_.ring_bytes) {
    std::string error;
    cur_.pm4_capture_ok = guest::CapturePm4Dependencies(
        *batch_, cur_,
        [this](uint32_t address, uint32_t length) -> std::span<const uint8_t> {
          const uint8_t* source = access_.Readable(address, length);
          return source ? std::span<const uint8_t>(source, length) : std::span<const uint8_t>{};
        },
        error, &capture_mirror_);
    if (!cur_.pm4_capture_ok) {
      ++stats_.capture_failures;
      if (stats_.capture_failures <= 8) Warn("native PM4 capture: " + error);
    }
  }
  if (cur_ok_ && cur_.device &&
      (cur_.op == Op::kDraw || cur_.op == Op::kDrawIndexed || cur_.op == Op::kDrawInline)) {
    // After the PM4 scan, so the fetch constants it wrote are current.
    CaptureTextures(base);
  }
  if (cur_.op == Op::kResolve) {
    cur_.resolve_copy_draw = capture_mirror_.copy_draws != copy_draws_before;
    cur_.resolve_copy_dest_info = capture_mirror_.last_copy_dest_info;
  }
  if (!cur_ok_) {
    // Some referenced memory was unreadable: replaying a half-captured command would feed
    // the renderer garbage, so the command is dropped and counted.
    ++stats_.capture_failures;
    if (stats_.capture_failures <= 8)
      Warn("native capture: dropped op " + std::to_string(int(cur_.op)) +
           " (referenced guest memory is not readable)");
    batch_->ranges.resize(cur_.range_first);
    batch_->streams.resize(cur_.stream_first);
    cur_ = WorkCmd{};
    return;
  }
  cur_.range_count = uint32_t(batch_->ranges.size()) - cur_.range_first;
  cur_.stream_count = uint32_t(batch_->streams.size()) - cur_.stream_first;
  batch_->cmds.push_back(cur_);
  ++stats_.commands;
  if (cur_.op == Op::kBeginTiling) capture_tiling_active_ = true;
  if (cur_.op == Op::kEndTiling) capture_tiling_active_ = false;
  if (batch_->cmds.size() >= options_.batch_max_commands ||
      batch_->bytes.size() >= options_.batch_max_bytes)
    FlushBatch();
}

// ---------------------------------------------------------------------------
// Queue and worker
// ---------------------------------------------------------------------------

void Frontend::FlushBatch() {
  if (!batch_ || batch_->cmds.empty()) return;
  std::unique_ptr<WorkBatch> next;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    work_queue_.push_back(std::move(batch_));
    ++batches_submitted_;
    ++stats_.batches;
    if (!free_batches_.empty()) {
      next = std::move(free_batches_.back());
      free_batches_.pop_back();
    }
  }
  queue_cv_.notify_one();
  batch_ = next ? std::move(next) : std::make_unique<WorkBatch>();
}

void Frontend::WaitWorkerIdle(uint64_t batches) {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  done_cv_.wait(lock, [this, batches] { return stop_.load() || batches_done_ >= batches; });
}

void Frontend::WorkerMain() {
  for (;;) {
    std::unique_ptr<WorkBatch> batch;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return stop_.load() || !work_queue_.empty(); });
      if (stop_.load()) return;
      batch = std::move(work_queue_.front());
      work_queue_.pop_front();
    }
    for (const WorkCmd& cmd : batch->cmds) Execute(*batch, cmd);
    batch->Clear();
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      free_batches_.push_back(std::move(batch));
      ++batches_done_;
    }
    done_cv_.notify_all();
  }
}

void Frontend::Execute(const WorkBatch& batch, const WorkCmd& cmd) {
  if (stop_.load() || sink_failed_) return;
  guest::RenderPacket packet;
  std::string error;
  if (!guest::ReplayCapturedRenderPacket(batch, cmd, mirror_, packet, error)) {
    // One undecodable command costs a draw, not the session.
    ++stats_.decode_failures;
    if (stats_.decode_failures <= 16)
      Warn("native packet decode (op " + std::to_string(int(cmd.op)) + "): " + error);
    return;
  }
  if (!sink_(std::move(packet), error)) {
    sink_failed_ = true;
    ++stats_.sink_failures;
    if (!stop_.load()) Warn("native renderer stopped: " + error);
  }
}

// ---------------------------------------------------------------------------
// Buffer tracking
// ---------------------------------------------------------------------------

bool Frontend::RefreshTrackedBuffer(TrackedBuffer& t) {
  // Small buffers (the cloth vertices live in ~2.5 KB streams) are also hashed once per
  // guest frame: their rewrites do not always go through an Unlock.
  if (t.size <= kHashedBufferMax && t.hashed_frame != front_frame_) {
    t.hashed_frame = front_frame_;
    if (const uint8_t* bytes = access_.ReadablePhysical(t.address & 0x1FFFFFFF, t.size)) {
      const uint64_t hash = XXH3_64bits(bytes, t.size);
      if (t.hash_valid && hash != t.content_hash) {
        t.dirty = true;
        t.clean.clear();
      }
      t.content_hash = hash;
      t.hash_valid = true;
    }
  }
  return t.dirty;
}

Frontend::BufferPlan Frontend::PlanBuffer(uint32_t address, uint32_t size, uint32_t decl,
                                          uint32_t stride, uint32_t index_format, uint32_t phase,
                                          uint32_t need_begin, uint32_t need_end, bool& ok,
                                          uint32_t reset_index) {
  BufferPlan plan;
  ok = size && size <= (64u << 20);
  if (!ok) return plan;
  const uint64_t key_parts[] = {address, size, decl, stride, index_format, phase, reset_index};
  plan.key = XXH3_64bits(key_parts, sizeof(key_parts));
  plan.address = address;
  plan.size = size;
  plan.decl = decl;
  plan.stride = stride;
  plan.format = index_format;
  plan.phase = phase;
  plan.reset_index = reset_index;
  // Vertex data is swapped per vertex: the swap range must start on a vertex.
  auto align_range = [&](uint32_t& b, uint32_t& e) {
    e = std::min(e, size);
    if ((index_format & 3) == 1 && (index_format >> 2) < 2) {
      b &= ~1u;
      e = (e + 1) & ~1u;
    } else if (index_format & 3) {
      b &= ~3u;
      e = (e + 3) & ~3u;
    } else if (stride) {
      b = b < phase ? phase : b - ((b - phase) % stride);
      const uint32_t n = (e > b ? e - b + stride - 1 : 0) / stride;
      e = std::min(b + n * stride, size - ((size - phase) % stride));
    }
    e = std::min(e, size);
  };
  auto it = tracked_.find(plan.key);
  if (it != tracked_.end()) {
    TrackedBuffer& t = it->second;
    RefreshTrackedBuffer(t);
    if (!t.dirty) return plan;
    uint32_t b = need_begin, e = need_end;
    align_range(b, e);
    if (e <= b) return plan;
    for (const auto& [cb, ce] : t.clean) {
      if (cb <= b && e <= ce) return plan;
    }
    if (!CaptureBytes(kPhysicalAlias + address + b, e - b)) {
      ok = false;
      return plan;
    }
    plan.action = 1;
    plan.begin = b;
    plan.end = e;
    ++stats_.buffer_uploads;
    stats_.buffer_upload_bytes += e - b;
    t.clean.emplace_back(b, e);
    if (t.clean.size() > 64) {
      // Too fragmented: start over (later draws upload their ranges again).
      t.clean.clear();
      t.dirty = true;
    }
    return plan;
  }
  if (!CaptureBytes(kPhysicalAlias + address, size)) {
    ok = false;
    return plan;
  }
  TrackedBuffer& t = tracked_[plan.key];
  t.address = address;
  t.size = size;
  if (size <= kHashedBufferMax) {
    if (const uint8_t* bytes = access_.ReadablePhysical(address & 0x1FFFFFFF, size)) {
      t.content_hash = XXH3_64bits(bytes, size);
      t.hash_valid = true;
      t.hashed_frame = front_frame_;
    }
  }
  for (uint32_t page = (address & 0x1FFFFFFF) >> 16; page <= ((address & 0x1FFFFFFF) + size - 1) >> 16;
       ++page) {
    buffer_pages_[page].push_back(&t);
  }
  ++stats_.buffers_tracked;
  ++stats_.buffer_uploads;
  stats_.buffer_upload_bytes += size;
  plan.action = 2;
  plan.begin = 0;
  plan.end = size;
  return plan;
}

void Frontend::InvalidateGuestRange(uint32_t address, uint32_t size) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  if (!size) return;
  // Physical addresses of buffers are compared on the low 29 bits.
  const uint32_t a = address & 0x1FFFFFFF;
  const uint64_t stamp = ++invalidation_stamp_;
  for (uint32_t page = a >> 16; page <= (a + size - 1) >> 16; ++page) {
    auto pit = buffer_pages_.find(page);
    if (pit == buffer_pages_.end()) continue;
    for (TrackedBuffer* tp : pit->second) {
      TrackedBuffer& t = *tp;
      if (t.invalidation_stamp == stamp) continue;  // seen on an earlier page
      t.invalidation_stamp = stamp;
      const uint32_t b = t.address & 0x1FFFFFFF;
      if (t.dirty && t.clean.empty()) continue;  // already fully dirty
      if (b < a + size && a < b + t.size) {
        t.dirty = true;
        t.clean.clear();
      }
    }
  }
}

bool Frontend::DynamicVertexFetch(uint8_t* base, uint32_t dev) {
  for (uint32_t off : {kDev.shader_a, kDev.shader_b}) {
    const auto shader = Shader(Load32(base, dev + off));
    if (shader && shader->vertex && shader->dynamic_vertex_fetch) return true;
  }
  return false;
}

void Frontend::ResolveRange(VertexRange& range) {
  if (range.resolved) return;
  range.resolved = true;
  const uint32_t isize = range.index32 ? 4 : 2;
  if (!range.ib_phys || uint64_t(range.start_index + range.index_count) * isize > range.ib_size) return;
  const uint8_t* idx = access_.ReadablePhysical(range.ib_phys & 0x1FFFFFFF, range.ib_size);
  if (!idx) return;
  uint32_t lo = ~0u, hi = 0;
  const uint32_t end_index = range.start_index + range.index_count;
  for (uint32_t i = range.start_index; i < end_index; ++i) {
    uint32_t x = guest::LoadIndex(idx, i, range.index32, range.index_endian);
    if (range.index32) x &= 0xFFFFFFu;
    if (x == range.reset_index) continue;
    lo = std::min(lo, x);
    hi = std::max(hi, x);
  }
  if (lo > hi) return;
  const int64_t f = int64_t(lo) + range.base_vertex, e = int64_t(hi) + range.base_vertex + 1;
  if (f >= 0 && e > f) {
    range.first = uint32_t(f);
    range.end = uint32_t(e);
  }
}

bool Frontend::PlanStreams(uint8_t* base, uint32_t dev, uint32_t decl, VertexRange* range) {
  if (!decl) return false;
  if (DynamicVertexFetch(base, dev)) range = nullptr;
  const uint32_t decl_count = std::min(Load32(base, decl + 0x18), 64u);
  uint32_t streams_used = 0;
  for (uint32_t i = 0; i < decl_count; ++i) {
    const uint32_t e = decl + 0x34 + 12 * i;
    const uint32_t s = Load16(base, e);
    if (s == 0xFF) break;
    if (s < 16) streams_used |= 1u << s;
  }
  for (uint32_t s = 0; s < 16; ++s) {
    if (!(streams_used & (1u << s))) continue;
    // Vertex fetch constant for stream s (XDK: slot 95 - s).
    const uint32_t fetch0 = Load32(base, dev + kDevVertexFetch0 - 8 * s);
    const uint32_t fetch1 = Load32(base, dev + kDevVertexFetch0 - 8 * s + 4);
    const uint32_t address = fetch0 & ~3u;
    const uint32_t size = ((fetch1 >> 2) & 0xFFFFFF) * 4;
    const uint32_t stride = Load8(base, dev + kDev.stream_strides + s) * 4;
    if (!address || !size || !stride) return false;
    // Cache the whole guest vertex buffer (SetStreamSource offsets vary per draw into large
    // shared buffers) and bind at an offset.
    uint32_t buffer_base = address, buffer_size = size;
    if (const uint32_t vb_object = Load32(base, dev + kDev.stream_buffers + 4 * s)) {
      const uint32_t v = Load32(base, vb_object + 0x18) & ~3u;
      const uint32_t phys = (v & 0x1FFFFFFFu) + (v >= 0xE0000000u ? 0x1000u : 0u);
      const uint32_t full = ((Load32(base, vb_object + 0x1C) >> 2) & 0xFFFFFF) * 4;
      if (phys <= address && address + size <= phys + full) {
        buffer_base = phys;
        buffer_size = full;
      }
    }
    const uint32_t offset = address - buffer_base;
    const uint32_t phase = offset % stride;
    StreamPlan sp;
    sp.stream = s;
    sp.offset = offset;
    sp.size = size;
    sp.stride = stride;
    FrontStreamCache& sc = front_stream_cache_[s];
    if (sc.tracked && sc.address == buffer_base && sc.size == buffer_size && sc.decl == decl &&
        sc.stride == stride && sc.phase == phase && !RefreshTrackedBuffer(*sc.tracked)) {
      // Clean and cached: no lookup, no range needed.
      sp.buffer.key = sc.key;
      sp.buffer.address = buffer_base;
      sp.buffer.size = buffer_size;
      sp.buffer.decl = decl;
      sp.buffer.stride = stride;
      sp.buffer.format = s << 8;
      sp.buffer.phase = phase;
    } else {
      uint32_t need_begin = 0, need_end = ~0u;
      if (range) {
        ResolveRange(*range);
        if (range->end != ~0u) {
          const uint64_t b = uint64_t(offset) + uint64_t(range->first) * stride;
          const uint64_t e = uint64_t(offset) + uint64_t(range->end) * stride;
          need_begin = uint32_t(std::min<uint64_t>(b, buffer_size));
          need_end = uint32_t(std::min<uint64_t>(e, buffer_size));
        }
      }
      bool ok = false;
      sp.buffer = PlanBuffer(buffer_base, buffer_size, decl, stride, s << 8, phase, need_begin,
                             need_end, ok);
      if (!ok) return false;
      auto it = tracked_.find(sp.buffer.key);
      sc.address = buffer_base;
      sc.size = buffer_size;
      sc.decl = decl;
      sc.stride = stride;
      sc.format = s << 8;
      sc.phase = phase;
      sc.key = sp.buffer.key;
      sc.tracked = it != tracked_.end() ? &it->second : nullptr;
    }
    batch_->streams.push_back(sp);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Hook entry points
// ---------------------------------------------------------------------------

void Frontend::DrawVertices(uint8_t* base, uint32_t prim, uint32_t start_vertex,
                            uint32_t vertex_count) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  const uint32_t dev = GuestDevice();
  if (!dev || stop_.load()) return;
  if (prim == kPrimRectangleList) {
    // Rect lists need CPU expansion: read stream 0 through the physical view (0xA0000000
    // maps physical memory 1:1) and draw inline.
    const uint32_t fetch0 = Load32(base, dev + kDevVertexFetch0);
    const uint32_t stride = Load8(base, dev + kDev.stream_strides) * 4;
    const uint32_t phys = fetch0 & ~3u;
    if (!phys || !stride) return;
    const uint32_t data = kPhysicalAlias + phys + start_vertex * stride;
    BeginCmd(Op::kDrawInline);
    CaptureRing(base, dev);
    CaptureDevice(base, dev);
    CaptureBytes(data, vertex_count * stride);
    cur_.u[0] = prim;
    cur_.u[1] = data;
    cur_.u[2] = vertex_count;
    cur_.u[3] = stride;
    EndCmd(base);
    return;
  }
  BeginCmd(Op::kDraw);
  CaptureRing(base, dev);
  CaptureDevice(base, dev);
  cur_.u[0] = prim;
  cur_.u[1] = start_vertex;
  cur_.u[2] = vertex_count;
  VertexRange draw_range;
  draw_range.first = start_vertex;
  draw_range.end = start_vertex + vertex_count;
  cur_.streams_ok = PlanStreams(base, dev, Load32(base, dev + kDev.vertex_decl), &draw_range);
  EndCmd(base);
}

void Frontend::DrawIndexedVertices(uint8_t* base, uint32_t prim, int32_t base_vertex,
                                   uint32_t start_index, uint32_t index_count) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  const uint32_t dev = GuestDevice();
  if (!dev || stop_.load()) return;
  BeginCmd(Op::kDrawIndexed);
  CaptureRing(base, dev);
  CaptureDevice(base, dev);
  cur_.u[0] = prim;
  cur_.u[1] = uint32_t(base_vertex);
  cur_.u[2] = start_index;
  cur_.u[3] = index_count;
  // Vertex range referenced by the indices, scanned only if a dynamic vertex buffer of this
  // draw is dirty.
  VertexRange draw_range;
  if (const uint32_t ib_object = Load32(base, dev + kDev.index_buffer)) {
    const uint32_t header = Load32(base, ib_object);
    const bool index32 = (header & 0x80000000u) != 0;
    const uint32_t endian = (header >> 29) & 3;
    // +0x18 is a guest *virtual* address: convert like the XDK draw prologue (physical = low
    // 29 bits, +4 KB for the 0xE0000000 view).
    const uint32_t v = Load32(base, ib_object + 0x18);
    const uint32_t address = (v & 0x1FFFFFFFu) + (v >= 0xE0000000u ? 0x1000u : 0u);
    uint32_t size = Load32(base, ib_object + 0x1C) & 0x00FFFFFFu;
    if (endian >= 2) size = (size + 3) & ~3u;
    draw_range.resolved = false;
    draw_range.ib_phys = address;
    draw_range.ib_size = size;
    draw_range.start_index = start_index;
    draw_range.index_count = index_count;
    draw_range.base_vertex = base_vertex;
    draw_range.index32 = index32;
    draw_range.index_endian = endian;
    const bool strip = prim == kPrimTriangleStrip || prim == kPrimLineStrip;
    uint32_t reset_index = UINT32_MAX;
    if (strip && (LoadReg(base, dev, kRegPaSuScModeCntl) & (1u << 21))) {
      const uint32_t reset = LoadReg(base, dev, kRegMultiPrimIbResetIndex) & 0xFFFFFFu;
      if (index32 || reset <= UINT16_MAX) reset_index = reset;
    }
    draw_range.reset_index = reset_index;
    const uint32_t isize = index32 ? 4 : 2;
    bool ok = false;
    cur_.index = PlanBuffer(address, size, 0, 0, (index32 ? 2u : 1u) | (endian << 2), 0,
                            start_index * isize, (start_index + index_count) * isize, ok,
                            reset_index);
    cur_.has_index = ok;
    cur_.index32 = index32;
    cur_.index_size = size;
    if (prim == kPrimQuadList && ok && uint64_t(start_index + index_count) * isize <= size) {
      // Quads have no host topology: every group of 4 indices (a b c d) becomes the triangles
      // (a b c) (a c d). The expanded list is captured with the command; base_vertex stays a
      // draw argument.
      const uint8_t* idx = access_.ReadablePhysical(address & 0x1FFFFFFF, size);
      std::vector<uint32_t> tris;
      std::string error;
      if (!idx || !guest::NormalizeIndices({idx, size}, start_index, index_count,
                                           {index32, endian, UINT32_MAX},
                                           guest::Primitive::kQuads, tris, error)) {
        cur_.has_index = false;
        Warn("native: quad normalization failed: " + error);
      }
      cur_.u[4] = uint32_t(batch_->bytes.size());
      cur_.u[5] = uint32_t(tris.size());
      batch_->bytes.insert(batch_->bytes.end(), reinterpret_cast<const uint8_t*>(tris.data()),
                           reinterpret_cast<const uint8_t*>(tris.data()) + tris.size() * 4);
    }
  }
  cur_.streams_ok = PlanStreams(base, dev, Load32(base, dev + kDev.vertex_decl), &draw_range);
  EndCmd(base);
}

void Frontend::DrawInlineVertices(uint8_t* base, uint32_t prim, uint32_t data,
                                  uint32_t vertex_count, uint32_t stride) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  const uint32_t dev = GuestDevice();
  if (!dev || stop_.load()) return;
  BeginCmd(Op::kDrawInline);
  CaptureRing(base, dev);
  CaptureDevice(base, dev);
  if (data && stride) CaptureBytes(data, vertex_count * stride);
  cur_.u[0] = prim;
  cur_.u[1] = data;
  cur_.u[2] = vertex_count;
  cur_.u[3] = stride;
  EndCmd(base);
}

void Frontend::Resolve(uint8_t* base, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
                       uint32_t dest_point, uint32_t clear_color, float clear_z,
                       uint32_t clear_stencil, uint32_t level, uint32_t slice) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  const uint32_t dev = GuestDevice();
  if (!dev || stop_.load()) return;
  BeginCmd(Op::kResolve);
  CaptureRing(base, dev);
  CaptureDevice(base, dev);
  if (src_rect) CaptureBytes(src_rect, 16);
  if (dest_point) CaptureBytes(dest_point, 8);
  if (dest_texture) CaptureBytes(dest_texture, 0x40);
  if (clear_color) CaptureBytes(clear_color, 16);
  cur_.u[0] = flags;
  cur_.u[1] = src_rect;
  cur_.u[2] = dest_texture;
  cur_.u[3] = dest_point;
  cur_.u[4] = clear_color;
  cur_.u[5] = clear_stencil;
  cur_.u[6] = level;
  cur_.u[7] = slice;
  cur_.f = clear_z;
  EndCmd(base);
}

void Frontend::BeginTiling(uint8_t* base, uint32_t count, uint32_t rects, uint32_t clear_color,
                           float clear_z, uint32_t clear_stencil) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  if (stop_.load()) return;
  const uint32_t dev = GuestDevice();
  BeginCmd(Op::kBeginTiling);
  if (dev) {
    CaptureRing(base, dev);
    CaptureDevice(base, dev);
  }
  if (rects && count) CaptureBytes(rects, 16 * std::min(count, 64u));
  if (clear_color) CaptureBytes(clear_color, 16);
  cur_.u[0] = count;
  cur_.u[1] = rects;
  cur_.u[2] = clear_color;
  cur_.u[3] = clear_stencil;
  cur_.f = clear_z;
  EndCmd(base);
}

void Frontend::EndTiling(uint8_t* base) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  if (stop_.load()) return;
  BeginCmd(Op::kEndTiling);
  EndCmd(base);
}

void Frontend::Clear(uint8_t* base, uint32_t count, uint32_t rects, uint32_t flags,
                     const float color[4], float z, uint32_t stencil) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  const uint32_t dev = GuestDevice();
  if (!dev || stop_.load()) return;
  BeginCmd(Op::kClear);
  CaptureRing(base, dev);
  CaptureDevice(base, dev);
  if (rects && count) CaptureBytes(rects, 16 * std::min(count, 64u));
  cur_.u[0] = rects ? std::min(count, 64u) : 0;
  cur_.u[1] = rects;
  cur_.u[2] = flags;
  std::memcpy(&cur_.u[3], color, 4 * sizeof(float));
  cur_.u[7] = stencil;
  cur_.f = z;
  EndCmd(base);
}

void Frontend::SyncRing(uint8_t* base, uint32_t dev) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  if (stop_.load()) return;
  BeginCmd(Op::kRing);
  CaptureRing(base, dev);
  EndCmd(base);
}

void Frontend::ResyncRing(uint8_t* base, uint32_t dev) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  if (dev) ring_last_ = Load32(base, dev + kDev.ring_write) + 4;
}

void Frontend::OnSwap(uint8_t* base, uint32_t front_buffer_texture, uint64_t swap_number) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  if (stop_.load()) return;
  ++front_frame_;
  ++stats_.swaps;
  BeginCmd(Op::kSwap);
  CaptureRing(base, GuestDevice());
  if (front_buffer_texture) CaptureBytes(front_buffer_texture, 0x40);
  cur_.u[0] = front_buffer_texture;
  cur_.u64 = swap_number;
  auto gamma = std::make_shared<std::array<uint32_t, 256>>();
  const bool available = gamma_source_ && gamma_source_(*gamma);
  cur_.gamma_enabled = available && std::any_of(gamma->begin(), gamma->end(),
                                                [](uint32_t entry) { return entry != 0; });
  cur_.gamma = std::move(gamma);
  EndCmd(base);
  FlushBatch();
  uint64_t submitted;
  {
    std::lock_guard<std::mutex> qlock(queue_mutex_);
    submitted = batches_submitted_;
  }
  // Lag 0: the frame is recorded before the guest continues. Lag 1: the worker may still be
  // recording this frame while the guest builds the next one (everything a command reads is
  // captured, so the guest cannot disturb it).
  WaitWorkerIdle(options_.worker_lag ? prev_swap_batches_ : submitted);
  prev_swap_batches_ = submitted;
}

}  // namespace superman_returns::native
