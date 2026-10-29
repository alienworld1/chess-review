#include "review/Retry.h"
#include <algorithm>
#include <cmath>
namespace chess_review::review {
namespace {
struct MeasuredGap {
    double value;
    std::optional<int> ticks;
};
int expectedTicks(const PositionEvidence& evidence, const ExpectedScore& score, chess::Color side) {
    if (score.source == ConversionSource::Terminal) {
        return static_cast<int>(score.value * 2000);
    }
    const auto& wdl = *evidence.evaluation.wdl;
    return 2 * (evidence.evaluation.perspective == side ? wdl.wins : wdl.losses) + wdl.draws;
}
std::optional<MeasuredGap> candidateGap(const PositionEvidence& before, chess::Move move,
                                        const ConversionConfig& conversion) {
    const auto found =
        std::find_if(before.candidates.begin(), before.candidates.end(), [move](const auto& c) {
            return !c.continuation.empty() && c.continuation.front() == move;
        });
    if (found == before.candidates.end()) {
        return {};
    }
    const auto best =
        convertEvaluation(before.evaluation, before.position.board.sideToMove, conversion);
    const auto value =
        convertEvaluation(found->evaluation, before.position.board.sideToMove, conversion);
    if (best.issue || value.issue || !best.expected || !value.expected ||
        best.expected->source != value.expected->source) {
        return {};
    }
    if (best.expected->source == ConversionSource::NativeWdl) {
        const auto ticks = [](const RawEvaluation& e, chess::Color side) {
            return 2 * (e.perspective == side ? e.wdl->wins : e.wdl->losses) + e.wdl->draws;
        };
        const int difference = ticks(before.evaluation, before.position.board.sideToMove) -
                               ticks(found->evaluation, before.position.board.sideToMove);
        return MeasuredGap{static_cast<double>(difference) / 2000, difference};
    }
    return MeasuredGap{best.expected->value - value.expected->value, {}};
}
RetryGrade grade(double gap, bool first, const ClassificationConfig& thresholds) {
    if (gap >= thresholds.inaccuracyLoss) {
        return RetryGrade::TryAgain;
    }
    if (first) {
        return RetryGrade::Best;
    }
    if (gap <= thresholds.excellentLoss) {
        return RetryGrade::Strong;
    }
    return RetryGrade::Okay;
}
} // namespace
RetryResult gradeRetry(const chess::Position& root, chess::Move attempted, const MoveEvidence& deep,
                       const MoveEvidence& verified, const Configuration& config,
                       const PositionEvidence* deepChild, const PositionEvidence* verifiedChild) {
    if ((deepChild == nullptr) != (verifiedChild == nullptr) || !root.isLegal(attempted) ||
        deep.playedMove != verified.playedMove ||
        deep.before.engineIdentity != verified.before.engineIdentity) {
        return {};
    }
    const auto d = classify(root, deep, config.classification, config.conversion);
    const auto v = classify(root, verified, config.classification, config.conversion);
    if (!assessStability(d, v, deep, verified, config.refinement).stable || !d.bestMove ||
        !v.bestMove) {
        return {};
    }
    std::optional<MeasuredGap> dg, vg;
    if (deepChild && verifiedChild) {
        const auto dp = pairEvidence(root, attempted, deep.before, *deepChild, config.conversion);
        const auto vp =
            pairEvidence(root, attempted, verified.before, *verifiedChild, config.conversion);
        if (dp.issue || vp.issue) {
            return {};
        }
        // classify owns the exact-WDL subtraction and best-move conflict rules.
        const auto dc = classify(root, dp, config.classification, config.conversion);
        const auto vc = classify(root, vp, config.classification, config.conversion);
        if (!assessStability(dc, vc, dp, vp, config.refinement).stable) {
            return {};
        }
        const auto measure = [](const MoveEvidence& pair, const ClassificationResult& result) {
            MeasuredGap gap{*result.loss, {}};
            if (result.before.expected->source != ConversionSource::ApproximateCentipawns &&
                result.after.expected->source != ConversionSource::ApproximateCentipawns) {
                gap.ticks = expectedTicks(pair.before, *result.before.expected, pair.mover) -
                            expectedTicks(pair.after, *result.after.expected, pair.mover);
            }
            return gap;
        };
        dg = measure(dp, dc);
        vg = measure(vp, vc);
    } else {
        dg = candidateGap(deep.before, attempted, config.conversion);
        vg = candidateGap(verified.before, attempted, config.conversion);
    }
    if (!dg || !vg || !std::isfinite(dg->value) || !std::isfinite(vg->value) || dg->value < 0 ||
        vg->value < 0) {
        return {};
    }
    const double drift = dg->ticks && vg->ticks
                             ? static_cast<double>(std::abs(*dg->ticks - *vg->ticks)) / 2000
                             : std::abs(dg->value - vg->value);
    if (drift > config.refinement.maximumDrift) {
        return {};
    }
    const bool first = attempted == *d.bestMove && attempted == *v.bestMove;
    const auto a = grade(dg->value, first, config.classification);
    const auto b = grade(vg->value, first, config.classification);
    if (a != b) {
        return {};
    }
    return {b, vg->value};
}
} // namespace chess_review::review
