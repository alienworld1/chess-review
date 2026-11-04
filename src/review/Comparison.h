#pragma once
#include "review/Configuration.h"
#include <optional>
#include <vector>

namespace chess_review::review {
// Derived from owned attempts, never a persisted or caller-supplied chess claim.
enum class ComparisonReason {
    Pending,
    Confirmed,
    Coverage,
    NoAlternative,
    Insufficient,
    MissingCandidate,
    Disagreement,
    ScoreDrift
};
enum class ComparisonSource { RootCandidate, PlayedChild };
struct MoveComparison {
    chess::Move move;
    std::optional<int> rank; // Absent when the played move is outside root candidates.
    bool played = false;
    ComparisonSource source = ComparisonSource::RootCandidate;
    std::optional<ExpectedScore> expected;
    std::optional<double> gap;
    std::optional<bool> equivalent;
    ComparisonReason reason = ComparisonReason::Pending;
};
struct AlternativeComparison {
    std::vector<MoveComparison> moves; // Played first, then distinct latest candidates.
    ComparisonReason reason = ComparisonReason::Pending;
    [[nodiscard]] bool needsEvidence() const {
        return reason != ComparisonReason::Confirmed && reason != ComparisonReason::Coverage &&
               reason != ComparisonReason::NoAlternative;
    }
};
// Revalidates both pairs from the original board. An absent older pair cannot confirm anything.
[[nodiscard]] AlternativeComparison compareAlternatives(const chess::Position& root,
                                                        const MoveEvidence& latest,
                                                        const MoveEvidence* older,
                                                        const Configuration& config);
} // namespace chess_review::review
