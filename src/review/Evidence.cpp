#include "review/Evidence.h"
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
std::optional<EvidenceIssue> candidateIssue(const chess::Position& position,
                                            const PositionEvidence& evidence,
                                            const ConversionConfig& config) {
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
    std::optional<std::int64_t> previousCp;
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
        const auto converted =
            convertEvaluation(candidate.evaluation, position.sideToMove(), config);
        if (converted.issue) {
            return converted.issue;
        }
        if (previous && previous->source != converted.expected->source) {
            return EvidenceIssue::MixedConversion;
        }
        if ((previous && previous->value < converted.expected->value) ||
            (previousCp && *previousCp < candidate.evaluation.score->value)) {
            return EvidenceIssue::ContradictoryCandidates;
        }
        previous = converted.expected;
        previousCp = candidate.evaluation.score->value;
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
    }
    return std::nullopt;
}
EvaluationResult positionEvaluation(const PositionEvidence& evidence, chess::Color perspective,
                                    const ConversionConfig& config) {
    if (evidence.terminal) {
        const double value = *evidence.terminal == TerminalOutcome::Stalemate
                                 ? 0.5
                                 : (evidence.position.board.sideToMove == perspective ? 0.0 : 1.0);
        return {ExpectedScore{value, perspective, ConversionSource::Terminal,
                              expectedScoreModelVersion, config},
                std::nullopt};
    }
    if (!evidence.complete) {
        return {std::nullopt, EvidenceIssue::Incomplete};
    }
    if (evidence.restricted) {
        return {std::nullopt, EvidenceIssue::RestrictedSearch};
    }
    return convertEvaluation(evidence.evaluation, perspective, config);
}
} // namespace
EvaluationResult convertEvaluation(const RawEvaluation& raw, chess::Color perspective,
                                   const ConversionConfig& config) {
    const auto fail = [](EvidenceIssue issue) { return EvaluationResult{std::nullopt, issue}; };
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
            return fail(EvidenceIssue::MateClaim);
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
    return {ExpectedScore{value, perspective, source, expectedScoreModelVersion, config},
            std::nullopt};
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
                          const ConversionConfig& config) {
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
    const auto beforeIssue = candidateIssue(position, result.before, config);
    const auto afterIssue = candidateIssue(next, result.after, config);
    if (beforeIssue || afterIssue) {
        result.beforeEvaluation.issue = beforeIssue;
        result.afterEvaluation.issue = afterIssue;
        result.issue = beforeIssue ? beforeIssue : afterIssue;
        return result;
    }
    result.beforeEvaluation = positionEvaluation(result.before, mover, config);
    result.afterEvaluation = positionEvaluation(result.after, mover, config);
    if (result.beforeEvaluation.issue || result.afterEvaluation.issue) {
        result.issue = result.beforeEvaluation.issue ? result.beforeEvaluation.issue
                                                     : result.afterEvaluation.issue;
        return result;
    }
    const auto& beforeExpected = *result.beforeEvaluation.expected;
    const auto& afterExpected = *result.afterEvaluation.expected;
    if (beforeExpected.source != afterExpected.source &&
        beforeExpected.source != ConversionSource::Terminal &&
        afterExpected.source != ConversionSource::Terminal) {
        result.issue = EvidenceIssue::MixedConversion;
        return result;
    }
    result.signedDelta = beforeExpected.value - afterExpected.value;
    if (*result.signedDelta < 0) {
        result.issue = EvidenceIssue::NegativeLoss;
        return result;
    }
    result.loss = result.signedDelta;
    for (const auto& candidate : result.before.candidates) {
        if (candidate.continuation.front() == playedMove) {
            result.playedMoveRank = candidate.rank;
        }
    }
    return result;
}
} // namespace chess_review::review
