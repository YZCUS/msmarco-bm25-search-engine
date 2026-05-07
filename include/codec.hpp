#pragma once

// Codec selection layer. The build system picks one implementation by
// defining IDX_CODEC_VarByte (default) or IDX_CODEC_Raw32. Anywhere index
// bytes are written or read, prefer this header over the concrete codec
// header so that switching the codec only touches the build configuration.

#include "raw32.hpp"
#include "varbyte.hpp"

namespace idx::codec {

#if defined(IDX_CODEC_Raw32)

inline constexpr const char* kName = "Raw32";

inline void encode(std::uint32_t v, std::vector<std::uint8_t>& out) {
    idx::raw32::encode(v, out);
}

inline std::uint32_t decode(const std::uint8_t* data, std::size_t& consumed) {
    return idx::raw32::decode(data, consumed);
}

inline std::uint32_t decode_bounded(const std::uint8_t* data, std::size_t available,
                                    std::size_t& consumed) {
    return idx::raw32::decode_bounded(data, available, consumed);
}

#else  // default: VarByte

inline constexpr const char* kName = "VarByte";

inline void encode(std::uint32_t v, std::vector<std::uint8_t>& out) {
    idx::varbyte::encode(v, out);
}

inline std::uint32_t decode(const std::uint8_t* data, std::size_t& consumed) {
    return idx::varbyte::decode(data, consumed);
}

inline std::uint32_t decode_bounded(const std::uint8_t* data, std::size_t available,
                                    std::size_t& consumed) {
    return idx::varbyte::decode_bounded(data, available, consumed);
}

#endif

}  // namespace idx::codec
