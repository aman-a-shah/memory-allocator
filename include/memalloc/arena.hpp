#pragma once

#include <cstddef>

// The Arena is the OS-backed backing store for the whole allocator. At startup
// it reserves one large contiguous region via mmap and hands out aligned
// sub-blocks to the Slab and TLSF engines using a simple bump (cursor) pointer.
//
// Phase 1 scope: reserve / carve / reset / release. The arena does NOT track
// or reclaim individual sub-blocks — that is the job of the engines layered on
// top. The only way to reclaim arena space is reset() (rewind everything) or
// destruction (munmap).

namespace memalloc {

class Arena {
public:
    // Reserves `capacity_bytes` from the OS via mmap. The actual reserved size
    // is rounded up to a multiple of the system page size and is observable via
    // capacity(). Throws std::bad_alloc if the mapping fails.
    explicit Arena(std::size_t capacity_bytes);

    ~Arena();

    // Non-copyable (owns a unique OS mapping).
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // Movable: transfers ownership of the mapping.
    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;

    // Carves `bytes` from the arena, aligned to `alignment` (must be a power of
    // two). Returns a pointer into the mapping, or nullptr if there is not
    // enough remaining space. The returned memory is zero-filled (mmap of
    // anonymous memory is guaranteed zeroed) until first reuse after reset().
    void* allocate(std::size_t bytes,
                   std::size_t alignment = alignof(std::max_align_t)) noexcept;

    // Rewinds the cursor to the base, logically freeing every prior allocation.
    // Does not return memory to the OS.
    void reset() noexcept;

    // Total usable bytes in the mapping (page-rounded).
    std::size_t capacity() const noexcept { return capacity_; }

    // Bytes handed out so far, including alignment padding.
    std::size_t used() const noexcept { return offset_; }

    // Bytes still available for allocation (before alignment padding).
    std::size_t remaining() const noexcept { return capacity_ - offset_; }

    // Base address of the mapping.
    void* base() const noexcept { return base_; }

    // True if `p` points within this arena's mapping.
    bool owns(const void* p) const noexcept;

private:
    char* base_ = nullptr;       // start of the mmap region
    std::size_t capacity_ = 0;   // page-rounded reserved size
    std::size_t offset_ = 0;     // bump cursor (bytes used from base_)
};

}  // namespace memalloc
