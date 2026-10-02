#include "sr_native_present.h"
#include "sr_native_system.h"

#include <atomic>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
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
    group.Add([&] { joined.push_back(1); return true; });
    group.Add([&] { joined.push_back(2); return true; });
    assert(group.size() == 2);
    assert(group.JoinAll());
    assert((joined == std::vector<int>{2, 1}));  // Reverse creation order.
    assert(group.JoinAll());  // Repeated shutdown joins nothing twice.
    assert(group.size() == 0);
  }
  assert(joined.size() == 2);

  // Partial setup: only the worker that was created is joined, exactly once.
  joined.clear();
  {
    WorkerGroup group;
    group.Add([&] { joined.push_back(7); return true; });
  }
  assert((joined == std::vector<int>{7}));

  // A group that never received a worker is harmless.
  { WorkerGroup group; }

  // The stop signal precedes joining: joiners observe cancellation.
  WorkerStop stop;
  bool saw_cancel = false;
  {
    WorkerGroup group(&stop);
    group.Add([&] { saw_cancel = stop.cancelled(); return true; });
  }
  assert(saw_cancel);

  // A worker that does not exit in time is kept, reported, and joined on a later attempt;
  // the ones that did join are not joined again.
  joined.clear();
  bool slow_exits = false;
  WorkerGroup group;
  group.Add([&] { joined.push_back(1); return true; });
  group.Add([&] { if (!slow_exits) return false; joined.push_back(2); return true; });
  group.Add([&] { joined.push_back(3); return true; });
  assert(!group.JoinAll());
  assert((joined == std::vector<int>{3, 1}));  // The slow one blocked nothing else.
  assert(group.size() == 1);
  slow_exits = true;
  assert(group.JoinAll());
  assert((joined == std::vector<int>{3, 1, 2}));
  assert(group.size() == 0);
}

// --- Presentation sequence with fake acquisition/submission callbacks ----------------------

struct FakeVulkan {
  bool pass_ok = true, resources_ok = true, framebuffer_ok = true;
  SubmitResult submit = SubmitResult::kOk;
  bool cancelled = false;
  unsigned pass_created = 0, resources_created = 0, framebuffers_created = 0, submits = 0, resets = 0;
  unsigned pass_destroyed = 0, resources_destroyed = 0, framebuffers_destroyed = 0, reports = 0;
  std::vector<std::string> order;
  std::function<FenceWait()> wait = [] { return FenceWait::kComplete; };

  ClearOps Ops() {
    ClearOps ops;
    ops.create_render_pass = [this] { ++pass_created; return pass_ok; };
    ops.create_command_resources = [this] { ++resources_created; return resources_ok; };
    ops.create_framebuffer = [this](uint64_t) { ++framebuffers_created; return framebuffer_ok; };
    ops.record_and_submit = [this] { ++submits; order.push_back("submit"); return submit; };
    ops.wait_fence = [this] { return wait(); };
    ops.reset_for_reuse = [this] { ++resets; order.push_back("reset"); };
    ops.destroy_framebuffers = [this] { ++framebuffers_destroyed; order.push_back("fb"); };
    ops.destroy_command_resources = [this] { ++resources_destroyed; order.push_back("pool"); };
    ops.destroy_render_pass = [this] { ++pass_destroyed; order.push_back("pass"); };
    ops.cancelled = [this] { return cancelled; };
    ops.report = [this](const char*) { ++reports; };
    return ops;
  }
};

static void SequencePresentsAndTearsDownInOrder() {
  FakeVulkan vk;
  ClearSequence sequence(vk.Ops());
  assert(sequence.Clear(1) == ClearResult::kPresented);
  assert(sequence.Clear(1) == ClearResult::kPresented);
  assert(vk.pass_created == 1 && vk.resources_created == 1 && vk.submits == 2);
  assert(!sequence.in_flight());
  assert(vk.resets == 2);  // The fence and pool are reused only after completion.
  assert(sequence.Teardown(4));
  assert((vk.order.end()[-3] == "fb" && vk.order.end()[-2] == "pool" && vk.order.end()[-1] == "pass"));
  assert(sequence.Teardown(4));  // Repeated shutdown destroys nothing twice.
  assert(vk.pass_destroyed == 1 && vk.resources_destroyed == 1 && vk.framebuffers_destroyed == 1);
}

