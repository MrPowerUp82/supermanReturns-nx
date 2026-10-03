#include "sr_native_command_ledger.h"
#include <cassert>
#include <cstdio>
using namespace sr::native;
int main() {
  CommandLedger ledger;
  PacketStamp first{{1,0x1000},0xC0012200,{0,0x30004}};
  PacketStamp second{{1,0x100c},0xC0012200,{0,0x30004}};
  assert(ledger.Match(first).result==NativeResult::kPending);
  NativeCommand clear;clear.kind=CommandKind::kClear;clear.payload=ClearPayload{};
  clear.stamps={first,second};
  const auto serial=ledger.Publish(clear);assert(serial!=0);
  auto match=ledger.Match(first);assert(match.result==NativeResult::kComplete && !match.last_stamp);
  // A retry before accepting the effect probes the same command.
  assert(ledger.Match(first).serial==serial);
  auto bad=first;bad.payload[1]^=1;
  assert(ledger.Match(bad).result==NativeResult::kInvalid);
  bad=first;bad.site.allocation_epoch=2;
  assert(ledger.Match(bad).result==NativeResult::kInvalid);
  assert(ledger.Match(first).serial==serial);
  assert(ledger.Commit(first,serial+1)==NativeResult::kInvalid);
  assert(ledger.Commit(first,serial)==NativeResult::kComplete);
  assert(ledger.Match(first).result==NativeResult::kInvalid);
  assert(ledger.Retire(serial)==NativeResult::kPending);
  assert(ledger.Match(second).last_stamp);
  assert(ledger.Commit(second,serial)==NativeResult::kComplete);
  auto owned=ledger.Find(serial);assert(owned && owned->stamps==clear.stamps);
  clear.stamps.clear();assert(owned->stamps.size()==2);
  assert(ledger.Retire(serial)==NativeResult::kComplete);
  assert(!ledger.Find(serial));
  ledger.Reset(2);
  NativeCommand draw;draw.payload=DrawPayload{};first.site.allocation_epoch=2;draw.stamps={first};
  const auto later=ledger.Publish(draw);assert(later>serial);
  first.site.allocation_epoch=1;assert(ledger.Match(first).result==NativeResult::kInvalid);
  first.site.allocation_epoch=2;assert(ledger.Match(first).serial==later);
  std::puts("ledger: delayed publication, exact words/epoch, retry and retirement passed");
}
