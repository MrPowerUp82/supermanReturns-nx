#pragma once
#include "sr_native_commands.h"
namespace sr::native {
class StateMirror;
class StateCapture {
 public:
  NativeResult Snapshot(const GuestMemory&, GuestAddress device, uint64_t frame,
                        const StateMirror&, CapturedState&) const;
};
}  // namespace sr::native
