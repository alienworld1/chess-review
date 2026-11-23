#include "review/Classification.h"
#include <algorithm>
#include <cmath>
namespace chess_review::review {
namespace {
// WDL and proven terminal values are exact multiples of 1/2000. Subtract counts
// before conversion to avoid cancellation at decimal classification boundaries.
int expectedTicks(const PositionEvidence& evidence, const ExpectedScore& expected,
                  chess::Color perspective) {
    if (expected.source == ConversionSource::Terminal ||
        expected.source == ConversionSource::Mate) {
        return static_cast<int>(expected.value * 2000);
    }
    const auto& wdl = *evidence.evaluation.wdl;
    return 2 * (evidence.evaluation.perspective == perspective ? wdl.wins : wdl.losses) + wdl.draws;
}
} // namespace

ClassificationResult classify(const chess::Position& position, const MoveEvidence& evidence,
                              const ClassificationConfig& config,
                              const ConversionConfig& conversion, std::uint32_t modelVersion) {
    ClassificationResult out;
    out.modelVersion = modelVersion;
    out.thresholds = config;
    out.mover = position.sideToMove();
    const auto rule = [modelVersion](const char* prefix) {
        return std::string(prefix) +
               (modelVersion == 1 ? ".v1" : (modelVersion == 2 ? ".v2" : ".v3"));
    };
    if (!supportsClassificationModel(modelVersion)) {
        out.issue = ClassificationIssue::Evidence;
        out.evidenceIssue = EvidenceIssue::IncompatibleModel;
        out.ruleId = "normal.unsupported_model";
        return out;
    }
    if (!std::isfinite(config.excellentLoss) || config.excellentLoss <= 0 ||
        config.excellentLoss >= 1 || !std::isfinite(config.inaccuracyLoss) ||
        !std::isfinite(config.mistakeLoss) || !std::isfinite(config.blunderLoss) ||
        config.inaccuracyLoss <= 0 || config.inaccuracyLoss >= config.mistakeLoss ||
        config.mistakeLoss >= config.blunderLoss || config.blunderLoss >= 1) {
        out.issue = ClassificationIssue::InvalidConfiguration;
        out.ruleId = rule("normal.invalid_configuration");
        return out;
    }
    const std::uint32_t expectedModel = modelVersion >= 3 ? 2U : 1U;
    const auto paired = pairEvidence(position, evidence.playedMove, evidence.before, evidence.after,
                                     conversion, expectedModel);
    out.before = paired.beforeEvaluation;
    out.after = paired.afterEvaluation;
    out.signedDelta = paired.signedDelta;
    out.loss = paired.loss;
    if (paired.issue || !paired.loss) {
        out.issue = ClassificationIssue::Evidence;
        out.evidenceIssue = paired.issue.value_or(EvidenceIssue::Incomplete);
        out.ruleId = rule("normal.insufficient_evidence");
        return out;
    }
    if (modelVersion >= 2 &&
        (paired.beforeEvaluation.expected->source == ConversionSource::NativeWdl ||
         paired.beforeEvaluation.expected->source == ConversionSource::Terminal) &&
        (paired.afterEvaluation.expected->source == ConversionSource::NativeWdl ||
         paired.afterEvaluation.expected->source == ConversionSource::Terminal)) {
        const int deltaTicks =
            expectedTicks(paired.before, *paired.beforeEvaluation.expected, paired.mover) -
            expectedTicks(paired.after, *paired.afterEvaluation.expected, paired.mover);
        out.signedDelta = static_cast<double>(deltaTicks) / 2000.0;
        out.loss = modelVersion >= 3 ? std::max(0.0, *out.signedDelta) : *out.signedDelta;
        if (modelVersion < 3) {
            out.signedDelta = out.loss;
        }
    }
    out.playedMoveRank = paired.playedMoveRank;
    out.bestMove = paired.before.candidates.front().continuation.front();
    const auto best = *paired.beforeEvaluation.expected;
    for (const auto& candidate : paired.before.candidates) {
        if (candidate.rank == 1) {
            continue;
        }
        const auto value =
            convertEvaluation(candidate.evaluation, paired.mover, best.config, expectedModel);
        double gap = std::max(0.0, best.value - value.expected->value);
        if (modelVersion >= 2 && best.source == ConversionSource::NativeWdl &&
            value.expected->source == ConversionSource::NativeWdl) {
            const auto& wdl = *candidate.evaluation.wdl;
            const int candidateTicks =
                2 * (candidate.evaluation.perspective == paired.mover ? wdl.wins : wdl.losses) +
                wdl.draws;
            gap = static_cast<double>(expectedTicks(paired.before, best, paired.mover) -
                                      candidateTicks) /
                  2000.0;
        }
        out.alternatives.push_back(
            {candidate.continuation.front(), candidate.rank, gap, gap <= config.excellentLoss});
    }
    if (modelVersion >= 3 && position.legalMoves().size() == 1) {
        out.loss = 0.0;
        out.label = Classification::Best;
        out.ruleId = "forced.best.v3";
        return out;
    }
    if (modelVersion >= 3 && paired.after.terminal == TerminalOutcome::Checkmate) {
        out.loss = 0.0;
        out.label = Classification::Best;
        out.ruleId = "checkmate.best.v3";
        return out;
    }
    if (modelVersion >= 3 && paired.playedMoveRank == 1) {
        out.loss = 0.0;
        out.label = Classification::Best;
        out.ruleId = "verified_candidate.best.v3";
        return out;
    }
    const double loss = *out.loss;
    if (paired.playedMoveRank == 1 && loss >= config.inaccuracyLoss) {
        out.issue = ClassificationIssue::BestMoveConflict;
        out.ruleId = rule("normal.best_move_conflict");
    } else if (loss >= config.blunderLoss) {
        out.label = Classification::Blunder;
        out.ruleId = rule("expected_score_loss.blunder");
    } else if (loss >= config.mistakeLoss) {
        out.label = Classification::Mistake;
        out.ruleId = rule("expected_score_loss.mistake");
    } else if (loss >= config.inaccuracyLoss) {
        out.label = Classification::Inaccuracy;
        out.ruleId = rule("expected_score_loss.inaccuracy");
    } else if (paired.playedMoveRank == 1) {
        out.label = Classification::Best;
        out.ruleId = rule("verified_candidate.best");
    } else if (loss <= config.excellentLoss) {
        out.label = Classification::Excellent;
        out.ruleId = rule("expected_score_loss.excellent");
    } else {
        out.label = Classification::Good;
        out.ruleId = rule("expected_score_loss.good");
    }
    return out;
}

void applyBookClassification(ClassificationResult& result) {
    if (!result.issue && result.label) {
        result.label = Classification::Book;
        result.ruleId = "opening.book.v1";
        result.stable = true;
    }
}
} // namespace chess_review::review
