#pragma once
#include "review/Configuration.h"
#include <cstddef>
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
    Blunder = 5,
    Brilliant = 6,
    Great = 7,
    Miss = 8,
    Book = 9
};
inline constexpr std::size_t classificationCount = 10;
[[nodiscard]] constexpr bool isError(Classification label) {
    return label == Classification::Inaccuracy || label == Classification::Mistake ||
           label == Classification::Blunder || label == Classification::Miss;
}
[[nodiscard]] constexpr bool isKeyLabel(Classification label) {
    return label == Classification::Mistake || label == Classification::Blunder ||
           label == Classification::Miss || label == Classification::Great ||
           label == Classification::Brilliant;
}
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
// Apply a verified mainline book fact to an otherwise usable classification.
void applyBookClassification(ClassificationResult& result);
} // namespace chess_review::review
