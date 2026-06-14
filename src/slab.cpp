#include "memalloc/slab.hpp"

#include <algorithm>

#include "memalloc/align.hpp"

namespace memalloc {

namespace {

// A slot must be able to hold the intrusive free-list pointer while free.
constexpr std::size_t kMinSlotBytes = sizeof(void*);

}  // namespace

SlabPool::SlabPool(Arena& arena, std::size_t slot_size,
                   std::size_t slot_alignment,
                   std::size_t slots_per_block) noexcept
    : arena_(arena) {
    // Alignment must be a power of two and large enough for a pointer so the
    // intrusive FreeNode stored in a free slot is itself aligned.
    std::size_t align = std::max(slot_alignment, alignof(void*));
    if (!is_power_of_two(align)) {
        align = alignof(std::max_align_t);
    }
    slot_alignment_ = align;

    // Round the slot up to hold a pointer and to a multiple of the alignment,
    // so consecutive slots in a block stay aligned.
    slot_size_ = align_up(std::max(slot_size, kMinSlotBytes), slot_alignment_);

    slots_per_block_ = slots_per_block == 0 ? 1 : slots_per_block;
}

bool SlabPool::grow() noexcept {
    const std::size_t block_bytes = slot_size_ * slots_per_block_;
    void* block = arena_.allocate(block_bytes, slot_alignment_);
    if (block == nullptr) {
        return false;
    }

    // Thread every slot in the new block onto the free list. Walking forward
    // and pushing each slot leaves the list head at the last slot; order does
    // not matter for a homogeneous pool.
    char* cursor = static_cast<char*>(block);
    for (std::size_t i = 0; i < slots_per_block_; ++i) {
        auto* node = reinterpret_cast<FreeNode*>(cursor);
        node->next = free_head_;
        free_head_ = node;
        cursor += slot_size_;
    }

    capacity_ += slots_per_block_;
    free_count_ += slots_per_block_;
    ++block_count_;
    return true;
}

void* SlabPool::allocate() noexcept {
    if (free_head_ == nullptr && !grow()) {
        return nullptr;  // Arena exhausted.
    }

    FreeNode* node = free_head_;
    free_head_ = node->next;
    --free_count_;
    return node;
}

void SlabPool::deallocate(void* p) noexcept {
    if (p == nullptr) {
        return;
    }
    auto* node = static_cast<FreeNode*>(p);
    node->next = free_head_;
    free_head_ = node;
    ++free_count_;
}

}  // namespace memalloc
