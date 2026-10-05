// The Xenos texture decoder needs the ReXGlue SDK. The host tests stand in for the one function
// the guest layer calls from it: a bound texture is one 4 KB range at the fetch constant's base.
#include "pcvk/graphics/guest/texture_capture.h"
namespace superman_returns::graphics::guest {
bool DescribeTextureRanges(std::span<const uint32_t, 6> fetch, std::vector<TextureRange>& ranges,
                           std::string& error) {
  if ((fetch[0] & 3) != 2) {
    error = "not a texture fetch constant";
    return false;
  }
  ranges = {{fetch[1] & ~0xFFFu, 4096}};
  return true;
}
}  // namespace superman_returns::graphics::guest
