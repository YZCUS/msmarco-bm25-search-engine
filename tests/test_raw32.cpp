// test_raw32: round-trip and width invariants for the fixed-size codec used
// as the "uncompressed" baseline in the compression-ratio benchmark.

#include <cstdint>
#include <vector>

#include "raw32.hpp"
#include "test_helpers.hpp"

namespace {

void test_every_value_uses_four_bytes() {
    const std::uint32_t cases[] = {0u, 1u, 127u, 128u, 65535u, 65536u, 1234567u, UINT32_MAX};
    for (auto v : cases) {
        std::vector<std::uint8_t> buf;
        idx::raw32::encode(v, buf);
        IDX_CHECK_EQ(buf.size(), 4u);

        std::size_t consumed = 0;
        const auto out = idx::raw32::decode(buf.data(), consumed);
        IDX_CHECK_EQ(consumed, 4u);
        IDX_CHECK_EQ(out, v);
    }
}

void test_little_endian_layout() {
    std::vector<std::uint8_t> buf;
    idx::raw32::encode(0x04030201u, buf);
    IDX_CHECK_EQ(static_cast<int>(buf[0]), 0x01);
    IDX_CHECK_EQ(static_cast<int>(buf[1]), 0x02);
    IDX_CHECK_EQ(static_cast<int>(buf[2]), 0x03);
    IDX_CHECK_EQ(static_cast<int>(buf[3]), 0x04);
}

void test_sequence_decode() {
    const std::uint32_t cases[] = {0u, 0xFFu, 0xDEADBEEFu, 42u};
    std::vector<std::uint8_t> buf;
    for (auto v : cases) idx::raw32::encode(v, buf);

    std::size_t pos = 0;
    for (auto v : cases) {
        std::size_t consumed = 0;
        IDX_CHECK_EQ(idx::raw32::decode(buf.data() + pos, consumed), v);
        IDX_CHECK_EQ(consumed, 4u);
        pos += consumed;
    }
    IDX_CHECK_EQ(pos, buf.size());
}

}  // namespace

int main() {
    test_every_value_uses_four_bytes();
    test_little_endian_layout();
    test_sequence_decode();
    return idx::testing::report("test_raw32");
}
