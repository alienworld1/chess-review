#include "review/Classification.h"
#include <cmath>
namespace chess_review::review {
ClassificationResult classify(const chess::Position& position, const MoveEvidence& evidence,
                              const ClassificationConfig& config,
                              const ConversionConfig& conversion) {
    ClassificationResult out;
    out.thresholds = config;
    out.mover = position.sideToMove();
    if (!std::isfinite(config.excellentLoss) || config.excellentLoss <= 0 ||
        config.excellentLoss >= 1 || !std::isfinite(config.inaccuracyLoss) ||
        !std::isfinite(config.mistakeLoss) || !std::isfinite(config.blunderLoss) ||
        config.inaccuracyLoss <= 0 || config.inaccuracyLoss >= config.mistakeLoss ||
        config.mistakeLoss >= config.blunderLoss || config.blunderLoss >= 1) {
        out.issue = ClassificationIssue::InvalidConfiguration;
        out.ruleId = "normal.invalid_configuration.v1";
        return out;
    }
    const auto paired =
        pairEvidence(position, evidence.playedMove, evidence.before, evidence.after, conversion);
    out.before = paired.beforeEvaluation;
    out.after = paired.afterEvaluation;
    out.signedDelta = paired.signedDelta;
    out.loss = paired.loss;
    if (paired.issue || !paired.loss) {
        out.issue = ClassificationIssue::Evidence;
        out.evidenceIssue = paired.issue.value_or(EvidenceIssue::Incomplete);
        out.ruleId = "normal.insufficient_evidence.v1";
        return out;
    }
    out.playedMoveRank = paired.playedMoveRank;
    out.bestMove = paired.before.candidates.front().continuation.front();
    const auto best = *paired.beforeEvaluation.expected;
    for (const auto& candidate : paired.before.candidates) {
        if (candidate.rank == 1) {
            continue;
        }
        const auto value = convertEvaluation(candidate.evaluation, paired.mover, best.config);
        const double gap = best.value - value.expected->value;
        out.alternatives.push_back(
            {candidate.continuation.front(), candidate.rank, gap, gap <= config.excellentLoss});
    }
    const double loss = *paired.loss;
    if (paired.playedMoveRank == 1 && loss >= config.inaccuracyLoss) {
        out.issue = ClassificationIssue::BestMoveConflict;
        out.ruleId = "normal.best_move_conflict.v1";
    } else if (loss >= config.blunderLoss) {
        out.label = Classification::Blunder;
        out.ruleId = "expected_score_loss.blunder.v1";
    } else if (loss >= config.mistakeLoss) {
        out.label = Classification::Mistake;
        out.ruleId = "expected_score_loss.mistake.v1";
    } else if (loss >= config.inaccuracyLoss) {
        out.label = Classification::Inaccuracy;
        out.ruleId = "expected_score_loss.inaccuracy.v1";
    } else if (paired.playedMoveRank == 1) {
        out.label = Classification::Best;
        out.ruleId = "verified_candidate.best.v1";
    } else if (loss <= config.excellentLoss) {
        out.label = Classification::Excellent;
        out.ruleId = "expected_score_loss.excellent.v1";
    } else {
        out.label = Classification::Good;
        out.ruleId = "expected_score_loss.good.v1";
    }
    return out;
}
} // namespace chess_review::review
