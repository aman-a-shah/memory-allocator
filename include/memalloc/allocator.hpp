#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "memalloc/arena.hpp"
#include "memalloc/slab.hpp"
#include "memalloc/tlsf.hpp"

// Unified allocator facade.
//
// One entry point that routes each request to the engine best suited for it:
//   * small fixed-size requests  -> a per-size-class Slab pool (O(1), zero
//     header, ideal for the repetitive trading objects)
//   * everything larger          -> the TLSF engine (O(1) good-fit + coalescing)
//
// Both engines draw from a single Arena (the plan's "one mmap region split into
// two engines"). Because the slab path is zero-header, deallocate() cannot read
// a tag off the pointer to learn which engine owns it. Instead the facade keeps
// an address-range registry: each slab pool reports the blocks it carves, and
// deallocate() binary-searches those ranges -- a hit routes to the owning pool,
// a miss routes to TLSF. This preserves the zero-header slab fast path.

namespace memalloc {

class Allocator : private SlabBlockObserver {
public:
    struct Config {
        std::size_t arena_bytes = 16u * 1024 * 1024;   // total OS reservation
        std::size_t tlsf_region_bytes = 256u * 1024;   // TLSF growth chunk
        std::size_t slab_block_target = 64u * 1024;    // bytes per slab block
    };

    Allocator() noexcept;
    explicit Allocator(const Config& cfg) noexcept;

    // Builds the allocator over a caller-provided arena (moved in) instead of
    // mmapping its own. `cfg.arena_bytes` is ignored. This is how the concurrent
    // layer runs one independent engine per thread over a shared-region
    // superblock. The moved-in arena may be a borrowed (non-owning) Arena.
    Allocator(Arena&& arena, const Config& cfg) noexcept;

    // Non-copyable / non-movable: the engines hold references into our Arena.
    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    // Allocates at least `bytes`. Routes by size; falls back to TLSF if a slab
    // class cannot grow. Returns nullptr only when the Arena is exhausted.
    void* allocate(std::size_t bytes) noexcept;

    // Frees a pointer from allocate() (nullptr is a no-op), routing to whichever
    // engine owns its address.
    void deallocate(void* p) noexcept;

    // Typed helpers: allocate + construct, destruct + free.
    template <typename T, typename... Args>
    T* create(Args&&... args) {
        static_assert(alignof(T) <= alignof(std::max_align_t),
                      "over-aligned types are not supported by the facade");
        void* mem = allocate(sizeof(T));
        if (mem == nullptr) {
            return nullptr;
        }
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    template <typename T>
    void destroy(T* p) noexcept {
        if (p == nullptr) {
            return;
        }
        p->~T();
        deallocate(p);
    }

    // --- Introspection (used by tests / diagnostics) ---
    // Largest request still served by the slab path.
    std::size_t slab_threshold() const noexcept {
        return classes_.empty() ? 0 : classes_.back();
    }
    // True if a request of `bytes` would be served by the slab path.
    bool routes_to_slab(std::size_t bytes) const noexcept {
        return bytes != 0 && bytes <= slab_threshold();
    }
    std::size_t slab_live() const noexcept { return slab_live_; }
    std::size_t tlsf_live() const noexcept { return tlsf_live_; }

    // True if `p` lies within this allocator's arena (i.e. this engine owns it).
    // Used by the concurrent layer to route a free to its home thread cache.
    bool owns(const void* p) const noexcept { return arena_.owns(p); }

    // Payload bytes currently handed out by the TLSF engine (for fragmentation
    // accounting in benchmarks).
    std::size_t tlsf_used_bytes() const noexcept { return tlsf_.used_bytes(); }
    std::size_t tlsf_free_bytes() const noexcept { return tlsf_.free_bytes(); }

    // Total bytes the arena has committed to the engines so far (the process's
    // resident footprint high-water mark). Flat across cycles == no leak/growth.
    std::size_t arena_used_bytes() const noexcept { return arena_.used(); }

private:
    void on_slab_block(SlabPool* pool, void* base,
                       std::size_t bytes) noexcept override;

    // Shared construction tail: builds the per-class slab pools.
    void init_pools(const Config& cfg) noexcept;

    // Pool for a request size, or nullptr if it should go to TLSF.
    SlabPool* pool_for_size(std::size_t bytes) const noexcept;
    // Owning pool for a pointer, or nullptr if TLSF owns it.
    SlabPool* pool_for_ptr(const void* p) const noexcept;

    struct Range {
        std::uintptr_t lo;
        std::uintptr_t hi;
        SlabPool* pool;
    };

    Arena arena_;
    Tlsf tlsf_;
    std::vector<std::size_t> classes_;               // size classes, ascending
    std::vector<std::unique_ptr<SlabPool>> pools_;   // parallel to classes_
    std::vector<Range> ranges_;                      // slab blocks, sorted by lo
    std::size_t slab_live_ = 0;
    std::size_t tlsf_live_ = 0;
};

}  // namespace memalloc
