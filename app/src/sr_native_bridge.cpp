#include "sr_native_bridge.h"
#include "sr_native_profile.h"
#include <bit>
#include <algorithm>
#include <limits>
#include <utility>

namespace sr::native {
NativeCaptureBridge::NativeCaptureBridge(const GuestMemory& memory, PhysicalAddress physical)
    : memory_(memory), physical_(std::move(physical)) { mirror_.ResetSegment(epoch_); }
void NativeCaptureBridge::SetEnabled(bool enabled) {
  if (enabled_ == enabled) return;
  enabled_=enabled; cursor_=0; device_=0; depth_=0; scope_stamps_.clear();
  scope_payload_=DrawPayload{};
  mirror_=StateMirror{}; mirror_.ResetSegment(++epoch_);
}
NativeResult NativeCaptureBridge::Sync(GuestAddress device) {
  if (uint64_t(device)+profile::kDevice.ring_write+4 > (uint64_t{1}<<32)) return NativeResult::kInvalid;
  uint32_t last;
  if (!memory_.ReadU32(device+profile::kDevice.ring_write,last) || last>UINT32_MAX-4)
    return NativeResult::kInvalid;
  const uint32_t end=last+4;
  if (!cursor_) { cursor_=end; device_=device; return NativeResult::kComplete; }
  if (device_!=device || end<cursor_ || end-cursor_>(1u<<22) || (end-cursor_)%4)
    return NativeResult::kInvalid;
  if (end==cursor_) return NativeResult::kComplete;
  uint32_t physical;
  std::vector<std::byte> bytes;
  if (!physical_ || !physical_(cursor_,physical) || !memory_.Copy(cursor_,end-cursor_,bytes))
    return NativeResult::kInvalid;
  auto result=mirror_.Scan(memory_,bytes,{epoch_,physical});
  if (result!=NativeResult::kComplete) return result;
  if (depth_) scope_stamps_.insert(scope_stamps_.end(),mirror_.Stamps().begin(),mirror_.Stamps().end());
  cursor_=end;
  return NativeResult::kComplete;
}
CaptureCall NativeCaptureBridge::BeginCall(CommandKind kind, GuestAddress device) {
  CaptureCall call{kind,device,next_id_++,depth_+1,NativeResult::kCancelled};
  if (!enabled_) return call;
  if (!depth_) {
    scope_stamps_.clear(); scope_result_=Sync(device);
    scope_kind_=kind;scope_payload_=DrawPayload{};
    scope_id_=call.id;
  } else if (device_!=device) scope_result_=NativeResult::kInvalid;
  ++depth_; call.result=scope_result_;
  return call;
}
void NativeCaptureBridge::SetPayload(const CaptureCall& call,const CommandPayload& payload) {
  if (call.depth==1 && depth_ && call.result!=NativeResult::kCancelled) {
    scope_payload_=payload;
    if (call.result!=NativeResult::kComplete) scope_result_=call.result;
  }
}
NativeResult NativeCaptureBridge::CaptureBeforeWait(const GuestMemory& memory,NativeCommand& out) {
  if (!enabled_ || !depth_) return NativeResult::kPending;
  if (scope_result_!=NativeResult::kComplete) return scope_result_;
  const auto result=Sync(device_);
  if (result!=NativeResult::kComplete) {scope_result_=result;return result;}
  const bool effects=std::any_of(scope_stamps_.begin(),scope_stamps_.end(),[](const auto& stamp) {
    const auto opcode=(stamp.header>>8)&0x7f;
    return stamp.header>>30==3 && (opcode==0x22 || opcode==0x36 || opcode==0x64);
  });
  if (!effects) return NativeResult::kPending;
  NativeCommand snapshot;snapshot.kind=scope_kind_;snapshot.payload=scope_payload_;
  const auto captured=StateCapture{}.Snapshot(memory,device_,frame_,mirror_,snapshot.state);
  if (captured!=NativeResult::kComplete) return captured;
  snapshot.stamps=std::move(scope_stamps_);scope_stamps_.clear();out=std::move(snapshot);
  return NativeResult::kComplete;
}
NativeResult NativeCaptureBridge::EndCall(CaptureCall& call, const GuestMemory& memory, NativeCommand& out) {
  if (!enabled_ || !call.id || call.result==NativeResult::kCancelled) return NativeResult::kCancelled;
  if (call.depth!=depth_) return NativeResult::kInvalid;
  call.id=0;
  if (call.result!=NativeResult::kComplete) scope_result_=call.result;
  if (depth_>1) { --depth_; return NativeResult::kPending; }
  auto result=scope_result_==NativeResult::kComplete ? Sync(call.device) : scope_result_;
  --depth_;
  scope_payload_=DrawPayload{};
  if (result!=NativeResult::kComplete) return result;
  CapturedState state;
  result=StateCapture{}.Snapshot(memory,call.device,frame_,mirror_,state);
  if (result!=NativeResult::kComplete) return result;
  out.kind=call.kind; out.state=std::move(state); out.stamps=std::move(scope_stamps_);
  if (call.kind==CommandKind::kSwap) ++frame_;
  return NativeResult::kComplete;
}
NativeResult NativeCaptureBridge::BeforeSegmentChange(GuestAddress device) {
  if (!enabled_) return NativeResult::kCancelled;
  auto result=Sync(device);
  if (depth_ && result!=NativeResult::kComplete) scope_result_=result;
  return result;
}
NativeResult NativeCaptureBridge::AfterSegmentChange(GuestAddress device) {
  if (!enabled_) return NativeResult::kCancelled;
  cursor_=0; device_=0; mirror_.ResetSegment(++epoch_);
  auto result=Sync(device);
  if (depth_ && result!=NativeResult::kComplete) scope_result_=result;
  return result;
}
bool ReadClearArguments(const GuestMemory& memory, GuestAddress rect, GuestAddress color,
                        uint32_t stencil, float depth, ClearPayload& out) {
  ClearPayload copy=out; copy.depth=depth; copy.stencil=stencil;
  if (color) {
    std::vector<std::byte> bytes;
    if (!memory.Copy(color,16,bytes)) return false;
    for(unsigned i=0;i<4;++i) {
      uint32_t word=0; for(unsigned j=0;j<4;++j) word=(word<<8)|std::to_integer<uint32_t>(bytes[i*4+j]);
      copy.color[i]=std::bit_cast<float>(word);
    }
  }
  if (rect) {
    std::vector<std::byte> bytes;
    if (!memory.Copy(rect,16,bytes)) return false;
    Rect value; int32_t* fields[]={&value.left,&value.top,&value.right,&value.bottom};
    for(unsigned i=0;i<4;++i) {
      uint32_t word=0; for(unsigned j=0;j<4;++j) word=(word<<8)|std::to_integer<uint32_t>(bytes[i*4+j]);
      *fields[i]=std::bit_cast<int32_t>(word);
    }
    copy.rects={value};
  }
  out=std::move(copy); return true;
}
} // namespace sr::native
