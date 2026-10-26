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
bool validCandidates(const chess::Position& position, const PositionEvidence& evidence) {
    if (evidence.terminal) {
        return evidence.candidates.empty();
    }
    if (evidence.candidates.empty() || evidence.candidates.size() > 256 ||
        evidence.candidates.front().evaluation != evidence.evaluation) {
        return false;
    }
    std::vector<chess::Move> roots;
    int rank = 0;
    for (const auto& candidate : evidence.candidates) {
        if (candidate.rank != ++rank || candidate.continuation.empty() ||
            candidate.continuation.size() > 1024 ||
            candidate.evaluation.perspective != position.sideToMove() ||
            !candidate.evaluation.score) {
            return false;
        }
        const auto converted = convertEvaluation(candidate.evaluation, position.sideToMove());
        if (converted.issue == EvidenceIssue::Invalid) {
            return false;
        }
        auto replay = position;
        for (const auto& move : candidate.continuation) {
            if (std::holds_alternative<chess::Error>(replay.play(move))) {
                return false;
            }
        }
        const auto root = candidate.continuation.front();
        for (const auto& seen : roots) {
            if (seen == root) {
                return false;
            }
        }
        roots.push_back(root);
    }
    return true;
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
        value = (raw.wdl->wins + raw.wdl->draws / 2.0) / 1000.0;
        source = ConversionSource::NativeWdl;
    } else if (raw.score) {
        const double x =
            std::log(3.0) * static_cast<double>(raw.score->value) / config.fallbackScaleCp;
        const double e = std::exp(-std::abs(x));
        value = x >= 0 ? 1.0 / (1.0 + e) : e / (1.0 + e);
        source = ConversionSource::ApproximateCentipawns;
    } else {
        return fail(EvidenceIssue::MissingEvaluation);
    }
    if (raw.perspective != perspective) {
        value = 1 - value;
    }
    return {ExpectedScore{value, perspective, source, expectedScoreModelVersion, config},
            std::nullopt};
}
PositionEvidence terminalEvidence(const chess::Position& position) {
    PositionEvidence evidence;
    evidence.position = position.context();
    evidence.evaluation.perspective = position.sideToMove();
    if (position.legalMoves().empty()) {
        evidence.complete = true;
        evidence.terminal =
            position.inCheck() ? TerminalOutcome::Checkmate : TerminalOutcome::Stalemate;
    }
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
    const auto beforeTerminal = terminalEvidence(position).terminal;
    const auto afterTerminal = terminalEvidence(next).terminal;
    if (!validConfig(config) || result.before.terminal != beforeTerminal ||
        result.after.terminal != afterTerminal || result.before.evaluation.perspective != mover ||
        result.after.evaluation.perspective != next.sideToMove()) {
        result.issue = EvidenceIssue::Invalid;
        return result;
    }
    result.mate.before = mateScore(result.before.evaluation, mover);
    result.mate.after = mateScore(result.after.evaluation, mover);
    result.beforeEvaluation = positionEvaluation(result.before, mover, config);
    result.afterEvaluation = positionEvaluation(result.after, mover, config);
    if (result.before.modelVersion != evidenceModelVersion ||
        result.after.modelVersion != evidenceModelVersion) {
        result.issue = EvidenceIssue::IncompatibleModel;
    } else if (result.before.engineIdentity.empty() ||
               (!afterTerminal && result.before.engineIdentity != result.after.engineIdentity)) {
        result.issue = EvidenceIssue::IncompatibleEngine;
    } else if (result.beforeEvaluation.issue || result.afterEvaluation.issue) {
        result.issue = result.beforeEvaluation.issue ? result.beforeEvaluation.issue
                                                     : result.afterEvaluation.issue;
    } else {
        const auto& a = *result.beforeEvaluation.expected;
        const auto& b = *result.afterEvaluation.expected;
        if (a.source != b.source && a.source != ConversionSource::Terminal &&
            b.source != ConversionSource::Terminal) {
            result.issue = EvidenceIssue::MixedConversion;
        } else {
            result.signedDelta = a.value - b.value;
            if (*result.signedDelta < 0) {
                result.issue = EvidenceIssue::NegativeLoss;
            } else {
                result.loss = result.signedDelta;
            }
        }
    }
    // Invalid candidate payloads cannot support a loss or a verified rank.
    if ((result.before.complete && !validCandidates(position, result.before)) ||
        (result.after.complete && !validCandidates(next, result.after))) {
        result.loss.reset();
        result.issue = EvidenceIssue::Invalid;
        return result;
    }
    bool exactCandidates = true;
    for (const auto& candidate : result.before.candidates) {
        if (!candidate.evaluation.score || candidate.evaluation.score->bound != Bound::Exact) {
            exactCandidates = false;
        }
    }
    if (result.before.complete && !result.before.restricted && exactCandidates &&
        !result.beforeEvaluation.issue) {
        for (const auto& candidate : result.before.candidates) {
            if (candidate.continuation.front() == playedMove) {
                result.playedMoveRank = candidate.rank;
            }
        }
    }
    return result;
}
} // namespace chess_review::review
