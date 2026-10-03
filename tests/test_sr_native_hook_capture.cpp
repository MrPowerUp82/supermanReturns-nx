#include "sr_native_bridge.h"
#include <algorithm>
#include "sr_native_profile.h"
#include <cassert>
#include <bit>
#include <cstdio>

using namespace sr::native;
int main() {
  std::vector<std::byte> ram(0x20000);
  auto put=[&](uint32_t at,uint32_t v){for(unsigned i=0;i<4;++i) ram[at+i]=std::byte(v>>(24-i*8));};
  GuestMemory memory({
    [&](uint32_t a,uint32_t n,bool){return uint64_t(a)+n<=ram.size();},
    [&](uint32_t a,std::span<std::byte> out){std::copy_n(ram.begin()+a,out.size(),out.begin());return true;},
    [&](uint32_t a,std::span<const std::byte> in){std::copy(in.begin(),in.end(),ram.begin()+a);return true;}
  });
  NativeCaptureBridge bridge(memory,[](uint32_t a,uint32_t& p){p=a;return true;});
  const uint32_t dev=0x100, ring=0x10000;
  put(dev+profile::kDevice.ring_write,ring-4);
  bridge.SetEnabled(true);
  auto outer=bridge.BeginCall(CommandKind::kDraw,dev);
  auto nested=bridge.BeginCall(CommandKind::kDraw,dev);
  put(ring,0xC0012200);put(ring+4,0);put(ring+8,0x30004);
  put(dev+profile::kDevice.ring_write,ring+8);
  NativeCommand cmd; DrawPayload draw; draw.primitive=4; draw.count=3; cmd.payload=draw;
  unsigned originals=0;
  auto original=[&] { ++originals; draw.count=99; put(dev+profile::kDevice.vs_constants,0x3f800000); };
  original();
  assert(bridge.EndCall(nested,memory,cmd)==NativeResult::kPending);
  assert(bridge.EndCall(outer,memory,cmd)==NativeResult::kComplete);
  assert(cmd.stamps.size()==1 && cmd.stamps[0].site.physical_address==ring);
  assert(std::get<DrawPayload>(cmd.payload).count==3);
  assert(originals==1 && cmd.state.vs_constants[0]==0x3f800000);
  put(ring+8,0); assert(cmd.stamps[0].payload[1]==0x30004);
  // Calls on separate bridge instances cannot inherit another thread's nesting.
  NativeCaptureBridge other(memory,[](uint32_t a,uint32_t& p){p=a;return true;});
  other.SetEnabled(true);
  auto call=other.BeginCall(CommandKind::kClear,dev);
  ClearPayload clear; assert(ReadClearArguments(memory,0,0x18000,7,0.25f,clear));
  put(0x18000,std::bit_cast<uint32_t>(0.5f));
  put(0x18004,std::bit_cast<uint32_t>(1.0f));
  assert(ReadClearArguments(memory,0,0x18000,7,0.25f,clear));
  assert(clear.color[0]==0.5f && clear.color[1]==1.0f && clear.depth==0.25f && clear.stencil==7);
  cmd.payload=clear;
  assert(other.EndCall(call,memory,cmd)==NativeResult::kComplete);
  // A rejected argument copy must poison the operation, including a nested call.
  outer=bridge.BeginCall(CommandKind::kClear,dev);
  nested=bridge.BeginCall(CommandKind::kDraw,dev);
  nested.result=NativeResult::kInvalid;
  assert(bridge.EndCall(nested,memory,cmd)==NativeResult::kPending);
  assert(bridge.EndCall(outer,memory,cmd)==NativeResult::kInvalid);
  call=bridge.BeginCall(CommandKind::kDraw,dev);
  assert(bridge.EndCall(call,memory,cmd)==NativeResult::kComplete);
  // An operation may emit a tail, switch allocation, then emit another tail.
  call=bridge.BeginCall(CommandKind::kClear,dev);
  ClearPayload checkpoint_args;checkpoint_args.flags=3;
  bridge.SetPayload(call,checkpoint_args);
  put(ring+12,0xC0012200);put(ring+16,0);put(ring+20,0x30004);
  put(dev+profile::kDevice.ring_write,ring+20);
  assert(bridge.BeforeSegmentChange(dev)==NativeResult::kComplete);
  NativeCommand checkpoint;
  assert(bridge.CaptureBeforeWait(memory,checkpoint)==NativeResult::kComplete);
  assert(checkpoint.kind==CommandKind::kClear && checkpoint.stamps.size()==1);
  assert(std::get<ClearPayload>(checkpoint.payload).flags==3);
  assert(bridge.ScopeId()==call.id);
  assert(bridge.CaptureBeforeWait(memory,cmd)==NativeResult::kPending);
  const uint32_t new_ring=0x11000;
  put(dev+profile::kDevice.ring_write,new_ring-4);
  assert(bridge.AfterSegmentChange(dev)==NativeResult::kComplete);
  put(new_ring,0xC0012200);put(new_ring+4,0);put(new_ring+8,0x30004);
  put(dev+profile::kDevice.ring_write,new_ring+8);
  assert(bridge.EndCall(call,memory,cmd)==NativeResult::kComplete);
  assert(cmd.stamps.size()==1);
  assert(checkpoint.stamps[0].site.physical_address==ring+12);
  assert(cmd.stamps[0].site.physical_address==new_ring);
  assert(checkpoint.stamps[0].site.allocation_epoch!=cmd.stamps[0].site.allocation_epoch);
  bridge.SetEnabled(false); call=bridge.BeginCall(CommandKind::kDraw,dev);
  original();
  assert(bridge.EndCall(call,memory,cmd)==NativeResult::kCancelled);
  assert(originals==2);
  // Preserve the rejected segment for diagnostics without consuming its cursor.
  unsigned rejected=0;
  std::vector<std::byte> rejected_bytes;
  NativeCaptureBridge diagnostic(memory,[](uint32_t a,uint32_t& p){p=a;return true;},
    [&](NativeResult result,PacketSite site,std::span<const std::byte> bytes) {
      assert(result==NativeResult::kInvalid && site.physical_address==new_ring);
      rejected_bytes.assign(bytes.begin(),bytes.end());++rejected;
    });
  put(dev+profile::kDevice.ring_write,new_ring-4);
  diagnostic.SetEnabled(true);
  call=diagnostic.BeginCall(CommandKind::kDraw,dev);
  put(new_ring,0xC0012200);put(new_ring+4,0); // Missing final payload word.
  put(dev+profile::kDevice.ring_write,new_ring+4);
  assert(diagnostic.EndCall(call,memory,cmd)==NativeResult::kInvalid);
  assert(rejected==1 && rejected_bytes.size()==8);
  put(new_ring+8,0x30004);put(dev+profile::kDevice.ring_write,new_ring+8);
  call=diagnostic.BeginCall(CommandKind::kDraw,dev);
  assert(call.result==NativeResult::kComplete);
  assert(diagnostic.EndCall(call,memory,cmd)==NativeResult::kComplete);
  assert(rejected==1 && rejected_bytes.size()==8);
  std::puts("hook bridge: nested capture, copied arguments and float clear passed");
}
