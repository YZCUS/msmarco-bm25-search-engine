#pragma once

// LSB-first variable-length integer codec (a.k.a. VarByte / varint).
// Compatible with Protocol Buffers and Lucene VInt encoding.
//
// Encoding rules:
//   - Continuation bit is the most-significant bit of each byte.
//   - The 7 low bits of each byte carry the value, least-significant chunk first.
//   - Zero encodes to a single 0x00 byte.
//   - A uint32_t value occupies at most 5 bytes (ceil(32 / 7)).
//
// This header is intentionally header-only so both the index builder and the
// search engine see the exact same encoding.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace idx::varbyte {

inline void encode(std::uint32_t v, std::vector<std::uint8_t>& out) {
    while (v >= 0x80u) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80u));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

// Decode a single value starting at `data`. On return, `consumed` holds the
// number of bytes read. Caller must guarantee that `data` has at least 5
// readable bytes available for safety.
inline std::uint32_t decode(const std::uint8_t* data, std::size_t& consumed) {
    std::uint32_t v = 0;
    std::uint32_t shift = 0;
    consumed = 0;
    while (true) {
        const std::uint8_t b = data[consumed++];
        v |= static_cast<std::uint32_t>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) return v;
        shift += 7;
        // Five 7-bit chunks already cover uint32. Anything beyond is malformed.
        if (shift >= 35) {
            throw std::runtime_error("varbyte: decode overflow");
        }
    }
}

inline std::uint32_t decode_bounded(const std::uint8_t* data, std::size_t available,
                                    std::size_t& consumed) {
    std::uint32_t v = 0;
    std::uint32_t shift = 0;
    consumed = 0;
    while (true) {
        if (consumed >= available) {
            throw std::runtime_error("varbyte: truncated value");
        }
        const std::uint8_t b = data[consumed++];
        v |= static_cast<std::uint32_t>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) return v;
        shift += 7;
        if (shift >= 35) {
            throw std::runtime_error("varbyte: decode overflow");
        }
    }
}

// Convenience wrapper that decodes a fixed-size vector representation.
inline std::uint32_t decode(const std::vector<std::uint8_t>& bytes) {
    std::size_t consumed = 0;
    return decode(bytes.data(), consumed);
}

}  // namespace idx::varbyte
