#include "memalloc/concurrent.hpp"

#include "memalloc/align.hpp"

namespace memalloc {

ConcurrentAllocator::ConcurrentAllocator() : ConcurrentAllocator(Config{}) {}

ConcurrentAllocator::ConcurrentAllocator(const Config& cfg)
    : global_(cfg.global_arena_bytes), cfg_(cfg) {
    // Each thread cache must fit at least one superblock; clamp defensively so a
    // misconfiguration cannot hand out a zero-sized region.
    if (cfg_.superblock_bytes == 0) {
        cfg_.superblock_bytes = 1u << 20;
    }
}

ConcurrentAllocator::~ConcurrentAllocator() {
    // Free every cache created during the run. The caches' arenas are borrowed
    // views into global_, so destroying them does not munmap anything; global_'s
    // own destructor releases the single mapping.
    ThreadCache* c = registry_.load(std::memory_order_acquire);
    while (c != nullptr) {
        ThreadCache* next = c->next_registry;
        delete c;
        c = next;
    }
}

void* ConcurrentAllocator::carve_superblock() noexcept {
    const std::size_t want = cfg_.superblock_bytes;
    const std::size_t cap = global_.capacity();
    std::size_t old = cursor_.load(std::memory_order_relaxed);
    std::size_t aligned;
    do {
        // Page-align each superblock so adjacent caches never share a page.
        aligned = align_up(old, 4096);
        if (aligned > cap || want > cap - aligned) {
            return nullptr;  // global arena exhausted
        }
    } while (!cursor_.compare_exchange_weak(old, aligned + want,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed));
    return static_cast<char*>(global_.base()) + aligned;
}

ThreadCache* ConcurrentAllocator::create_cache() noexcept {
    void* sb = carve_superblock();
    if (sb == nullptr) {
        return nullptr;
    }
    Arena borrowed(sb, cfg_.superblock_bytes);
    const int id = next_id_.fetch_add(1, std::memory_order_relaxed);
    auto* cache = new (std::nothrow)
        ThreadCache(std::move(borrowed), cfg_.thread_cfg, id);
    if (cache == nullptr) {
        return nullptr;
    }

    // Publish into the lock-free registry (CAS push at head).
    ThreadCache* head = registry_.load(std::memory_order_relaxed);
    do {
        cache->next_registry = head;
    } while (!registry_.compare_exchange_weak(head, cache,
                                              std::memory_order_release,
                                              std::memory_order_relaxed));
    thread_count_.fetch_add(1, std::memory_order_release);
    return cache;
}

ThreadCache* ConcurrentAllocator::local() noexcept {
    // One cache per (thread, allocator-instance). The function-local thread_local
    // is created lazily on first use by each thread and reused thereafter.
    thread_local ThreadCache* cache = nullptr;
    thread_local ConcurrentAllocator* owner = nullptr;
    if (cache != nullptr && owner == this) {
        return cache;
    }
    cache = create_cache();
    owner = this;
    return cache;
}

ThreadCache* ConcurrentAllocator::find_owner(const void* p) const noexcept {
    // Walk the registry; arena bounds are immutable, so no lock is needed. The
    // list is short (one node per live thread).
    for (ThreadCache* c = registry_.load(std::memory_order_acquire);
         c != nullptr; c = c->next_registry) {
        if (c->owns(p)) {
            return c;
        }
    }
    return nullptr;
}

}  // namespace memalloc
