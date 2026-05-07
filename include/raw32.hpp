#pragma once

// Fixed 4-byte little-endian uint32 codec. Used as the "uncompressed" baseline
// in the VarByte vs Raw32 ablation that quantifies how much disk space the
// VarByte path saves on real MS MARCO data.
//
// Format:
//   bytes[0..3] = uint32 little-endian
//   `decode` always advances exactly 4 bytes, regardless of value.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace idx::raw32 {

inline void encode(std::uint32_t v, std::vector<std::uint8_t>& out) {
    const std::size_t base = out.size();
    out.resize(base + 4);
    out[base + 0] = static_cast<std::uint8_t>(v);
    out[base + 1] = static_cast<std::uint8_t>(v >> 8);
    out[base + 2] = static_cast<std::uint8_t>(v >> 16);
    out[base + 3] = static_cast<std::uint8_t>(v >> 24);
}

inline std::uint32_t decode(const std::uint8_t* data, std::size_t& consumed) {
    consumed = 4;
    return static_cast<std::uint32_t>(data[0])
         | static_cast<std::uint32_t>(data[1]) << 8
         | static_cast<std::uint32_t>(data[2]) << 16
         | static_cast<std::uint32_t>(data[3]) << 24;
}

inline std::uint32_t decode_bounded(const std::uint8_t* data, std::size_t available,
                                    std::size_t& consumed) {
    if (available < 4) {
        throw std::runtime_error("raw32: truncated value");
    }
    return decode(data, consumed);
}

}  // namespace idx::raw32
