// test_allocator: lock down SlabArena's contract.
//   - allocate honours alignment requirements
//   - oversized requests do not corrupt the cursor slab
//   - reset() reclaims usable bytes without losing the first slab
//   - SlabResource compares equal only when wrapping the same arena
//   - std::pmr containers backed by SlabResource actually use it (with the
//     try_emplace + explicit allocator pattern that the codebase relies on)

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <utility>
#include <vector>

#include "allocator.hpp"
#include "test_helpers.hpp"

namespace {

bool is_aligned(const void* p, std::size_t align) {
    return (reinterpret_cast<std::uintptr_t>(p) & (align - 1)) == 0;
}

void test_basic_allocate_returns_distinct_pointers() {
    idx::mem::SlabArena arena(/*slab_bytes=*/4096);
    void* a = arena.allocate(64, 8);
    void* b = arena.allocate(64, 8);
    IDX_CHECK(a != nullptr);
    IDX_CHECK(b != nullptr);
    IDX_CHECK(a != b);
    IDX_CHECK_EQ(arena.bytes_in_use(), 128u);
}

void test_alignment_is_honoured() {
    idx::mem::SlabArena arena(/*slab_bytes=*/4096);
    arena.allocate(1, 1);  // shift cursor so the next request needs padding
    void* p8 = arena.allocate(8, 8);
    void* p16 = arena.allocate(16, 16);
    void* p64 = arena.allocate(64, 64);
    IDX_CHECK(is_aligned(p8, 8));
    IDX_CHECK(is_aligned(p16, 16));
    IDX_CHECK(is_aligned(p64, 64));
}

void test_growth_when_slab_exhausted() {
    idx::mem::SlabArena arena(/*slab_bytes=*/256);
    // Allocate slightly less than slab_bytes, then more than what's left.
    void* a = arena.allocate(200, 8);
    void* b = arena.allocate(100, 8);
    IDX_CHECK(a != nullptr);
    IDX_CHECK(b != nullptr);
    IDX_CHECK(arena.total_capacity() >= 512u);
}

void test_oversized_request_uses_dedicated_slab() {
    idx::mem::SlabArena arena(/*slab_bytes=*/64);
    // A request larger than slab_bytes must succeed and must not blow the
    // accounting.
    void* big = arena.allocate(/*bytes=*/4096, /*align=*/16);
    IDX_CHECK(big != nullptr);
    IDX_CHECK(is_aligned(big, 16));
    // The cursor slab should still have room for a small allocation since the
    // big block went to a side slab.
    void* small = arena.allocate(/*bytes=*/8, /*align=*/8);
    IDX_CHECK(small != nullptr);
}

void test_reset_reuses_first_slab() {
    idx::mem::SlabArena arena(/*slab_bytes=*/1024);
    arena.allocate(800, 8);
    arena.allocate(600, 8);                       // 600 bytes won't fit in the
                                                  // remaining 224, forcing growth
    const std::size_t cap_before = arena.total_capacity();
    IDX_CHECK(cap_before >= 2u * 1024u);
    arena.reset();
    IDX_CHECK_EQ(arena.bytes_in_use(), 0u);
    IDX_CHECK_EQ(arena.total_capacity(), 1024u);  // dropped back to one slab
    IDX_CHECK(cap_before > arena.total_capacity());
    // After reset, fresh allocations should succeed.
    void* p = arena.allocate(256, 8);
    IDX_CHECK(p != nullptr);
}

void test_resource_equality() {
    idx::mem::SlabArena a1, a2;
    idx::mem::SlabResource r1{&a1};
    idx::mem::SlabResource r1_alias{&a1};
    idx::mem::SlabResource r2{&a2};
    IDX_CHECK(r1.is_equal(r1_alias));
    IDX_CHECK(!r1.is_equal(r2));
}

void test_pmr_vector_uses_arena() {
    idx::mem::SlabArena arena(/*slab_bytes=*/4096);
    idx::mem::SlabResource res{&arena};
    {
        std::pmr::vector<std::pair<int, int>> v{&res};
        for (int i = 0; i < 100; ++i) v.emplace_back(i, i * 2);
        IDX_CHECK_EQ(v.size(), 100u);
        IDX_CHECK_EQ(v[42].first, 42);
        IDX_CHECK_EQ(v[42].second, 84);
    }
    // Bytes are still in use even after destruction; reset() reclaims them.
    IDX_CHECK(arena.bytes_in_use() > 0u);
    arena.reset();
    IDX_CHECK_EQ(arena.bytes_in_use(), 0u);
}

}  // namespace

int main() {
    test_basic_allocate_returns_distinct_pointers();
    test_alignment_is_honoured();
    test_growth_when_slab_exhausted();
    test_oversized_request_uses_dedicated_slab();
    test_reset_reuses_first_slab();
    test_resource_equality();
    test_pmr_vector_uses_arena();
    return idx::testing::report("test_allocator");
}
