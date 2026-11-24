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
bool sameMate(const std::optional<RawScore>& a, const std::optional<RawScore>& b);
bool sameEvaluation(const EvaluationResult& a, const EvaluationResult& b) {
    if (a.issue || b.issue || !a.expected || !b.expected) {
        return false;
    }
    const auto& x = *a.expected;
    const auto& y = *b.expected;
    return std::tie(x.value, x.perspective, x.source, x.modelVersion, x.config) ==
           std::tie(y.value, y.perspective, y.source, y.modelVersion, y.config);
}
bool consistentClassification(const ClassificationResult& result, const MoveEvidence& pair) {
    const bool validLoss =
        pair.loss && std::isfinite(*pair.loss) && *pair.loss >= 0 && *pair.loss <= 1;
    const bool deltaMatches =
        result.signedDelta.has_value() == pair.signedDelta.has_value() &&
        (!result.signedDelta || std::abs(*result.signedDelta - *pair.signedDelta) <= 1e-14);
    const bool consistentDelta =
        result.modelVersion >= 3
            ? (deltaMatches && (result.playedMoveRank == 1 ||
                                (result.loss && std::abs(*result.loss - *pair.loss) <= 1e-14)))
            : (result.signedDelta == result.loss && pair.signedDelta == pair.loss);
    return validExpected(pair.beforeEvaluation, pair.mover) &&
           validExpected(pair.afterEvaluation, pair.mover) &&
           sameEvaluation(result.before, pair.beforeEvaluation) &&
           sameEvaluation(result.after, pair.afterEvaluation) &&
           result.playedMoveRank == pair.playedMoveRank && validLoss && consistentDelta &&
           !pair.before.candidates.empty() &&
           !pair.before.candidates.front().continuation.empty() &&
           result.bestMove == pair.before.candidates.front().continuation.front();
}
bool validThresholds(const ClassificationConfig& config) {
    return std::isfinite(config.excellentLoss) && config.excellentLoss > 0 &&
           config.excellentLoss < 1 && std::isfinite(config.inaccuracyLoss) &&
           std::isfinite(config.mistakeLoss) && std::isfinite(config.blunderLoss) &&
           config.inaccuracyLoss > 0 && config.inaccuracyLoss < config.mistakeLoss &&
           config.mistakeLoss < config.blunderLoss && config.blunderLoss < 1;
}
// Best takes precedence below Inaccuracy. Excellent is reachable only below Inaccuracy.
std::array<std::optional<double>, 4> applicableBoundaries(const ClassificationResult& result) {
    const auto& c = result.thresholds;
    if (result.playedMoveRank == 1) {
        if (result.modelVersion >= 3) {
            return {std::nullopt, std::nullopt, std::nullopt, std::nullopt};
        }
        return {c.inaccuracyLoss, std::nullopt, std::nullopt, std::nullopt};
    }
    return {c.inaccuracyLoss, c.mistakeLoss, c.blunderLoss,
            c.excellentLoss < c.inaccuracyLoss ? std::optional(c.excellentLoss) : std::nullopt};
}
bool nearBoundary(double loss, double boundary, double margin, bool native,
                  std::uint32_t modelVersion) {
    // Canonical half-permille cutoffs have exact integer distances. Compare the
    // converted distance directly with the margin so adjacent margins stay distinct.
    const double boundaryTicks = std::round(boundary * 2000);
    if (native && boundaryTicks / 2000 == boundary) {
        return std::abs(std::round(loss * 2000) - boundaryTicks) / 2000 <= margin;
    }
    if (modelVersion == 2) {
        // Preserve the captured policy-8 predicate; long double precision varies by platform.
        return std::abs(static_cast<long double>(loss) - static_cast<long double>(boundary)) <=
               static_cast<long double>(margin);
    }
    // Model 3 uses canonical double subtraction on every supported platform.
    return std::abs(loss - boundary) <= margin;
}
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
    const bool signedDeltaMatches =
        result.signedDelta.has_value() == pair.signedDelta.has_value() &&
        (!result.signedDelta || std::abs(*result.signedDelta - *pair.signedDelta) <= 1e-14);
    const bool inconsistentLoss =
        result.modelVersion >= 3
            ? ((result.loss &&
                (!expectedLoss || (result.playedMoveRank != 1 && *result.loss != *expectedLoss))) ||
               !signedDeltaMatches)
            : ((result.loss &&
                (result.loss != expectedLoss || result.signedDelta != result.loss)) ||
               (pair.loss && pair.signedDelta != pair.loss));
    if ((pipelineVersion >= 8 && !validThresholds(result.thresholds)) ||
        !supportsPipelineModel(pipelineVersion) ||
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
    if (pipelineVersion >= 14 && result.playedMoveRank == 1 && result.signedDelta &&
        *result.signedDelta >= result.thresholds.inaccuracyLoss &&
        std::find(scan.reasons.begin(), scan.reasons.end(), RefinementReason::SearchConflict) ==
            scan.reasons.end()) {
        scan.reasons.push_back(RefinementReason::SearchConflict);
    }
    if (pipelineVersion >= 13) {
        scan.repeatedMateSignal = sameMate(pair.mate.before, pair.mate.after);
    }
    if (pipelineVersion >= 3) {
        if (scan.state == ScanState::Usable) {
            double measuredTicks = *scan.loss * 2000;
            const bool exactSource =
                (pair.beforeEvaluation.expected->source == ConversionSource::NativeWdl ||
                 pair.beforeEvaluation.expected->source == ConversionSource::Terminal) &&
                (pair.afterEvaluation.expected->source == ConversionSource::NativeWdl ||
                 pair.afterEvaluation.expected->source == ConversionSource::Terminal);
            if (exactSource) {
                const auto before = ticks(pair.before, pair.mover);
                const auto after = ticks(pair.after, pair.mover);
                if (!before || !after || (*before < *after && pipelineVersion < 14)) {
                    scan.state = ScanState::Invalid;
                    scan.reasons.clear();
                    scan.loss.reset();
                    scan.signedDelta.reset();
                    return scan;
                }
                measuredTicks = static_cast<double>(std::max(0, *before - *after));
            }
            // Keep released policy 3's decisions reproducible. Scaling adjacent floating
            // values can merge them; policy 4 compares the canonical loss directly.
            const double measuredLoss = !exactSource ? *scan.loss : measuredTicks / 2000;
            const bool qualifies = pipelineVersion == 3
                                       ? measuredTicks >= result.thresholds.inaccuracyLoss * 2000
                                       : measuredLoss >= result.thresholds.inaccuracyLoss;
            if (qualifies) {
                scan.reasons.push_back(RefinementReason::ExpectedScoreLoss);
                scan.qualifyingLoss = pipelineVersion == 3 ? measuredTicks / 2000 : measuredLoss;
            }
            const auto boundaries =
                pipelineVersion >= 8
                    ? applicableBoundaries(result)
                    : std::array<std::optional<double>, 4>{
                          result.thresholds.inaccuracyLoss, result.thresholds.mistakeLoss,
                          result.thresholds.blunderLoss, std::nullopt};
            for (const auto boundary : boundaries) {
                if (!boundary) {
                    continue;
                }
                if (pipelineVersion >= 14 && measuredLoss == 0.0 &&
                    *boundary == result.thresholds.excellentLoss) {
                    continue;
                }
                if (pipelineVersion >= 8
                        ? nearBoundary(measuredLoss, *boundary, config.boundaryMargin, exactSource,
                                       pipelineVersion == 8 ? 2 : 3)
                        : std::abs(measuredTicks - *boundary * 2000) <=
                              config.boundaryMargin * 2000) {
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
void addOpeningDeparture(ScanAssessment& scan, bool firstOutsideBook) {
    if (firstOutsideBook && scan.qualifyingLoss) {
        scan.reasons.push_back(RefinementReason::OpeningDeparture);
    }
}
RefinementQueueResult refinementQueue(std::span<const RefinementCandidate> candidates,
                                      std::uint32_t pipelineVersion, Quality quality) {
    if (!supportsPipelineModel(pipelineVersion)) {
        return RefinementQueueIssue::UnsupportedPolicy;
    }
    const auto moveLimit = refinementMoveLimit(pipelineVersion, quality);
    std::vector<std::tuple<int, double, int>> ranked;
    const auto priority = [pipelineVersion,
                           bounded = moveLimit.has_value()](const RefinementCandidate& candidate) {
        const auto& scan = candidate.scan;
        const auto has = [&](RefinementReason reason) {
            return std::find(scan.reasons.begin(), scan.reasons.end(), reason) !=
                   scan.reasons.end();
        };
        int tier = 6;
        double loss = 0;
        if (pipelineVersion >= 14 && bounded) {
            if (has(RefinementReason::SacrificeSignal) ||
                (has(RefinementReason::MateSignal) && !scan.repeatedMateSignal)) {
                tier = 0;
            } else if (scan.qualifyingLoss) {
                tier = 1;
                loss = *scan.qualifyingLoss;
            } else if (has(RefinementReason::SearchConflict)) {
                tier = 2;
            } else if (has(RefinementReason::TacticalOpportunity) ||
                       has(RefinementReason::UniqueResource) || scan.opportunityLoss) {
                tier = 3;
                loss = scan.opportunityLoss.value_or(0.0);
            } else if (has(RefinementReason::InsufficientStrength) ||
                       has(RefinementReason::MissingEvaluation) ||
                       has(RefinementReason::BoundedEvaluation) ||
                       has(RefinementReason::MixedConversion)) {
                tier = 4;
            } else if (has(RefinementReason::ThresholdProximity)) {
                tier = 5;
            } else if (scan.repeatedMateSignal) {
                tier = 6;
            }
            return std::tuple{tier, -loss, candidate.positionIndex};
        }
        if (has(RefinementReason::MateSignal) && (!bounded || !scan.repeatedMateSignal)) {
            tier = 0;
        } else if (bounded && scan.qualifyingLoss) {
            tier = 1;
            loss = *scan.qualifyingLoss;
        } else if (has(RefinementReason::SearchConflict) &&
                   (!bounded || scan.issue != EvidenceIssue::NegativeLoss)) {
            tier = bounded ? 2 : 1;
        } else if (has(RefinementReason::TacticalOpportunity) ||
                   has(RefinementReason::SacrificeSignal) ||
                   has(RefinementReason::UniqueResource)) {
            tier = bounded ? 3 : 2;
        } else if (scan.qualifyingLoss) {
            tier = 3;
            loss = *scan.qualifyingLoss;
        } else if (scan.opportunityLoss) {
            tier = 4;
            loss = *scan.opportunityLoss;
        } else if (has(RefinementReason::InsufficientStrength) ||
                   has(RefinementReason::MissingEvaluation) ||
                   has(RefinementReason::BoundedEvaluation) ||
                   has(RefinementReason::MixedConversion)) {
            tier = 5;
        } else if (bounded &&
                   (scan.repeatedMateSignal || scan.issue == EvidenceIssue::NegativeLoss)) {
            tier = 7;
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
            const auto validDelta = [](std::optional<double> value) {
                return !value || (std::isfinite(*value) && std::abs(*value) <= 1);
            };
            const auto has = [&](RefinementReason reason) {
                return std::find(scan.reasons.begin(), scan.reasons.end(), reason) !=
                       scan.reasons.end();
            };
            if (scan.pipelineVersion != pipelineVersion ||
                (scan.state != ScanState::Usable && scan.state != ScanState::Unusable &&
                 scan.state != ScanState::AwaitingEvidence) ||
                !validLoss(scan.loss) ||
                (pipelineVersion >= 14 ? !validDelta(scan.signedDelta)
                                       : !validLoss(scan.signedDelta)) ||
                !validLoss(scan.qualifyingLoss) || !validLoss(scan.opportunityLoss) ||
                (scan.repeatedMateSignal &&
                 (pipelineVersion < 13 || !has(RefinementReason::MateSignal))) ||
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
                case RefinementReason::TacticalOpportunity:
                case RefinementReason::SacrificeSignal:
                case RefinementReason::UniqueResource:
                    if (pipelineVersion < 10) {
                        return RefinementQueueIssue::InvalidCandidate;
                    }
                    break;
                case RefinementReason::OpeningDeparture:
                    if (!scan.qualifyingLoss) {
                        return RefinementQueueIssue::InvalidCandidate;
                    }
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
    if (moveLimit && ranked.size() > *moveLimit) {
        ranked.resize(*moveLimit);
    }
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
bool stronger(const PositionEvidence& first, const PositionEvidence& second,
              bool allowEqualExactMate = false) {
    if (first.position != second.position) {
        return false;
    }
    if (first.terminal && second.terminal) {
        return first.terminal == second.terminal;
    }
    if (!first.complete || !second.complete || first.engineIdentity != second.engineIdentity ||
        first.attemptIdentity.empty() || second.attemptIdentity.empty() ||
        first.attemptIdentity == second.attemptIdentity || !first.strength.nodes ||
        !second.strength.nodes || !first.strength.depth || !second.strength.depth ||
        *second.strength.depth < *first.strength.depth) {
        return false;
    }
    if (*second.strength.nodes > *first.strength.nodes) {
        return true;
    }
    return allowEqualExactMate && sameMate(first.evaluation.score, second.evaluation.score) &&
           *first.strength.nodes > 0 && *second.strength.nodes > 0;
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
    if (first.source == ConversionSource::Mate && second.source == ConversionSource::Mate) {
        return std::abs(first.value - second.value);
    }
    if ((first.source == ConversionSource::NativeWdl ||
         first.source == ConversionSource::Terminal) &&
        (second.source == ConversionSource::NativeWdl ||
         second.source == ConversionSource::Terminal)) {
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
           expected.perspective == mover && supportsExpectedScoreModel(expected.modelVersion) &&
           std::isfinite(expected.value) && expected.value >= 0 && expected.value <= 1 &&
           std::isfinite(expected.config.fallbackScaleCp) && expected.config.fallbackScaleCp >= 1 &&
           expected.config.fallbackScaleCp <= 10000 &&
           (expected.source == ConversionSource::NativeWdl ||
            expected.source == ConversionSource::ApproximateCentipawns ||
            expected.source == ConversionSource::Terminal ||
            expected.source == ConversionSource::Mate);
}
} // namespace
std::optional<double> accuracyLoss(const MoveEvidence& pair) {
    if (pair.issue || !pair.loss || !pair.beforeEvaluation.expected ||
        !pair.afterEvaluation.expected) {
        return std::nullopt;
    }
    if ((pair.beforeEvaluation.expected->source == ConversionSource::NativeWdl ||
         pair.beforeEvaluation.expected->source == ConversionSource::Terminal) &&
        (pair.afterEvaluation.expected->source == ConversionSource::NativeWdl ||
         pair.afterEvaluation.expected->source == ConversionSource::Terminal)) {
        const auto before = ticks(pair.before, pair.mover);
        const auto after = ticks(pair.after, pair.mover);
        if (!before || !after) {
            return std::nullopt;
        }
        if (*before < *after) {
            return pair.beforeEvaluation.expected->modelVersion >= 2 ? std::optional{0.0}
                                                                     : std::nullopt;
        }
        return static_cast<double>(*before - *after) / 2000;
    }
    return pair.loss;
}
std::optional<std::uint64_t> escalationBudget(std::uint64_t verificationNodes, int round) {
    constexpr std::uint64_t ceiling = 1000000000000ULL;
    if (verificationNodes == 0 || verificationNodes > ceiling || round < 1 || round > 2) {
        return {};
    }
    auto previous = verificationNodes;
    for (int i = 1; i <= round; ++i) {
        const auto next = previous > ceiling / 2 ? ceiling : previous * 2;
        if (next == previous) {
            return {};
        }
        previous = next;
    }
    return previous;
}
std::optional<std::uint64_t> stageBudget(const NodeBudgets& budgets, EvidenceStage stage) {
    switch (stage) {
    case EvidenceStage::Quick:
        return budgets.quickScanNodes;
    case EvidenceStage::Deepening:
        return budgets.deepeningNodes;
    case EvidenceStage::Verification:
        return budgets.verificationNodes;
    case EvidenceStage::EscalationOne:
        return escalationBudget(budgets.verificationNodes, 1);
    case EvidenceStage::EscalationTwo:
        return escalationBudget(budgets.verificationNodes, 2);
    }
    return {};
}
bool needsEscalation(bool selected, bool complete, const StabilityAssessment& stability) {
    return selected && complete && !stability.stable && !stability.persistentMate;
}
EscalationStop escalationStop(bool selected, const StabilityAssessment& stability,
                              int completedRound, std::uint64_t verificationNodes) {
    if (!selected) {
        return EscalationStop::NotSelected;
    }
    if (stability.stable) {
        return EscalationStop::Confirmed;
    }
    if (stability.persistentMate) {
        return EscalationStop::PersistentMate;
    }
    if (completedRound >= 2) {
        return EscalationStop::RoundLimit;
    }
    if (!escalationBudget(verificationNodes, completedRound + 1)) {
        return EscalationStop::BudgetCeiling;
    }
    return EscalationStop::Pending;
}
StabilityAssessment assessStability(const ClassificationResult& deep,
                                    const ClassificationResult& verified,
                                    const MoveEvidence& deepPair, const MoveEvidence& verifiedPair,
                                    const RefinementConfig& config, std::uint32_t modelVersion,
                                    bool checkThresholds) {
    StabilityAssessment out;
    out.reason = StabilityReason::Insufficient;
    out.modelVersion = modelVersion;
    if (modelVersion != 1 && modelVersion != 2 && modelVersion != 3 && modelVersion != 4) {
        return out;
    }
    if (deepPair.mover != verifiedPair.mover || deepPair.playedMove != verifiedPair.playedMove ||
        !std::isfinite(config.maximumDrift) || config.maximumDrift < 0 || config.maximumDrift > 1) {
        return out;
    }
    if (!stronger(deepPair.before, verifiedPair.before, modelVersion >= 4) ||
        !stronger(deepPair.after, verifiedPair.after, modelVersion >= 4)) {
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
    if (modelVersion < 4 && (deep.before.expected->source != verified.before.expected->source ||
                             deep.after.expected->source != verified.after.expected->source)) {
        out.reason = StabilityReason::Disagreement;
        return out;
    }
    if (deep.label != verified.label || deep.bestMove != verified.bestMove ||
        deep.thresholds != verified.thresholds ||
        deep.before.expected->config != deep.after.expected->config ||
        deep.before.expected->config != verified.before.expected->config ||
        deep.before.expected->config != verified.after.expected->config) {
        out.reason = StabilityReason::Disagreement;
        return out;
    }
    if (modelVersion >= 4 && (((deepPair.mate.before || verifiedPair.mate.before) && !beforeMate) ||
                              ((deepPair.mate.after || verifiedPair.mate.after) && !afterMate))) {
        out.reason = StabilityReason::Disagreement;
        return out;
    }
    const bool stableLoss = modelVersion >= 4 && config.maximumDrift > 0.0 && deep.loss &&
                            verified.loss &&
                            std::abs(*verified.loss - *deep.loss) <= config.maximumDrift;
    const auto positionDriftLimit = [&](const ExpectedScore& firstExpected,
                                        const ExpectedScore& secondExpected) {
        if (stableLoss && firstExpected.source == ConversionSource::ApproximateCentipawns &&
            secondExpected.source == ConversionSource::ApproximateCentipawns) {
            return std::max(config.maximumDrift, 0.04);
        }
        return config.maximumDrift;
    };
    if (drift(deepPair.before, verifiedPair.before, *deep.before.expected,
              *verified.before.expected) >
            positionDriftLimit(*deep.before.expected, *verified.before.expected) ||
        drift(deepPair.after, verifiedPair.after, *deep.after.expected, *verified.after.expected) >
            positionDriftLimit(*deep.after.expected, *verified.after.expected)) {
        out.reason = StabilityReason::ScoreDrift;
        return out;
    }
    if (modelVersion >= 2 && checkThresholds) {
        const auto a = deep.modelVersion >= 3 ? deep.loss : accuracyLoss(deepPair);
        const auto b = verified.modelVersion >= 3 ? verified.loss : accuracyLoss(verifiedPair);
        const std::uint32_t requiredClassModel = modelVersion >= 4 ? 3U : 2U;
        if (!validThresholds(deep.thresholds) || !std::isfinite(config.boundaryMargin) ||
            config.boundaryMargin < 0 || config.boundaryMargin > 1 ||
            deep.modelVersion != requiredClassModel || !a || !b || !std::isfinite(*a) ||
            !std::isfinite(*b) || *a < 0 || *a > 1 || *b < 0 || *b > 1 || deep.loss != a ||
            verified.loss != b || !consistentClassification(deep, deepPair) ||
            !consistentClassification(verified, verifiedPair)) {
            return out;
        }
        // The union uses all non-best boundaries if either witness is non-best.
        // Threshold configurations already agree, so no allocation/deduplication is needed.
        const auto boundaries = applicableBoundaries(deep.playedMoveRank == 1 ? verified : deep);
        const bool nativeA = deep.before.expected->source == ConversionSource::NativeWdl ||
                             deep.before.expected->source == ConversionSource::Terminal;
        const bool nativeB = verified.before.expected->source == ConversionSource::NativeWdl ||
                             verified.before.expected->source == ConversionSource::Terminal;
        for (const auto boundary : boundaries) {
            if (!boundary) {
                continue;
            }
            if (modelVersion >= 4 && *a == 0.0 && *b == 0.0 &&
                *boundary == deep.thresholds.excellentLoss) {
                continue;
            }
            if (nearBoundary(*a, *boundary, config.boundaryMargin, nativeA, modelVersion) ||
                nearBoundary(*b, *boundary, config.boundaryMargin, nativeB, modelVersion)) {
                out.thresholds.push_back({*boundary, {*a, *b}, config.boundaryMargin});
            }
        }
        if (!out.thresholds.empty()) {
            out.reason = StabilityReason::ThresholdProximity;
            return out;
        }
    }
    out.reason = StabilityReason::Confirmed;
    out.stable = true;
    return out;
}
std::optional<double> moveAccuracy(std::optional<double> loss, std::uint32_t modelVersion) {
    if (!supportsAccuracyModel(modelVersion) || !loss || !std::isfinite(*loss) || *loss < 0 ||
        *loss > 1) {
        return std::nullopt;
    }
    if (*loss == 0) {
        return 100;
    }
    const double raw = 103.1668 * std::exp(-0.04354 * 100 * *loss) - 3.1669;
    return std::clamp(modelVersion >= 2 ? raw + 1.0 : raw, 0.0, 100.0);
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
std::optional<double> gameAccuracyWeighted(std::span<const double> moves,
                                           std::span<const double> weights) {
    if (moves.empty() || moves.size() != weights.size()) {
        return std::nullopt;
    }
    double weightedSum = 0;
    double weightTotal = 0;
    double reciprocals = 0;
    bool zero = false;
    for (std::size_t i = 0; i < moves.size(); ++i) {
        const double value = moves[i];
        const double weight = weights[i];
        if (!std::isfinite(value) || value < 0 || value > 100 || !std::isfinite(weight) ||
            weight <= 0) {
            return std::nullopt;
        }
        weightedSum += value * weight;
        weightTotal += weight;
        if (value == 0) {
            zero = true;
        } else {
            reciprocals += 1 / value;
        }
    }
    if (weightTotal <= 0) {
        return std::nullopt;
    }
    const auto count = static_cast<double>(moves.size());
    return (weightedSum / weightTotal + (zero ? 0 : count / reciprocals)) / 2;
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
    if (whiteWinPercents_.empty() && move.whiteExpectedBefore &&
        std::isfinite(*move.whiteExpectedBefore)) {
        whiteWinPercents_.push_back(*move.whiteExpectedBefore * 100.0);
    }
    if (move.whiteExpectedAfter && std::isfinite(*move.whiteExpectedAfter)) {
        whiteWinPercents_.push_back(*move.whiteExpectedAfter * 100.0);
    } else if (!whiteWinPercents_.empty()) {
        whiteWinPercents_.push_back(whiteWinPercents_.back());
    }
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
        if (const auto value = moveAccuracy(move.loss, accuracyModel_)) {
            ++side.evaluated;
            accuracies_[sideIndex].push_back(*value);
            if (!whiteWinPercents_.empty()) {
                accuracyWinIndices_[sideIndex].push_back(whiteWinPercents_.size() - 1);
            }
        }
    }
    const bool major = move.label && isKeyLabel(*move.label);
    const bool mateTransition = mateWinner(move.before) != mateWinner(move.after);
    KeyMomentSelection selection;
    if (bothSides_ || move.mover == selectedSide_) {
        if (move.loss && moveAccuracy(move.loss, accuracyModel_) &&
            (!facts_.largestLoss || *move.loss > *facts_.largestLoss)) {
            facts_.largestLoss = move.loss;
            facts_.largestLossPosition = move.positionIndex;
        }
        const bool instructiveDeparture = move.bookDeparture && move.label && isError(*move.label);
        if (major || mateTransition || instructiveDeparture) {
            selection.selected = true;
            ++facts_.keyMoments;
            if (!bothSides_ && previousOpponentError_) {
                selection.opponentContext = move.positionIndex - 1;
            }
        }
    }
    previousOpponentError_ = move.mover != selectedSide_ && move.label && isError(*move.label) &&
                             *move.label != Classification::Inaccuracy;
    return selection;
}
SummaryFacts SummaryBuilder::finish() const {
    auto result = facts_;
    if (accuracyModel_ >= 2 && whiteWinPercents_.size() >= 2) {
        const std::size_t totalPositions = whiteWinPercents_.size();
        const std::size_t windowSize = std::clamp<std::size_t>(totalPositions / 10, 2, 8);
        std::vector<double> padded(windowSize - 1, whiteWinPercents_.front());
        padded.insert(padded.end(), whiteWinPercents_.begin(), whiteWinPercents_.end());
        std::vector<double> plyWeights(totalPositions, 0.5);
        for (std::size_t idx = 0; idx < totalPositions; ++idx) {
            double mean = 0;
            for (std::size_t k = 0; k < windowSize; ++k) {
                mean += padded[idx + k];
            }
            mean /= static_cast<double>(windowSize);
            double var = 0;
            for (std::size_t k = 0; k < windowSize; ++k) {
                const double d = padded[idx + k] - mean;
                var += d * d;
            }
            const double stdev = std::sqrt(var / static_cast<double>(windowSize));
            plyWeights[idx] = std::clamp(stdev, 0.5, 12.0);
        }
        for (std::size_t side = 0; side < result.sides.size(); ++side) {
            if (accuracies_[side].empty()) {
                continue;
            }
            if (accuracyWinIndices_[side].size() == accuracies_[side].size()) {
                std::vector<double> weights;
                weights.reserve(accuracies_[side].size());
                for (const std::size_t posIdx : accuracyWinIndices_[side]) {
                    weights.push_back(plyWeights[posIdx]);
                }
                result.sides[side].accuracy = gameAccuracyWeighted(accuracies_[side], weights);
            } else {
                result.sides[side].accuracy = gameAccuracy(accuracies_[side]);
            }
        }
        return result;
    }
    for (std::size_t side = 0; side < result.sides.size(); ++side) {
        result.sides[side].accuracy = gameAccuracy(accuracies_[side]);
    }
    return result;
}
} // namespace chess_review::review
