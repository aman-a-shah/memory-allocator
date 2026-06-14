#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "memalloc/arena.hpp"

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
//
// Physical layout of a block in memory:
//
//   +----------------------------------------------------------+
//   | size | flags | prev_phys | <-------- payload --------->  |
//   +----------------------------------------------------------+
//   ^                          ^
//   block                      payload pointer (returned to caller)
//
// `size`, `flags`, and `prev_phys` (16 bytes) are the always-present header
// overhead. The next_free/prev_free links live *inside* the payload region and
// are only valid while the block is free -- when the block is allocated those
// bytes belong to the caller. `prev_phys` is the boundary tag that lets free()
// navigate to the previous physical block for backward coalescing; the next
// physical block is found from `size`.
struct BlockHeader {
    std::uint32_t size;       // payload size in bytes (excludes the 16B header)
    std::uint32_t flags;      // bit 0: IsAllocated, bit 1: IsPreviousAllocated
    BlockHeader* prev_phys;   // previous physical block (nullptr at region start)
    BlockHeader* next_free;   // free-list links (valid only while free; overlap
    BlockHeader* prev_free;   //   the payload region)
};

inline constexpr std::uint32_t kFlagAllocated = 0x1;
inline constexpr std::uint32_t kFlagPrevAllocated = 0x2;

// Always-present header bytes (size + flags + prev_phys). The payload begins
// here, so next_free/prev_free overlap it.
inline constexpr std::uint32_t kBlockHeaderOverhead = 16;
// Smallest payload that can still hold the two free-list links.
inline constexpr std::uint32_t kMinPayload = 16;

inline bool block_is_free(const BlockHeader* b) noexcept {
    return (b->flags & kFlagAllocated) == 0;
}
inline bool block_is_allocated(const BlockHeader* b) noexcept {
    return (b->flags & kFlagAllocated) != 0;
}
inline void block_set_allocated(BlockHeader* b) noexcept {
    b->flags |= kFlagAllocated;
}
inline void block_set_free(BlockHeader* b) noexcept {
    b->flags &= ~kFlagAllocated;
}
inline bool block_prev_is_free(const BlockHeader* b) noexcept {
    return (b->flags & kFlagPrevAllocated) == 0;
}
inline void block_set_prev_allocated(BlockHeader* b) noexcept {
    b->flags |= kFlagPrevAllocated;
}
inline void block_set_prev_free(BlockHeader* b) noexcept {
    b->flags &= ~kFlagPrevAllocated;
}

// Pointer the caller receives / hands back.
inline void* block_payload(BlockHeader* b) noexcept {
    return reinterpret_cast<char*>(b) + kBlockHeaderOverhead;
}
inline BlockHeader* block_from_payload(void* p) noexcept {
    return reinterpret_cast<BlockHeader*>(static_cast<char*>(p) -
                                          kBlockHeaderOverhead);
}
// Next physical block (computed from this block's size).
inline BlockHeader* block_next_phys(BlockHeader* b) noexcept {
    return reinterpret_cast<BlockHeader*>(reinterpret_cast<char*>(b) +
                                          kBlockHeaderOverhead + b->size);
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

// ---- TLSF allocator ------------------------------------------------------
//
// The variable-size engine. It carves one or more contiguous regions from the
// Arena, lays each out as a single large free block bounded by a zero-size
// allocated sentinel, and serves requests with O(1) good-fit lookup + split on
// allocate() and O(1) bidirectional boundary-tag coalescing on free().
class Tlsf {
public:
    // Reserves an initial region of `initial_region_bytes` from the Arena and
    // uses that size as the default growth chunk when it later runs dry.
    explicit Tlsf(Arena& arena,
                  std::size_t initial_region_bytes = 1u << 20) noexcept;

    Tlsf(const Tlsf&) = delete;
    Tlsf& operator=(const Tlsf&) = delete;

    // Allocates at least `bytes`, aligned to kAlignSize. Grows from the Arena
    // if no free block fits. Returns nullptr only if the Arena is exhausted.
    void* allocate(std::size_t bytes) noexcept;

    // Returns a block previously handed out by allocate() (nullptr is a no-op),
    // coalescing with free physical neighbors.
    void free(void* p) noexcept;

    // Total payload bytes currently free / handed out (excludes header overhead).
    std::size_t free_bytes() const noexcept { return free_payload_; }
    std::size_t used_bytes() const noexcept { return used_payload_; }
    std::size_t region_count() const noexcept { return regions_.size(); }

    // Walks every region's physical block list checking all invariants
    // (prev_phys links, prev-allocated flags, no adjacent free blocks, size
    // alignment, and that block extents tile each region exactly). For tests.
    bool validate() const noexcept;

private:
    struct Region {
        BlockHeader* main;
        std::size_t bytes;
    };

    static std::uint32_t adjust_request(std::size_t bytes) noexcept;
    bool add_region(std::size_t bytes) noexcept;
    // Removes `block` from the matrix, splits off any sizable remainder back
    // into the matrix, marks it allocated, and returns it.
    BlockHeader* prepare_used(BlockHeader* block, std::uint32_t adjust) noexcept;

    Arena& arena_;
    FreeListMatrix matrix_;
    std::size_t region_bytes_;       // default growth chunk
    std::size_t free_payload_ = 0;
    std::size_t used_payload_ = 0;
    std::vector<Region> regions_;
};

}  // namespace memalloc
