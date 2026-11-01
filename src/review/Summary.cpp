#include "review/Summary.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
namespace chess_review::review {
namespace {
std::optional<int> ticks(const PositionEvidence& evidence, chess::Color perspective);
bool validExpected(const EvaluationResult& result, chess::Color mover);
std::vector<RefinementReason> refinementReasons(const ClassificationResult& result,
                                                const MoveEvidence& pair,
                                                const RefinementConfig& config,
                                                bool includeLegacyLoss = true) {
    std::vector<RefinementReason> reasons;
    if (pair.mate.before || pair.mate.after) {
        reasons.push_back(RefinementReason::MateSignal);
    }
    if (pair.issue == EvidenceIssue::ContradictoryCandidates ||
        pair.issue == EvidenceIssue::NegativeLoss ||
        result.issue == ClassificationIssue::BestMoveConflict) {
        reasons.push_back(RefinementReason::SearchConflict);
    }
    if (!includeLegacyLoss || !result.loss) {
        return reasons;
    }
    if (*result.loss >= result.thresholds.mistakeLoss) {
        reasons.push_back(RefinementReason::MajorLoss);
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
    // Keep threshold comparisons in the native half-permille unit.
    for (const double boundary : {result.thresholds.excellentLoss, result.thresholds.inaccuracyLoss,
                                  result.thresholds.mistakeLoss, result.thresholds.blunderLoss}) {
        if (std::abs(measuredTicks - boundary * 2000) <= config.boundaryMargin * 2000) {
            reasons.push_back(RefinementReason::ThresholdProximity);
            break;
        }
    }
    return reasons;
}
bool invalidIssue(std::optional<EvidenceIssue> issue) {
    if (!issue) {
        return false;
    }
    switch (*issue) {
    case EvidenceIssue::Incomplete:
    case EvidenceIssue::Bounded:
    case EvidenceIssue::MateClaim:
    case EvidenceIssue::MissingEvaluation:
    case EvidenceIssue::MixedConversion:
    case EvidenceIssue::NegativeLoss:
    case EvidenceIssue::ContradictoryCandidates:
        return false;
    case EvidenceIssue::Invalid:
    case EvidenceIssue::ContextMismatch:
    case EvidenceIssue::IncompatibleEngine:
    case EvidenceIssue::IncompatibleModel:
    case EvidenceIssue::RestrictedSearch:
        return true;
    }
    return true;
}
} // namespace
bool needsRefinement(const ClassificationResult& result, const MoveEvidence& pair,
                     const RefinementConfig& config) {
    return !refinementReasons(result, pair, config).empty();
}
ScanAssessment assessQuickScan(const ClassificationResult& result, const MoveEvidence& pair,
                               const RefinementConfig& config, std::uint32_t pipelineVersion) {
    ScanAssessment scan;
    scan.pipelineVersion = pipelineVersion;
    scan.state = ScanState::Unusable;
    scan.issue = pair.issue ? pair.issue : result.evidenceIssue;
    const auto rawIssue = [](const PositionEvidence& evidence) -> std::optional<EvidenceIssue> {
        if (evidence.terminal) {
            return std::nullopt;
        }
        return convertEvaluation(evidence.evaluation, evidence.evaluation.perspective).issue;
    };
    const auto rawBeforeIssue = rawIssue(pair.before);
    const auto rawAfterIssue = rawIssue(pair.after);
    const auto invalidLoss = [](std::optional<double> value) {
        return value && (!std::isfinite(*value) || *value < 0 || *value > 1);
    };
    const auto invalidDelta = [](std::optional<double> value) {
        return value && (!std::isfinite(*value) || std::abs(*value) > 1);
    };
    const auto invalidExpected = [&](const EvaluationResult& evaluation) {
        return evaluation.expected && !validExpected(evaluation, pair.mover);
    };
    const bool numeric = pair.loss || result.loss || pair.signedDelta || result.signedDelta;
    const bool missingNumericEvidence =
        numeric && (!pair.beforeEvaluation.expected || !pair.afterEvaluation.expected ||
                    !result.before.expected || !result.after.expected);
    const auto expectedLoss = result.modelVersion == 1 ? pair.loss : accuracyLoss(pair);
    const bool inconsistentLoss =
        (result.loss && (result.loss != expectedLoss || result.signedDelta != result.loss)) ||
        (pair.loss && pair.signedDelta != pair.loss);
    if (!supportsPipelineModel(pipelineVersion) ||
        !supportsClassificationModel(result.modelVersion) ||
        pair.before.modelVersion != evidenceModelVersion ||
        pair.after.modelVersion != evidenceModelVersion ||
        (pair.mover != chess::Color::White && pair.mover != chess::Color::Black) ||
        result.mover != pair.mover || missingNumericEvidence || inconsistentLoss ||
        (pipelineVersion >= 3 &&
         (!std::isfinite(result.thresholds.inaccuracyLoss) ||
          !std::isfinite(result.thresholds.mistakeLoss) ||
          !std::isfinite(result.thresholds.blunderLoss) || result.thresholds.inaccuracyLoss <= 0 ||
          result.thresholds.inaccuracyLoss >= result.thresholds.mistakeLoss ||
          result.thresholds.mistakeLoss >= result.thresholds.blunderLoss ||
          result.thresholds.blunderLoss >= 1)) ||
        invalidExpected(result.before) || invalidExpected(result.after) ||
        invalidExpected(pair.beforeEvaluation) || invalidExpected(pair.afterEvaluation) ||
        invalidLoss(result.loss) || invalidLoss(pair.loss) || invalidDelta(result.signedDelta) ||
        invalidDelta(pair.signedDelta) ||
        (result.issue && *result.issue != ClassificationIssue::Evidence &&
         *result.issue != ClassificationIssue::BestMoveConflict) ||
        !std::isfinite(config.boundaryMargin) || config.boundaryMargin < 0 ||
        config.boundaryMargin > 1 || !std::isfinite(config.maximumDrift) ||
        config.maximumDrift < 0 || config.maximumDrift > 1 || config.multiPv < 1 ||
        config.multiPv > 256 || invalidIssue(scan.issue) ||
        invalidIssue(pair.beforeEvaluation.issue) || invalidIssue(pair.afterEvaluation.issue) ||
        invalidIssue(rawBeforeIssue) || invalidIssue(rawAfterIssue)) {
        scan.state = ScanState::Invalid;
        if (pipelineVersion == 1) {
            scan.reasons = refinementReasons(result, pair, config);
        }
        return scan;
    }
    if (!pair.issue && !result.issue && pair.loss && result.loss && pair.signedDelta &&
        std::isfinite(*pair.loss) && std::isfinite(*result.loss) &&
        std::isfinite(*pair.signedDelta)) {
        scan.state = ScanState::Usable;
        scan.signedDelta = pair.signedDelta;
        scan.loss = result.loss;
    }
    scan.reasons = refinementReasons(result, pair, config, pipelineVersion < 3);
    if (pipelineVersion >= 3) {
        if (scan.state == ScanState::Usable) {
            double measuredTicks = *scan.loss * 2000;
            if (pair.beforeEvaluation.expected->source != ConversionSource::ApproximateCentipawns &&
                pair.afterEvaluation.expected->source != ConversionSource::ApproximateCentipawns) {
                const auto before = ticks(pair.before, pair.mover);
                const auto after = ticks(pair.after, pair.mover);
                if (!before || !after || *before < *after) {
                    scan.state = ScanState::Invalid;
                    scan.reasons.clear();
                    scan.loss.reset();
                    scan.signedDelta.reset();
                    return scan;
                }
                measuredTicks = static_cast<double>(*before - *after);
            }
            // Keep released policy 3's decisions reproducible. Scaling adjacent floating
            // values can merge them; policy 4 compares the canonical loss directly.
            const double measuredLoss =
                pair.beforeEvaluation.expected->source == ConversionSource::ApproximateCentipawns
                    ? *scan.loss
                    : measuredTicks / 2000;
            const bool qualifies = pipelineVersion == 3
                                       ? measuredTicks >= result.thresholds.inaccuracyLoss * 2000
                                       : measuredLoss >= result.thresholds.inaccuracyLoss;
            if (qualifies) {
                scan.reasons.push_back(RefinementReason::ExpectedScoreLoss);
                scan.qualifyingLoss = pipelineVersion == 3 ? measuredTicks / 2000 : measuredLoss;
            }
            for (const double boundary :
                 {result.thresholds.inaccuracyLoss, result.thresholds.mistakeLoss,
                  result.thresholds.blunderLoss}) {
                if (std::abs(measuredTicks - boundary * 2000) <= config.boundaryMargin * 2000) {
                    scan.reasons.push_back(RefinementReason::ThresholdProximity);
                    break;
                }
            }
        }
    }
    if (pipelineVersion == 1) {
        return scan;
    }
    const auto addGap = [&](EvidenceIssue issue, RefinementReason reason) {
        if (scan.issue == issue || pair.beforeEvaluation.issue == issue ||
            pair.afterEvaluation.issue == issue || rawBeforeIssue == issue ||
            rawAfterIssue == issue) {
            scan.reasons.push_back(reason);
        }
    };
    addGap(EvidenceIssue::Incomplete, RefinementReason::InsufficientStrength);
    if ((!pair.before.terminal && !pair.before.evaluation.score) ||
        (!pair.after.terminal && !pair.after.evaluation.score)) {
        scan.reasons.push_back(RefinementReason::MissingEvaluation);
    }
    addGap(EvidenceIssue::Bounded, RefinementReason::BoundedEvaluation);
    addGap(EvidenceIssue::MixedConversion, RefinementReason::MixedConversion);
    return scan;
}

void addOpportunityContext(ScanAssessment& current, const ScanAssessment& previous,
                           int previousPosition) {
    std::erase(current.reasons, RefinementReason::OpportunityReply);
    current.opportunityPosition.reset();
    current.opportunityLoss.reset();
    if (previousPosition < 1 || !current.pipelineVersion ||
        current.pipelineVersion != previous.pipelineVersion ||
        !supportsPipelineModel(*current.pipelineVersion) || *current.pipelineVersion < 3 ||
        (current.state != ScanState::Usable && current.state != ScanState::Unusable) ||
        previous.state != ScanState::Usable || !previous.qualifyingLoss ||
        !std::isfinite(*previous.qualifyingLoss) || *previous.qualifyingLoss <= 0 ||
        *previous.qualifyingLoss > 1) {
        return;
    }
    current.opportunityPosition = previousPosition;
    current.opportunityLoss = previous.qualifyingLoss;
    current.reasons.push_back(RefinementReason::OpportunityReply);
}
RefinementQueueResult refinementQueue(std::span<const RefinementCandidate> candidates,
                                      std::uint32_t pipelineVersion) {
    if (!supportsPipelineModel(pipelineVersion)) {
        return RefinementQueueIssue::UnsupportedPolicy;
    }
    std::vector<std::tuple<int, double, int>> ranked;
    const auto priority = [](const RefinementCandidate& candidate) {
        const auto& scan = candidate.scan;
        const auto has = [&](RefinementReason reason) {
            return std::find(scan.reasons.begin(), scan.reasons.end(), reason) !=
                   scan.reasons.end();
        };
        int tier = 5;
        double loss = 0;
        if (has(RefinementReason::MateSignal)) {
            tier = 0;
        } else if (has(RefinementReason::SearchConflict)) {
            tier = 1;
        } else if (scan.qualifyingLoss) {
            tier = 2;
            loss = *scan.qualifyingLoss;
        } else if (scan.opportunityLoss) {
            tier = 3;
            loss = *scan.opportunityLoss;
        } else if (has(RefinementReason::InsufficientStrength) ||
                   has(RefinementReason::MissingEvaluation) ||
                   has(RefinementReason::BoundedEvaluation) ||
                   has(RefinementReason::MixedConversion)) {
            tier = 4;
        }
        return std::tuple{tier, -loss, candidate.positionIndex};
    };
    for (const auto& candidate : candidates) {
        const auto& scan = candidate.scan;
        if (candidate.positionIndex < 1) {
            return RefinementQueueIssue::InvalidCandidate;
        }
        if (pipelineVersion >= 3) {
            const auto validLoss = [](std::optional<double> value) {
                return !value || (std::isfinite(*value) && *value >= 0 && *value <= 1);
            };
            const auto has = [&](RefinementReason reason) {
                return std::find(scan.reasons.begin(), scan.reasons.end(), reason) !=
                       scan.reasons.end();
            };
            if (scan.pipelineVersion != pipelineVersion ||
                (scan.state != ScanState::Usable && scan.state != ScanState::Unusable &&
                 scan.state != ScanState::AwaitingEvidence) ||
                !validLoss(scan.loss) || !validLoss(scan.signedDelta) ||
                !validLoss(scan.qualifyingLoss) || !validLoss(scan.opportunityLoss) ||
                (scan.state == ScanState::AwaitingEvidence && scan.needsRefinement()) ||
                (scan.qualifyingLoss &&
                 (scan.state != ScanState::Usable || *scan.qualifyingLoss == 0)) ||
                has(RefinementReason::ExpectedScoreLoss) != scan.qualifyingLoss.has_value() ||
                has(RefinementReason::OpportunityReply) != scan.opportunityLoss.has_value() ||
                scan.opportunityLoss.has_value() != scan.opportunityPosition.has_value() ||
                (scan.opportunityLoss &&
                 (*scan.opportunityLoss == 0 || *scan.opportunityPosition < 1 ||
                  *scan.opportunityPosition != candidate.positionIndex - 1))) {
                return RefinementQueueIssue::InvalidCandidate;
            }
            for (const auto reason : scan.reasons) {
                switch (reason) {
                case RefinementReason::MajorLoss:
                case RefinementReason::ThresholdProximity:
                case RefinementReason::MateSignal:
                case RefinementReason::SearchConflict:
                case RefinementReason::InsufficientStrength:
                case RefinementReason::MissingEvaluation:
                case RefinementReason::BoundedEvaluation:
                case RefinementReason::MixedConversion:
                case RefinementReason::ExpectedScoreLoss:
                case RefinementReason::OpportunityReply:
                    break;
                default:
                    return RefinementQueueIssue::InvalidCandidate;
                }
            }
        }
        if (scan.needsRefinement()) {
            ranked.push_back(pipelineVersion >= 3 ? priority(candidate)
                                                  : std::tuple{0, 0.0, candidate.positionIndex});
        }
    }
    // All sort keys are finite, and each priority is derived once per candidate.
    std::sort(ranked.begin(), ranked.end());
    std::set<int> seen;
    std::vector<int> positions;
    for (const auto& candidate : ranked) {
        const int positionIndex = std::get<2>(candidate);
        for (const int index : {positionIndex - 1, positionIndex}) {
            if (seen.insert(index).second) {
                positions.push_back(index);
            }
        }
    }
    return positions;
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
    return a && b && a->kind == ScoreKind::Mate && b->kind == ScoreKind::Mate &&
           a->bound == Bound::Exact && b->bound == Bound::Exact && a->value != 0 && b->value != 0 &&
           (a->value > 0) == (b->value > 0);
}
std::optional<int> ticks(const PositionEvidence& evidence, chess::Color perspective) {
    if (evidence.terminal) {
        if (*evidence.terminal != TerminalOutcome::Checkmate &&
            *evidence.terminal != TerminalOutcome::Stalemate) {
            return std::nullopt;
        }
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
bool validExpected(const EvaluationResult& result, chess::Color mover) {
    if (result.issue || !result.expected) {
        return false;
    }
    const auto& expected = *result.expected;
    return (mover == chess::Color::White || mover == chess::Color::Black) &&
           expected.perspective == mover && expected.modelVersion == expectedScoreModelVersion &&
           std::isfinite(expected.value) && expected.value >= 0 && expected.value <= 1 &&
           std::isfinite(expected.config.fallbackScaleCp) && expected.config.fallbackScaleCp >= 1 &&
           expected.config.fallbackScaleCp <= 10000 &&
           (expected.source == ConversionSource::NativeWdl ||
            expected.source == ConversionSource::ApproximateCentipawns ||
            expected.source == ConversionSource::Terminal);
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
    if (deep.issue || verified.issue || !supportsClassificationModel(deep.modelVersion) ||
        !supportsClassificationModel(verified.modelVersion) ||
        deep.modelVersion != verified.modelVersion || deep.mover != deepPair.mover ||
        verified.mover != verifiedPair.mover || deep.mover != verified.mover || !deep.bestMove ||
        !verified.bestMove || !deep.label || !verified.label ||
        *deep.label < Classification::Best || *deep.label > Classification::Blunder ||
        *verified.label < Classification::Best || *verified.label > Classification::Blunder ||
        !validExpected(deep.before, deep.mover) || !validExpected(deep.after, deep.mover) ||
        !validExpected(verified.before, verified.mover) ||
        !validExpected(verified.after, verified.mover)) {
        return out;
    }
    if (deep.label != verified.label || deep.bestMove != verified.bestMove ||
        deep.thresholds != verified.thresholds ||
        deep.before.expected->source != verified.before.expected->source ||
        deep.after.expected->source != verified.after.expected->source ||
        deep.before.expected->config != deep.after.expected->config ||
        deep.before.expected->config != verified.before.expected->config ||
        deep.before.expected->config != verified.after.expected->config) {
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
