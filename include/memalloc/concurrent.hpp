#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

#include "memalloc/allocator.hpp"
#include "memalloc/arena.hpp"

// Lock-free multi-threaded allocator (Phase 6).
//
// The design follows the plan's interview defenses literally:
//
//   * One global shared arena (a single large mmap region) is carved into
//     fixed-size "superblocks" with an atomic compare-and-swap bump cursor --
//     the only synchronization on the cold path, hit once per thread.
//
//   * Each thread owns a private, isolated ThreadCache: a full single-thread
//     `Allocator` (slab + TLSF) running over one superblock. Allocation and the
//     same-thread free path therefore take *no* locks and touch *no* shared
//     state -- they are exactly the single-thread fast paths from Phase 5.
//
//   * Cross-thread free: a block freed by a thread other than its owner is not
//     touched in place. It is pushed onto the owning cache's lock-free MPSC
//     "remote free" stack (a Treiber stack threaded through the freed block's
//     own memory -- zero extra storage). The owner drains this stack at the top
//     of its next allocation and recycles the slots locally. This keeps the hot
//     path free of cross-thread writes and the critical path lock-free.
//
// Ownership of a pointer is resolved by address: each cache's arena occupies a
// disjoint superblock range, so a free walks the (small) registry of caches and
// asks each `owns(p)`. The arena bounds are immutable after construction, so the
// walk needs no lock.

namespace memalloc {

// Per-thread allocation cache: an isolated slab+TLSF engine plus a lock-free
// inbox for blocks freed by other threads. Never accessed by two threads on its
// allocation path; only `push_remote()` and `owns()` are called cross-thread,
// and both are synchronization-safe.
class ThreadCache {
public:
    ThreadCache(Arena&& arena, const Allocator::Config& cfg, int id) noexcept
        : inner_(std::move(arena), cfg), id_(id) {}

    ThreadCache(const ThreadCache&) = delete;
    ThreadCache& operator=(const ThreadCache&) = delete;

    // Owner-thread fast path: reclaim anything other threads freed, then serve
    // the request from the private engine. No locks, no shared writes.
    void* allocate(std::size_t bytes) noexcept {
        drain_remote();
        return inner_.allocate(bytes);
    }

    // Owner-thread free of a pointer this cache owns.
    void deallocate_local(void* p) noexcept { inner_.deallocate(p); }

    // True if `p` was handed out by this cache (its arena covers the address).
    // Safe to call from any thread: arena bounds are fixed at construction.
    bool owns(const void* p) const noexcept { return inner_.owns(p); }

    // Cross-thread free: push `p` onto this cache's remote-free stack. Lock-free
    // (MPSC); the intrusive link is stored in the freed block's own memory, so
    // every block (slab slot >= 8B, TLSF payload >= 16B) has room for it.
    void push_remote(void* p) noexcept {
        void* head = remote_head_.load(std::memory_order_relaxed);
        do {
            *reinterpret_cast<void**>(p) = head;
        } while (!remote_head_.compare_exchange_weak(
            head, p, std::memory_order_release, std::memory_order_relaxed));
        remote_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Owner-thread drain: detach the whole remote list in one atomic exchange
    // and free each block locally.
    void drain_remote() noexcept {
        void* node = remote_head_.exchange(nullptr, std::memory_order_acquire);
        while (node != nullptr) {
            void* next = *reinterpret_cast<void**>(node);
            inner_.deallocate(node);
            node = next;
        }
    }

    int id() const noexcept { return id_; }
    const Allocator& engine() const noexcept { return inner_; }

    // Registry link (singly linked list of all caches; CAS-pushed at creation).
    ThreadCache* next_registry = nullptr;

private:
    Allocator inner_;
    std::atomic<void*> remote_head_{nullptr};
    std::atomic<std::uint64_t> remote_count_{0};
    int id_;
};

class ConcurrentAllocator {
public:
    struct Config {
        // Total OS reservation for the whole process (lazily committed).
        std::size_t global_arena_bytes = 1u << 30;   // 1 GiB
        // Per-thread superblock carved from the global arena.
        std::size_t superblock_bytes = 32u * 1024 * 1024;  // 32 MiB
        // Engine configuration applied to each thread's private allocator.
        Allocator::Config thread_cfg{};
    };

    ConcurrentAllocator();
    explicit ConcurrentAllocator(const Config& cfg);
    ~ConcurrentAllocator();

    ConcurrentAllocator(const ConcurrentAllocator&) = delete;
    ConcurrentAllocator& operator=(const ConcurrentAllocator&) = delete;

    // Lock-free fast path on the owning thread.
    void* allocate(std::size_t bytes) noexcept {
        ThreadCache* c = local();
        return c != nullptr ? c->allocate(bytes) : nullptr;
    }

    // Routes to the owning thread cache: a local free is immediate; a remote
    // free is pushed to the owner's lock-free inbox.
    void deallocate(void* p) noexcept {
        if (p == nullptr) {
            return;
        }
        ThreadCache* c = local();
        if (c != nullptr && c->owns(p)) {
            c->deallocate_local(p);
            return;
        }
        if (ThreadCache* owner = find_owner(p)) {
            owner->push_remote(p);
        }
        // A pointer owned by no cache is not ours; ignore (matches free(garbage)
        // being UB -- we simply never corrupt state).
    }

    // Typed helpers mirroring the single-thread facade.
    template <typename T, typename... Args>
    T* create(Args&&... args) {
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

    // Number of thread caches created so far (diagnostics / tests).
    std::size_t thread_count() const noexcept {
        return thread_count_.load(std::memory_order_acquire);
    }

private:
    ThreadCache* local() noexcept;
    ThreadCache* create_cache() noexcept;
    ThreadCache* find_owner(const void* p) const noexcept;
    void* carve_superblock() noexcept;

    Arena global_;                                  // owns the mmap region
    std::atomic<std::size_t> cursor_{0};            // CAS bump over global_
    std::atomic<ThreadCache*> registry_{nullptr};   // all caches (lock-free)
    std::atomic<int> next_id_{0};
    std::atomic<std::size_t> thread_count_{0};
    Config cfg_;
};

}  // namespace memalloc
