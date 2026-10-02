// Native graphics system: SDK adapter around the PM4 executor in sr_native_ring.
// Adapted from nfsmw-nx nfsmw_nativo_sistema.cpp (revision df2de32); the NFSMW hooks,
// shader library and scene paths are intentionally not imported. See docs/native-renderer.md.

#include "sr_native_system.h"

#include "sr_native_ring.h"
#include "sr_native_shader_safety.h"

#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

#include <fmt/format.h>

namespace sr::native {
namespace {

namespace xenos = rex::graphics::xenos;
using Clock = std::chrono::steady_clock;
using rex::X_STATUS;

constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x0000FFFF;
constexpr uint32_t kPhysicalLimit = 0x20000000;
constexpr uint32_t kMaxRingSizeLog2 = 26;  // (1 << (size_log2 + 3)) bytes must fit the physical space.
constexpr uint32_t kRegisterCount = uint32_t(rex::graphics::RegisterFile::kRegisterCount);
constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
constexpr uint32_t kRegD1GrphPrimarySurface = 0x1844;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;
constexpr auto kReportInterval = std::chrono::seconds(5);
constexpr auto kRepeatInterval = std::chrono::seconds(5);

// --- Physical memory access ----------------------------------------------------------
//
// TranslatePhysical only masks the address. Every access goes through the physical heap
// page table first: the range must be committed and carry the needed protection.

class PhysicalMemory {
 public:
  explicit PhysicalMemory(rex::memory::Memory* memory) : memory_(memory) {}

  // A Vd* pointer is either a canonical physical address or an alias the SDK recognizes.
  bool NormalizePointer(uint32_t pointer, uint32_t& physical) const {
    if (pointer < kPhysicalLimit) {
      physical = pointer;
      return true;
    }
    const uint32_t translated = memory_->GetPhysicalAddress(pointer);
    if (translated == UINT32_MAX || translated >= kPhysicalLimit) return false;
    physical = translated;
    return true;
  }

  bool Valid(uint32_t physical, uint64_t bytes, bool write) const {
    if (!bytes) return false;
    const uint64_t end = uint64_t(physical) + bytes;
    if (end > kPhysicalLimit) return false;
    rex::memory::VirtualHeap* heap = memory_->GetPhysicalHeap();
    if (!heap) return false;
    const uint32_t page = heap->page_size();
    const uint32_t need = rex::memory::kMemoryProtectRead |
                          (write ? uint32_t(rex::memory::kMemoryProtectWrite) : 0u);
    uint64_t cursor = physical;
    while (cursor < end) {
      rex::memory::HeapAllocationInfo info{};
      if (!heap->QueryRegionInfo(uint32_t(cursor), &info)) return false;
      if (!(info.state & rex::memory::kMemoryAllocationCommit)) return false;
      if ((info.protect & need) != need) return false;
      const uint64_t region_end = (cursor & ~uint64_t(page - 1)) + info.region_size;
      if (region_end <= cursor) return false;
      cursor = region_end;
    }
    return true;
  }

  const std::byte* Host(uint32_t physical) const {
    return memory_->TranslatePhysical<const std::byte*>(physical);
  }
  std::byte* HostWritable(uint32_t physical) const {
    return memory_->TranslatePhysical<std::byte*>(physical);
  }

  // The two low bits of a GPU address select the byte order.
  bool ReadWord(uint32_t address, uint32_t& value) const {
    const uint32_t physical = address & ~uint32_t(3);
    if (!Valid(physical, 4, false)) return false;
    uint32_t raw;
    std::memcpy(&raw, Host(physical), sizeof(raw));
    value = xenos::GpuSwap(raw, static_cast<xenos::Endian>(address & 3));
    return true;
  }
  bool WriteWord(uint32_t address, uint32_t value) const {
    const uint32_t physical = address & ~uint32_t(3);
    if (!Valid(physical, 4, true)) return false;
    const uint32_t raw = xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 3));
    std::memcpy(HostWritable(physical), &raw, sizeof(raw));
    return true;
  }

 private:
  rex::memory::Memory* memory_;
};

