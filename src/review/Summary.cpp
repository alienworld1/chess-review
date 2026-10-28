#include "review/Summary.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace chess_review::review {
namespace {
std::optional<int> ticks(const PositionEvidence& evidence, chess::Color perspective);
}
bool needsRefinement(const ClassificationResult& result, const MoveEvidence& pair,
                     const RefinementConfig& config) {
    if (pair.mate.before || pair.mate.after ||
        pair.issue == EvidenceIssue::ContradictoryCandidates ||
        pair.issue == EvidenceIssue::NegativeLoss ||
        result.issue == ClassificationIssue::BestMoveConflict) {
        return true;
    }
    if (!result.loss) {
        return false;
    }
    if (*result.loss >= result.thresholds.mistakeLoss) {
        return true;
    }
    double measuredTicks = *result.loss * 2000;
    if (!pair.issue && pair.beforeEvaluation.expected && pair.afterEvaluation.expected &&
        pair.beforeEvaluation.expected->source != ConversionSource::ApproximateCentipawns &&
        pair.afterEvaluation.expected->source != ConversionSource::ApproximateCentipawns) {
        const auto before = ticks(pair.before, pair.mover);
        const auto after = ticks(pair.after, pair.mover);
        if (before && after) {
            measuredTicks = static_cast<double>(*before - *after);
        }
    }
    // Compare in the native half-permille unit; subtracting decimal thresholds
    // first can incorrectly exclude an exact one-point boundary (e.g. 9% vs 10%).
    for (const double boundary : {result.thresholds.excellentLoss, result.thresholds.inaccuracyLoss,
                                  result.thresholds.mistakeLoss, result.thresholds.blunderLoss}) {
        if (std::abs(measuredTicks - boundary * 2000) <= config.boundaryMargin * 2000) {
            return true;
        }
    }
    return false;
}
namespace {
bool stronger(const PositionEvidence& first, const PositionEvidence& second) {
    if (first.terminal && second.terminal) {
        return first.terminal == second.terminal;
    }
    return first.complete && second.complete && !first.attemptIdentity.empty() &&
           !second.attemptIdentity.empty() && first.attemptIdentity != second.attemptIdentity &&
           first.strength.nodes && second.strength.nodes &&
           *second.strength.nodes > *first.strength.nodes && first.strength.depth &&
           second.strength.depth && *second.strength.depth >= *first.strength.depth;
}
bool sameMate(const std::optional<RawScore>& a, const std::optional<RawScore>& b) {
    return a && b && a->bound == Bound::Exact && b->bound == Bound::Exact && a->value != 0 &&
           b->value != 0 && (a->value > 0) == (b->value > 0);
}
std::optional<int> ticks(const PositionEvidence& evidence, chess::Color perspective) {
    if (evidence.terminal) {
        return *evidence.terminal == TerminalOutcome::Stalemate
                   ? 1000
                   : (evidence.position.board.sideToMove == perspective ? 0 : 2000);
    }
    if (!evidence.evaluation.wdl) {
        return std::nullopt;
    }
    const auto& wdl = *evidence.evaluation.wdl;
    if (wdl.wins < 0 || wdl.draws < 0 || wdl.losses < 0 || wdl.wins > 1000 || wdl.draws > 1000 ||
        wdl.losses > 1000 || wdl.wins + wdl.draws + wdl.losses != 1000) {
        return std::nullopt;
    }
    return 2 * (evidence.evaluation.perspective == perspective ? wdl.wins : wdl.losses) + wdl.draws;
}
double drift(const PositionEvidence& a, const PositionEvidence& b, const ExpectedScore& first,
             const ExpectedScore& second) {
    if (first.source != ConversionSource::ApproximateCentipawns &&
        second.source != ConversionSource::ApproximateCentipawns) {
        const auto before = ticks(a, first.perspective);
        const auto after = ticks(b, first.perspective);
        if (!before || !after) {
            return std::numeric_limits<double>::infinity();
        }
        return static_cast<double>(std::abs(*before - *after)) / 2000;
    }
    return std::abs(first.value - second.value);
}
} // namespace
std::optional<double> accuracyLoss(const MoveEvidence& pair) {
    if (pair.issue || !pair.loss || !pair.beforeEvaluation.expected ||
        !pair.afterEvaluation.expected) {
        return std::nullopt;
    }
    if (pair.beforeEvaluation.expected->source != ConversionSource::ApproximateCentipawns &&
        pair.afterEvaluation.expected->source != ConversionSource::ApproximateCentipawns) {
        const auto before = ticks(pair.before, pair.mover);
        const auto after = ticks(pair.after, pair.mover);
        if (!before || !after || *before < *after) {
            return std::nullopt;
        }
        return static_cast<double>(*before - *after) / 2000;
    }
    return pair.loss;
}
StabilityAssessment assessStability(const ClassificationResult& deep,
                                    const ClassificationResult& verified,
                                    const MoveEvidence& deepPair, const MoveEvidence& verifiedPair,
                                    const RefinementConfig& config) {
    StabilityAssessment out{StabilityReason::Insufficient};
    if (!std::isfinite(config.maximumDrift) || config.maximumDrift < 0 || config.maximumDrift > 1) {
        return out;
    }
    if (!stronger(deepPair.before, verifiedPair.before) ||
        !stronger(deepPair.after, verifiedPair.after)) {
        return out;
    }
    const bool beforeMate = sameMate(deepPair.mate.before, verifiedPair.mate.before);
    const bool afterMate = sameMate(deepPair.mate.after, verifiedPair.mate.after);
    out.persistentMate = (beforeMate || afterMate) &&
                         ((!deepPair.mate.before && !verifiedPair.mate.before) || beforeMate) &&
                         ((!deepPair.mate.after && !verifiedPair.mate.after) || afterMate);
    if (!deep.bestMove || !verified.bestMove || !deep.label || !verified.label ||
        !deep.before.expected || !deep.after.expected || !verified.before.expected ||
        !verified.after.expected) {
        return out;
    }
    if (deep.label != verified.label || deep.bestMove != verified.bestMove ||
        deep.before.expected->source != verified.before.expected->source ||
        deep.after.expected->source != verified.after.expected->source) {
        out.reason = StabilityReason::Disagreement;
        return out;
    }
    if (drift(deepPair.before, verifiedPair.before, *deep.before.expected,
              *verified.before.expected) > config.maximumDrift ||
        drift(deepPair.after, verifiedPair.after, *deep.after.expected, *verified.after.expected) >
            config.maximumDrift) {
        out.reason = StabilityReason::ScoreDrift;
        return out;
    }
    out.reason = StabilityReason::Confirmed;
    out.stable = true;
    return out;
}
std::optional<double> moveAccuracy(std::optional<double> loss) {
    if (!loss || !std::isfinite(*loss) || *loss < 0 || *loss > 1) {
        return std::nullopt;
    }
    if (*loss == 0) {
        return 100;
    }
    return std::clamp(103.1668 * std::exp(-0.04354 * 100 * *loss) - 3.1669, 0.0, 100.0);
}
std::optional<double> gameAccuracy(std::span<const double> moves) {
    if (moves.empty()) {
        return std::nullopt;
    }
    double sum = 0;
    double reciprocals = 0;
    bool zero = false;
    for (const double value : moves) {
        if (!std::isfinite(value) || value < 0 || value > 100) {
            return std::nullopt;
        }
        sum += value;
        if (value == 0) {
            zero = true;
        } else {
            reciprocals += 1 / value;
        }
    }
    const auto count = static_cast<double>(moves.size());
    return (sum / count + (zero ? 0 : count / reciprocals)) / 2;
}
namespace {
std::optional<chess::Color> mateWinner(const RawEvaluation& evaluation) {
    if (!evaluation.score || evaluation.score->kind != ScoreKind::Mate ||
        evaluation.score->value == 0) {
        return std::nullopt;
    }
    return evaluation.score->value > 0
               ? evaluation.perspective
               : (evaluation.perspective == chess::Color::White ? chess::Color::Black
                                                                : chess::Color::White);
}
} // namespace
KeyMomentSelection SummaryBuilder::add(const SummaryMoveFacts& move) {
    const auto sideIndex = static_cast<std::size_t>(move.mover);
    auto& side = facts_.sides[sideIndex];
    ++side.total;
    if (!move.processed) {
        ++side.unprocessed;
    } else {
        if (move.stable) {
            ++side.stable;
        }
        if (move.label) {
            ++side.counts[static_cast<std::size_t>(*move.label)];
        } else {
            ++side.withheld;
        }
        if (const auto value = moveAccuracy(move.loss)) {
            ++side.evaluated;
            accuracies_[sideIndex].push_back(*value);
        }
    }
    const bool major = move.label && *move.label >= Classification::Mistake;
    const bool mateTransition = mateWinner(move.before) != mateWinner(move.after);
    KeyMomentSelection selection;
    if (move.mover == selectedSide_) {
        if (move.loss && moveAccuracy(move.loss) &&
            (!facts_.largestLoss || *move.loss > *facts_.largestLoss)) {
            facts_.largestLoss = move.loss;
            facts_.largestLossPosition = move.positionIndex;
        }
        if (major || mateTransition) {
            selection.selected = true;
            ++facts_.keyMoments;
            if (previousOpponentError_) {
                selection.opponentContext = move.positionIndex - 1;
            }
        }
    }
    previousOpponentError_ = move.mover != selectedSide_ && major;
    return selection;
}
SummaryFacts SummaryBuilder::finish() const {
    auto result = facts_;
    for (std::size_t side = 0; side < result.sides.size(); ++side) {
        result.sides[side].accuracy = gameAccuracy(accuracies_[side]);
    }
    return result;
}
} // namespace chess_review::review
