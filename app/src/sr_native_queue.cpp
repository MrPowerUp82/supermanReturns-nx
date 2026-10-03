#include "sr_native_queue.h"
#include <chrono>
#include <limits>
#include <stdexcept>

namespace sr::native {
uint64_t NativeCommandBytes(const NativeCommand& command) {
  uint64_t bytes=sizeof(command);
  auto add=[&](uint64_t count,uint64_t size) {
    if (count>(std::numeric_limits<uint64_t>::max()-bytes)/size) {
      bytes=std::numeric_limits<uint64_t>::max();return;
    }
    bytes+=count*size;
  };
  add(command.stamps.capacity(),sizeof(PacketStamp));
  for(const auto& stamp:command.stamps) add(stamp.payload.capacity(),sizeof(uint32_t));
  add(command.state.patched_vs.capacity(),sizeof(uint32_t));
  add(command.state.patched_ps.capacity(),sizeof(uint32_t));
  if(const auto* draw=std::get_if<DrawPayload>(&command.payload)) {
    add(draw->inline_vertices.capacity(),1);add(draw->indices.capacity(),1);
  } else if(const auto* clear=std::get_if<ClearPayload>(&command.payload)) {
    add(clear->rects.capacity(),sizeof(Rect));
  }
  return bytes;
}
NativeQueue::NativeQueue(QueueLimits limits):limits_(limits) {
  if (!limits.commands || !limits.bytes) throw std::invalid_argument("native queue limits must be positive");
}
Serial NativeQueue::Push(NativeCommand command,const std::function<bool()>& cancelled) {
  const auto cost=NativeCommandBytes(command);
  if (cost>limits_.bytes) return 0;
  for (;;) {
    // Application cancellation callbacks run outside the queue lock.
    if (cancelled && cancelled()) return 0;
    std::unique_lock lock(mutex_);
    if(cancelled_ || next_==std::numeric_limits<Serial>::max() ||
        (command.serial && command.serial!=next_)) return 0;
    if(entries_.size()<limits_.commands && cost<=limits_.bytes-bytes_) {
      command.serial=next_;
      auto snapshot=std::make_unique<NativeCommand>(std::move(command));
      const auto serial=next_;
      // Publish the index first, with rollback if map allocation fails.
      pending_.push_back(serial);
      try { entries_.emplace(serial,Entry{std::move(snapshot),cost,Stage::kQueued}); }
      catch (...) { pending_.pop_back();throw; }
      ++next_;bytes_+=cost;
      changed_.notify_all();return serial;
    }
    changed_.wait_for(lock,std::chrono::milliseconds(10));
  }
}
NativeResult NativeQueue::Pop(NativeCommand& out) {
  std::unique_lock lock(mutex_);
  changed_.wait(lock,[&]{return cancelled_ || !pending_.empty();});
  if(cancelled_) return NativeResult::kCancelled;
  const auto serial=pending_.front();
  auto& entry=entries_.at(serial);
  out=std::move(*entry.command);entry.command.reset();
  entry.stage=Stage::kActive;pending_.pop_front();
  return NativeResult::kComplete;
}
NativeResult NativeQueue::Complete(Serial serial) {
  std::lock_guard lock(mutex_);
  const auto it=entries_.find(serial);
  if (cancelled_) {
    // Retire cancelled active snapshots without inventing completion progress.
    if(it!=entries_.end() && it->second.stage==Stage::kActive) {
      bytes_-=it->second.bytes;entries_.erase(it);
    }
    changed_.notify_all();return NativeResult::kCancelled;
  }
  if(it==entries_.end() || it->second.stage!=Stage::kActive) return NativeResult::kInvalid;
  bytes_-=it->second.bytes;it->second.bytes=0;it->second.stage=Stage::kDone;
  while (!entries_.empty()) {
    auto first=entries_.begin();
    if(first->first!=completed_+1 || first->second.stage!=Stage::kDone) break;
    completed_=first->first;entries_.erase(first);
  }
  changed_.notify_all();return NativeResult::kComplete;
}
NativeResult NativeQueue::WaitThrough(Serial serial,const std::function<bool()>& cancelled) {
  for (;;) {
    if(cancelled && cancelled()) return NativeResult::kCancelled;
    std::unique_lock lock(mutex_);
    if(cancelled_) return NativeResult::kCancelled;
    if(serial>=next_) return NativeResult::kInvalid;
    if(serial<=completed_) return NativeResult::kComplete;
    changed_.wait_for(lock,std::chrono::milliseconds(10));
  }
}
void NativeQueue::Cancel() {
  std::lock_guard lock(mutex_);
  cancelled_=true;pending_.clear();
  for(auto it=entries_.begin();it!=entries_.end();) {
    if(it->second.stage==Stage::kActive) {++it;continue;}
    bytes_-=it->second.bytes;it=entries_.erase(it);
  }
  changed_.notify_all();
}
Serial NativeQueue::CompletedSerial() const { std::lock_guard lock(mutex_);return completed_; }
uint64_t NativeQueue::InFlightBytes() const { std::lock_guard lock(mutex_);return bytes_; }
} // namespace sr::native
