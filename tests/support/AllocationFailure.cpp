#include "AllocationFailure.h"

#include <cstdlib>
#include <new>
#include <optional>

// Isolated test executable: fail each ordinary allocation in turn, including allocations
// inside standard containers and Position. Never install this override in the application.
namespace {
thread_local std::optional<std::size_t> allocationsBeforeFailure;
}

void* operator new(std::size_t size) {
    if (allocationsBeforeFailure) {
        if (*allocationsBeforeFailure == 0) {
            throw std::bad_alloc{};
        }
        --*allocationsBeforeFailure;
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

namespace chess_review::test_support {
FailAfter::FailAfter(std::size_t count) { allocationsBeforeFailure = count; }
FailAfter::~FailAfter() { allocationsBeforeFailure.reset(); }
} // namespace chess_review::test_support
