#pragma once
// GuestAccess (pcvk front end) over the ReXGlue guest memory: every range is checked against
// the heap page table before a pointer is returned, because on Horizon an access to an
// uncommitted page is a process-killing fault, not a recoverable error.
#include "pcvk/native_renderer/frontend.h"

namespace rex::memory {
class Memory;
}

namespace sr::vk {
class SdkGuestAccess final : public superman_returns::native::GuestAccess {
 public:
  explicit SdkGuestAccess(rex::memory::Memory* memory) : memory_(memory) {}
  const uint8_t* Readable(uint32_t address, uint32_t length) override;
  const uint8_t* ReadablePhysical(uint32_t physical, uint32_t length) override;

 private:
  rex::memory::Memory* memory_;
};
}  // namespace sr::vk
