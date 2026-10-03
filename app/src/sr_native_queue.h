#pragma once
#include "sr_native_commands.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

namespace sr::native {
uint64_t NativeCommandBytes(const NativeCommand&);
struct QueueLimits { size_t commands=512; uint64_t bytes=8ull*1024*1024; };
// Admission and completion are separate. Popped snapshots remain charged until
// the executor reports completion; completion never crosses an unfinished serial.
class NativeQueue {
 public:
  explicit NativeQueue(QueueLimits = {});
  Serial Push(NativeCommand,const std::function<bool()>& cancelled);
  NativeResult Pop(NativeCommand&);
  NativeResult Complete(Serial);
  NativeResult WaitThrough(Serial,const std::function<bool()>& cancelled);
  void Cancel();
  Serial CompletedSerial() const;
  uint64_t InFlightBytes() const;
 private:
  enum class Stage { kQueued,kActive,kDone };
  struct Entry {
    std::unique_ptr<NativeCommand> command;
    uint64_t bytes=0;
    Stage stage=Stage::kQueued;
  };
  QueueLimits limits_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::map<Serial,Entry> entries_;
  std::deque<Serial> pending_;
  Serial next_=1,completed_=0;
  uint64_t bytes_=0;
  bool cancelled_=false;
};
} // namespace sr::native
