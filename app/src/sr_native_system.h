#pragma once

// Native graphics system entry points and the SDK-independent lifecycle helpers.
// The SDK interface is forward-declared: include <rex/system/interfaces/graphics.h>
// where the unique_ptr is destroyed.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace rex::system {
class IGraphicsSystem;
}

namespace sr::native {

// Cooperative cancellation shared by the ring and vblank workers.
class WorkerStop {
 public:
  bool cancelled() const { return cancelled_.load(std::memory_order_acquire); }
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancelled_.store(true, std::memory_order_release);
    }
    changed_.notify_all();
  }
  // Returns when cancelled or after the duration; callers re-check cancelled().
  void WaitFor(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait_for(lock, duration, [this] { return cancelled(); });
  }

 private:
  std::atomic<bool> cancelled_{false};
  std::mutex mutex_;
  std::condition_variable changed_;
};

// A new ring generation restarts the read cursor; the same generation keeps it.
struct RingGeneration {
  uint32_t generation = 0;
  uint32_t read_word = 0;
};
inline void ResetOnGeneration(RingGeneration& ring, uint32_t generation) {
  if (ring.generation != generation) {
    ring.generation = generation;
    ring.read_word = 0;
  }
}

// Owns the stop-and-join action of every created worker. Each action runs once,
// newest first, after the optional stop signal is raised. Safe after partial setup.
class WorkerGroup {
 public:
  WorkerGroup() = default;
  explicit WorkerGroup(WorkerStop* stop) : stop_(stop) {}
  WorkerGroup(const WorkerGroup&) = delete;
  WorkerGroup& operator=(const WorkerGroup&) = delete;
  ~WorkerGroup() { JoinAll(); }

  void Add(std::function<void()> join) {
    std::lock_guard<std::mutex> lock(mutex_);
    joiners_.push_back(std::move(join));
  }
  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return joiners_.size();
  }
  void JoinAll() {
    std::vector<std::function<void()>> joiners;
    {
      // Taken out under the lock, joined without it: a worker may call Add/size.
      std::lock_guard<std::mutex> lock(mutex_);
      joiners.swap(joiners_);
    }
    if (stop_ && !joiners.empty()) stop_->Stop();
    for (auto it = joiners.rbegin(); it != joiners.rend(); ++it) (*it)();
  }

 private:
  WorkerStop* stop_ = nullptr;
  mutable std::mutex mutex_;
  std::vector<std::function<void()>> joiners_;
};

// Increases only after a packet was processed, a read-pointer write-back happened
// or an interrupt was delivered. The vblank timer alone is not game progress.
inline std::atomic<uint64_t>& NativeProgressCounter() {
  static std::atomic<uint64_t> counter{0};
  return counter;
}
inline uint64_t NativeProgress() { return NativeProgressCounter().load(std::memory_order_acquire); }
inline void RecordNativeProgress() { NativeProgressCounter().fetch_add(1, std::memory_order_acq_rel); }

// With configuration_valid=false the system's setup fails before any resource is made.
std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem(bool configuration_valid = true);

}  // namespace sr::native
