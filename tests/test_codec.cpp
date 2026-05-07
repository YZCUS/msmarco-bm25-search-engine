// test_codec: exercise idx::codec for whichever codec was selected at build
// time. Round-trip a wide range of values and assert that idx::codec::kName
// matches the IDX_CODEC_ macro.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "codec.hpp"
#include "test_helpers.hpp"

namespace {

void test_round_trip_for_active_codec() {
    const std::uint32_t cases[] = {
        0u, 1u, 127u, 128u, 16383u, 16384u, 65535u, 65536u,
        2097151u, 2097152u, 268435455u, UINT32_MAX,
    };
    std::vector<std::uint8_t> buf;
    for (auto v : cases) idx::codec::encode(v, buf);

    std::size_t pos = 0;
    for (auto v : cases) {
        std::size_t consumed = 0;
        const auto got = idx::codec::decode(buf.data() + pos, consumed);
        IDX_CHECK_EQ(got, v);
        IDX_CHECK(consumed >= 1u && consumed <= 5u);
        pos += consumed;
    }
    IDX_CHECK_EQ(pos, buf.size());
}

void test_codec_name_matches_build_macro() {
#if defined(IDX_CODEC_Raw32)
    IDX_CHECK_EQ(std::string{idx::codec::kName}, std::string{"Raw32"});
#else
    IDX_CHECK_EQ(std::string{idx::codec::kName}, std::string{"VarByte"});
#endif
}

}  // namespace

int main() {
    test_round_trip_for_active_codec();
    test_codec_name_matches_build_macro();
    return idx::testing::report("test_codec");
}
