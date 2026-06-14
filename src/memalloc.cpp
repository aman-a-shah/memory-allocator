#include "memalloc/memalloc.hpp"

namespace memalloc {

Version version() noexcept {
    return Version{0, 1, 0};
}

const char* version_string() noexcept {
    return "0.1.0";
}

}  // namespace memalloc
