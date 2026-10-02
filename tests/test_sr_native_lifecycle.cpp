#include "sr_native_system.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

using namespace sr::native;
using namespace std::chrono_literals;

static void GenerationTests() {
  RingGeneration ring{1, 7};
  ResetOnGeneration(ring, 1);
  assert(ring.generation == 1 && ring.read_word == 7);
  ResetOnGeneration(ring, 2);
  assert(ring.generation == 2 && ring.read_word == 0);
  ring.read_word = 9;
  ResetOnGeneration(ring, 2);
  assert(ring.read_word == 9);
}

static void StopTests() {
  WorkerStop stop;
  assert(!stop.cancelled());
  stop.Stop();
  stop.Stop();
  assert(stop.cancelled());
  const auto start = std::chrono::steady_clock::now();
  stop.WaitFor(10s);  // Already cancelled: must return immediately.
  assert(std::chrono::steady_clock::now() - start < 2s);
}

static void StopDuringWaitTests() {
  WorkerStop stop;
  std::atomic<bool> entered{false};
  std::atomic<bool> left{false};
  std::thread worker([&] {
    entered = true;
    stop.WaitFor(30s);
    left = true;
  });
  while (!entered) std::this_thread::yield();
  std::this_thread::sleep_for(20ms);
  assert(!left);  // A wait without cancellation must not end early.
  const auto start = std::chrono::steady_clock::now();
  stop.Stop();
  worker.join();
  assert(left && stop.cancelled());
  assert(std::chrono::steady_clock::now() - start < 5s);
}

static void WaitTimeoutTests() {
  WorkerStop stop;
  const auto start = std::chrono::steady_clock::now();
  stop.WaitFor(30ms);
  assert(!stop.cancelled());  // Timing out is not cancellation.
  assert(std::chrono::steady_clock::now() - start >= 25ms);
}

static void WorkerGroupTests() {
  std::vector<int> joined;
  {
    WorkerGroup group;
    group.Add([&] { joined.push_back(1); });
    group.Add([&] { joined.push_back(2); });
    assert(group.size() == 2);
    group.JoinAll();
    assert((joined == std::vector<int>{2, 1}));  // Reverse creation order.
    group.JoinAll();  // Repeated shutdown joins nothing twice.
    assert(group.size() == 0);
  }
  assert(joined.size() == 2);

  // Partial setup: only the worker that was created is joined, exactly once.
  joined.clear();
  {
    WorkerGroup group;
    group.Add([&] { joined.push_back(7); });
  }
  assert((joined == std::vector<int>{7}));

  // A group that never received a worker is harmless.
  { WorkerGroup group; }

  // The stop signal precedes joining: joiners observe cancellation.
  WorkerStop stop;
  bool saw_cancel = false;
  {
    WorkerGroup group(&stop);
    group.Add([&] { saw_cancel = stop.cancelled(); });
  }
  assert(saw_cancel);
}

static void ProgressTests() {
  const uint64_t before = NativeProgress();
  RecordNativeProgress();
  RecordNativeProgress();
  assert(NativeProgress() == before + 2);
}

int main() {
  GenerationTests();
  StopTests();
  StopDuringWaitTests();
  WaitTimeoutTests();
  WorkerGroupTests();
  ProgressTests();
  return 0;
}
