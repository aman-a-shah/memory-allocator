#pragma once

#include <cstddef>
#include <new>
#include <utility>

#include "memalloc/arena.hpp"

// Fixed-size slab allocator.
//
// A SlabPool hands out identically sized slots carved from the Arena. It is the
// fast path for the repetitive, same-shape objects a trading system churns
// through (Order, TradeExecution, tick updates).
//
// Two properties make it cheap:
//   * Intrusive free list -- a free slot stores the "next free" pointer inside
//     its own (unused) memory, so there is no external bookkeeping structure.
//   * Zero header overhead -- a live allocation is a raw slot pointer with no
//     prepended metadata. Every slot in a pool is the same size, so free() can
//     simply push the pointer back onto the list without needing to know which
//     block it came from.
//
// allocate() and deallocate() are O(1) pointer manipulations. The pool only
// touches the Arena when it runs out of slots and must grow by one more block.

namespace memalloc {

class SlabPool;

// Optional hook invoked whenever a pool carves a new block from the Arena. A
// higher layer (e.g. the unified Allocator) uses it to record which address
// ranges belong to which pool, so deallocate() can be routed by address without
// reintroducing a per-object header (which would break the zero-header design).
class SlabBlockObserver {
public:
    virtual void on_slab_block(SlabPool* pool, void* base,
                               std::size_t bytes) noexcept = 0;

protected:
    ~SlabBlockObserver() = default;
};

class SlabPool {
public:
    // Creates a pool of `slot_size`-byte slots aligned to `slot_alignment`.
    // The effective slot size is rounded up so that (a) it can hold the
    // intrusive free-list pointer and (b) it is a multiple of the alignment;
    // the effective alignment is at least alignof(void*). The pool grows by
    // `slots_per_block` slots per Arena request. If `observer` is non-null it is
    // notified of every block the pool carves from the Arena.
    SlabPool(Arena& arena, std::size_t slot_size, std::size_t slot_alignment,
             std::size_t slots_per_block = 64,
             SlabBlockObserver* observer = nullptr) noexcept;

    // Non-copyable, non-movable: holds a reference to its Arena and raw
    // free-list pointers into it.
    SlabPool(const SlabPool&) = delete;
    SlabPool& operator=(const SlabPool&) = delete;

    // Returns a free slot, growing from the Arena if necessary. Returns nullptr
    // only if the Arena is exhausted. The returned memory is uninitialized.
    void* allocate() noexcept;

    // Returns a slot to the free list. `p` must be a pointer previously handed
    // out by this pool's allocate() (passing nullptr is a no-op).
    void deallocate(void* p) noexcept;

    // The actual per-slot size after rounding for the free-list pointer/align.
    std::size_t slot_size() const noexcept { return slot_size_; }
    std::size_t slot_alignment() const noexcept { return slot_alignment_; }
    std::size_t slots_per_block() const noexcept { return slots_per_block_; }

    // Total slots carved from the Arena so far.
    std::size_t capacity() const noexcept { return capacity_; }
    // Slots currently on the free list.
    std::size_t free_count() const noexcept { return free_count_; }
    // Slots currently handed out to callers.
    std::size_t in_use() const noexcept { return capacity_ - free_count_; }
    // Number of blocks requested from the Arena (grows on each refill).
    std::size_t block_count() const noexcept { return block_count_; }

private:
    struct FreeNode {
        FreeNode* next;
    };

    // Requests one more block from the Arena and links its slots into the free
    // list. Returns false if the Arena cannot satisfy the request.
    bool grow() noexcept;

    Arena& arena_;
    SlabBlockObserver* observer_;
    FreeNode* free_head_ = nullptr;
    std::size_t slot_size_;
    std::size_t slot_alignment_;
    std::size_t slots_per_block_;
    std::size_t capacity_ = 0;
    std::size_t free_count_ = 0;
    std::size_t block_count_ = 0;
};

// Typed convenience wrapper. Sizes/aligns the underlying SlabPool for T and
// adds construct()/destroy() that run T's constructor/destructor in place.
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(Arena& arena, std::size_t slots_per_block = 64) noexcept
        : pool_(arena, sizeof(T), alignof(T), slots_per_block) {}

    // Raw slot (uninitialized storage for a T), or nullptr if exhausted.
    T* allocate() noexcept { return static_cast<T*>(pool_.allocate()); }

    // Returns a raw slot to the pool. Does NOT run T's destructor.
    void deallocate(T* p) noexcept { pool_.deallocate(p); }

    // Allocates a slot and constructs a T in place. Returns nullptr if the
    // Arena is exhausted (no construction is attempted in that case).
    template <typename... Args>
    T* create(Args&&... args) {
        void* mem = pool_.allocate();
        if (mem == nullptr) {
            return nullptr;
        }
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    // Runs T's destructor and returns the slot to the pool (nullptr is a no-op).
    void destroy(T* p) noexcept {
        if (p == nullptr) {
            return;
        }
        p->~T();
        pool_.deallocate(p);
    }

    std::size_t capacity() const noexcept { return pool_.capacity(); }
    std::size_t in_use() const noexcept { return pool_.in_use(); }
    std::size_t free_count() const noexcept { return pool_.free_count(); }
    std::size_t block_count() const noexcept { return pool_.block_count(); }

    SlabPool& pool() noexcept { return pool_; }

private:
    SlabPool pool_;
};

}  // namespace memalloc
