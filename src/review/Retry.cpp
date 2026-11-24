#include "review/Retry.h"
#include "review/Intelligence.h"
#include <algorithm>
#include <cmath>
namespace chess_review::review {
namespace {
struct MeasuredGap {
    double value;
    std::optional<int> ticks;
};
bool prefersV2Model(const MoveEvidence& evidence) {
    return (evidence.beforeEvaluation.expected &&
            evidence.beforeEvaluation.expected->modelVersion >= 2) ||
           (evidence.before.evaluation.score &&
            evidence.before.evaluation.score->kind == ScoreKind::Mate);
}
bool hasPositiveNodes(const PositionEvidence& evidence) {
    return evidence.terminal || (evidence.strength.nodes && *evidence.strength.nodes > 0);
}
std::optional<RetryHint> baseRetryHint(const chess::Position& root,
                                       const PositionEvidence& before) {
    if (before.candidates.empty() || before.candidates.front().continuation.empty()) {
        return {};
    }
    const auto best = before.candidates.front().continuation.front();
    const auto piece = root.pieceAt(best.from);
    if (!piece) {
        return {};
    }
    return RetryHint{RetryIdea::General, best, piece->type};
}
int expectedTicks(const PositionEvidence& evidence, const ExpectedScore& score, chess::Color side) {
    if (score.source == ConversionSource::Terminal || score.source == ConversionSource::Mate) {
        return static_cast<int>(score.value * 2000);
    }
    const auto& wdl = *evidence.evaluation.wdl;
    return 2 * (evidence.evaluation.perspective == side ? wdl.wins : wdl.losses) + wdl.draws;
}
std::optional<MeasuredGap> candidateGap(const PositionEvidence& before, chess::Move move,
                                        const ConversionConfig& conversion,
                                        std::uint32_t expectedModel) {
    const auto found =
        std::find_if(before.candidates.begin(), before.candidates.end(), [move](const auto& c) {
            return !c.continuation.empty() && c.continuation.front() == move;
        });
    if (found == before.candidates.end()) {
        return {};
    }
    const auto best = convertEvaluation(before.evaluation, before.position.board.sideToMove,
                                        conversion, expectedModel);
    const auto value = convertEvaluation(found->evaluation, before.position.board.sideToMove,
                                         conversion, expectedModel);
    if (best.issue || value.issue || !best.expected || !value.expected ||
        (expectedModel == 1 && best.expected->source != value.expected->source)) {
        return {};
    }
    if (best.expected->source == ConversionSource::NativeWdl &&
        value.expected->source == ConversionSource::NativeWdl) {
        const auto ticks = [](const RawEvaluation& e, chess::Color side) {
            return 2 * (e.perspective == side ? e.wdl->wins : e.wdl->losses) + e.wdl->draws;
        };
        const int difference = ticks(before.evaluation, before.position.board.sideToMove) -
                               ticks(found->evaluation, before.position.board.sideToMove);
        return MeasuredGap{static_cast<double>(difference) / 2000, difference};
    }
    return MeasuredGap{std::max(0.0, best.expected->value - value.expected->value), {}};
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
    const bool useV2 = prefersV2Model(deep) || prefersV2Model(verified);
    const std::uint32_t classModel = useV2 ? 3U : 2U;
    const std::uint32_t expectedModel = useV2 ? 2U : 1U;
    const std::uint32_t stabilityModel = useV2 ? 4U : 1U;
    const auto d = classify(root, deep, config.classification, config.conversion, classModel);
    const auto v = classify(root, verified, config.classification, config.conversion, classModel);
    if (!assessStability(d, v, deep, verified, config.refinement, stabilityModel, false).stable ||
        !d.bestMove || !v.bestMove) {
        return {};
    }
    std::optional<MeasuredGap> dg, vg;
    if (deepChild && verifiedChild) {
        const auto dp = pairEvidence(root, attempted, deep.before, *deepChild, config.conversion,
                                     expectedModel);
        const auto vp = pairEvidence(root, attempted, verified.before, *verifiedChild,
                                     config.conversion, expectedModel);
        if (dp.issue || vp.issue) {
            return {};
        }
        // classify owns the exact-WDL subtraction and best-move conflict rules.
        const auto dc = classify(root, dp, config.classification, config.conversion, classModel);
        const auto vc = classify(root, vp, config.classification, config.conversion, classModel);
        if (!assessStability(dc, vc, dp, vp, config.refinement, stabilityModel, false).stable) {
            return {};
        }
        const auto measure = [](const MoveEvidence& pair, const ClassificationResult& result) {
            MeasuredGap gap{*result.loss, {}};
            if ((result.before.expected->source == ConversionSource::NativeWdl ||
                 result.before.expected->source == ConversionSource::Terminal) &&
                (result.after.expected->source == ConversionSource::NativeWdl ||
                 result.after.expected->source == ConversionSource::Terminal)) {
                gap.ticks = expectedTicks(pair.before, *result.before.expected, pair.mover) -
                            expectedTicks(pair.after, *result.after.expected, pair.mover);
            }
            return gap;
        };
        dg = measure(dp, dc);
        vg = measure(vp, vc);
    } else {
        dg = candidateGap(deep.before, attempted, config.conversion, expectedModel);
        vg = candidateGap(verified.before, attempted, config.conversion, expectedModel);
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
RetryResult gradeRetry(const chess::Position& root, chess::Move attempted,
                       const MoveEvidence& evidence, const Configuration& config,
                       const PositionEvidence* child) {
    if (!root.isLegal(attempted) || !hasPositiveNodes(evidence.before) ||
        !hasPositiveNodes(evidence.after) || (child && !hasPositiveNodes(*child))) {
        return {};
    }
    const bool useV2 = prefersV2Model(evidence);
    const std::uint32_t classModel = useV2 ? 3U : 2U;
    const std::uint32_t expectedModel = useV2 ? 2U : 1U;
    const auto original =
        classify(root, evidence, config.classification, config.conversion, classModel);
    if (original.issue || !original.bestMove || !original.label) {
        return {};
    }
    std::optional<MeasuredGap> gap;
    if (child) {
        const auto paired = pairEvidence(root, attempted, evidence.before, *child,
                                         config.conversion, expectedModel);
        if (paired.issue) {
            return {};
        }
        const auto classified =
            classify(root, paired, config.classification, config.conversion, classModel);
        if (classified.issue || !classified.loss) {
            return {};
        }
        gap = MeasuredGap{*classified.loss, {}};
    } else {
        gap = candidateGap(evidence.before, attempted, config.conversion, expectedModel);
    }
    if (!gap || !std::isfinite(gap->value) || gap->value < 0) {
        return {};
    }
    return {grade(gap->value, attempted == *original.bestMove, config.classification), gap->value};
}
std::optional<RetryHint> retryHint(const chess::Position& root, const MoveEvidence& deep,
                                   const MoveEvidence& verified, const Configuration& config) {
    const auto base = baseRetryHint(root, verified.before);
    if (!base ||
        gradeRetry(root, base->bestMove, deep, verified, config).grade != RetryGrade::Best) {
        return {};
    }
    auto hint = *base;
    const auto older = tacticalFacts(root, deep, config);
    const auto newer = tacticalFacts(root, verified, config);
    // Explicit priority makes the idea independent of detector output order. Matching
    // concrete witnesses does not establish that every defence has this consequence.
    for (const auto kind :
         {TacticalKind::Checkmate, TacticalKind::Fork, TacticalKind::HangingPiece,
          TacticalKind::Promotion, TacticalKind::Exchange, TacticalKind::Material}) {
        for (const auto& fact : newer) {
            if (fact.kind != kind || fact.source != FactSource::Best ||
                fact.beneficiary != root.sideToMove() ||
                (kind == TacticalKind::HangingPiece && fact.linePlies > 1) ||
                ((kind == TacticalKind::Promotion || kind == TacticalKind::Exchange ||
                  kind == TacticalKind::Material) &&
                 !fact.retainedOpportunity)) {
                continue;
            }
            const bool matches = std::any_of(older.begin(), older.end(), [&](const auto& old) {
                return old.kind == fact.kind && old.source == fact.source &&
                       old.beneficiary == fact.beneficiary && old.actor == fact.actor &&
                       old.square == fact.square && old.targets == fact.targets &&
                       old.material == fact.material &&
                       old.retainedOpportunity == fact.retainedOpportunity;
            });
            if (!matches) {
                continue;
            }
            switch (kind) {
            case TacticalKind::Checkmate:
                hint.idea = RetryIdea::Checkmate;
                break;
            case TacticalKind::Fork:
                hint.idea = RetryIdea::Fork;
                break;
            case TacticalKind::HangingPiece:
                hint.idea = RetryIdea::HangingPiece;
                break;
            case TacticalKind::Promotion:
                hint.idea = RetryIdea::Promotion;
                break;
            case TacticalKind::Exchange:
                hint.idea = RetryIdea::Exchange;
                break;
            case TacticalKind::Material:
                hint.idea = RetryIdea::Material;
                break;
            case TacticalKind::Sacrifice:
                break;
            }
            return hint;
        }
    }
    return hint;
}
std::optional<RetryHint> retryHint(const chess::Position& root, const MoveEvidence& evidence,
                                   const Configuration& config) {
    const auto base = baseRetryHint(root, evidence.before);
    if (!base || gradeRetry(root, base->bestMove, evidence, config).grade != RetryGrade::Best) {
        return {};
    }
    return base;
}
} // namespace chess_review::review
