#pragma once

#include <cstddef>

namespace chess_review::test_support {
// Only link the implementation into the isolated allocation-test executable.
class FailAfter {
  public:
    explicit FailAfter(std::size_t count);
    ~FailAfter();
    FailAfter(const FailAfter&) = delete;
    FailAfter& operator=(const FailAfter&) = delete;
};
} // namespace chess_review::test_support
