#pragma once
#include <cstdint>
namespace chess_review::review {
// Stable format-1 wire values. Complete describes finished processing, not chess validity.
enum class JobStatus {
    Queued = 0,
    QuickScanning = 1,
    Reviewable = 2,
    Deepening = 3,
    Deriving = 4,
    Complete = 5,
    Partial = 6,
    Cancelled = 7,
    Interrupted = 8,
    Failed = 9
};
enum class JobFailure { None = 0, Engine = 1, Storage = 2, Evidence = 3, Configuration = 4 };
using ReviewId = std::int64_t;
using ReviewJobId = std::int64_t;
} // namespace chess_review::review
