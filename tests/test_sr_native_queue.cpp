#include "sr_native_queue.h"
#include <cassert>
#include <chrono>
#include <future>
#include <cstdio>
using namespace sr::native;
using namespace std::chrono_literals;
static NativeCommand Draw(unsigned bytes=0) {
  NativeCommand command;
  DrawPayload payload;payload.inline_vertices.resize(bytes);
  command.payload=std::move(payload);return command;
}
int main() {
  const auto cost=NativeCommandBytes(Draw());
  assert(cost>=sizeof(NativeCommand));
  assert(NativeCommandBytes(Draw(1024))>=cost+1024);
  NativeQueue bounded({1,cost*4});
  const auto first=bounded.Push(Draw(),{});assert(first==1);
  NativeCommand active;
  assert(bounded.Pop(active)==NativeResult::kComplete && active.serial==first);
  auto producer=std::async(std::launch::async,[&]{return bounded.Push(Draw(),{});});
  assert(producer.wait_for(30ms)==std::future_status::timeout);
  assert(bounded.InFlightBytes()==cost); // Pop does not release GPU-owned snapshot budget.
  assert(bounded.Complete(first)==NativeResult::kComplete);
  assert(producer.wait_for(2s)==std::future_status::ready && producer.get()==2);
  assert(bounded.Pop(active)==NativeResult::kComplete && active.serial==2);
  assert(bounded.Complete(2)==NativeResult::kComplete && bounded.InFlightBytes()==0);
  assert(bounded.Complete(2)==NativeResult::kInvalid);

  NativeQueue gaps({4,cost*4});
  auto a=gaps.Push(Draw(),{}),b=gaps.Push(Draw(),{});
  assert(gaps.Complete(a)==NativeResult::kInvalid); // Cannot complete work not popped by executor.
  assert(gaps.Pop(active)==NativeResult::kComplete && active.serial==a);
  assert(gaps.Pop(active)==NativeResult::kComplete && active.serial==b);
  auto waiter=std::async(std::launch::async,[&]{return gaps.WaitThrough(b,{});});
  assert(gaps.Complete(b)==NativeResult::kComplete && gaps.CompletedSerial()==0);
  assert(waiter.wait_for(30ms)==std::future_status::timeout);
  assert(gaps.Complete(a)==NativeResult::kComplete && gaps.CompletedSerial()==b);
  assert(waiter.wait_for(2s)==std::future_status::ready && waiter.get()==NativeResult::kComplete);
  assert(gaps.WaitThrough(b+1,{})==NativeResult::kInvalid);
  assert(gaps.Push(Draw(),{})==b+1);
  assert(gaps.WaitThrough(b,{})==NativeResult::kComplete && gaps.CompletedSerial()==b);

  NativeQueue bytes_only({4,cost});
  assert(bytes_only.Push(Draw(1),{})==0); // Oversized command rejects immediately.
  assert(bytes_only.Push(Draw(),[&]{assert(bytes_only.InFlightBytes()==0);return true;})==0 && bytes_only.CompletedSerial()==0);
  NativeCommand numbered=Draw();numbered.serial=2;
  assert(bytes_only.Push(numbered,{})==0); // External ledger serial cannot create an admission gap.
  numbered.serial=1;assert(bytes_only.Push(numbered,{})==1);
  auto blocked=std::async(std::launch::async,[&]{return bytes_only.Push(Draw(),{});});
  assert(blocked.wait_for(30ms)==std::future_status::timeout);
  bytes_only.Cancel();
  assert(blocked.wait_for(2s)==std::future_status::ready && blocked.get()==0);
  assert(bytes_only.Pop(active)==NativeResult::kCancelled);

  NativeQueue empty;
  auto consumer=std::async(std::launch::async,[&]{return empty.Pop(active);});
  assert(consumer.wait_for(30ms)==std::future_status::timeout);
  empty.Cancel();
  assert(consumer.wait_for(2s)==std::future_status::ready && consumer.get()==NativeResult::kCancelled);
  assert(empty.WaitThrough(0,{})==NativeResult::kCancelled);
  NativeQueue pending;
  auto token=pending.Push(Draw(),{});
  assert(pending.Pop(active)==NativeResult::kComplete);
  auto unfinished=std::async(std::launch::async,[&]{return pending.WaitThrough(token,{});});
  assert(unfinished.wait_for(30ms)==std::future_status::timeout);
  pending.Cancel();
  assert(unfinished.wait_for(2s)==std::future_status::ready && unfinished.get()==NativeResult::kCancelled);
  assert(pending.CompletedSerial()==0 && pending.Complete(token)==NativeResult::kCancelled);
  assert(pending.InFlightBytes()==0);
  std::puts("native queue: bounded in-flight snapshots, completion gaps and cancellation passed");
}
