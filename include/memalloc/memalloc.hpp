#pragma once

// Custom hybrid Slab + TLSF memory allocator.
//
// Phase 0: this header only exposes version/build metadata so the library,
// tests, and benchmarks have something real to link against. The allocator
// API is introduced in later phases (Arena -> Slab -> TLSF -> facade).

namespace memalloc {

// Semantic version, kept in sync with the CMake project() version.
struct Version {
    int major;
    int minor;
    int patch;
};

// Returns the compiled-in library version.
Version version() noexcept;

// Human-readable version string, e.g. "0.1.0".
const char* version_string() noexcept;

}  // namespace memalloc
