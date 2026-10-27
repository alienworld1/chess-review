#pragma once
#include "review/Configuration.h"
#include <optional>
#include <string>
#include <vector>
namespace chess_review::review {
inline constexpr std::uint32_t classificationModelVersion = 1;
enum class Classification { Best, Excellent, Good, Inaccuracy, Mistake, Blunder };
enum class ClassificationIssue { Evidence, InvalidConfiguration, BestMoveConflict };
struct AlternativeAssessment {
    chess::Move move;
    int rank = 0;
    double gap = 0;
    bool nearlyEquivalent = false;
};
struct ClassificationResult {
    std::optional<Classification> label;
    std::string ruleId;
    std::optional<ClassificationIssue> issue;
    std::optional<EvidenceIssue> evidenceIssue;
    ClassificationConfig thresholds;
    chess::Color mover = chess::Color::White;
    EvaluationResult before;
    EvaluationResult after;
    std::optional<double> signedDelta;
    std::optional<double> loss;
    std::optional<chess::Move> bestMove;
    std::optional<int> playedMoveRank;
    std::vector<AlternativeAssessment> alternatives;
    std::uint32_t modelVersion = classificationModelVersion;
    bool stable = false;
};
// Revalidates the pair from its original board witness; supplied derived fields are not trusted.
[[nodiscard]] ClassificationResult classify(const chess::Position& position,
                                            const MoveEvidence& evidence,
                                            const ClassificationConfig& config,
                                            const ConversionConfig& conversion = {});
} // namespace chess_review::review
