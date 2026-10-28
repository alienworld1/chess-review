#pragma once
#include "review/Configuration.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace chess_review::review {
inline constexpr std::uint32_t classificationModelVersion = 2;
inline constexpr bool supportsClassificationModel(std::uint32_t version) {
    return version == 1 || version == 2;
}
// Explicit durable document tags; model 1 remains readable.
enum class Classification {
    Best = 0,
    Excellent = 1,
    Good = 2,
    Inaccuracy = 3,
    Mistake = 4,
    Blunder = 5
};
enum class ClassificationIssue { Evidence = 0, InvalidConfiguration = 1, BestMoveConflict = 2 };
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
[[nodiscard]] ClassificationResult
classify(const chess::Position& position, const MoveEvidence& evidence,
         const ClassificationConfig& config, const ConversionConfig& conversion = {},
         std::uint32_t modelVersion = classificationModelVersion);
} // namespace chess_review::review
