#pragma once
#include "review/Classification.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>
namespace chess_review::review {
inline constexpr std::uint32_t stabilityModelVersion = 1;
inline constexpr std::uint32_t accuracyModelVersion = 1;
inline constexpr std::uint32_t pipelineModelVersion = 4;
[[nodiscard]] constexpr bool supportsPipelineModel(std::uint32_t version) {
    // Keep released policies explicit when the default advances.
    return version == 1 || version == 2 || version == 3 || version == 4;
}
enum class ScanState { AwaitingEvidence, Usable, Unusable, Invalid };
enum class RefinementReason {
    MajorLoss,
    ThresholdProximity,
    MateSignal,
    SearchConflict,
    InsufficientStrength,
    MissingEvaluation,
    BoundedEvaluation,
    MixedConversion,
    ExpectedScoreLoss,
    OpportunityReply
};
struct ScanAssessment {
    std::optional<std::uint32_t> pipelineVersion;
    ScanState state = ScanState::AwaitingEvidence;
    std::optional<EvidenceIssue> issue;
    std::optional<double> signedDelta;
    std::optional<double> loss;
    std::vector<RefinementReason> reasons;
    // Set only by adaptive policies from usable original quick evidence.
    std::optional<double> qualifyingLoss;
    std::optional<double> opportunityLoss;
    std::optional<int> opportunityPosition;
    [[nodiscard]] bool needsRefinement() const { return !reasons.empty(); }
    bool operator==(const ScanAssessment&) const = default;
};
struct ScanCoverage {
    int awaiting = 0;
    int usable = 0;
    int unusable = 0;
    int invalid = 0;
    bool operator==(const ScanCoverage&) const = default;
};
// Inputs must come from validated owned quick evidence and its original-board classifier.
// An absent pair is represented by the default (AwaitingEvidence) assessment.
[[nodiscard]] ScanAssessment assessQuickScan(const ClassificationResult& result,
                                             const MoveEvidence& pair,
                                             const RefinementConfig& config,
                                             std::uint32_t pipelineVersion = pipelineModelVersion);
// Apply only to adjacent original-mainline assessments, never refined presentation values.
void addOpportunityContext(ScanAssessment& current, const ScanAssessment& previous,
                           int previousPosition);
struct RefinementCandidate {
    int positionIndex;
    ScanAssessment scan;
};
enum class RefinementQueueIssue { UnsupportedPolicy, InvalidCandidate };
using RefinementQueueResult = std::variant<std::vector<int>, RefinementQueueIssue>;
// Assessments must come from validated quick evidence and adjacent opportunity context.
// Invalid inputs are errors, never an empty successful plan or a fallback policy.
// Policies 1/2 retain chronological position order; adaptive policies rank then expand pairs.
[[nodiscard]] RefinementQueueResult refinementQueue(std::span<const RefinementCandidate> candidates,
                                                    std::uint32_t pipelineVersion);
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
// Pairs/classifications must come from validated original-board witnesses. Incompatible
// derived values fail closed; this is not a replacement for raw evidence validation.
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
