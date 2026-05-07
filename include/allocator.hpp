#pragma once

// Monotonic slab arena and a std::pmr::memory_resource adapter.
//
// Intended use:
//   - Build path: keep one arena per spill batch; reset() between batches.
//   - Query path: keep one thread-local arena per worker; reset() per query.
//
// Caveats:
//   - Not thread-safe. One arena per thread.
//   - do_deallocate is a no-op; memory comes back only via reset().
//   - std::pmr::unordered_map::operator[] does NOT propagate the outer
//     allocator to inner containers; construct inner vectors explicitly with
//     try_emplace + the allocator argument, otherwise the arena is bypassed.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <vector>

namespace idx::mem {

class SlabArena {
public:
    explicit SlabArena(std::size_t slab_bytes = 8 * 1024 * 1024)
        : slab_bytes_(slab_bytes) {
        grow();
    }

    SlabArena(const SlabArena&) = delete;
    SlabArena& operator=(const SlabArena&) = delete;
    SlabArena(SlabArena&&) noexcept = default;
    SlabArena& operator=(SlabArena&&) noexcept = default;

    void* allocate(std::size_t bytes, std::size_t align) {
        const auto cur = reinterpret_cast<std::uintptr_t>(cursor_);
        const auto aligned = (cur + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
        const std::size_t pad = aligned - cur;
        if (pad + bytes > remaining_) {
            // Oversized request gets its own slab so the cursor slab is preserved.
            if (bytes + align > slab_bytes_) {
                slabs_.emplace_back(std::make_unique<std::byte[]>(bytes + align));
                const auto base = reinterpret_cast<std::uintptr_t>(slabs_.back().get());
                const auto a = (base + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
                bytes_in_use_ += bytes;
                return reinterpret_cast<void*>(a);
            }
            grow();
            return allocate(bytes, align);
        }
        cursor_ = reinterpret_cast<std::byte*>(aligned + bytes);
        remaining_ -= (pad + bytes);
        bytes_in_use_ += bytes;
        return reinterpret_cast<void*>(aligned);
    }

    // Drop all but the first slab and rewind the cursor to the slab head.
    void reset() noexcept {
        if (slabs_.size() > 1) slabs_.resize(1);
        cursor_ = slabs_.front().get();
        remaining_ = slab_bytes_;
        bytes_in_use_ = 0;
    }

    std::size_t bytes_in_use() const noexcept { return bytes_in_use_; }
    std::size_t total_capacity() const noexcept { return slabs_.size() * slab_bytes_; }

private:
    void grow() {
        slabs_.emplace_back(std::make_unique<std::byte[]>(slab_bytes_));
        cursor_ = slabs_.back().get();
        remaining_ = slab_bytes_;
    }

    std::vector<std::unique_ptr<std::byte[]>> slabs_;
    std::byte* cursor_ = nullptr;
    std::size_t remaining_ = 0;
    std::size_t slab_bytes_;
    std::size_t bytes_in_use_ = 0;
};

class SlabResource final : public std::pmr::memory_resource {
public:
    explicit SlabResource(SlabArena* arena) : arena_(arena) {}

protected:
    void* do_allocate(std::size_t bytes, std::size_t align) override {
        return arena_->allocate(bytes, align);
    }

    void do_deallocate(void*, std::size_t, std::size_t) override {
        // Monotonic resource; reclaiming happens via SlabArena::reset().
    }

    bool do_is_equal(const memory_resource& other) const noexcept override {
        const auto* o = dynamic_cast<const SlabResource*>(&other);
        return o != nullptr && o->arena_ == arena_;
    }

private:
    SlabArena* arena_;
};

}  // namespace idx::mem
