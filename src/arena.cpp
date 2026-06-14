#include "memalloc/arena.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <new>
#include <utility>

#include "memalloc/align.hpp"

namespace memalloc {

namespace {

std::size_t page_size() noexcept {
    const long ps = ::sysconf(_SC_PAGESIZE);
    return ps > 0 ? static_cast<std::size_t>(ps) : 4096;
}

}  // namespace

Arena::Arena(std::size_t capacity_bytes) {
    const std::size_t ps = page_size();
    // Round up to a whole number of pages; never reserve zero.
    std::size_t cap = align_up(capacity_bytes == 0 ? 1 : capacity_bytes, ps);

    void* p = ::mmap(nullptr, cap, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        throw std::bad_alloc();
    }

    base_ = static_cast<char*>(p);
    capacity_ = cap;
    offset_ = 0;
}

Arena::~Arena() {
    if (base_ != nullptr) {
        ::munmap(base_, capacity_);
    }
}

Arena::Arena(Arena&& other) noexcept
    : base_(other.base_), capacity_(other.capacity_), offset_(other.offset_) {
    other.base_ = nullptr;
    other.capacity_ = 0;
    other.offset_ = 0;
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        if (base_ != nullptr) {
            ::munmap(base_, capacity_);
        }
        base_ = other.base_;
        capacity_ = other.capacity_;
        offset_ = other.offset_;
        other.base_ = nullptr;
        other.capacity_ = 0;
        other.offset_ = 0;
    }
    return *this;
}

void* Arena::allocate(std::size_t bytes, std::size_t alignment) noexcept {
    if (bytes == 0 || !is_power_of_two(alignment)) {
        return nullptr;
    }

    // Align the current cursor up to the requested boundary.
    const std::size_t aligned_offset = align_up(offset_, alignment);

    // Overflow-safe capacity check: aligned_offset + bytes <= capacity_.
    if (aligned_offset > capacity_ || bytes > capacity_ - aligned_offset) {
        return nullptr;
    }

    void* result = base_ + aligned_offset;
    offset_ = aligned_offset + bytes;
    return result;
}

void Arena::reset() noexcept {
    offset_ = 0;
}

bool Arena::owns(const void* p) const noexcept {
    const char* c = static_cast<const char*>(p);
    return c >= base_ && c < base_ + capacity_;
}

}  // namespace memalloc
