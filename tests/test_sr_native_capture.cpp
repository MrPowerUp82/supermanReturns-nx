#include "sr_native_capture.h"
#include "sr_native_mirror.h"
#include "sr_native_profile.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
using namespace sr::native;
static std::vector<std::byte> BE(std::initializer_list<uint32_t> words) {
  std::vector<std::byte> bytes;
  for (auto word : words) for (int shift : {24,16,8,0}) bytes.push_back(std::byte((word>>shift)&255));
  return bytes;
}
int main() {
  std::vector<std::byte> ram(0x9000);
  auto put = [&](uint32_t a, uint32_t value) { auto bytes=BE({value}); std::memcpy(ram.data()+a,bytes.data(),4); };
  GuestMemory memory({[&](uint32_t a,uint32_t n,bool){return uint64_t(a)+n<=ram.size();},
      [&](uint32_t a,std::span<std::byte> out){std::memcpy(out.data(),ram.data()+a,out.size());return true;}, {}});
  StateMirror mirror;
  mirror.ResetSegment(1);
  auto set = BE({0xc0042d00,12,0x3f800000,0x40000000,0x40400000,0x40800000});
  assert(mirror.Scan(memory,set,{1,0x100}) == NativeResult::kComplete);
  assert(mirror.Register(0x400c)==0x3f800000);
  auto version=mirror.VsVersion();
  assert(mirror.Stamps().size()==1);
  assert(mirror.Scan(memory,set,{1,0x100}) == NativeResult::kComplete);
  assert(mirror.VsVersion()==version && mirror.Stamps().empty());
  auto broken=BE({0x00014000,0xdeadbeef});
  assert(mirror.Scan(memory,broken,{1,0x118}) == NativeResult::kInvalid);
  assert(mirror.Register(0x4000)==0 && mirror.VsVersion()==version);
  auto indirect=BE({0xc0013f00,0x400,3});
  // This virtual-only binding has no physical reader: reject, never skip.
  assert(mirror.Scan(memory,indirect,{1,0x118}) == NativeResult::kInvalid);
  assert(mirror.VsVersion()==version);
  mirror.ResetSegment(2);
  assert(mirror.Scan(memory,set,{1,0x100}) == NativeResult::kInvalid);
  assert(mirror.Scan(memory,set,{2,0x100}) == NativeResult::kComplete);
  const auto latest_version=mirror.VsVersion();
  constexpr uint32_t device=0x1000;
  put(device+profile::kDevice.vs_constants+12*4,0xdeadbeef);
  put(device+profile::kDevice.viewport,9); put(device+profile::kDevice.viewport+4,7);
  put(device+profile::kDevice.viewport+8,1280); put(device+profile::kDevice.viewport+12,720);
  put(device+profile::kDevice.viewport+16,0); put(device+profile::kDevice.viewport+20,0x3f800000);
  put(device+profile::kDevice.stream_strides,0x05060708);
  put(device+profile::kDevice.register_shadow+0xe*4,0x000a0009);
  put(device+profile::kDevice.register_shadow+0xf*4,0x02d00500);
  StateCapture capture;
  CapturedState state;
  assert(capture.Snapshot(memory,device,4,mirror,state) == NativeResult::kComplete);
  assert(state.vs_constants[12]==0x3f800000 && state.viewport.width==1280 && state.viewport.x==9);
  assert(state.stream_strides[0]==20 && state.stream_strides[3]==32);
  assert(state.scissor.left==9 && state.scissor.top==10 && state.scissor.right==1280 && state.scissor.bottom==720);
  put(device+profile::kDevice.vs_constants+12*4,99);
  assert(state.vs_constants[12]==0x3f800000 && state.frame==4);
  CapturedState before=state;
  assert(capture.Snapshot(memory,0xffff0000,8,mirror,state)==NativeResult::kInvalid);
  assert(state.vs_constants==before.vs_constants && state.frame==4);
  // Cycle in an indirect must fail without publishing the preceding register write.
  std::map<uint32_t,std::vector<std::byte>> physical;
  physical[0xa0000400]=BE({0xc0013f00,0x400,3});
  GuestMemory cyclic({[&](uint32_t a,uint32_t n,bool){return physical.contains(a)&&physical[a].size()==n;},
      [&](uint32_t a,std::span<std::byte> out){std::memcpy(out.data(),physical[a].data(),out.size());return true;},{},
      [&](uint32_t a,uint32_t n){return physical.contains(0xa0000000+a)&&physical[0xa0000000+a].size()==n;},
      [&](uint32_t a,std::span<std::byte> out){std::memcpy(out.data(),physical[0xa0000000+a].data(),out.size());return true;}});
  mirror.ResetSegment(3);
  assert(mirror.Scan(cyclic,indirect,{3,0x100})==NativeResult::kInvalid);
  assert(mirror.VsVersion()==latest_version);
  // Physical shader/constant reads must not depend on a committed A000 alias.
  GuestMemory gpu({{}, {}, {},
    [](uint32_t a,uint32_t n){return (a==0x100 && n==4)||(a==0x200 && n==8);},
    [](uint32_t a,std::span<std::byte> target){
      const auto data=a==0x100?BE({0x12345678}):BE({0x87654321,0xabcdef01});
      std::copy(data.begin(),data.end(),target.begin());return true;
    }});
  StateMirror loaded;loaded.ResetSegment(1);
  assert(loaded.Scan(gpu,BE({0xc0022f00,0x100,0x7f0,1,0xc0012700,0x201,2}),{1,0x400})==NativeResult::kComplete);
  assert(loaded.Register(0x47f0)==0x12345678);
  assert(loaded.ShaderCode(false)==std::vector<uint32_t>({0x87654321,0xabcdef01}));
  auto unknown=BE({0xc0007f00,0});
  assert(mirror.Scan(memory,unknown,{3,0x100})==NativeResult::kUnsupported);
  assert(mirror.VsVersion()==latest_version);
  std::puts("capture: inline constants, transactional parsing, replay, cycles and ownership passed");
}
