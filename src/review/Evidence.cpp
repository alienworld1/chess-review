#include "review/Evidence.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <variant>

namespace chess_review::review {
namespace {
bool validConfig(const ConversionConfig& c) {
    return std::isfinite(c.fallbackScaleCp) && c.fallbackScaleCp >= 1 && c.fallbackScaleCp <= 10000;
}
bool validColor(chess::Color c) { return c == chess::Color::White || c == chess::Color::Black; }
std::optional<RawScore> mateScore(const RawEvaluation& raw, chess::Color perspective) {
    if (!raw.score || raw.score->kind != ScoreKind::Mate) {
        return std::nullopt;
    }
    auto result = *raw.score;
    // Preserve an unrepresentable inverse as raw evidence; invalid conversion is withheld.
    if (raw.perspective != perspective) {
        if (result.value == std::numeric_limits<std::int64_t>::min()) {
            return std::nullopt;
        }
        result.value = -result.value;
        if (result.bound == Bound::Lower) {
            result.bound = Bound::Upper;
        } else if (result.bound == Bound::Upper) {
            result.bound = Bound::Lower;
        }
    }
    return result;
}
std::optional<TerminalOutcome> terminalOutcome(const chess::Position& position) {
    if (!position.legalMoves().empty()) {
        return std::nullopt;
    }
    return position.inCheck() ? TerminalOutcome::Checkmate : TerminalOutcome::Stalemate;
}
// Compare two exact candidate scores from the side-to-move's perspective:
// winning mate (shorter > longer) > centipawns > losing mate (longer > shorter).
bool scoreOrderedNotWorse(const RawScore& prev, const RawScore& curr) {
    auto tier = [](const RawScore& s) -> int {
        if (s.kind == ScoreKind::Mate) {
            return s.value > 0 ? 2 : 0;
        }
        return 1;
    };
    const int prevTier = tier(prev);
    const int currTier = tier(curr);
    if (prevTier != currTier) {
        return prevTier > currTier;
    }
    if (prevTier == 2) {
        return prev.value <= curr.value;
    }
    if (prevTier == 0) {
        return prev.value <= curr.value; // e.g., -5 before -2 (losing slower is better)
    }
    return prev.value >= curr.value;
}
bool compatibleConversion(ConversionSource a, ConversionSource b, std::uint32_t modelVersion) {
    if (a == b || a == ConversionSource::Terminal || b == ConversionSource::Terminal) {
        return true;
    }
    if (modelVersion >= 2 && (a == ConversionSource::Mate || b == ConversionSource::Mate)) {
        return true;
    }
    return false;
}
std::optional<EvidenceIssue> candidateIssue(const chess::Position& position,
                                            const PositionEvidence& evidence,
                                            const ConversionConfig& config,
                                            std::uint32_t modelVersion) {
    if (!supportsExpectedScoreModel(modelVersion)) {
        return EvidenceIssue::IncompatibleModel;
    }
    if (evidence.terminal) {
        return evidence.candidates.empty() ? std::nullopt : std::optional{EvidenceIssue::Invalid};
    }
    if (!evidence.complete) {
        return EvidenceIssue::Incomplete;
    }
    if (evidence.restricted) {
        return EvidenceIssue::RestrictedSearch;
    }
    if (evidence.candidates.empty() || evidence.candidates.size() > 256 ||
        evidence.candidates.front().evaluation != evidence.evaluation ||
        (evidence.strength.depth && *evidence.strength.depth < 0)) {
        return EvidenceIssue::Invalid;
    }
    if (!evidence.strength.depth || *evidence.strength.depth == 0) {
        return EvidenceIssue::Incomplete;
    }
    std::vector<chess::Move> roots;
    int rank = 0;
    std::optional<ExpectedScore> previous;
    std::optional<RawScore> previousScore;
    bool mateClaim = false;
    for (const auto& candidate : evidence.candidates) {
        if (candidate.rank != ++rank || candidate.continuation.empty() ||
            candidate.continuation.size() > 1024 ||
            candidate.evaluation.perspective != position.sideToMove() ||
            !candidate.evaluation.score ||
            (candidate.strength.depth && *candidate.strength.depth < 0)) {
            return EvidenceIssue::Invalid;
        }
        if (candidate.strength.depth != evidence.strength.depth) {
            return EvidenceIssue::Incomplete;
        }
        auto replay = position;
        for (const auto& move : candidate.continuation) {
            if (std::holds_alternative<chess::Error>(replay.play(move))) {
                return EvidenceIssue::Invalid;
            }
        }
        const auto root = candidate.continuation.front();
        for (const auto& seen : roots) {
            if (seen == root) {
                return EvidenceIssue::Invalid;
            }
        }
        roots.push_back(root);
        const auto converted =
            convertEvaluation(candidate.evaluation, position.sideToMove(), config, modelVersion);
        if (modelVersion == 1 && converted.issue == EvidenceIssue::MateClaim) {
            // A numeric conversion gap is not permission to skip legal/provenance
            // validation of this or the remaining candidate continuations.
            mateClaim = true;
            previous.reset();
            previousScore.reset();
            continue;
        }
        if (converted.issue) {
            return converted.issue;
        }
        if (previous &&
            !compatibleConversion(previous->source, converted.expected->source, modelVersion)) {
            return EvidenceIssue::MixedConversion;
        }
        if (previous && previous->value < converted.expected->value) {
            return EvidenceIssue::ContradictoryCandidates;
        }
        if (previousScore && !scoreOrderedNotWorse(*previousScore, *candidate.evaluation.score)) {
            return EvidenceIssue::ContradictoryCandidates;
        }
        previous = converted.expected;
        previousScore = *candidate.evaluation.score;
    }
    return mateClaim ? std::optional(EvidenceIssue::MateClaim) : std::nullopt;
}
EvaluationResult positionEvaluation(const PositionEvidence& evidence, chess::Color perspective,
                                    const ConversionConfig& config, std::uint32_t modelVersion) {
    if (evidence.terminal) {
        const double value = *evidence.terminal == TerminalOutcome::Stalemate
                                 ? 0.5
                                 : (evidence.position.board.sideToMove == perspective ? 0.0 : 1.0);
        return {ExpectedScore{value, perspective, ConversionSource::Terminal, modelVersion, config},
                std::nullopt};
    }
    if (!evidence.complete) {
        return {std::nullopt, EvidenceIssue::Incomplete};
    }
    if (evidence.restricted) {
        return {std::nullopt, EvidenceIssue::RestrictedSearch};
    }
    return convertEvaluation(evidence.evaluation, perspective, config, modelVersion);
}
} // namespace
std::optional<EvidenceIssue> validatePositionEvidence(const chess::Position& position,
                                                      const PositionEvidence& evidence,
                                                      const ConversionConfig& config,
                                                      std::uint32_t modelVersion) {
    if (!supportsExpectedScoreModel(modelVersion)) {
        return EvidenceIssue::IncompatibleModel;
    }
    if (position.context() != evidence.position || evidence.modelVersion != evidenceModelVersion ||
        evidence.engineIdentity.empty() || evidence.attemptIdentity.empty() ||
        evidence.sessionIdentity.empty() ||
        evidence.evaluation.perspective != position.sideToMove() ||
        evidence.terminal != terminalOutcome(position) || !validConfig(config)) {
        return EvidenceIssue::Invalid;
    }
    return candidateIssue(position, evidence, config, modelVersion);
}
EvaluationResult convertEvaluation(const RawEvaluation& raw, chess::Color perspective,
                                   const ConversionConfig& config, std::uint32_t modelVersion) {
    const auto fail = [](EvidenceIssue issue) { return EvaluationResult{std::nullopt, issue}; };
    if (!supportsExpectedScoreModel(modelVersion)) {
        return fail(EvidenceIssue::IncompatibleModel);
    }
    if (!validConfig(config) || !validColor(perspective) || !validColor(raw.perspective)) {
        return fail(EvidenceIssue::Invalid);
    }
    if (raw.wdl && (raw.wdl->wins < 0 || raw.wdl->draws < 0 || raw.wdl->losses < 0 ||
                    raw.wdl->wins > 1000 || raw.wdl->draws > 1000 || raw.wdl->losses > 1000 ||
                    raw.wdl->wins + raw.wdl->draws + raw.wdl->losses != 1000)) {
        return fail(EvidenceIssue::Invalid);
    }
    if (raw.score) {
        if (raw.score->kind != ScoreKind::Centipawns && raw.score->kind != ScoreKind::Mate) {
            return fail(EvidenceIssue::Invalid);
        }
        if (raw.score->bound != Bound::Exact) {
            return fail(raw.score->bound == Bound::Lower || raw.score->bound == Bound::Upper
                            ? EvidenceIssue::Bounded
                            : EvidenceIssue::Invalid);
        }
        if (raw.score->kind == ScoreKind::Mate) {
            if (modelVersion == 1) {
                return fail(EvidenceIssue::MateClaim);
            }
            if (raw.score->value == 0 ||
                (raw.perspective != perspective &&
                 raw.score->value == std::numeric_limits<std::int64_t>::min())) {
                return fail(EvidenceIssue::Invalid);
            }
            const bool winning =
                raw.perspective == perspective ? raw.score->value > 0 : raw.score->value < 0;
            return {ExpectedScore{winning ? 1.0 : 0.0, perspective, ConversionSource::Mate,
                                  modelVersion, config},
                    std::nullopt};
        }
    }
    double value;
    ConversionSource source;
    if (config.preferWdl && raw.wdl) {
        const int wins = raw.perspective == perspective ? raw.wdl->wins : raw.wdl->losses;
        value = (wins + raw.wdl->draws / 2.0) / 1000.0;
        source = ConversionSource::NativeWdl;
    } else if (raw.score) {
        const double cp =
            static_cast<double>(raw.score->value) * (raw.perspective == perspective ? 1.0 : -1.0);
        const double x = std::log(3.0) * cp / config.fallbackScaleCp;
        const double e = std::exp(-std::abs(x));
        value = x >= 0 ? 1.0 / (1.0 + e) : e / (1.0 + e);
        source = ConversionSource::ApproximateCentipawns;
    } else {
        return fail(EvidenceIssue::MissingEvaluation);
    }
    return {ExpectedScore{value, perspective, source, modelVersion, config}, std::nullopt};
}
PositionEvidence terminalEvidence(const chess::Position& position) {
    PositionEvidence evidence;
    evidence.position = position.context();
    evidence.evaluation.perspective = position.sideToMove();
    evidence.terminal = terminalOutcome(position);
    evidence.complete = evidence.terminal.has_value();
    return evidence;
}
MoveEvidence pairEvidence(const chess::Position& position, chess::Move playedMove,
                          PositionEvidence before, PositionEvidence after,
                          const ConversionConfig& config, std::uint32_t modelVersion) {
    const auto mover = position.sideToMove();
    MoveEvidence result{std::move(before),
                        playedMove,
                        std::move(after),
                        mover,
                        {},
                        {},
                        {},
                        {},
                        {},
                        {std::nullopt, std::nullopt, mover},
                        std::nullopt};
    if (!supportsExpectedScoreModel(modelVersion)) {
        result.issue = EvidenceIssue::IncompatibleModel;
        return result;
    }
    auto next = position;
    if (position.context() != result.before.position ||
        std::holds_alternative<chess::Error>(next.play(playedMove)) ||
        next.context() != result.after.position) {
        result.issue = EvidenceIssue::ContextMismatch;
        return result;
    }
    const auto beforeTerminal = terminalOutcome(position);
    const auto afterTerminal = terminalOutcome(next);
    if (!validConfig(config) || result.before.terminal != beforeTerminal ||
        result.after.terminal != afterTerminal || result.before.evaluation.perspective != mover ||
        result.after.evaluation.perspective != next.sideToMove()) {
        result.issue = EvidenceIssue::Invalid;
        return result;
    }
    result.mate.before = mateScore(result.before.evaluation, mover);
    result.mate.after = mateScore(result.after.evaluation, mover);
    if (result.before.modelVersion != evidenceModelVersion ||
        result.after.modelVersion != evidenceModelVersion) {
        result.issue = EvidenceIssue::IncompatibleModel;
        return result;
    }
    if (result.before.engineIdentity.empty() ||
        (!afterTerminal && result.before.engineIdentity != result.after.engineIdentity)) {
        result.issue = EvidenceIssue::IncompatibleEngine;
        return result;
    }
    const auto beforeIssue = candidateIssue(position, result.before, config, modelVersion);
    const auto afterIssue = candidateIssue(next, result.after, config, modelVersion);
    if (beforeIssue || afterIssue) {
        result.beforeEvaluation.issue = beforeIssue;
        result.afterEvaluation.issue = afterIssue;
        result.issue = beforeIssue ? beforeIssue : afterIssue;
        return result;
    }
    result.beforeEvaluation = positionEvaluation(result.before, mover, config, modelVersion);
    result.afterEvaluation = positionEvaluation(result.after, mover, config, modelVersion);
    if (result.beforeEvaluation.issue || result.afterEvaluation.issue) {
        result.issue = result.beforeEvaluation.issue ? result.beforeEvaluation.issue
                                                     : result.afterEvaluation.issue;
        return result;
    }
    const auto& beforeExpected = *result.beforeEvaluation.expected;
    const auto& afterExpected = *result.afterEvaluation.expected;
    if (!compatibleConversion(beforeExpected.source, afterExpected.source, modelVersion)) {
        result.issue = EvidenceIssue::MixedConversion;
        return result;
    }
    result.signedDelta = beforeExpected.value - afterExpected.value;
    if (*result.signedDelta < 0) {
        if (modelVersion == 1) {
            result.issue = EvidenceIssue::NegativeLoss;
            return result;
        }
        result.loss = 0.0;
    } else {
        result.loss = result.signedDelta;
    }
    for (const auto& candidate : result.before.candidates) {
        if (candidate.continuation.front() == playedMove) {
            result.playedMoveRank = candidate.rank;
        }
    }
    return result;
}
} // namespace chess_review::review
