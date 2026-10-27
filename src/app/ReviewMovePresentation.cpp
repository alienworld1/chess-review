#include "app/ReviewMovePresentation.h"
#include <QObject>
#include <QStringList>
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
    text << QObject::tr(
        "Provisional — targeted deepening and stability checks are not available yet.");
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
