#pragma once
#include "review/Classification.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
namespace chess_review::review {
inline constexpr std::uint32_t stabilityModelVersion = 1;
inline constexpr std::uint32_t accuracyModelVersion = 1;
enum class EvidenceStage { Quick = 0, Deepening = 1, Verification = 2 };
enum class StabilityReason {
    Unselected,
    Pending,
    Insufficient,
    Disagreement,
    ScoreDrift,
    Confirmed
};
struct StabilityAssessment {
    StabilityReason reason = StabilityReason::Unselected;
    bool stable = false;
    bool persistentMate = false;
    std::uint32_t modelVersion = stabilityModelVersion;
};
[[nodiscard]] bool needsRefinement(const ClassificationResult& result, const MoveEvidence& pair,
                                   const RefinementConfig& config);
[[nodiscard]] StabilityAssessment assessStability(const ClassificationResult& deep,
                                                  const ClassificationResult& verified,
                                                  const MoveEvidence& deepPair,
                                                  const MoveEvidence& verifiedPair,
                                                  const RefinementConfig& config);
[[nodiscard]] std::optional<double> accuracyLoss(const MoveEvidence& validatedPair);
// Independent of classification thresholds. Missing/invalid loss is excluded, never zero-filled.
[[nodiscard]] std::optional<double> moveAccuracy(std::optional<double> loss);
[[nodiscard]] std::optional<double> gameAccuracy(std::span<const double> moves);
struct SideSummary {
    int total = 0;
    int evaluated = 0;
    int stable = 0;
    int withheld = 0;
    int unprocessed = 0;
    std::array<int, 6> counts{};
    std::optional<double> accuracy;
};
struct SummaryMoveFacts {
    int positionIndex = 0;
    chess::Color mover = chess::Color::White;
    bool processed = false;
    bool stable = false;
    std::optional<Classification> label;
    std::optional<double> loss;
    RawEvaluation before;
    RawEvaluation after;
};
struct KeyMomentSelection {
    bool selected = false;
    std::optional<int> opponentContext;
};
struct SummaryFacts {
    std::array<SideSummary, 2> sides;
    int keyMoments = 0;
    std::optional<double> largestLoss;
    std::optional<int> largestLossPosition;
};
// Stream original-mainline facts in chronological order. No engine, UI or storage dependencies.
class SummaryBuilder {
  public:
    explicit SummaryBuilder(chess::Color selectedSide) : selectedSide_(selectedSide) {}
    [[nodiscard]] KeyMomentSelection add(const SummaryMoveFacts& move);
    [[nodiscard]] SummaryFacts finish() const;

  private:
    chess::Color selectedSide_;
    SummaryFacts facts_;
    std::array<std::vector<double>, 2> accuracies_;
    bool previousOpponentError_ = false;
};
} // namespace chess_review::review