class NativeSystem;

// MMIO callbacks cannot be unregistered, so the registered context outlives every system:
// it is never freed, and callbacks only reach a system while `target` is set.
struct MmioBridge {
  std::atomic<NativeSystem*> target{nullptr};
  std::atomic<uint32_t> active{0};
  std::mutex mutex;
  rex::memory::Memory* registered_memory = nullptr;
};
MmioBridge& Bridge() {
  static MmioBridge* bridge = new MmioBridge;  // Intentionally leaked (see above).
  return *bridge;
}

class NativeSystem final : public rex::system::IGraphicsSystem {
 public:
  explicit NativeSystem(bool configuration_valid)
      : configuration_valid_(configuration_valid), registers_(new std::atomic<uint32_t>[kRegisterCount]) {
    for (uint32_t i = 0; i < kRegisterCount; ++i) registers_[i].store(0, std::memory_order_relaxed);
  }
  ~NativeSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (!configuration_valid_) {
      REXLOG_ERROR("[sr-native] invalid renderer configuration; native setup refused");
      return X_STATUS_UNSUCCESSFUL;
    }
    if (presenter_) return X_STATUS_SUCCESS;
    app_context_ = app_context;
    if (!provider_) {
      provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
      if (!provider_) {
        REXLOG_ERROR("[sr-native] unable to create the Vulkan device");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[sr-native] unable to create the presenter");
      provider_.reset();
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[sr-native] SDK provider and presenter created; no Xenos emulation");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    if (!configuration_valid_) return X_STATUS_UNSUCCESSFUL;
    if (!dispatcher || !kernel_state || !dispatcher->memory()) return X_STATUS_UNSUCCESSFUL;
    if (workers_started_) return X_STATUS_SUCCESS;
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    physical_ = std::make_unique<PhysicalMemory>(memory_);

    if (!RegisterMmio()) return X_STATUS_UNSUCCESSFUL;

    last_report_ = Clock::now();
    // Workers are created only after every resource they use is validated. A failure
    // while creating one leaves the group to stop and join whatever already started.
    workers_.reset(new WorkerGroup(&stop_));
    if (!StartWorker("sr-native vblank", [this]() { return VblankLoop(); }) ||
        !StartWorker("sr-native ring", [this]() { return RingLoop(); })) {
      Shutdown();
      return X_STATUS_UNSUCCESSFUL;
    }
    workers_started_ = true;
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[sr-native] interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    if (!physical_) {
      Fail("InitializeRingBuffer before guest GPU setup");
      return;
    }
    uint32_t base = 0;
    // Validate before the shift: an out-of-range size would be undefined behaviour.
    if (size_log2 > kMaxRingSizeLog2 || !physical_->NormalizePointer(ptr, base) || (base & 3)) {
      Fail(fmt::format("invalid ring pointer {:08X} size_log2 {}", ptr, size_log2));
      return;
    }
    const uint32_t words = uint32_t(1) << (size_log2 + 1);
    if (!physical_->Valid(base, uint64_t(words) * 4, false)) {
      Fail(fmt::format("ring {:08X} (+{} words) is not committed readable memory", base, words));
      return;
    }
    {
      std::lock_guard<std::mutex> lock(work_mutex_);
      ring_base_ = base;
      ring_words_ = words;
      ++ring_generation_;
    }
    work_changed_.notify_all();
    REXLOG_INFO("[sr-native] ring at {:08X}, {} words", base, words);
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    uint32_t physical = 0;
    if (!physical_ || !physical_->NormalizePointer(ptr, physical) || (physical & 3) ||
        !physical_->Valid(physical, 4, true)) {
      Fail(fmt::format("invalid read pointer write-back address {:08X}", ptr));
      return;
    }
    writeback_.store(physical, std::memory_order_release);
  }

  // Idempotent. Order: stop callbacks reaching us, stop and join workers (no lock held),
  // then release the presenter on the UI thread and the provider.
  void Shutdown() override {
    if (shutdown_started_.exchange(true)) return;
    DisableMmio();
    if (workers_) {
      stop_.Stop();
      { std::lock_guard<std::mutex> lock(work_mutex_); }
      work_changed_.notify_all();
      workers_->JoinAll();
      workers_.reset();
    }
    if (workers_started_ || ring_generation_) ReportSummary("final");
    if (presenter_) {
      if (app_context_) {
        app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      }
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  // --- Worker management ---------------------------------------------------------

  bool StartWorker(const char* name, std::function<int()> body) {
    rex::system::object_ref<rex::system::XHostThread> thread(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, std::move(body)));
    thread->set_name(name);
    if (XFAILED(thread->Create())) {
      REXLOG_ERROR("[sr-native] unable to create worker {}", name);
      return false;
    }
    workers_->Add([thread]() mutable {
      thread->Wait(0, 0, 0, nullptr);
      thread.reset();
    });
    return true;
  }

  void Fail(const std::string& reason) {
    if (!failed_.exchange(true)) {
      REXLOG_ERROR("[sr-native] FAILED: {}", reason);
    }
    { std::lock_guard<std::mutex> lock(work_mutex_); }
    work_changed_.notify_all();
  }

  // --- MMIO ------------------------------------------------------------------------

  bool RegisterMmio() {
    MmioBridge& bridge = Bridge();
    std::lock_guard<std::mutex> lock(bridge.mutex);
    if (bridge.registered_memory != memory_) {
      if (bridge.registered_memory) {
        REXLOG_ERROR("[sr-native] the GPU MMIO range belongs to another guest memory");
        return false;
      }
      if (!memory_->AddVirtualMappedRange(kMmioBase, kMmioMask, kMmioSize, &bridge, &MmioRead,
                                          &MmioWrite)) {
        REXLOG_ERROR("[sr-native] unable to register the GPU MMIO range");
        return false;
      }
      bridge.registered_memory = memory_;
    }
    bridge.target.store(this, std::memory_order_release);
    mmio_enabled_ = true;
    return true;
  }

  void DisableMmio() {
    if (!mmio_enabled_) return;
    MmioBridge& bridge = Bridge();
    NativeSystem* expected = this;
    bridge.target.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    // A guest callback may be inside this system right now; wait it out before release.
    while (bridge.active.load(std::memory_order_acquire) != 0) {
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    mmio_enabled_ = false;
  }

  static uint32_t MmioRead(void*, void* context, uint32_t address) {
    auto* bridge = static_cast<MmioBridge*>(context);
    bridge->active.fetch_add(1, std::memory_order_acq_rel);
    NativeSystem* system = bridge->target.load(std::memory_order_acquire);
    const uint32_t value = system ? system->ReadMmio(address) : 0;
    bridge->active.fetch_sub(1, std::memory_order_acq_rel);
    return value;
  }
  static void MmioWrite(void*, void* context, uint32_t address, uint32_t value) {
    auto* bridge = static_cast<MmioBridge*>(context);
    bridge->active.fetch_add(1, std::memory_order_acq_rel);
    if (NativeSystem* system = bridge->target.load(std::memory_order_acquire)) {
      system->WriteMmio(address, value);
    }
    bridge->active.fetch_sub(1, std::memory_order_acq_rel);
  }

  // Fixed values and effects follow GraphicsSystem::ReadRegister/WriteRegister.
  uint32_t ReadMmio(uint32_t address) {
    const uint32_t index = (address & 0xFFFF) / 4;
    switch (index) {
      case kRegRbEdramTiming: return 0x08100748;
      case kRegRbBcControl: return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus: return 1;
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return (std::min(uint32_t(mode.display_width), uint32_t(0x0FFF)) << 16) |
               std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      default: break;
    }
    return index < kRegisterCount ? registers_[index].load(std::memory_order_acquire) : 0;
  }

  void WriteMmio(uint32_t address, uint32_t value) {
    const uint32_t index = (address & 0xFFFF) / 4;
    if (index == kRegCpRbWptr) {
      write_pointer_.store(value, std::memory_order_release);
      mmio_wptr_writes_.fetch_add(1, std::memory_order_relaxed);
      { std::lock_guard<std::mutex> lock(work_mutex_); }  // No lost wakeup against the waiter.
      work_changed_.notify_all();
    } else if (index != kRegD1GrphPrimarySurface) {
      LogOnce("mmio-write", index, value);
    }
    if (index < kRegisterCount) registers_[index].store(value, std::memory_order_release);
  }

  void LogOnce(const char* kind, uint32_t index, uint32_t value) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (logged_.size() >= 64 || !logged_.insert({kind, index}).second) return;
    REXLOG_INFO("[sr-native] first {} of register {:04X} = {:08X}", kind, index, value);
  }

  // --- Services handed to the PM4 executor -----------------------------------------

  Services MakeServices() {
    Services s;
    s.read_register = [this](uint32_t index, uint32_t& value) { return ReadRegister(index, value); };
    s.write_register = [this](uint32_t index, uint32_t value) { return WriteRegister(index, value); };
    s.read_memory = [this](uint32_t address, uint32_t& value) { return physical_->ReadWord(address, value); };
    s.write_memory = [this](uint32_t address, uint32_t value) {
      return physical_->WriteWord(address, value);
    };
    s.read_indirect = [this](uint32_t address, uint32_t dwords, std::vector<std::byte>& out) {
      const uint64_t bytes = uint64_t(dwords) * 4;
      if (!physical_->Valid(address, bytes, false)) return false;
      out.resize(size_t(bytes));
      std::memcpy(out.data(), physical_->Host(address), size_t(bytes));
      return true;
    };
    s.interrupt = [this](uint32_t cpu) { return DeliverInterrupt(1, cpu); };
    s.present = [this](uint32_t a, uint32_t b, uint32_t c) { return Present(a, b, c); };
    s.shader_is_memory_safe = [this](uint32_t stage, std::span<const uint32_t> code) {
      return shader_safety_.IsSafe(stage, code);
    };
    s.cancelled = [this]() { return stop_.cancelled(); };
    s.pause_wait = [this]() {
      PublishReadPointer();
      stop_.WaitFor(std::chrono::milliseconds(1));
    };
    s.report_wait = [this](uint32_t info, uint32_t address, uint32_t reference, uint32_t mask) {
      ReportWait(info, address, reference, mask);
    };
    return s;
  }

  bool ReadRegister(uint32_t index, uint32_t& value) {
    if (index < kRegisterCount) {
      value = registers_[index].load(std::memory_order_acquire);
      return true;
    }
    const auto it = extended_registers_.find(index);
    value = it != extended_registers_.end() ? it->second : 0;
    return true;
  }

  // Includes the SDK's observable effects: scratch write-back and the COHER dirty flag.
  // A refused effect leaves the register unchanged.
  bool WriteRegister(uint32_t index, uint32_t value) {
    using namespace rex::graphics;
    if (index >= kRegisterCount) {
      extended_registers_[index] = value;
      return true;
    }
    if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t slot = index - XE_GPU_REG_SCRATCH_REG0;
      if ((1u << slot) & registers_[XE_GPU_REG_SCRATCH_UMSK].load(std::memory_order_acquire)) {
        const uint32_t address =
            registers_[XE_GPU_REG_SCRATCH_ADDR].load(std::memory_order_acquire) + slot * 4;
        if (!physical_->Valid(address, 4, true)) return false;
        rex::memory::store_and_swap<uint32_t>(physical_->HostWritable(address), value);
      }
      registers_[index].store(value, std::memory_order_release);
      return true;
    }
    if (index == XE_GPU_REG_COHER_STATUS_HOST) value |= 0x80000000u;
    registers_[index].store(value, std::memory_order_release);
    return true;
  }

  bool DeliverInterrupt(uint32_t source, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) return false;  // Not installed yet: the packet stays pending.
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) return false;
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
    if (source == 1) {
      interrupts_delivered_.fetch_add(1, std::memory_order_relaxed);
      RecordNativeProgress();
    }
    return true;
  }

  // Native presentation arrives with the Vulkan clear path. Until then a swap neither
  // completes nor is faked: it blocks and says so.
  bool Present(uint32_t, uint32_t, uint32_t) {
    if (!present_blocked_logged_.exchange(true)) {
      REXLOG_WARN("[sr-native] BLOCKED: swap received but native presentation is not available");
    }
    return false;
  }

  // --- Workers ------------------------------------------------------------------------

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    while (!stop_.cancelled()) {
      const auto now = Clock::now();
      if (now - next > std::chrono::milliseconds(250)) next = now;  // No replay after a long pause.
      while (now >= next && !stop_.cancelled()) {
        vblanks_.fetch_add(1, std::memory_order_relaxed);
        DeliverInterrupt(0, 2);  // Timer only: never counted as game progress.
        next += interval;
      }
      stop_.WaitFor(std::chrono::milliseconds(1));
    }
    return 0;
  }

  struct RingSnapshot {
    uint32_t base = 0, words = 0, generation = 0;
  };

  int RingLoop() {
    RingGeneration ring;
    std::unique_ptr<RingExecutor> executor;
    RingSnapshot snapshot;
    uint32_t seen_write = UINT32_MAX;
    while (!stop_.cancelled()) {
      {
        std::unique_lock<std::mutex> lock(work_mutex_);
        work_changed_.wait_for(lock, std::chrono::milliseconds(4), [&] {
          return stop_.cancelled() || failed_.load(std::memory_order_acquire) ||
                 ring_generation_ != ring.generation ||
                 write_pointer_.load(std::memory_order_acquire) != seen_write || retry_pending_;
        });
        snapshot = {ring_base_, ring_words_, ring_generation_};
      }
      if (stop_.cancelled()) break;
      MaybeReport();
      if (failed_.load(std::memory_order_acquire)) continue;  // Terminal: idle until shutdown.
      if (snapshot.generation != ring.generation) {
        ResetOnGeneration(ring, snapshot.generation);
        executor = std::make_unique<RingExecutor>(MakeServices());
        seen_write = UINT32_MAX;
        published_read_ = 0;
        retry_pending_ = false;
      }
      if (!executor || !snapshot.words) continue;
      const uint32_t write = write_pointer_.load(std::memory_order_acquire);
      if (!physical_->Valid(snapshot.base, uint64_t(snapshot.words) * 4, false)) {
        Fail("ring memory is no longer committed readable memory");
        continue;
      }
      seen_write = write;
      Cursor cursor{std::span<const std::byte>(physical_->Host(snapshot.base), size_t(snapshot.words) * 4),
                    ring.read_word, write & (snapshot.words - 1), snapshot.words - 1};
      active_cursor_ = &cursor;
      PacketResult result;
      for (;;) {
        result = executor->ProcessNext(cursor);
        if (result != PacketResult::kConsumed) break;
        RecordNativeProgress();
      }
      active_cursor_ = nullptr;
      ring.read_word = cursor.position;
      PublishReadPointer(ring.read_word);
      last_counters_ = executor->counters();
      switch (result) {
        case PacketResult::kIncomplete:
          retry_pending_ = false;
          break;
        case PacketResult::kBlocked: {
          ReportBlocked(executor->counters(), ring.read_word);
          retry_pending_ = true;
          stop_.WaitFor(std::chrono::milliseconds(1));
          break;
        }
        case PacketResult::kInvalid:
          Fail(fmt::format("invalid PM4 packet at ring word {} (opcode area {:02X})", ring.read_word,
                           executor->counters().last_blocked_opcode));
          break;
        case PacketResult::kCancelled:
          return 0;
        default:
          break;
      }
    }
    return 0;
  }

  // The read pointer only reports packets that were fully consumed.
  void PublishReadPointer(uint32_t position) {
    const uint32_t address = writeback_.load(std::memory_order_acquire);
    if (!address || position == published_read_) return;
    if (!physical_->Valid(address, 4, true)) {
      Fail(fmt::format("read pointer write-back {:08X} is no longer writable memory", address));
      return;
    }
    rex::memory::store_and_swap<uint32_t>(physical_->HostWritable(address), position);
    published_read_ = position;
    RecordNativeProgress();
  }
  // During a long WAIT_REG_MEM the earlier packets of the batch are already consumed.
  void PublishReadPointer() {
    if (active_cursor_) PublishReadPointer(active_cursor_->position);
  }

  // --- Diagnostics -----------------------------------------------------------------------

  void ReportBlocked(const RingCounters& counters, uint32_t ring_word) {
    const auto now = Clock::now();
    const bool first = !blocked_reported_;
    if (!first && now - last_blocked_report_ < kRepeatInterval) return;
    blocked_reported_ = true;
    last_blocked_report_ = now;
    REXLOG_WARN("[sr-native] BLOCKED opcode={:02X} at={:08X} ring_word={} blocked={} shader_blocked={} "
                "last_progress={}",
                counters.last_blocked_opcode, counters.last_blocked_address, ring_word,
                counters.blocked, counters.draws_shader_blocked, NativeProgress());
  }

  void ReportWait(uint32_t info, uint32_t address, uint32_t reference, uint32_t mask) {
    const auto now = Clock::now();
    if (wait_reported_ && now - last_wait_report_ < kRepeatInterval) return;
    wait_reported_ = true;
    last_wait_report_ = now;
    REXLOG_WARN("[sr-native] WAIT pending info={:08X} address={:08X} reference={:08X} mask={:08X}", info,
                address, reference, mask);
  }

  void MaybeReport() {
    if (Clock::now() - last_report_ >= kReportInterval) ReportSummary("interval");
  }

  void ReportSummary(const char* kind) {
    last_report_ = Clock::now();
    const RingCounters& c = last_counters_;
    REXLOG_INFO(
        "[sr-native] summary kind={} packets={} indirects={} swaps={} refreshes={} draws_omitted={} "
        "blocked={} invalid={} interrupts={} vblanks={} wptr_writes={} progress={}{}",
        kind, c.packets, c.indirects, c.swap_requests, c.refresh_completed, c.draws_omitted, c.blocked,
        c.invalid, c.interrupts, vblanks_.load(std::memory_order_relaxed),
        mmio_wptr_writes_.load(std::memory_order_relaxed), NativeProgress(),
        std::string_view(kind) == "final" ? " shutdown=complete" : "");
  }

  // --- State ---------------------------------------------------------------------------------

  const bool configuration_valid_;
  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  std::unique_ptr<PhysicalMemory> physical_;

  ShaderSafetyCache shader_safety_;
  WorkerStop stop_;
  std::unique_ptr<WorkerGroup> workers_;
  bool workers_started_ = false;
  bool mmio_enabled_ = false;
  std::atomic<bool> shutdown_started_{false};
  std::atomic<bool> failed_{false};

  std::unique_ptr<std::atomic<uint32_t>[]> registers_;
  std::unordered_map<uint32_t, uint32_t> extended_registers_;  // Ring thread only.

  std::mutex work_mutex_;
  std::condition_variable work_changed_;
  uint32_t ring_base_ = 0, ring_words_ = 0, ring_generation_ = 0;
  bool retry_pending_ = false;  // Ring thread only.
  std::atomic<uint32_t> write_pointer_{0};
  std::atomic<uint32_t> writeback_{0};
  Cursor* active_cursor_ = nullptr;  // Ring thread only.
  uint32_t published_read_ = 0;

  std::atomic<uint32_t> callback_{0}, callback_data_{0};
  std::atomic<uint64_t> vblanks_{0}, interrupts_delivered_{0}, mmio_wptr_writes_{0};
  std::atomic<bool> present_blocked_logged_{false};

  std::mutex log_mutex_;
  std::set<std::pair<std::string, uint32_t>> logged_;
  RingCounters last_counters_;  // Ring thread writes; summary reads after join or on that thread.
  Clock::time_point last_report_, last_blocked_report_, last_wait_report_;
  bool blocked_reported_ = false, wait_reported_ = false;
};

}  // namespace

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem(bool configuration_valid) {
  return std::make_unique<NativeSystem>(configuration_valid);
}

}  // namespace sr::native
