#include "memalloc/allocator.hpp"

#include <algorithm>

namespace memalloc {

namespace {
// Default fixed-size classes for the slab path. Sizes above the largest class
// fall through to TLSF.
constexpr std::size_t kDefaultClasses[] = {16, 32, 64, 128, 256};
}  // namespace

Allocator::Allocator() noexcept : Allocator(Config{}) {}

Allocator::Allocator(const Config& cfg) noexcept
    : arena_(cfg.arena_bytes), tlsf_(arena_, cfg.tlsf_region_bytes) {
    init_pools(cfg);
}

Allocator::Allocator(Arena&& arena, const Config& cfg) noexcept
    : arena_(std::move(arena)), tlsf_(arena_, cfg.tlsf_region_bytes) {
    init_pools(cfg);
}

void Allocator::init_pools(const Config& cfg) noexcept {
    const std::size_t slot_align = alignof(std::max_align_t);
    for (std::size_t cls : kDefaultClasses) {
        // Aim for ~slab_block_target bytes per block, with a sane floor.
        std::size_t slots = cfg.slab_block_target / cls;
        if (slots < 32) {
            slots = 32;
        }
        classes_.push_back(cls);
        pools_.push_back(std::unique_ptr<SlabPool>(
            new SlabPool(arena_, cls, slot_align, slots, this)));
    }
}

SlabPool* Allocator::pool_for_size(std::size_t bytes) const noexcept {
    if (bytes == 0 || classes_.empty() || bytes > classes_.back()) {
        return nullptr;
    }
    // Smallest class that fits the request.
    auto it = std::lower_bound(classes_.begin(), classes_.end(), bytes);
    const std::size_t idx = static_cast<std::size_t>(it - classes_.begin());
    return pools_[idx].get();
}

SlabPool* Allocator::pool_for_ptr(const void* p) const noexcept {
    const auto addr = reinterpret_cast<std::uintptr_t>(p);
    // ranges_ is sorted by lo; find the last range with lo <= addr.
    auto it = std::upper_bound(
        ranges_.begin(), ranges_.end(), addr,
        [](std::uintptr_t a, const Range& r) { return a < r.lo; });
    if (it == ranges_.begin()) {
        return nullptr;  // before every slab block -> TLSF
    }
    --it;
    if (addr >= it->lo && addr < it->hi) {
        return it->pool;
    }
    return nullptr;  // falls between slab blocks -> TLSF
}

void Allocator::on_slab_block(SlabPool* pool, void* base,
                              std::size_t bytes) noexcept {
    const auto lo = reinterpret_cast<std::uintptr_t>(base);
    const Range r{lo, lo + bytes, pool};
    // Keep ranges_ sorted by lo so pool_for_ptr can binary-search.
    auto it = std::upper_bound(
        ranges_.begin(), ranges_.end(), lo,
        [](std::uintptr_t a, const Range& e) { return a < e.lo; });
    ranges_.insert(it, r);
}

void* Allocator::allocate(std::size_t bytes) noexcept {
    if (bytes == 0) {
        return nullptr;
    }

    if (SlabPool* pool = pool_for_size(bytes)) {
        if (void* p = pool->allocate()) {
            ++slab_live_;
            return p;
        }
        // Slab class could not grow; fall through to TLSF.
    }

    if (void* p = tlsf_.allocate(bytes)) {
        ++tlsf_live_;
        return p;
    }
    return nullptr;
}

void Allocator::deallocate(void* p) noexcept {
    if (p == nullptr) {
        return;
    }
    if (SlabPool* pool = pool_for_ptr(p)) {
        pool->deallocate(p);
        --slab_live_;
    } else {
        tlsf_.free(p);
        --tlsf_live_;
    }
}

}  // namespace memalloc
