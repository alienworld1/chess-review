#pragma once
#include "chess/Position.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chess_review::review {
inline constexpr std::uint32_t evidenceModelVersion = 1;
inline constexpr std::uint32_t expectedScoreModelVersion = 1;
enum class ScoreKind { Centipawns, Mate };
enum class Bound { Exact, Lower, Upper };
struct RawScore {
    ScoreKind kind = ScoreKind::Centipawns;
    std::int64_t value = 0;
    Bound bound = Bound::Exact;
    bool operator==(const RawScore&) const = default;
};
struct Wdl {
    int wins = 0;
    int draws = 0;
    int losses = 0;
    bool operator==(const Wdl&) const = default;
};
struct RawEvaluation {
    chess::Color perspective = chess::Color::White;
    std::optional<RawScore> score;
    std::optional<Wdl> wdl;
    bool operator==(const RawEvaluation&) const = default;
};
struct ConversionConfig {
    double fallbackScaleCp = 300;
    bool preferWdl = true;
    bool operator==(const ConversionConfig&) const = default;
};
enum class ConversionSource { NativeWdl, ApproximateCentipawns, Terminal };
struct ExpectedScore {
    double value = 0.5;
    chess::Color perspective = chess::Color::White;
    ConversionSource source = ConversionSource::ApproximateCentipawns;
    std::uint32_t modelVersion = expectedScoreModelVersion;
    ConversionConfig config;
};
enum class EvidenceIssue {
    Invalid,
    Incomplete,
    Bounded,
    MateClaim,
    MissingEvaluation,
    ContextMismatch,
    IncompatibleEngine,
    IncompatibleModel,
    MixedConversion,
    NegativeLoss,
    RestrictedSearch
};
struct EvaluationResult {
    std::optional<ExpectedScore> expected;
    std::optional<EvidenceIssue> issue;
};
struct AnalysisStrength {
    std::optional<int> depth;
    std::optional<std::uint64_t> nodes;
    std::optional<std::uint64_t> timeMs;
};
struct CandidateEvidence {
    int rank = 1;
    RawEvaluation evaluation;
    std::vector<chess::Move> continuation;
    AnalysisStrength strength;
};
enum class TerminalOutcome { Checkmate, Stalemate };
struct PositionEvidence {
    chess::PositionContext position;
    RawEvaluation evaluation;
    std::vector<CandidateEvidence> candidates;
    AnalysisStrength strength;
    std::string engineIdentity;
    std::string sessionIdentity;
    std::string attemptIdentity;
    std::string createdAtUtc;
    bool complete = false;
    bool restricted = false;
    std::optional<TerminalOutcome> terminal;
    std::uint32_t modelVersion = evidenceModelVersion;
};
struct MateInformation {
    std::optional<RawScore> before;
    std::optional<RawScore> after;
    chess::Color perspective = chess::Color::White;
};
struct MoveEvidence {
    PositionEvidence before;
    chess::Move playedMove;
    PositionEvidence after;
    chess::Color mover = chess::Color::White;
    EvaluationResult beforeEvaluation;
    EvaluationResult afterEvaluation;
    std::optional<double> signedDelta;
    std::optional<double> loss;
    std::optional<int> playedMoveRank;
    MateInformation mate;
    std::optional<EvidenceIssue> issue;
};
[[nodiscard]] EvaluationResult convertEvaluation(const RawEvaluation& raw, chess::Color perspective,
                                                 const ConversionConfig& config = {});
// Local proof only: no PGN result, repetition, move-count or material adjudication.
[[nodiscard]] PositionEvidence terminalEvidence(const chess::Position& position);
// The supplied board is a replay witness; complete history equality gates its use.
[[nodiscard]] MoveEvidence pairEvidence(const chess::Position& position, chess::Move playedMove,
                                        PositionEvidence before, PositionEvidence after,
                                        const ConversionConfig& config = {});
} // namespace chess_review::review
