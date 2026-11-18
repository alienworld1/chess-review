#pragma once
#include "review/Comparison.h"
#include "review/Summary.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace chess_review::review {
inline constexpr std::uint32_t specialLabelModelVersion = 3;
inline constexpr std::uint32_t motifModelVersion = 3;
struct IntelligenceModels {
    std::uint32_t special;
    std::uint32_t motif;
};
[[nodiscard]] constexpr std::optional<IntelligenceModels>
intelligenceModelsForPipeline(std::uint32_t pipeline) {
    switch (pipeline) {
    case 10:
        return IntelligenceModels{1, 1};
    case 11:
        return IntelligenceModels{2, 2};
    case 12:
    case 13:
        return IntelligenceModels{3, 3};
    default:
        return std::nullopt;
    }
}
inline constexpr std::size_t tacticalLineLimit = 16;
enum class TacticalKind { Material, Exchange, HangingPiece, Fork, Promotion, Checkmate, Sacrifice };
enum class FactSource { Played, Best };
// A line consequence is a prediction along this legal line, not a proof against all defences.
struct TacticalFact {
    TacticalKind kind = TacticalKind::Material;
    FactSource source = FactSource::Played;
    chess::Color beneficiary = chess::Color::White;
    chess::PieceType actor = chess::PieceType::Pawn;
    chess::Square square;
    std::vector<chess::Square> targets;
    int material = 0;
    std::vector<chess::Move> line;
    int linePlies = 0;
    std::uint32_t modelVersion = motifModelVersion;
    // A positive consequence retained at a quiet endpoint or locally proven mate.
    // Line-only arithmetic and a promotion subsequently captured do not qualify.
    bool retainedOpportunity = false;
};
enum class SpecialStatus { NotCandidate, Pending, Verified, Rejected, Insufficient };
struct SpecialAssessment {
    SpecialStatus status = SpecialStatus::NotCandidate;
    std::optional<Classification> label;
    std::string ruleId;
    std::uint32_t modelVersion = specialLabelModelVersion;
    SpecialLabelConfig thresholds;
    std::optional<double> alternativeGap;
    bool verificationPossible = true;
    std::vector<TacticalFact> witnesses;
    [[nodiscard]] bool needsEvidence() const {
        return verificationPossible &&
               (status == SpecialStatus::Pending || status == SpecialStatus::Insufficient);
    }
};
[[nodiscard]] bool validSpecialConfig(const SpecialLabelConfig& config);
// Bounded legal replay; no engine, Qt or storage. Illegal lines return no facts.
[[nodiscard]] std::vector<TacticalFact>
inspectTacticalLine(const chess::Position& root, std::span<const chess::Move> line,
                    FactSource source, std::uint32_t modelVersion = motifModelVersion);
// Both functions revalidate supplied pairs from the original board before interpreting them.
void addIntelligenceCandidates(ScanAssessment& scan, const chess::Position& root,
                               const MoveEvidence& pair, const Configuration& config);
[[nodiscard]] std::vector<TacticalFact>
tacticalFacts(const chess::Position& root, const MoveEvidence& pair, const Configuration& config,
              std::uint32_t modelVersion = motifModelVersion);
[[nodiscard]] SpecialAssessment
verifySpecial(const chess::Position& root, const MoveEvidence& latest, const MoveEvidence* older,
              const Configuration& config, std::uint32_t modelVersion = specialLabelModelVersion);
} // namespace chess_review::review
