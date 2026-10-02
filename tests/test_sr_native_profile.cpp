#include "sr_native_profile.h"
#include <cassert>
#include <cstdio>
using namespace sr::native;
int main() {
  const auto& d = profile::kDevice;
  assert(d.fetch_constants == 0x400 && d.vs_constants == 0x700 && d.ps_constants == 0x1700);
  assert(d.viewport == 0x3058 && d.shader_a == 0x3080 && d.shader_b == 0x3084);
  assert(d.ring_write == 0x28 && d.ring_limit == 0x2c);
  assert(profile::kDrawVertices == 0x820fbbf8 && profile::kClear == 0x82101a58);
  assert(profile::kSwap == 0x82112050);
  assert(profile::kRegisterShadow.size() == 7);
  for (const auto& range : profile::kRegisterShadow)
    assert(uint64_t(range.offset) + uint64_t(range.count) * 4 <= d.size);
  std::puts("Superman profile: confirmed layout tests passed");
}
