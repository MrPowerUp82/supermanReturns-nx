// Guest-thread front end of the native Vulkan renderer.
//
// Ported from superman_returns_recomp (port/src/native_renderer/native_renderer.cpp,
// "Front end (guest threads)" and the Draw*/Resolve/Clear/OnSwap entry points) and cut
// down to what the Vulkan path needs: there is no D3D12 code here, no write-watch
// (Horizon cannot deliver the page faults), and every access to guest memory goes through
// GuestAccess so the unit can be tested on a host without the ReXGlue runtime.
//
// What it does, per hooked D3D call (the original has already run, so the XDK flushed its
// dirty state into the command segment):
//   1. copies the PM4 words written since the previous capture (CaptureRing),
//   2. copies the D3DDevice state the neutral decoder reads (CaptureDevice),
//   3. plans and copies the vertex/index/texture bytes the draw references,
//   4. queues the command; a worker thread replays the PM4 bytes through its own register
//      mirror and hands the decoded RenderPacket to the sink (the Vulkan recorder).
// Nothing a command reads from guest memory is read again after the hook returns, so the
// guest can overwrite it while the worker is still recording an earlier frame.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../graphics/guest/captured_batch.h"
#include "../graphics/guest/render_packet.h"
#include "../graphics/guest/shader_capture.h"
#include "../graphics/guest/texture_capture.h"
#include "pm4_mirror.h"

namespace superman_returns::native {

// Validated read access to guest memory. A null result means the range is not committed
// readable memory; the pointer stays valid at least until the caller copies from it.
class GuestAccess {
 public:
  virtual ~GuestAccess() = default;
  // `address` is a guest virtual address, or a cached physical alias (0xA0000000-0xBFFFFFFF).
  virtual const uint8_t* Readable(uint32_t address, uint32_t length) = 0;
  // Canonical GPU physical address (below 512 MB).
  virtual const uint8_t* ReadablePhysical(uint32_t physical, uint32_t length) = 0;
};

struct FrontendOptions {
  // Textures up to this many bytes are re-validated (hashed) once per guest frame: movie
  // and UI textures are written by the guest without any hook. Larger ones are only
  // re-checked every `large_texture_recheck_frames` frames (no write watch on Horizon).
  uint32_t texture_per_frame_max_bytes = 4u << 20;
  uint32_t large_texture_recheck_frames = 30;
  // Let the worker finish frame N while the guest builds frame N+1.
  bool worker_lag = true;
  uint32_t batch_max_commands = 32;
  uint32_t batch_max_bytes = 4u << 20;
};

struct FrontendStats {
  std::atomic<uint64_t> commands{0}, batches{0}, swaps{0};
  std::atomic<uint64_t> capture_failures{0}, decode_failures{0}, sink_failures{0};
  std::atomic<uint64_t> texture_failures{0}, ring_resyncs{0};
  std::atomic<uint64_t> buffers_tracked{0}, buffer_uploads{0}, buffer_upload_bytes{0};
};

class Frontend {
 public:
  using PacketSink = std::function<bool(graphics::guest::RenderPacket&&, std::string&)>;
  using ShaderLookup =
      std::function<std::shared_ptr<const graphics::guest::ShaderCapture>(uint32_t object)>;
  // Fills the 256-entry display gamma ramp; false when the guest never programmed one.
  using GammaSource = std::function<bool(std::array<uint32_t, 256>&)>;
  using Logger = std::function<void(const std::string&)>;

  explicit Frontend(GuestAccess& access, FrontendOptions options = {});
  ~Frontend();
  Frontend(const Frontend&) = delete;
  Frontend& operator=(const Frontend&) = delete;

  void SetLoggers(Logger info, Logger warn);
  void SetShaderLookup(ShaderLookup lookup);
  void SetGammaSource(GammaSource source);

  // Starts the recording worker. `cancel` is invoked by Stop so a sink blocked on the GPU
  // can give up. False when already started or stopped.
  bool Start(PacketSink sink, std::function<void()> cancel = {});
  // Flushes nothing: queued commands are dropped. Joins the worker. Idempotent.
  void Stop();
  bool running() const { return started_ && !stop_.load(); }

  void NoteGuestDevice(uint32_t device);

  // Hook entry points. `base` is the guest memory base used for device-structure loads.
  void DrawVertices(uint8_t* base, uint32_t prim, uint32_t start_vertex, uint32_t vertex_count);
  void DrawIndexedVertices(uint8_t* base, uint32_t prim, int32_t base_vertex, uint32_t start_index,
                           uint32_t index_count);
  void DrawInlineVertices(uint8_t* base, uint32_t prim, uint32_t data, uint32_t vertex_count,
                          uint32_t stride);
  void Resolve(uint8_t* base, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
               uint32_t dest_point, uint32_t clear_color, float clear_z, uint32_t clear_stencil,
               uint32_t level, uint32_t slice);
  void BeginTiling(uint8_t* base, uint32_t count, uint32_t rects, uint32_t clear_color,
                   float clear_z, uint32_t clear_stencil);
  void EndTiling(uint8_t* base);
  void Clear(uint8_t* base, uint32_t count, uint32_t rects, uint32_t flags, const float color[4],
             float z, uint32_t stencil);
  void SyncRing(uint8_t* base, uint32_t dev);
  void ResyncRing(uint8_t* base, uint32_t dev);
  void OnSwap(uint8_t* base, uint32_t front_buffer_texture, uint64_t swap_number);
  // D3DVertexBuffer/IndexBuffer::Unlock: the guest rewrote the buffer.
  void InvalidateGuestRange(uint32_t address, uint32_t size);

