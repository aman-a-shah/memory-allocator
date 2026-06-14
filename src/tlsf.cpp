#include "memalloc/tlsf.hpp"

#include "memalloc/align.hpp"

namespace memalloc {

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

}  // namespace memalloc
