#include "memalloc/tlsf.hpp"

#include <algorithm>
#include <cstddef>

#include "memalloc/align.hpp"

namespace memalloc {

static_assert(offsetof(BlockHeader, next_free) == kBlockHeaderOverhead,
              "payload must begin exactly after size+flags+prev_phys");

BinIndex tlsf_mapping_insert(std::uint32_t size) noexcept {
    BinIndex bi;
    if (size < kSmallBlockSize) {
        // Linear first-level region: a single fl, sl spaced every kAlignSize.
        bi.fl = 0;
        bi.sl = static_cast<int>(size / (kSmallBlockSize / kSLIndexCount));
    } else {
        const int fl = tlsf_fls(size);
        bi.sl = static_cast<int>(
            (size >> (fl - kSLIndexCountLog2)) ^ static_cast<std::uint32_t>(kSLIndexCount));
        bi.fl = fl - (kFLIndexShift - 1);
    }
    return bi;
}

BinIndex tlsf_mapping_search(std::uint32_t size) noexcept {
    if (size >= kSmallBlockSize) {
        // Round up so the resulting bin contains only blocks >= size.
        const std::uint32_t round =
            (1u << (tlsf_fls(size) - kSLIndexCountLog2)) - 1;
        size += round;
    }
    return tlsf_mapping_insert(size);
}

FreeListMatrix::FreeListMatrix() noexcept : fl_bitmap_(0) {
    for (int f = 0; f < kFLIndexCount; ++f) {
        sl_bitmap_[f] = 0;
        for (int s = 0; s < kSLIndexCount; ++s) {
            blocks_[f][s] = nullptr;
        }
    }
}

void FreeListMatrix::insert(BlockHeader* block) noexcept {
    const BinIndex bi = tlsf_mapping_insert(block->size);

    BlockHeader* head = blocks_[bi.fl][bi.sl];
    block->prev_free = nullptr;
    block->next_free = head;
    if (head != nullptr) {
        head->prev_free = block;
    }
    blocks_[bi.fl][bi.sl] = block;

    // Mark both bitmap levels as occupied.
    fl_bitmap_ |= (1u << bi.fl);
    sl_bitmap_[bi.fl] |= (1u << bi.sl);
}

void FreeListMatrix::remove(BlockHeader* block) noexcept {
    const BinIndex bi = tlsf_mapping_insert(block->size);

    BlockHeader* prev = block->prev_free;
    BlockHeader* next = block->next_free;
    if (next != nullptr) {
        next->prev_free = prev;
    }
    if (prev != nullptr) {
        prev->next_free = next;
    }

    // If we removed the head, advance it and clear bitmaps if the bin emptied.
    if (blocks_[bi.fl][bi.sl] == block) {
        blocks_[bi.fl][bi.sl] = next;
        if (next == nullptr) {
            sl_bitmap_[bi.fl] &= ~(1u << bi.sl);
            if (sl_bitmap_[bi.fl] == 0) {
                fl_bitmap_ &= ~(1u << bi.fl);
            }
        }
    }
}

BlockHeader* FreeListMatrix::find_suitable(std::uint32_t size,
                                           BinIndex* out_bin) const noexcept {
    // Align the request and clamp to the minimum block size before searching.
    std::uint32_t want = static_cast<std::uint32_t>(align_up(size, kAlignSize));
    if (want < kMinBlockSize) {
        want = kMinBlockSize;
    }

    const BinIndex bi = tlsf_mapping_search(want);
    int fl = bi.fl;
    int sl = bi.sl;

    // Within this first level, mask off bins below sl and take the lowest set.
    std::uint32_t sl_map = sl_bitmap_[fl] & (~0u << sl);
    if (sl_map == 0) {
        // No fit here -- jump to the next non-empty first level.
        const std::uint32_t fl_map = fl_bitmap_ & (~0u << (fl + 1));
        if (fl_map == 0) {
            return nullptr;  // nothing large enough
        }
        fl = tlsf_ffs(fl_map);
        sl_map = sl_bitmap_[fl];
    }
    sl = tlsf_ffs(sl_map);

    if (out_bin != nullptr) {
        out_bin->fl = fl;
        out_bin->sl = sl;
    }
    return blocks_[fl][sl];
}

// ---- Tlsf allocator ------------------------------------------------------

// The smallest region that can hold a main block plus the sentinel.
namespace {
constexpr std::size_t kSentinelBytes = kBlockHeaderOverhead;  // size 0, no payload
constexpr std::size_t kMinRegionBytes =
    2 * kBlockHeaderOverhead + kMinPayload;  // main header + min payload + sentinel
}  // namespace

Tlsf::Tlsf(Arena& arena, std::size_t initial_region_bytes) noexcept
    : arena_(arena),
      region_bytes_(std::max<std::size_t>(initial_region_bytes, kMinRegionBytes)) {
    add_region(region_bytes_);
}

std::uint32_t Tlsf::adjust_request(std::size_t bytes) noexcept {
    std::size_t want = align_up(bytes, kAlignSize);
    if (want < kMinPayload) {
        want = kMinPayload;
    }
    return static_cast<std::uint32_t>(want);
}

bool Tlsf::add_region(std::size_t bytes) noexcept {
    bytes = align_up(std::max(bytes, kMinRegionBytes), kAlignSize);

    void* base = arena_.allocate(bytes, alignof(BlockHeader));
    if (base == nullptr) {
        return false;
    }

    // [ main free block | zero-size allocated sentinel ]
    auto* main = static_cast<BlockHeader*>(base);
    main->size = static_cast<std::uint32_t>(bytes - kBlockHeaderOverhead - kSentinelBytes);
    main->flags = 0;
    block_set_free(main);
    block_set_prev_allocated(main);  // region start: nothing before it to merge
    main->prev_phys = nullptr;
    main->next_free = nullptr;
    main->prev_free = nullptr;

    // The sentinel only needs the 16-byte header (size/flags/prev_phys); it is
    // allocated and never joins a free list, so its next_free/prev_free fields
    // are never touched -- writing them would run past the region.
    BlockHeader* sentinel = block_next_phys(main);
    sentinel->size = 0;
    sentinel->flags = 0;
    block_set_allocated(sentinel);   // never coalesces forward into it
    block_set_prev_free(sentinel);   // its previous block (main) is free
    sentinel->prev_phys = main;

    matrix_.insert(main);
    free_payload_ += main->size;
    regions_.push_back({main, bytes});
    return true;
}

BlockHeader* Tlsf::prepare_used(BlockHeader* block, std::uint32_t adjust) noexcept {
    matrix_.remove(block);

    const std::uint32_t old_size = block->size;
    // Split only if the remainder can stand as its own block.
    if (old_size >= adjust + kBlockHeaderOverhead + kMinPayload) {
        auto* remainder = reinterpret_cast<BlockHeader*>(
            reinterpret_cast<char*>(block) + kBlockHeaderOverhead + adjust);
        remainder->size = old_size - adjust - kBlockHeaderOverhead;
        remainder->flags = 0;
        block_set_free(remainder);
        block_set_prev_allocated(remainder);  // `block` will be allocated
        remainder->prev_phys = block;
        remainder->next_free = nullptr;
        remainder->prev_free = nullptr;

        block->size = adjust;

        // The block that physically follows the remainder now sees `remainder`
        // as its (free) previous neighbor.
        BlockHeader* after = block_next_phys(remainder);
        after->prev_phys = remainder;
        block_set_prev_free(after);

        matrix_.insert(remainder);

        free_payload_ -= old_size;
        free_payload_ += remainder->size;
        used_payload_ += adjust;
    } else {
        free_payload_ -= old_size;
        used_payload_ += old_size;
    }

    block_set_allocated(block);
    block_set_prev_allocated(block_next_phys(block));
    return block;
}

void* Tlsf::allocate(std::size_t bytes) noexcept {
    if (bytes == 0) {
        return nullptr;
    }
    const std::uint32_t adjust = adjust_request(bytes);

    BlockHeader* block = matrix_.find_suitable(adjust);
    if (block == nullptr) {
        const std::size_t grow = std::max<std::size_t>(
            region_bytes_,
            static_cast<std::size_t>(adjust) + 2 * kBlockHeaderOverhead);
        if (!add_region(grow)) {
            return nullptr;
        }
        block = matrix_.find_suitable(adjust);
        if (block == nullptr) {
            return nullptr;
        }
    }

    block = prepare_used(block, adjust);
    return block_payload(block);
}

void Tlsf::free(void* p) noexcept {
    if (p == nullptr) {
        return;
    }
    BlockHeader* block = block_from_payload(p);
    used_payload_ -= block->size;
    block_set_free(block);

    // Backward coalesce: absorb the previous physical block if it is free.
    if (block_prev_is_free(block)) {
        BlockHeader* prev = block->prev_phys;
        matrix_.remove(prev);
        free_payload_ -= prev->size;
        prev->size += kBlockHeaderOverhead + block->size;
        block = prev;  // merged block starts at prev
    }

    // Forward coalesce: absorb the next physical block if it is free. The
    // sentinel is allocated, so this naturally stops at a region boundary.
    BlockHeader* next = block_next_phys(block);
    if (block_is_free(next)) {
        matrix_.remove(next);
        free_payload_ -= next->size;
        block->size += kBlockHeaderOverhead + next->size;
    }

    // Re-publish the merged block and fix up the following block's tags.
    BlockHeader* after = block_next_phys(block);
    after->prev_phys = block;
    block_set_prev_free(after);

    matrix_.insert(block);
    free_payload_ += block->size;
}

bool Tlsf::validate() const noexcept {
    for (const Region& region : regions_) {
        BlockHeader* walk = region.main;
        BlockHeader* prev = nullptr;
        std::size_t total = 0;

        while (walk->size != 0) {  // until the zero-size sentinel
            if (walk->prev_phys != prev) {
                return false;
            }
            const bool flag_prev_free = block_prev_is_free(walk);
            const bool actual_prev_free = prev != nullptr && block_is_free(prev);
            if (flag_prev_free != actual_prev_free) {
                return false;
            }
            if (prev != nullptr && block_is_free(prev) && block_is_free(walk)) {
                return false;  // adjacent free blocks must have been coalesced
            }
            if (walk->size % kAlignSize != 0) {
                return false;
            }
            if (block_is_free(walk) && walk->size < kMinPayload) {
                return false;
            }
            total += kBlockHeaderOverhead + walk->size;
            prev = walk;
            walk = block_next_phys(walk);
        }

        // `walk` is the sentinel.
        if (!block_is_allocated(walk) || walk->prev_phys != prev) {
            return false;
        }
        total += kBlockHeaderOverhead;
        if (total != region.bytes) {
            return false;
        }
    }
    return true;
}

}  // namespace memalloc
