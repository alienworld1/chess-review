#include "review/Retry.h"
#include <algorithm>
#include <cmath>
namespace chess_review::review {
namespace {
std::optional<double> candidateGap(const PositionEvidence& before, chess::Move move,
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
        return static_cast<double>(ticks(before.evaluation, before.position.board.sideToMove) -
                                   ticks(found->evaluation, before.position.board.sideToMove)) /
               2000.0;
    }
    return best.expected->value - value.expected->value;
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
    if (!root.isLegal(attempted) || deep.playedMove != verified.playedMove) {
        return {};
    }
    const auto d = classify(root, deep, config.classification, config.conversion);
    const auto v = classify(root, verified, config.classification, config.conversion);
    if (!assessStability(d, v, deep, verified, config.refinement).stable || !d.bestMove ||
        !v.bestMove) {
        return {};
    }
    std::optional<double> dg, vg;
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
        dg = dc.loss;
        vg = vc.loss;
    } else {
        dg = candidateGap(deep.before, attempted, config.conversion);
        vg = candidateGap(verified.before, attempted, config.conversion);
    }
    if (!dg || !vg || !std::isfinite(*dg) || !std::isfinite(*vg) || *dg < 0 || *vg < 0 ||
        std::abs(*dg - *vg) > config.refinement.maximumDrift) {
        return {};
    }
    const bool first = attempted == *d.bestMove && attempted == *v.bestMove;
    const auto a = grade(*dg, first, config.classification);
    const auto b = grade(*vg, first, config.classification);
    if (a != b) {
        return {};
    }
    return {b, vg};
}
} // namespace chess_review::review
