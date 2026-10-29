#pragma once
#include "review/Summary.h"
namespace chess_review::review {
inline constexpr std::uint32_t retryModelVersion = 1;
inline constexpr std::uint32_t explanationModelVersion = 1;
enum class RetryGrade { Best, Strong, Okay, TryAgain };
struct RetryResult {
    std::optional<RetryGrade> grade;
    std::optional<double> gap;
    bool success() const { return grade == RetryGrade::Best || grade == RetryGrade::Strong; }
};
// Both original pairs are revalidated against the original history-bearing witness.
// Absent child evidence permits grading only candidates demonstrated in both passes.
[[nodiscard]] RetryResult gradeRetry(const chess::Position& root, chess::Move attempted,
                                     const MoveEvidence& deep, const MoveEvidence& verified,
                                     const Configuration& configuration,
                                     const PositionEvidence* deepChild = nullptr,
                                     const PositionEvidence* verifiedChild = nullptr);
} // namespace chess_review::review
