#pragma once

#include "review/Retry.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace chess_review::puzzles {
inline constexpr std::uint32_t modelVersion = 2;
[[nodiscard]] constexpr bool supportsModelVersion(std::uint32_t version) {
    return version == 1 || version == 2;
}
enum class Origin { Generated, Manual };
enum class Goal { Practice, Mate, Material, Defence, Tactic };
enum class Outcome { Pending, Accepted, Rejected, Insufficient };
struct Policy {
    review::Configuration review;
    std::uint64_t baselineNodes = 5000000;
    std::uint64_t rootMinimumNodes = 0;
    int maximumPlayerMoves = 4;
    int maximumPositions = 64;
    int maximumBudgetUnits = 64;
    int minimumMaterialGain = 2;
    int immediateMaterialGain = 5;
    bool operator==(const Policy&) const = default;
};
struct Definition {
    chess::PositionContext position;
    Origin origin = Origin::Manual;
    Policy policy;
    std::uint32_t modelVersion = puzzles::modelVersion;
    bool operator==(const Definition&) const = default;
};
struct Edge {
    chess::Move move;
    std::size_t next = 0;
    bool operator==(const Edge&) const = default;
};
struct SolutionNode {
    std::vector<chess::Move> path;
    std::vector<Edge> moves;
    bool complete = false;
    review::RetryIdea idea = review::RetryIdea::General;
    bool operator==(const SolutionNode&) const = default;
};
struct Solution {
    Goal goal = Goal::Practice;
    int materialTarget = 0;
    std::vector<SolutionNode> nodes;
    bool operator==(const Solution&) const = default;
};
struct SearchNeed {
    chess::Position position;
    std::uint64_t nodes;
    int width;
};
struct Verification {
    Outcome outcome = Outcome::Pending;
    std::optional<SearchNeed> next;
    Solution solution;
};
struct ContinuationGoal {
    Goal goal;
    chess::Color solver;
    int startingMaterial;
    int materialTarget;
    int remainingPlayerMoves = 4;
};
enum class AlternativeGrade { Strong, TryAgain, Unavailable };
[[nodiscard]] AlternativeGrade gradeKnownMove(const chess::Position& root, chess::Move move,
                                              const Policy& policy,
                                              const review::PositionEvidence& older,
                                              const review::PositionEvidence& newer);
// Deterministic replay of completed evidence. No engine, Qt, storage, or hidden state.
// A Pending result requests the next bounded search. Persisting the input passes
// is sufficient to resume, including escalation and all completed branches.
[[nodiscard]] Verification verify(const chess::Position& root, const Definition& definition,
                                  std::span<const review::PositionEvidence> passes);
[[nodiscard]] Verification verifyContinuation(const chess::Position& root,
                                              const Definition& definition,
                                              std::span<const review::PositionEvidence> passes,
                                              ContinuationGoal goal);
// Boundary adapters must establish effective engine compatibility before normalizing
// width-dependent identities to one base identity. Raw evidence remains unchanged.
[[nodiscard]] AlternativeGrade gradeAlternative(const chess::Position& root, chess::Move move,
                                                const Policy& policy,
                                                const review::PositionEvidence& olderRoot,
                                                const review::PositionEvidence& newerRoot,
                                                const review::PositionEvidence& olderChild,
                                                const review::PositionEvidence& newerChild);
[[nodiscard]] bool validPolicy(const Policy& policy);
struct CandidateContext {
    bool isError = false;
    bool isSpecial = false;
    bool opponentBlundered = false;
};
// Cheap teaching eligibility from qualified review evidence. A large loss
// alone is not a candidate; independent deeper verification is still required.
[[nodiscard]] std::optional<Goal> candidateGoal(const chess::Position& root,
                                                const review::MoveEvidence* older,
                                                const review::MoveEvidence& newer,
                                                const Policy& policy,
                                                CandidateContext context = {});
[[nodiscard]] std::optional<Goal> candidateGoal(const chess::Position& root,
                                                const review::MoveEvidence& older,
                                                const review::MoveEvidence& newer,
                                                const Policy& policy);
// Root and leaf material balance, in pawn units, from the solver's perspective.
[[nodiscard]] int material(const chess::Position& position, chess::Color side);
} // namespace chess_review::puzzles
