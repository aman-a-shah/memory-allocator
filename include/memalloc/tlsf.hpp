#pragma once

#include <cstddef>
#include <cstdint>

// Two-Level Segregated Fit (TLSF) core: block header, size->bin mapping, the
// two-level bitmap index, and the segregated free-list matrix.
//
// Phase 3 scope: indexing and free-list bookkeeping ONLY. Blocks are inserted
// and removed from bins, and a good-fit lookup finds the smallest bin that can
// satisfy a request -- all in O(1) using hardware bit-scan instructions. There
// is no splitting or coalescing yet (Phase 4); tests pre-seed fixed blocks to
// validate the indexing.
//
// Mapping model (per plan.md section 2):
//   * First level (fl):  coarse power-of-two size class -> floor(log2(size)).
//   * Second level (sl): linear subdivision of each first-level range into
//                        SL_INDEX_COUNT sub-bins, minimizing internal waste.
// A set bit in the two-level bitmap means "this bin has at least one free
// block", so the smallest fitting non-empty bin is found with two bit-scans.

namespace memalloc {

// ---- Tuning constants ----------------------------------------------------
//
// Minimum allocation alignment (8 bytes). All block sizes are multiples of it.
inline constexpr int kAlignSizeLog2 = 3;
inline constexpr std::uint32_t kAlignSize = 1u << kAlignSizeLog2;  // 8

// Second-level subdivisions per first-level class. The plan suggests 4-8, but
// 32 (log2 = 5) keeps internal fragmentation near the <3% target while still
// fitting each sub-bitmap in a single 32-bit word.
inline constexpr int kSLIndexCountLog2 = 5;
inline constexpr int kSLIndexCount = 1 << kSLIndexCountLog2;  // 32

// Largest first-level class: supports blocks up to 2^32 - 1 bytes (~4 GiB),
// the full range of the uint32_t size field.
inline constexpr int kFLIndexMax = 32;

// Blocks below SMALL_BLOCK_SIZE live entirely in the linear first-level region
// (fl == 0); above it the log2 mapping kicks in.
inline constexpr int kFLIndexShift = kSLIndexCountLog2 + kAlignSizeLog2;   // 8
inline constexpr int kFLIndexCount = kFLIndexMax - kFLIndexShift + 1;      // 25
inline constexpr std::uint32_t kSmallBlockSize = 1u << kFLIndexShift;      // 256

inline constexpr std::uint32_t kMinBlockSize = kAlignSize;

// ---- Hardware bit-scan helpers (with portable fallback) ------------------
//
// tlsf_ffs: index of the least-significant set bit. word must be non-zero.
inline int tlsf_ffs(std::uint32_t word) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctz(word);
#else
    int n = 0;
    while ((word & 1u) == 0u) {
        word >>= 1;
        ++n;
    }
    return n;
#endif
}

// tlsf_fls: index of the most-significant set bit (floor(log2)). word non-zero.
inline int tlsf_fls(std::uint32_t word) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return 31 - __builtin_clz(word);
#else
    int n = 31;
    while ((word & 0x80000000u) == 0u) {
        word <<= 1;
        --n;
    }
    return n;
#endif
}

// ---- Block header (per plan.md section 3) --------------------------------
struct BlockHeader {
    std::uint32_t size;       // size of this block's payload, in bytes
    std::uint32_t flags;      // bit 0: IsAllocated, bit 1: IsPreviousAllocated
    BlockHeader* next_free;   // free-list links (valid only while free)
    BlockHeader* prev_free;
};

inline constexpr std::uint32_t kFlagAllocated = 0x1;
inline constexpr std::uint32_t kFlagPrevAllocated = 0x2;

inline bool block_is_free(const BlockHeader* b) noexcept {
    return (b->flags & kFlagAllocated) == 0;
}
inline void block_set_allocated(BlockHeader* b) noexcept {
    b->flags |= kFlagAllocated;
}
inline void block_set_free(BlockHeader* b) noexcept {
    b->flags &= ~kFlagAllocated;
}

// ---- Size -> bin mapping -------------------------------------------------
struct BinIndex {
    int fl;  // first-level index  [0, kFLIndexCount)
    int sl;  // second-level index [0, kSLIndexCount)
};

// Maps an exact block size to the bin it belongs in.
BinIndex tlsf_mapping_insert(std::uint32_t size) noexcept;

// Maps a *requested* size to a bin whose blocks are all >= size (rounds the
// request up within its first-level class before mapping). Use this for lookup.
BinIndex tlsf_mapping_search(std::uint32_t size) noexcept;

// ---- Segregated free-list matrix + two-level bitmap ----------------------
class FreeListMatrix {
public:
    FreeListMatrix() noexcept;

    // Links a free block into the bin determined by its size and marks the
    // corresponding bitmap bits. The block's size must already be set.
    void insert(BlockHeader* block) noexcept;

    // Unlinks a block from its bin, clearing bitmap bits if the bin empties.
    void remove(BlockHeader* block) noexcept;

    // Finds the head block of the smallest non-empty bin that can satisfy
    // `size` (after alignment rounding). Returns nullptr if none exists. Does
    // NOT remove the block. If `out_bin` is non-null it receives the chosen bin.
    BlockHeader* find_suitable(std::uint32_t size,
                               BinIndex* out_bin = nullptr) const noexcept;

    // Head of a specific bin (nullptr if empty) -- for inspection/tests.
    BlockHeader* bin_head(int fl, int sl) const noexcept {
        return blocks_[fl][sl];
    }
    bool bin_occupied(int fl, int sl) const noexcept {
        return (sl_bitmap_[fl] & (1u << sl)) != 0;
    }
    std::uint32_t fl_bitmap() const noexcept { return fl_bitmap_; }
    std::uint32_t sl_bitmap(int fl) const noexcept { return sl_bitmap_[fl]; }

private:
    std::uint32_t fl_bitmap_;                  // bit fl set => first level fl non-empty
    std::uint32_t sl_bitmap_[kFLIndexCount];   // bit sl set => bin (fl,sl) non-empty
    BlockHeader* blocks_[kFLIndexCount][kSLIndexCount];
};

}  // namespace memalloc
