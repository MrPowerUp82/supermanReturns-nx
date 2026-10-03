#include "sr_native_command_ledger.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace sr::native {
bool IsNativeEffectPacket(const PacketStamp& stamp) {
  if ((stamp.header>>30)!=3) return false;
  const auto opcode=(stamp.header>>8)&0x7f;
  return opcode==0x22 || opcode==0x36 || opcode==0x64;
}
Serial CommandLedger::Publish(NativeCommand command) {
  Entry entry;
  for (const auto& stamp:command.stamps) {
    if (!IsNativeEffectPacket(stamp)) continue;
    if (!stamp.site.allocation_epoch || (stamp.site.physical_address & 3) ||
        uint64_t(stamp.site.physical_address)+(stamp.payload.size()+1)*4>0x20000000 ||
        stamp.payload.size()!=((stamp.header>>16)&0x3fff)+1) return 0;
    entry.effects.push_back(stamp);
  }
  if (entry.effects.empty()) return 0;
  const bool payload_ok=
    (command.kind==CommandKind::kDraw && std::holds_alternative<DrawPayload>(command.payload)) ||
    (command.kind==CommandKind::kClear && std::holds_alternative<ClearPayload>(command.payload)) ||
    (command.kind==CommandKind::kResolve && std::holds_alternative<ResolvePayload>(command.payload)) ||
    (command.kind==CommandKind::kSwap && std::holds_alternative<SwapPayload>(command.payload));
  if (!payload_ok) return 0;
  std::lock_guard lock(mutex_);
  // Diagnostic ledger is bounded even before queue backpressure is integrated.
  if (pending_.size()+matched_.size()>=512 || next_serial_==std::numeric_limits<Serial>::max()) return 0;
  command.serial=next_serial_;
  entry.command=std::make_shared<const NativeCommand>(std::move(command));
  pending_.push_back(std::move(entry));return next_serial_++;
}
MatchResult CommandLedger::MatchLocked(const PacketStamp& stamp) const {
  if (pending_.empty()) return {};
  const auto& entry=pending_.front();
  if (entry.effects[entry.next]!=stamp) return {NativeResult::kInvalid,0,false};
  return {NativeResult::kComplete,entry.command->serial,entry.next+1==entry.effects.size()};
}
MatchResult CommandLedger::Match(const PacketStamp& stamp) const {
  std::lock_guard lock(mutex_);return MatchLocked(stamp);
}
NativeResult CommandLedger::Commit(const PacketStamp& stamp,Serial serial) {
  std::lock_guard lock(mutex_);
  const auto match=MatchLocked(stamp);
  if (match.result!=NativeResult::kComplete) return match.result;
  if (!serial || match.serial!=serial) return NativeResult::kInvalid;
  if (pending_.front().next+1==pending_.front().effects.size()) {
    // Allocate the destination before advancing the cursor. Allocation failure
    // must not acknowledge an effect that the caller cannot subsequently retire.
    matched_.push_back(pending_.front());
    matched_.back().next=matched_.back().effects.size();pending_.pop_front();
  } else ++pending_.front().next;
  return NativeResult::kComplete;
}
std::shared_ptr<const NativeCommand> CommandLedger::Find(Serial serial) const {
  std::lock_guard lock(mutex_);
  for (const auto* entries:{&pending_,&matched_})
    for (const auto& entry:*entries) if (entry.command->serial==serial) return entry.command;
  return {};
}
NativeResult CommandLedger::Retire(Serial serial) {
  std::lock_guard lock(mutex_);
  const auto match=std::find_if(matched_.begin(),matched_.end(),
    [&](const auto& entry){return entry.command->serial==serial;});
  if (match==matched_.end()) {
    for (const auto& entry:pending_) if (entry.command->serial==serial) return NativeResult::kPending;
    return NativeResult::kInvalid;
  }
  matched_.erase(match);return NativeResult::kComplete;
}
void CommandLedger::Reset(uint64_t ring_generation) {
  std::lock_guard lock(mutex_);
  pending_.clear();matched_.clear();ring_generation_=ring_generation;
  // Serial IDs remain monotonic: a reset cannot masquerade as old completion.
}
} // namespace sr::native
