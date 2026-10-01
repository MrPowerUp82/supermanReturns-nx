#pragma once

// Superman Returns shader containers use the layout XenosRecomp expects
// (signature 0x102A1100/0x102A1101), so nothing is converted. This header only
// validates a candidate before it is translated or packed: header sizes, the
// constant table and the microcode control flow (nfsmw::ValidarMicrocodigo,
// the same check the NFSMW port applies to its 2005 containers).
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "nfsmw_contenedor.h"

namespace sr {

constexpr uint32_t kHeaderSize = 0x24;
constexpr uint32_t kMaxContainer = 64 * 1024;

inline bool IsSignature(uint32_t flags) { return (flags & 0xFFFFFFFEu) == 0x102A1100u; }

// Throws std::runtime_error with the reason when the bytes are not a whole,
// self-consistent container. `data` must be exactly virtualSize + physicalSize.
inline nfsmw::Flujo Validate(const std::vector<uint8_t>& data) {
  const nfsmw::Lector l{data};
  l.rango(0, kHeaderSize);
  const uint32_t flags = l.u32(0), vs = l.u32(4), ps = l.u32(8);
  if (!IsSignature(flags)) throw std::runtime_error("unknown container signature");
  if (vs < kHeaderSize || !ps || uint64_t(vs) + ps != data.size() || data.size() > kMaxContainer)
    throw std::runtime_error("inconsistent container sizes");
  const bool pixel = !(flags & 1);
  const uint32_t ct = l.u32(16), def = l.u32(20), sh = l.u32(24);
  const auto in_virtual = [&](uint64_t o, uint64_t n) {
    if (o > vs || n > vs - o) throw std::runtime_error("table outside the virtual part");
  };
  in_virtual(sh, pixel ? 32 : 36);
  if (ct < kHeaderSize) throw std::runtime_error("missing constant table");
  in_virtual(ct, 32);
  const uint32_t base_ct = ct + 4, constants = l.u32(base_ct + 12);
  in_virtual(uint64_t(base_ct) + l.u32(base_ct + 16), uint64_t(constants) * 20);
  for (uint32_t i = 0; i < constants; ++i) {
    const uint64_t info = uint64_t(base_ct) + l.u32(base_ct + 16) + i * 20ull;
    const uint64_t name = uint64_t(base_ct) + l.u32(uint32_t(info));
    in_virtual(name, 1);
    if (!std::memchr(data.data() + name, 0, vs - name))
      throw std::runtime_error("constant name without terminator");
    in_virtual(uint64_t(base_ct) + l.u32(uint32_t(info + 12)), 16);
  }
  if (def) in_virtual(def, 24);
  const uint32_t code_offset = l.u32(sh), code_size = l.u32(sh + 4);
  if (uint64_t(code_offset) + code_size > ps)
    throw std::runtime_error("microcode outside the physical part");
  return nfsmw::ValidarMicrocodigo(l, vs + code_offset, code_size);
}

}  // namespace sr
