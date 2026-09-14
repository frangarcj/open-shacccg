#include "core/internal.hpp"
#include <cstdlib>
#include <cstring>

namespace vsc {

void *alloc(const VscAllocator &a, size_t size) {
    if (a.alloc) return a.alloc(size, a.userdata);
    return std::malloc(size);
}

void dealloc(const VscAllocator &a, void *ptr) {
    if (!ptr) return;
    if (a.free) a.free(ptr, a.userdata);
    else std::free(ptr);
}

char *dup_string(const VscAllocator &a, const std::string &s) {
    char *p = static_cast<char *>(alloc(a, s.size() + 1));
    if (!p) return nullptr;
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

} // namespace vsc
