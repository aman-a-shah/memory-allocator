#pragma once

#include <cstddef>
#include <cstdint>

// Alignment utilities shared across the allocator engines. All allocations in
// this project are aligned to hardware boundaries (8-byte default, 64-byte for
// cache-line-sensitive structures) so the CPU can read them in a single bus
// cycle without unaligned-access penalties.

namespace memalloc {

// Returns true if `x` is a power of two (and non-zero).
constexpr bool is_power_of_two(std::size_t x) noexcept {
    return x != 0 && (x & (x - 1)) == 0;
}

// Rounds `n` up to the nearest multiple of `alignment`.
// `alignment` must be a power of two.
constexpr std::size_t align_up(std::size_t n, std::size_t alignment) noexcept {
    return (n + (alignment - 1)) & ~(alignment - 1);
}

// Rounds `n` down to the nearest multiple of `alignment`.
// `alignment` must be a power of two.
constexpr std::size_t align_down(std::size_t n, std::size_t alignment) noexcept {
    return n & ~(alignment - 1);
}

// Returns true if `n` is already a multiple of `alignment`.
constexpr bool is_aligned(std::size_t n, std::size_t alignment) noexcept {
    return (n & (alignment - 1)) == 0;
}

// Pointer-typed convenience wrappers.
inline void* align_up_ptr(void* p, std::size_t alignment) noexcept {
    return reinterpret_cast<void*>(
        align_up(reinterpret_cast<std::uintptr_t>(p), alignment));
}

inline bool is_aligned_ptr(const void* p, std::size_t alignment) noexcept {
    return is_aligned(reinterpret_cast<std::uintptr_t>(p), alignment);
}

// Common alignment constants.
inline constexpr std::size_t kWordAlignment = alignof(std::max_align_t);
inline constexpr std::size_t kCacheLineSize = 64;

}  // namespace memalloc
