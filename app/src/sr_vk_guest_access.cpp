#include "sr_vk_guest_access.h"

#include <rex/system/xmemory.h>

namespace sr::vk {
namespace {
constexpr uint64_t kPhysicalLimit = 0x20000000ull;
constexpr uint32_t kAliasBase = 0xA0000000u, kAliasEnd = 0xC0000000u;

// True when [address, address + length) is committed and readable in `heap`.
template <class Heap>
bool Committed(Heap* heap, uint64_t address, uint64_t length) {
  if (!heap) return false;
  const uint32_t page = heap->page_size();
  if (!page) return false;
  const uint64_t end = address + length;
  uint64_t cursor = address;
  while (cursor < end) {
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(uint32_t(cursor), &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit) ||
        !(info.protect & rex::memory::kMemoryProtectRead))
      return false;
    const uint64_t next = (cursor & ~uint64_t(page - 1)) + info.region_size;
    if (next <= cursor) return false;
    cursor = next;
  }
  return true;
}
}  // namespace

const uint8_t* SdkGuestAccess::ReadablePhysical(uint32_t physical, uint32_t length) {
  if (!memory_ || !length || uint64_t(physical) + length > kPhysicalLimit) return nullptr;
  if (!Committed(memory_->GetPhysicalHeap(), physical, length)) return nullptr;
  return memory_->TranslatePhysical<const uint8_t*>(physical);
}

const uint8_t* SdkGuestAccess::Readable(uint32_t address, uint32_t length) {
  if (!memory_ || !length || uint64_t(address) + length > (uint64_t{1} << 32)) return nullptr;
  if (address >= kAliasBase && address < kAliasEnd) {
    // The cached physical view of GPU memory: the physical mapping is the one that is always
    // committed, the virtual alias may not be.
    return ReadablePhysical(address - kAliasBase, length);
  }
  auto* heap = memory_->LookupHeap(address);
  if (!heap || address < heap->heap_base() ||
      uint64_t(address) + length > uint64_t(heap->heap_base()) + heap->heap_size() ||
      memory_->LookupHeap(address + length - 1) != heap)
    return nullptr;
  if (!Committed(heap, address, length)) return nullptr;
  return memory_->TranslateVirtual<const uint8_t*>(address);
}
}  // namespace sr::vk