static void SequencePartialSetupTeardown() {
  {  // Nothing was ever created.
    FakeVulkan vk;
    ClearSequence sequence(vk.Ops());
    assert(sequence.Teardown(4));
    assert(vk.pass_destroyed + vk.resources_destroyed + vk.framebuffers_destroyed == 0);
  }
  {  // Render pass created, pool creation failed.
    FakeVulkan vk;
    vk.resources_ok = false;
    ClearSequence sequence(vk.Ops());
    assert(sequence.Clear(1) == ClearResult::kFailed);
    assert(vk.submits == 0);
    assert(sequence.Teardown(4));
    assert(vk.pass_destroyed == 1 && vk.resources_destroyed == 0 && vk.framebuffers_destroyed == 0);
  }
  {  // Pool created but the framebuffer is missing.
    FakeVulkan vk;
    vk.framebuffer_ok = false;
    ClearSequence sequence(vk.Ops());
    assert(sequence.Clear(1) == ClearResult::kFailed);
    assert(vk.submits == 0);
    assert(sequence.Teardown(4));
    assert(vk.resources_destroyed == 1 && vk.pass_destroyed == 1 && vk.framebuffers_destroyed == 0);
  }
  {  // The render pass itself failed: nothing to destroy, nothing submitted.
    FakeVulkan vk;
    vk.pass_ok = false;
    ClearSequence sequence(vk.Ops());
    assert(sequence.Clear(1) == ClearResult::kFailed);
    assert(sequence.Teardown(4));
    assert(vk.pass_destroyed == 0 && vk.resources_created == 0);
  }
}

static void SequenceFailedSubmissionIsNotPresented() {
  FakeVulkan vk;
  vk.submit = SubmitResult::kFailed;
  ClearSequence sequence(vk.Ops());
  assert(sequence.Clear(1) == ClearResult::kFailed);
  assert(!sequence.in_flight());  // Nothing was submitted, so nothing is waited on.
  assert(vk.resets == 0);
  vk.submit = SubmitResult::kOk;
  assert(sequence.Clear(1) == ClearResult::kPresented);  // A transient failure does not poison it.
  vk.submit = SubmitResult::kDeviceLost;
  assert(sequence.Clear(2) == ClearResult::kDeviceLost);
  assert(sequence.device_lost());
  assert(sequence.Clear(2) == ClearResult::kDeviceLost);  // Never retried on a lost device.
  assert(vk.submits == 3);
}

static void SequenceTimeoutsAreDiagnosedAndCancellable() {
  FakeVulkan vk;
  size_t slices = 0;
  vk.wait = [&] {
    if (++slices == 3) vk.cancelled = true;
    return FenceWait::kTimedOut;
  };
  ClearSequence sequence(vk.Ops());
  assert(sequence.Clear(1) == ClearResult::kCancelled);
  assert(vk.reports >= 1);       // A timeout is reported, not silent.
  assert(sequence.in_flight());  // The submission may still be using the resources.
  assert(vk.resets == 0);

  // Shutdown cannot drain it: resources are retained instead of destroyed under the GPU.
  assert(!sequence.Teardown(3));
  assert(vk.pass_destroyed + vk.resources_destroyed + vk.framebuffers_destroyed == 0);
  assert(sequence.in_flight());
}

static void SequenceDrainsAfterCancellation() {
  FakeVulkan vk;
  bool gpu_done = false;
  vk.wait = [&] { return gpu_done ? FenceWait::kComplete : FenceWait::kTimedOut; };
  ClearSequence sequence(vk.Ops());
  vk.cancelled = true;
  assert(sequence.Clear(1) == ClearResult::kCancelled);
  assert(sequence.in_flight() && vk.submits == 1);
  // A later request waits for the earlier submission before reusing anything.
  vk.cancelled = false;
  gpu_done = true;
  assert(sequence.Clear(1) == ClearResult::kPresented);
  assert(vk.submits == 2);
  assert((vk.order[0] == "submit" && vk.order[1] == "reset" && vk.order[2] == "submit"));
  assert(sequence.Teardown(2));
  assert(vk.pass_destroyed == 1);

  // Teardown after a cancelled submission completes once the GPU is done.
  FakeVulkan vk2;
  bool done2 = false;
  vk2.wait = [&] { return done2 ? FenceWait::kComplete : FenceWait::kTimedOut; };
  ClearSequence cancelled_sequence(vk2.Ops());
  vk2.cancelled = true;
  assert(cancelled_sequence.Clear(1) == ClearResult::kCancelled);
  assert(!cancelled_sequence.Teardown(2));
  done2 = true;
  assert(cancelled_sequence.Teardown(2));
  assert(vk2.pass_destroyed == 1 && vk2.resources_destroyed == 1 && vk2.framebuffers_destroyed == 1);
  assert(!cancelled_sequence.in_flight());
}

static void SequenceWaitFailureKeepsResources() {
  FakeVulkan vk;
  vk.wait = [] { return FenceWait::kFailed; };
  ClearSequence sequence(vk.Ops());
  assert(sequence.Clear(1) == ClearResult::kFailed);
  assert(sequence.in_flight());  // Completion is unknown: the resources may still be in use.
  assert(vk.resets == 0);
  vk.wait = [] { return FenceWait::kDeviceLost; };
  assert(sequence.Teardown(2));  // A lost device no longer runs work.
  assert(vk.pass_destroyed == 1);
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
  SequencePresentsAndTearsDownInOrder();
  SequencePartialSetupTeardown();
  SequenceFailedSubmissionIsNotPresented();
  SequenceTimeoutsAreDiagnosedAndCancellable();
  SequenceDrainsAfterCancellation();
  SequenceWaitFailureKeepsResources();
  return 0;
}
