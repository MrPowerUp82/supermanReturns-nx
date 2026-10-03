#pragma once
#include "sr_native_capture.h"
#include "sr_native_mirror.h"
#include <functional>

namespace sr::native {
struct CaptureCall {
  CommandKind kind = CommandKind::kDraw;
  GuestAddress device = 0;
  uint64_t id = 0;
  uint32_t depth = 0;
  NativeResult result = NativeResult::kCancelled;
};
// One instance per guest producer thread. Never holds a lock while the original
// PPC function runs. A nested operation belongs to its outer capture scope.
class NativeCaptureBridge {
 public:
  using PhysicalAddress = std::function<bool(GuestAddress,uint32_t&)>;
  using ScanFailure = std::function<void(NativeResult,PacketSite,std::span<const std::byte>)>;
  NativeCaptureBridge(const GuestMemory&, PhysicalAddress, ScanFailure = {});
  void SetEnabled(bool enabled);
  CaptureCall BeginCall(CommandKind, GuestAddress);
  void SetPayload(const CaptureCall&, const CommandPayload&);
  NativeResult CaptureBeforeWait(const GuestMemory&,NativeCommand&);
  NativeResult EndCall(CaptureCall&, const GuestMemory&, NativeCommand&);
  NativeResult BeforeSegmentChange(GuestAddress);
  NativeResult AfterSegmentChange(GuestAddress);
  const StateMirror& mirror() const { return mirror_; }
  uint64_t ScopeId() const { return scope_id_; }
 private:
  NativeResult Sync(GuestAddress);
  const GuestMemory& memory_;
  PhysicalAddress physical_;
  ScanFailure scan_failure_;
  StateMirror mirror_;
  GuestAddress cursor_ = 0, device_ = 0;
  uint64_t epoch_ = 1, next_id_ = 1, frame_ = 0;
  uint32_t depth_ = 0;
  bool enabled_ = false;
  NativeResult scope_result_ = NativeResult::kComplete;
  std::vector<PacketStamp> scope_stamps_;
  CommandKind scope_kind_=CommandKind::kDraw;
  CommandPayload scope_payload_;
  uint64_t scope_id_=0;
};
bool ReadClearArguments(const GuestMemory&, GuestAddress rect, GuestAddress color,
                        uint32_t stencil, float depth, ClearPayload&);
} // namespace sr::native
