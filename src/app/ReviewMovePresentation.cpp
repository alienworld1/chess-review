#include "app/ReviewMovePresentation.h"
#include <QObject>
#include <QStringList>
#include <cmath>
namespace chess_review::app {
namespace {
QString evidenceReason(review::EvidenceIssue issue) {
    using review::EvidenceIssue;
    switch (issue) {
    case EvidenceIssue::Invalid:
        return QObject::tr("Evidence is invalid or its candidate lines are inconsistent.");
    case EvidenceIssue::Incomplete:
        return QObject::tr("Analysis has insufficient measured strength or candidate coverage.");
    case EvidenceIssue::Bounded:
        return QObject::tr("A score is a bound, rather than an exact estimate.");
    case EvidenceIssue::MateClaim:
        return QObject::tr("Engine mate claims need further verification.");
    case EvidenceIssue::MissingEvaluation:
        return QObject::tr("An evaluation is missing.");
    case EvidenceIssue::ContextMismatch:
        return QObject::tr("The position histories do not match.");
    case EvidenceIssue::IncompatibleEngine:
        return QObject::tr("The engine identities do not match.");
    case EvidenceIssue::IncompatibleModel:
        return QObject::tr("The evidence model is unsupported.");
    case EvidenceIssue::MixedConversion:
        return QObject::tr("Before and after scores use incompatible conversion methods.");
    case EvidenceIssue::NegativeLoss:
        return QObject::tr(
            "The later search improves the mover's expected score; loss is withheld.");
    case EvidenceIssue::RestrictedSearch:
        return QObject::tr("The search restricted the available moves.");
    case EvidenceIssue::ContradictoryCandidates:
        return QObject::tr("Candidate ordering contradicts the scores.");
    }
    return QObject::tr("Evidence is unavailable.");
}
QString expected(const review::EvaluationResult& evaluation) {
    if (!evaluation.expected) {
        return QObject::tr("Unavailable");
    }
    const auto& e = *evaluation.expected;
    QString source;
    switch (e.source) {
    case review::ConversionSource::NativeWdl:
        source = QObject::tr("engine WDL");
        break;
    case review::ConversionSource::ApproximateCentipawns:
        source = QObject::tr("approximate centipawn conversion, scale %1 cp")
                     .arg(e.config.fallbackScaleCp, 0, 'g', 10);
        break;
    case review::ConversionSource::Terminal:
        source = QObject::tr("locally proven terminal outcome");
        break;
    }
    return QObject::tr("%1% (%2; conversion model %3)")
        .arg(e.value * 100, 0, 'f', 3)
        .arg(source)
        .arg(e.modelVersion);
}
QString strength(const review::AnalysisStrength& s) {
    return QObject::tr("depth %1; nodes %2; time %3 ms")
        .arg(s.depth ? QString::number(*s.depth) : QObject::tr("unknown"),
             s.nodes ? QString::number(*s.nodes) : QObject::tr("unknown"),
             s.timeMs ? QString::number(*s.timeMs) : QObject::tr("unknown"));
}
QString raw(const review::RawEvaluation& value) {
    QStringList fields;
    if (value.score) {
        const auto& score = *value.score;
        const QString bound = score.bound == review::Bound::Exact   ? QObject::tr("exact")
                              : score.bound == review::Bound::Lower ? QObject::tr("lower bound")
                                                                    : QObject::tr("upper bound");
        fields << QObject::tr("%1 %2 (%3)")
                      .arg(QString::number(score.value),
                           score.kind == review::ScoreKind::Mate ? QObject::tr("mate distance")
                                                                 : QObject::tr("cp"),
                           bound);
    }
    if (value.wdl) {
        fields << QObject::tr("WDL %1/%2/%3")
                      .arg(value.wdl->wins)
                      .arg(value.wdl->draws)
                      .arg(value.wdl->losses);
    }
    return QObject::tr("%1 to move: %2")
        .arg(value.perspective == chess::Color::White ? QObject::tr("White") : QObject::tr("Black"),
             fields.isEmpty() ? QObject::tr("no raw evaluation")
                              : fields.join(QStringLiteral("; ")));
}
} // namespace
QString classificationText(const std::optional<review::ClassificationResult>& result) {
    if (!result) {
        return QObject::tr("Awaiting evidence");
    }
    if (!result->label) {
        return QObject::tr("Unclassified");
    }
    using review::Classification;
    switch (*result->label) {
    case Classification::Best:
        return QObject::tr("Best");
    case Classification::Excellent:
        return QObject::tr("Excellent");
    case Classification::Good:
        return QObject::tr("Good");
    case Classification::Inaccuracy:
        return QObject::tr("Inaccuracy");
    case Classification::Mistake:
        return QObject::tr("Mistake");
    case Classification::Blunder:
        return QObject::tr("Blunder");
    }
    return {};
}
ReviewEvaluation reviewPositionEvaluation(const persistence::ReviewMove* row, bool initial) {
    ReviewEvaluation result;
    result.description = QObject::tr("White expected score: unavailable");
    if (!row || !row->classification) {
        return result;
    }
    const auto& evaluation = initial ? row->classification->before : row->classification->after;
    if (evaluation.issue || !evaluation.expected) {
        return result;
    }
    const auto& value = *evaluation.expected;
    if (!std::isfinite(value.value) || value.value < 0 || value.value > 1 ||
        value.modelVersion != review::expectedScoreModelVersion) {
        return result;
    }
    result.whiteExpected = value.perspective == chess::Color::White ? value.value : 1 - value.value;
    result.approximate = value.source == review::ConversionSource::ApproximateCentipawns;
    result.description =
        QObject::tr("White expected score: %1% · %2 · model %3")
            .arg(*result.whiteExpected * 100, 0, 'f', 1)
            .arg(result.approximate ? QObject::tr("approximate centipawn conversion")
                 : value.source == review::ConversionSource::Terminal
                     ? QObject::tr("locally proven terminal outcome")
                     : QObject::tr("engine WDL"))
            .arg(value.modelVersion);
    return result;
}
QString reviewMoveMarker(const persistence::ReviewMove* move) {
    if (!move || !move->classification) {
        return QObject::tr("Awaiting evidence");
    }
    QString glyph;
    if (move->classification->label) {
        switch (*move->classification->label) {
        case review::Classification::Best:
            glyph = QStringLiteral("★ ");
            break;
        case review::Classification::Excellent:
            glyph = QStringLiteral("E ");
            break;
        case review::Classification::Good:
            glyph = QStringLiteral("✓ ");
            break;
        case review::Classification::Inaccuracy:
            glyph = QStringLiteral("?! ");
            break;
        case review::Classification::Mistake:
            glyph = QStringLiteral("? ");
            break;
        case review::Classification::Blunder:
            glyph = QStringLiteral("?? ");
            break;
        }
    }
    return glyph + classificationText(move->classification) +
           (move->classification->label
                ? (move->classification->stable ? QObject::tr(" · stable")
                                                : QObject::tr(" · provisional"))
                : QString{});
}
QString reviewMomentHeadline(const persistence::ReviewMove* move, int position) {
    if (position == 0) {
        return QObject::tr("Initial position");
    }
    if (!move) {
        return QObject::tr("Position %1 — awaiting evidence").arg(position);
    }
    return QObject::tr("%1%2 %3 — %4")
        .arg(move->moveNumber)
        .arg(move->mover == chess::Color::White ? QStringLiteral(".") : QStringLiteral("..."),
             move->san, reviewMoveMarker(move));
}
QString reviewMomentExplanation(const persistence::ReviewMove* move, int position) {
    if (position == 0) {
        return QObject::tr("Explore the full game or continue to a key moment. The board shows "
                           "each position after the played move.");
    }
    if (!move || !move->classification) {
        return QObject::tr("Saved evidence is not available for this move yet. You can continue "
                           "navigating the full game.");
    }
    const auto& result = *move->classification;
    QStringList lines;
    if (result.loss) {
        lines << QObject::tr("This move reduced %1's expected score by %2 percentage points.")
                     .arg(move->mover == chess::Color::White ? QObject::tr("White")
                                                             : QObject::tr("Black"))
                     .arg(*result.loss * 100, 0, 'f', 1);
    } else if (result.evidenceIssue) {
        lines << evidenceReason(*result.evidenceIssue);
    } else {
        lines << QObject::tr(
            "A reliable numeric impact is unavailable. Inspect the saved evidence for details.");
    }
    lines << (result.stable ? QObject::tr("Two stronger passes agree on this result.")
                            : QObject::tr("Provisional: stability has not been established."));
    if (move->contextPosition) {
        lines << QObject::tr("Context: the opponent's preceding error at position %1.")
                     .arg(*move->contextPosition);
    }
    return lines.join(QLatin1Char('\n'));
}
QString reviewOverviewText(const persistence::ReviewOverview& overview, chess::Color selectedSide) {
    QStringList text;
    text << QObject::tr("Game summary · accuracy model %1 · expected-score evidence")
                .arg(review::accuracyModelVersion);
    text << QObject::tr("Recorded result: %1").arg(overview.result);
    if (overview.sides[0].total + overview.sides[1].total > 0 &&
        overview.sides[0].evaluated + overview.sides[1].evaluated == 0) {
        text << QObject::tr("No moves could be evaluated from the saved evidence. Finished "
                            "processing does not establish usable accuracy or classifications. "
                            "Inspect a move's evidence reason; Run Review collects a new run.");
    }
    for (std::size_t index = 0; index < 2; ++index) {
        const auto& side = overview.sides[index];
        text << QObject::tr("%1%2: accuracy %3 · evaluated %4/%5 · stable %6/%5")
                    .arg(index == 0 ? QObject::tr("White") : QObject::tr("Black"))
                    .arg(index == static_cast<std::size_t>(selectedSide)
                             ? QObject::tr(" (selected player)")
                             : QString{})
                    .arg(side.accuracy ? QString::number(*side.accuracy, 'f', 0) + QLatin1Char('%')
                                       : QObject::tr("unavailable"))
                    .arg(side.evaluated)
                    .arg(side.total)
                    .arg(side.stable);
        QStringList counts;
        for (std::size_t label = 0; label < side.counts.size(); ++label) {
            review::ClassificationResult classification;
            classification.label = static_cast<review::Classification>(label);
            counts << QObject::tr("%1: %2")
                          .arg(classificationText(classification))
                          .arg(side.counts[label]);
        }
        text << counts.join(QStringLiteral(" · "));
        text << QObject::tr("Withheld: %1 · unprocessed: %2%3")
                    .arg(side.withheld)
                    .arg(side.unprocessed)
                    .arg(side.stable < side.total ? QObject::tr(" · provisional summary")
                                                  : QString{});
    }
    text << QObject::tr("%1 chronological key moments for the selected player.")
                .arg(overview.totalKeyMoments);
    if (overview.largestLoss) {
        text << QObject::tr("Largest measured loss: %1 percentage points at position %2.")
                    .arg(*overview.largestLoss * 100, 0, 'f', 1)
                    .arg(*overview.largestLossPosition);
    }
    text << QObject::tr("Accuracy summarizes the available move-quality evidence; it is not a "
                        "measure of playing strength. Missing evidence is excluded.");
    return text.join(QLatin1Char('\n'));
}
QString reviewMoveDetails(const persistence::ReviewMove& move) {
    QStringList text;
    text << QObject::tr("%1%2 %3 — %4")
                .arg(move.moveNumber)
                .arg(move.mover == chess::Color::White ? QStringLiteral(".")
                                                       : QStringLiteral("..."),
                     move.san, classificationText(move.classification));
    if (!move.classification) {
        text << QObject::tr(
            "Both adjacent positions need saved evidence before this move can be classified.");
        return text.join(QLatin1Char('\n'));
    }
    const auto& r = *move.classification;
    text << (r.stable ? QObject::tr("Stable — two stronger passes agree.")
                      : QObject::tr("Provisional — stability has not been established."));
    switch (move.stability.reason) {
    case review::StabilityReason::Unselected:
        text << QObject::tr("Not selected for targeted refinement.");
        break;
    case review::StabilityReason::Pending:
        text << QObject::tr("Stronger evidence is pending.");
        break;
    case review::StabilityReason::Insufficient:
        text << QObject::tr("Stronger evidence does not meet stability requirements.");
        break;
    case review::StabilityReason::Disagreement:
        text << QObject::tr("Stronger passes disagree on the label or best move.");
        break;
    case review::StabilityReason::ScoreDrift:
        text << QObject::tr("Expected scores changed beyond the permitted drift.");
        break;
    case review::StabilityReason::Confirmed:
        break;
    }
    text << QObject::tr("Evidence stage: %1; stability model: %2")
                .arg(static_cast<int>(move.stage))
                .arg(move.stability.modelVersion);
    for (std::size_t i = 0; i < move.stabilityEvidenceIds.size(); ++i) {
        if (move.stabilityEvidenceIds[i] > 0) {
            const auto& strength = move.stabilityStrength[i];
            text << QObject::tr("Stability proof %1: evidence %2 · nodes %3 · depth %4")
                        .arg(static_cast<qulonglong>(i + 1))
                        .arg(move.stabilityEvidenceIds[i])
                        .arg(strength.nodes ? QString::number(*strength.nodes)
                                            : QObject::tr("terminal"))
                        .arg(strength.depth ? QString::number(*strength.depth)
                                            : QObject::tr("terminal"));
        }
    }
    if (move.stability.persistentMate) {
        text << QObject::tr("Both stronger passes report a mate for the same side. Numeric "
                            "interpretation remains withheld.");
    }
    if (move.contextPosition) {
        text << QObject::tr("Opponent error context: preceding position %1.")
                    .arg(*move.contextPosition);
    }
    if (r.issue == review::ClassificationIssue::BestMoveConflict) {
        text << QObject::tr(
            "The earlier search ranked this move first, but the later search crosses an error "
            "threshold. Classification withheld because the searches disagree.");
    } else if (r.evidenceIssue) {
        text << evidenceReason(*r.evidenceIssue);
    } else if (r.issue) {
        text << QObject::tr("Classification configuration is invalid.");
    }
    text << QObject::tr("Expected scores from the mover's perspective (%1):")
                .arg(move.mover == chess::Color::White ? QObject::tr("White")
                                                       : QObject::tr("Black"));
    text << QObject::tr("Before: %1").arg(expected(r.before));
    text << QObject::tr("After: %1").arg(expected(r.after));
    text << QObject::tr("Loss: %1")
                .arg(r.loss ? QObject::tr("%1 percentage points").arg(*r.loss * 100, 0, 'f', 3)
                            : QObject::tr("withheld"));
    if (r.signedDelta && !r.loss) {
        text << QObject::tr("Signed before − after: %1 percentage points")
                    .arg(*r.signedDelta * 100, 0, 'f', 3);
    }
    text << QObject::tr("Best verified candidate: %1; played rank: %2")
                .arg(move.bestSan.isEmpty() ? QObject::tr("unavailable") : move.bestSan,
                     r.playedMoveRank ? QString::number(*r.playedMoveRank)
                                      : QObject::tr("unknown"));
    if (r.alternatives.empty()) {
        text << QObject::tr("Equivalent alternatives: not assessed.");
    }
    for (std::size_t i = 0; i < r.alternatives.size(); ++i) {
        const auto& a = r.alternatives[i];
        text << QObject::tr("Alternative %1 (rank %2): gap %3 percentage points; %4")
                    .arg(move.alternativeSans[i])
                    .arg(a.rank)
                    .arg(a.gap * 100, 0, 'f', 3)
                    .arg(a.nearlyEquivalent ? QObject::tr("nearly equivalent")
                                            : QObject::tr("outside equivalence band"));
    }
    text << QObject::tr("Thresholds (percentage points): Excellent ≤ %1; Inaccuracy ≥ %2; Mistake "
                        "≥ %3; Blunder ≥ %4. Error bands take priority; Best requires verified "
                        "rank 1 below Inaccuracy.")
                .arg(r.thresholds.excellentLoss * 100, 0, 'g', 10)
                .arg(r.thresholds.inaccuracyLoss * 100, 0, 'g', 10)
                .arg(r.thresholds.mistakeLoss * 100, 0, 'g', 10)
                .arg(r.thresholds.blunderLoss * 100, 0, 'g', 10);
    text << QObject::tr("Rule: %1; classification model %2")
                .arg(QString::fromStdString(r.ruleId))
                .arg(r.modelVersion);
    if (r.modelVersion == 1) {
        text << QObject::tr("Saved model 1 retains its original floating-point boundary rules; new "
                            "classifications use model 2.");
    } else if (r.before.expected &&
               r.before.expected->source == review::ConversionSource::NativeWdl &&
               r.after.expected &&
               r.after.expected->source != review::ConversionSource::ApproximateCentipawns) {
        text << QObject::tr(
            "Loss and candidate gaps are calculated from exact WDL counts before conversion.");
    }
    text << QObject::tr("Raw before — %1").arg(raw(move.rawBefore));
    text << QObject::tr("Raw after — %1").arg(raw(move.rawAfter));
    text << QObject::tr("Before strength: %1").arg(strength(move.beforeStrength));
    text << QObject::tr("After strength: %1").arg(strength(move.afterStrength));
    text << QObject::tr("Evidence IDs: %1 → %2")
                .arg(move.beforeEvidenceId)
                .arg(move.afterEvidenceId);
    text << QObject::tr("Engine identity: %1").arg(move.engineIdentity);
    text << QObject::tr("Before session / attempt: %1 / %2")
                .arg(move.beforeSession, move.beforeAttempt);
    text << QObject::tr("After session / attempt: %1 / %2")
                .arg(move.afterSession, move.afterAttempt);
    return text.join(QLatin1Char('\n'));
}
} // namespace chess_review::app
