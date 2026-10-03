#pragma once
#include "sr_native_commands.h"
#include <deque>
#include <memory>
#include <mutex>

namespace sr::native {
struct MatchResult {
  NativeResult result=NativeResult::kPending;
  Serial serial=0;
  bool last_stamp=false;
};
// Match is a read-only probe. Commit acknowledges a packet only after the
// caller has accepted its effect; a blocked queue must leave the probe pending.
class CommandLedger {
 public:
  Serial Publish(NativeCommand);
  MatchResult Match(const PacketStamp&) const;
  NativeResult Commit(const PacketStamp&,Serial);
  std::shared_ptr<const NativeCommand> Find(Serial) const;
  NativeResult Retire(Serial);
  void Reset(uint64_t ring_generation);
 private:
  struct Entry {
    std::shared_ptr<const NativeCommand> command;
    std::vector<PacketStamp> effects;
    size_t next=0;
  };
  MatchResult MatchLocked(const PacketStamp&) const;
  mutable std::mutex mutex_;
  std::deque<Entry> pending_,matched_;
  Serial next_serial_=1;
  uint64_t ring_generation_=0;
};
bool IsNativeEffectPacket(const PacketStamp&);
} // namespace sr::native