  const FrontendStats& stats() const { return stats_; }

 private:
  using BufferPlan = graphics::guest::BufferPlan;
  using StreamPlan = graphics::guest::StreamPlan;
  using Op = graphics::guest::Op;
  using WorkCmd = graphics::guest::WorkCmd;
  using WorkBatch = graphics::guest::WorkBatch;

  struct TrackedBuffer {
    uint32_t address = 0, size = 0;
    bool dirty = false;
    std::vector<std::pair<uint32_t, uint32_t>> clean;  // [begin, end) while dirty
    uint64_t invalidation_stamp = 0;
    uint64_t content_hash = 0, hashed_frame = ~0ull;  // once per guest frame, small buffers
    bool hash_valid = false;
  };
  struct FrontStreamCache {
    uint32_t address = 0, size = 0, decl = 0, stride = 0, format = 0, phase = 0;
    uint64_t key = 0;
    TrackedBuffer* tracked = nullptr;
  };
  struct VertexRange {
    uint32_t first = 0, end = ~0u;
    bool resolved = true;
    uint32_t ib_phys = 0, ib_size = 0, start_index = 0, index_count = 0;
    int32_t base_vertex = 0;
    bool index32 = false;
    uint32_t index_endian = 0;
    uint32_t reset_index = UINT32_MAX;
  };
  struct CapturedTextureEntry {
    std::shared_ptr<const graphics::guest::TextureCapture> snapshot;
    uint64_t content_hash = 0, checked_frame = ~0ull;
  };

  static constexpr uint32_t kHashedBufferMax = 32 * 1024;

  void BeginCmd(Op op);
  void EndCmd(uint8_t* base);
  bool CaptureBytes(uint32_t address, uint32_t length);
  void CaptureTextures(uint8_t* base);
  void CaptureDevice(uint8_t* base, uint32_t dev);
  void CaptureRing(uint8_t* base, uint32_t dev);
  void FlushBatch();
  void WaitWorkerIdle(uint64_t batches);
  void WorkerMain();
  void Execute(const WorkBatch& batch, const WorkCmd& cmd);

  BufferPlan PlanBuffer(uint32_t address, uint32_t size, uint32_t decl, uint32_t stride,
                        uint32_t index_format, uint32_t phase, uint32_t need_begin,
                        uint32_t need_end, bool& ok, uint32_t reset_index = UINT32_MAX);
  bool PlanStreams(uint8_t* base, uint32_t dev, uint32_t decl, VertexRange* range);
  bool RefreshTrackedBuffer(TrackedBuffer& tracked);
  bool DynamicVertexFetch(uint8_t* base, uint32_t dev);
  void ResolveRange(VertexRange& range);
  std::shared_ptr<const graphics::guest::ShaderCapture> Shader(uint32_t object);
  uint32_t GuestDevice() const { return device_.load(std::memory_order_relaxed); }
  void Info(const std::string& text) const;
  void Warn(const std::string& text) const;

  GuestAccess& access_;
  FrontendOptions options_;
  FrontendStats stats_;
  Logger info_, warn_;
  ShaderLookup shader_lookup_;
  GammaSource gamma_source_;

  std::atomic<uint32_t> device_{0};
  std::mutex front_mutex_;
  std::unique_ptr<WorkBatch> batch_;
  WorkCmd cur_;
  bool cur_ok_ = true;
  uint64_t front_frame_ = 0;
  uint64_t capture_serial_ = 0;
  bool capture_tiling_active_ = false;
  uint32_t ring_last_ = 0;
  Pm4Mirror capture_mirror_;  // frontend scan (dependencies only)
  Pm4Mirror mirror_;          // worker replay (the register state decoders read)
  std::unordered_map<uint64_t, TrackedBuffer> tracked_;  // node-based: pointers stay valid
  std::unordered_map<uint32_t, std::vector<TrackedBuffer*>> buffer_pages_;
  uint64_t invalidation_stamp_ = 0;
  FrontStreamCache front_stream_cache_[17];
  std::map<std::array<uint32_t, 6>, CapturedTextureEntry> captured_textures_;

  PacketSink sink_;
  std::function<void()> cancel_sink_;
  bool started_ = false;
  std::atomic<bool> stop_{false};
  bool sink_failed_ = false;
  std::thread worker_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_, done_cv_;
  std::deque<std::unique_ptr<WorkBatch>> work_queue_;
  std::vector<std::unique_ptr<WorkBatch>> free_batches_;
  uint64_t batches_submitted_ = 0, batches_done_ = 0, prev_swap_batches_ = 0;
};

}  // namespace superman_returns::native
