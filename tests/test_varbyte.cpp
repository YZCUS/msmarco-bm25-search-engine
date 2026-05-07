// test_varbyte: round-trip every boundary value of the LSB-first VarByte
// codec and verify that sequences of values decode in order.

#include <cstdint>
#include <vector>

#include "test_helpers.hpp"
#include "varbyte.hpp"

namespace {

void check_roundtrip(std::uint32_t v, std::size_t expected_bytes) {
    std::vector<std::uint8_t> buf;
    idx::varbyte::encode(v, buf);
    IDX_CHECK_EQ(buf.size(), expected_bytes);

    std::size_t consumed = 0;
    const std::uint32_t out = idx::varbyte::decode(buf.data(), consumed);
    IDX_CHECK_EQ(out, v);
    IDX_CHECK_EQ(consumed, buf.size());
}

void test_single_value_boundaries() {
    check_roundtrip(0u, 1);
    check_roundtrip(1u, 1);
    check_roundtrip(127u, 1);             // 7-bit boundary
    check_roundtrip(128u, 2);             // first 2-byte value
    check_roundtrip(255u, 2);
    check_roundtrip(16383u, 2);           // 14-bit boundary
    check_roundtrip(16384u, 3);           // first 3-byte value
    check_roundtrip(2097151u, 3);         // 21-bit boundary
    check_roundtrip(2097152u, 4);         // first 4-byte value
    check_roundtrip(268435455u, 4);       // 28-bit boundary
    check_roundtrip(268435456u, 5);       // first 5-byte value
    check_roundtrip(UINT32_MAX, 5);       // codec must use exactly 5 bytes
}

void test_zero_uses_single_zero_byte() {
    std::vector<std::uint8_t> buf;
    idx::varbyte::encode(0u, buf);
    IDX_CHECK_EQ(buf.size(), 1u);
    IDX_CHECK_EQ(static_cast<int>(buf[0]), 0);
}

void test_continuation_bits_correct() {
    // 128 -> bytes: 10000000 00000001 (LSB first, MSB=1 on first byte)
    std::vector<std::uint8_t> buf;
    idx::varbyte::encode(128u, buf);
    IDX_CHECK_EQ(buf.size(), 2u);
    IDX_CHECK_EQ(static_cast<int>(buf[0]), 0x80);
    IDX_CHECK_EQ(static_cast<int>(buf[1]), 0x01);
}

void test_sequence_decode() {
    const std::uint32_t cases[] = {0u, 127u, 128u, 16383u, 16384u, 2097151u, UINT32_MAX};
    std::vector<std::uint8_t> buf;
    for (auto v : cases) idx::varbyte::encode(v, buf);

    std::size_t pos = 0;
    for (auto v : cases) {
        std::size_t consumed = 0;
        const std::uint32_t out = idx::varbyte::decode(buf.data() + pos, consumed);
        IDX_CHECK_EQ(out, v);
        pos += consumed;
    }
    IDX_CHECK_EQ(pos, buf.size());
}

}  // namespace

int main() {
    test_single_value_boundaries();
    test_zero_uses_single_zero_byte();
    test_continuation_bits_correct();
    test_sequence_decode();
    return idx::testing::report("test_varbyte");
}
