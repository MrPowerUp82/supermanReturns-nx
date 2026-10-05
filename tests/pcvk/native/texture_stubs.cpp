// The Xenos texture decoder needs the ReXGlue SDK. The front-end host tests never bind a
// texture, so these stand-ins only have to link (and fail loudly if they are ever reached).
#include "pcvk/graphics/guest/texture_capture.h"
namespace superman_returns::graphics::guest {
bool DescribeTextureRanges(std::span<const uint32_t, 6>, std::vector<TextureRange>&, std::string& error) {
  error = "texture decoder not available in host tests";
  return false;
}
bool CaptureTexture(std::span<const uint32_t, 6>, uint64_t, const GuestMemoryReader&,
                    std::shared_ptr<const TextureCapture>&, std::string& error) {
  error = "texture decoder not available in host tests";
  return false;
}
}  // namespace superman_returns::graphics::guest
