#include "app/ReviewMovePresentation.h"
#include "review/Retry.h"
#include <QObject>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
namespace chess_review::app {
namespace {
QString pieceText(chess::PieceType type) {
    switch (type) {
    case chess::PieceType::Pawn:
        return QObject::tr("pawn");
    case chess::PieceType::Knight:
        return QObject::tr("knight");
    case chess::PieceType::Bishop:
        return QObject::tr("bishop");
    case chess::PieceType::Rook:
        return QObject::tr("rook");
    case chess::PieceType::Queen:
        return QObject::tr("queen");
    case chess::PieceType::King:
        return QObject::tr("king");
    }
    return {};
}
QString specialStatus(review::SpecialStatus status) {
    switch (status) {
    case review::SpecialStatus::NotCandidate:
        return QObject::tr("not selected");
    case review::SpecialStatus::Pending:
        return QObject::tr("verification pending");
    case review::SpecialStatus::Verified:
        return QObject::tr("verified");
    case review::SpecialStatus::Rejected:
        return QObject::tr("requirements not met");
    case review::SpecialStatus::Insufficient:
        return QObject::tr("insufficient evidence");
    }
    return {};
}
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
QString refinementReason(review::RefinementReason reason, std::optional<std::uint32_t> policy) {
    using review::RefinementReason;
    switch (reason) {
    case RefinementReason::MajorLoss:
        return QObject::tr("a large expected-score loss");
    case RefinementReason::ThresholdProximity:
        return QObject::tr("loss near a classification threshold");
    case RefinementReason::MateSignal:
        return QObject::tr("an engine mate signal");
    case RefinementReason::SearchConflict:
        return QObject::tr("conflicting search estimates");
    case RefinementReason::InsufficientStrength:
        return QObject::tr("insufficient measured strength or candidate coverage");
    case RefinementReason::MissingEvaluation:
        return QObject::tr("a missing evaluation");
    case RefinementReason::BoundedEvaluation:
        return QObject::tr("a bounded evaluation");
    case RefinementReason::ExpectedScoreLoss:
        return policy == 3
                   ? QObject::tr(
                         "expected-score loss selected by the recorded policy-3 rounded cutoff")
                   : QObject::tr("expected-score loss at or above the Inaccuracy threshold");
    case RefinementReason::OpportunityReply:
        return QObject::tr(
            "a reply selected to investigate the preceding move's expected-score loss");
    case RefinementReason::TacticalOpportunity:
        return QObject::tr("a concrete tactical opportunity in the saved line");
    case RefinementReason::SacrificeSignal:
        return QObject::tr("a possible tactical sacrifice");
    case RefinementReason::UniqueResource:
        return QObject::tr(
            "a check evasion or constrained choice needing alternative verification");
    case RefinementReason::MixedConversion:
        return QObject::tr("incompatible score conversion methods");
    }
    return {};
}
QString scanDetails(const review::ScanAssessment& scan) {
    QStringList text;
    if (scan.pipelineVersion) {
        text << QObject::tr("Selection policy: %1 (captured for this review).")
                    .arg(*scan.pipelineVersion);
    }
    switch (scan.state) {
    case review::ScanState::AwaitingEvidence:
        text << QObject::tr("Original quick scan: awaiting adjacent saved evidence.");
        break;
    case review::ScanState::Usable:
        text << QObject::tr("Original quick scan: usable evaluation; this single pass did not "
                            "establish stability.");
        break;
    case review::ScanState::Unusable:
        text << QObject::tr(
            "Original quick scan: evidence retained; a usable numeric evaluation was withheld.");
        break;
    case review::ScanState::Invalid:
        text << QObject::tr(
            "Original quick scan: invalid evidence or policy; automatic repair is unavailable.");
        break;
    }
    if (scan.issue) {
        text << evidenceReason(*scan.issue);
    }
    if (scan.signedDelta) {
        text << QObject::tr("Quick-scan expected-score loss from the mover's perspective: %1 "
                            "percentage points.")
                    .arg(*scan.signedDelta * 100, 0, 'f', 3);
    }
    if (scan.opportunityPosition && scan.opportunityLoss) {
        text << QObject::tr("The preceding move (ply %1) lost %2 expected-score percentage points "
                            "in the original quick scan. This is an investigation signal, not a "
                            "verified missed chance.")
                    .arg(*scan.opportunityPosition)
                    .arg(*scan.opportunityLoss * 100, 0, 'f', 3);
    }
    if (scan.needsRefinement()) {
        QStringList reasons;
        for (const auto reason : scan.reasons) {
            reasons << refinementReason(reason, scan.pipelineVersion);
        }
        text << QObject::tr("Quick-scan refinement reasons: %1.")
                    .arg(reasons.join(QStringLiteral("; ")));
        text << QObject::tr("These describe the original scan. Stronger evidence may resolve them; "
                            "unresolved evidence stays provisional or unclassified.");
    }
    return text.join(QLatin1Char('\n'));
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
    case Classification::Brilliant:
        return QObject::tr("Brilliant");
    case Classification::Great:
        return QObject::tr("Great");
    case Classification::Miss:
        return QObject::tr("Miss");
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
    if (row->pendingRevision) {
        result.description += QObject::tr(" · previous checkpoint; updating");
    }
    return result;
}
std::vector<BoardAnnotation> reviewTacticalAnnotations(const persistence::ReviewMove& move) {
    std::vector<BoardAnnotation> annotations;
    for (const auto& fact : move.tacticalFacts) {
        if (fact.source != review::FactSource::Played || fact.linePlies != 1 ||
            fact.kind != review::TacticalKind::Fork) {
            continue;
        }
        annotations.push_back({fact.square, fact.square, 3});
        for (const auto target : fact.targets) {
            annotations.push_back({fact.square, target, 3});
        }
        break;
    }
    return annotations;
}
QString reviewMoveMarker(const persistence::ReviewMove* move) {
    if (!move || !move->classification) {
        return QObject::tr("Awaiting evidence");
    }
    QString glyph;
    if (move->classification->label) {
        switch (*move->classification->label) {
        case review::Classification::Brilliant:
            glyph = QStringLiteral("!! ");
            break;
        case review::Classification::Great:
            glyph = QStringLiteral("! ");
            break;
        case review::Classification::Miss:
            glyph = QStringLiteral("× ");
            break;
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
    if (move->pendingRevision) {
        return glyph + classificationText(move->classification) +
               QObject::tr(" · previous checkpoint; updating");
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
    if (move->pendingRevision) {
        lines << QObject::tr("Showing the previous saved checkpoint while revision %1 loads.")
                     .arg(*move->pendingRevision);
    }
    if (move->stability.persistentMate) {
        lines << QObject::tr(
            "Independent stronger engine searches report a mating outcome. This is an engine "
            "prediction, not an exhaustive proof against every defence.");
    }
    std::vector<const review::TacticalFact*> teachingFacts;
    for (const auto& fact : move->tacticalFacts) {
        if (fact.source == review::FactSource::Best && result.playedMoveRank == 1) {
            continue;
        }
        teachingFacts.push_back(&fact);
    }
    const auto priority = [&](const review::TacticalFact* fact) {
        const bool missed = result.label == review::Classification::Miss;
        const int source = (fact->source == review::FactSource::Best) == missed ? 0 : 1;
        int kind = 6;
        switch (fact->kind) {
        case review::TacticalKind::Sacrifice:
            kind = 0;
            break;
        case review::TacticalKind::Fork:
            kind = 1;
            break;
        case review::TacticalKind::Checkmate:
            kind = 2;
            break;
        case review::TacticalKind::Promotion:
            kind = 3;
            break;
        case review::TacticalKind::Exchange:
            kind = 4;
            break;
        case review::TacticalKind::Material:
            kind = 5;
            break;
        case review::TacticalKind::HangingPiece:
            kind = 6;
            break;
        }
        return std::pair{source, kind};
    };
    std::stable_sort(teachingFacts.begin(), teachingFacts.end(),
                     [&](const auto* a, const auto* b) { return priority(a) < priority(b); });
    for (const auto* teachingFact : teachingFacts) {
        const auto& fact = *teachingFact;
        const auto square = QString::fromStdString(fact.square.name());
        const auto context = fact.source == review::FactSource::Best
                                 ? QObject::tr("In the engine's saved candidate line: ")
                                 : QObject::tr("In the played move's line: ");
        const auto side =
            fact.beneficiary == chess::Color::White ? QObject::tr("White") : QObject::tr("Black");
        switch (fact.kind) {
        case review::TacticalKind::Material:
            lines << context +
                         QObject::tr(
                             "%1 gains %2 pawn units of material along the saved continuation.")
                             .arg(side)
                             .arg(fact.material);
            break;
        case review::TacticalKind::Exchange:
            lines << context + QObject::tr("%1 gains the exchange along the saved continuation.")
                                   .arg(side);
            break;
        case review::TacticalKind::HangingPiece:
            lines << context +
                         QObject::tr("An undefended piece on %1 can be captured.").arg(square);
            break;
        case review::TacticalKind::Fork: {
            QStringList targets;
            for (const auto target : fact.targets) {
                targets << QString::fromStdString(target.name());
            }
            lines << context +
                         QObject::tr("The %1 on %2 gives check and forks the pieces on %3.")
                             .arg(fact.modelVersion == 1 && fact.actor == chess::PieceType::Pawn
                                      ? QObject::tr("piece")
                                      : pieceText(fact.actor),
                                  square, targets.join(QStringLiteral(", ")));
            break;
        }
        case review::TacticalKind::Promotion:
            lines << context + QObject::tr("%1 promotes to a %2 on %3.")
                                   .arg(side, pieceText(fact.actor), square);
            break;
        case review::TacticalKind::Checkmate:
            lines << context + QObject::tr("The replayed position ends in verified checkmate on "
                                           "%1. Other defences are not proven by this line.")
                                   .arg(square);
            break;
        case review::TacticalKind::Sacrifice:
            lines << context + QObject::tr("%1 concedes %2 pawn units before the tactical "
                                           "resolution in this continuation.")
                                   .arg(side)
                                   .arg(fact.material);
            break;
        }
        if (fact.modelVersion >= 2 && !fact.retainedOpportunity &&
            (fact.kind == review::TacticalKind::Material ||
             fact.kind == review::TacticalKind::Exchange ||
             fact.kind == review::TacticalKind::Promotion)) {
            lines.back() +=
                QObject::tr(" This is a line consequence, not a verified winning opportunity.");
        }
        if (lines.size() >= 3) {
            break;
        }
    }
    if (move->special.status == review::SpecialStatus::Verified && result.stable &&
        result.label == move->special.label) {
        lines << QObject::tr("Special label verified by two independent stronger attempts. See the "
                             "evidence inspector for its rule.");
        if (move->special.modelVersion == 1) {
            lines << QObject::tr("Historical special-label model 1: a new review uses stricter "
                                 "consequence verification.");
        }
    } else if (move->special.needsEvidence() ||
               move->special.status == review::SpecialStatus::Verified) {
        lines << QObject::tr(
            "Special-label verification is incomplete; the ordinary classification is retained.");
    }
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
    if (result.bestMove && !result.issue && !move->bestSan.isEmpty()) {
        lines << QObject::tr("Best supported move: %1. Inspect its saved continuation.")
                     .arg(move->bestSan);
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
        const auto& scan = overview.quickScan[index];
        text << QObject::tr(
                    "%1 original quick-scan coverage: usable %2/%3 moves · retained gaps %4 · "
                    "awaiting %5 · invalid %6")
                    .arg(index == 0 ? QObject::tr("White") : QObject::tr("Black"))
                    .arg(scan.usable)
                    .arg(side.total)
                    .arg(scan.unusable)
                    .arg(scan.awaiting)
                    .arg(scan.invalid);
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
QString comparisonReasonText(review::ComparisonReason reason) {
    switch (reason) {
    case review::ComparisonReason::Pending:
        return QObject::tr("Awaiting independent comparison");
    case review::ComparisonReason::Confirmed:
        return QObject::tr("Comparison confirmed");
    case review::ComparisonReason::Coverage:
        return QObject::tr("Alternative equivalence not assessed: saved search width is one");
    case review::ComparisonReason::NoAlternative:
        return QObject::tr("No other legal move");
    case review::ComparisonReason::Insufficient:
        return QObject::tr("Insufficient comparable evidence");
    case review::ComparisonReason::MissingCandidate:
        return QObject::tr("Candidate missing from preceding pass");
    case review::ComparisonReason::Disagreement:
        return QObject::tr("Independent comparisons disagree");
    case review::ComparisonReason::ScoreDrift:
        return QObject::tr("Comparison changed beyond the drift limit");
    }
    return {};
}
QString reviewComparisonRow(const review::MoveComparison& row, const QString& san) {
    QStringList text;
    text << (row.played ? QObject::tr("Played: %1").arg(san) : san);
    if (row.gap) {
        text << QObject::tr("%1 percentage points below best").arg(*row.gap * 100, 0, 'f', 1);
    }
    if (row.equivalent) {
        text << (*row.equivalent ? QObject::tr("Equivalent quality") : QObject::tr("Weaker"));
    }
    if (row.reason != review::ComparisonReason::Coverage &&
        row.reason != review::ComparisonReason::NoAlternative) {
        text << (row.reason == review::ComparisonReason::Confirmed
                     ? QObject::tr("Confirmed")
                     : comparisonReasonText(row.reason));
    }
    if (row.expected && row.expected->source == review::ConversionSource::ApproximateCentipawns) {
        text << QObject::tr("Approximate");
    }
    if (row.source == review::ComparisonSource::PlayedChild) {
        text << QObject::tr("Saved after-move evaluation");
    }
    return text.join(QStringLiteral(" · "));
}
QString reviewMoveDetails(const persistence::ReviewMove& move) {
    QString specialDetails;
    if (move.pendingRevision) {
        specialDetails = QObject::tr("Previous saved checkpoint; revision %1 is loading.\n")
                             .arg(*move.pendingRevision);
    }
    if (!move.special.ruleId.empty()) {
        specialDetails += QObject::tr("Special verification model %1 · rule %2 · status %3\n")
                              .arg(move.special.modelVersion)
                              .arg(QString::fromStdString(move.special.ruleId))
                              .arg(specialStatus(move.special.status));
        const auto& c = move.special.thresholds;
        specialDetails += QObject::tr("Captured limits (percentage points): quality ≤%1; loss "
                                      "drift ≤%2; gap ≥%3; Miss loss ≥%4; viability %5; already "
                                      "winning %6; strict margin %7; sacrifice ≥%8 pawn units.\n")
                              .arg(c.maximumLoss * 100)
                              .arg(c.maximumLossDrift * 100)
                              .arg(c.alternativeGap * 100)
                              .arg(c.missLoss * 100)
                              .arg(c.viableScore * 100)
                              .arg(c.alreadyWinningScore * 100)
                              .arg(c.margin * 100)
                              .arg(c.sacrificeValue);
        if (move.special.alternativeGap) {
            specialDetails += QObject::tr("Best/second-best gap: %1 percentage points.\n")
                                  .arg(*move.special.alternativeGap * 100, 0, 'f', 3);
        }
    }

    QStringList text;
    text << comparisonReasonText(move.alternatives.reason);
    if (move.quickScan.pipelineVersion && *move.quickScan.pipelineVersion >= 6) {
        text << QObject::tr("Displayed comparison model: 2. Captured policy %1 plans comparison "
                            "work with model %2.")
                    .arg(*move.quickScan.pipelineVersion)
                    .arg(*move.quickScan.pipelineVersion == 6 ? 1 : 2);
    }
    text << QObject::tr("%1%2 %3 — %4")
                .arg(move.moveNumber)
                .arg(move.mover == chess::Color::White ? QStringLiteral(".")
                                                       : QStringLiteral("..."),
                     move.san, classificationText(move.classification));
    text << scanDetails(move.quickScan);
    if (!move.classification) {
        text << QObject::tr(
            "Both adjacent positions need saved evidence before this move can be classified.");
        return specialDetails + text.join(QLatin1Char('\n'));
    }
    const auto& r = *move.classification;
    text << (r.stable ? QObject::tr("Stable — two stronger passes agree.")
                      : QObject::tr("Provisional — stability has not been established."));
    if (move.quickScan.pipelineVersion >= 5 && move.refinementTarget) {
        text << QObject::tr("Adaptive deeper searches: round %1 of at most 2.")
                    .arg(move.escalationRound);
        const auto reason = [](review::StabilityReason value) {
            switch (value) {
            case review::StabilityReason::Disagreement:
                return QObject::tr("label or best-move disagreement");
            case review::StabilityReason::ThresholdProximity:
                return QObject::tr("loss near a classification threshold");
            case review::StabilityReason::ScoreDrift:
                return QObject::tr("expected-score drift");
            case review::StabilityReason::Pending:
                return QObject::tr("a newer adjacent pair is incomplete");
            default:
                return QObject::tr("insufficient comparable evidence");
            }
        };
        for (std::size_t round = 0; round < 2; ++round) {
            if (move.escalationEligible[round]) {
                text << QObject::tr("Round %1 investigation reason: %2.")
                            .arg(static_cast<qulonglong>(round + 1))
                            .arg(reason(move.escalationReasons[round]));
            }
        }
        switch (move.escalationStop) {
        case review::EscalationStop::Confirmed:
            text << QObject::tr("Stopped: two comparable passes confirm the result.");
            break;
        case review::EscalationStop::PersistentMate:
            text << QObject::tr("Stopped: a persistent mate signal is supported; numeric "
                                "interpretation remains withheld.");
            break;
        case review::EscalationStop::RoundLimit:
            text << QObject::tr("Stopped at the two-round limit. Further evidence is required; the "
                                "result remains unresolved.");
            break;
        case review::EscalationStop::BudgetCeiling:
            text << QObject::tr("Stopped at the node-budget ceiling. Stronger confirmation is "
                                "unavailable within this review budget.");
            break;
        case review::EscalationStop::Pending:
            text << QObject::tr("Further confirmation is required. Automatic work runs only while "
                                "this review is active; resume is explicit.");
            break;
        case review::EscalationStop::NotSelected:
            break;
        }
        if (move.stabilityEvidenceIds[2] > 0) {
            const auto label = [](std::optional<review::Classification> value) {
                review::ClassificationResult result;
                result.label = value;
                return classificationText(result);
            };
            text << QObject::tr("Compared stages %1 → %2: %3 → %4; best move %5.")
                        .arg(static_cast<int>(move.comparisonStages[0]))
                        .arg(static_cast<int>(move.comparisonStages[1]))
                        .arg(label(move.comparisonLabels[0]), label(move.comparisonLabels[1]),
                             !move.comparisonBestMoveChanged   ? QObject::tr("not comparable")
                             : *move.comparisonBestMoveChanged ? QObject::tr("changed")
                                                               : QObject::tr("unchanged"));
            const auto value = [](std::optional<double> score) {
                return score ? QObject::tr("%1%").arg(*score * 100, 0, 'f', 1)
                             : QObject::tr("unavailable");
            };
            text << QObject::tr("Mover expected scores: before %1 → %2; after %3 → %4.")
                        .arg(value(move.comparisonExpected[0]), value(move.comparisonExpected[2]),
                             value(move.comparisonExpected[1]), value(move.comparisonExpected[3]));
        }
    }
    switch (move.stability.reason) {
    case review::StabilityReason::Unselected:
        text << QObject::tr("Not selected for targeted refinement.");
        break;
    case review::StabilityReason::Pending:
        text << QObject::tr(
            "Stability requires a complete newer pair and comparable independent evidence.");
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
    case review::StabilityReason::ThresholdProximity:
        text << QObject::tr("The searches agree, but a loss remains too close to a classification "
                            "cutoff to confirm the label.");
        for (const auto& threshold : move.stability.thresholds) {
            text << QObject::tr("Observed losses: earlier %1, newer %2; cutoff: %3; "
                                "required clearance: > %4 (all in percentage points).")
                        .arg(threshold.losses[0] * 100, 0, 'g', 10)
                        .arg(threshold.losses[1] * 100, 0, 'g', 10)
                        .arg(threshold.cutoff * 100, 0, 'g', 10)
                        .arg(threshold.margin * 100, 0, 'g', 10);
        }
        text << QObject::tr("Additional searches may still leave the label provisional under "
                            "the captured margin.");
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
    text << QObject::tr("Generic explanation model %1").arg(review::explanationModelVersion);
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
    if (const auto accuracy = review::moveAccuracy(move.accuracyLoss)) {
        text << QObject::tr(
                    "Accuracy model 1: included; loss %1 percentage points; move accuracy %2. "
                    "Calculation uses unrounded saved evidence, independently of labels.")
                    .arg(*move.accuracyLoss * 100, 0, 'g', 17)
                    .arg(*accuracy, 0, 'g', 17);
    } else {
        text << QObject::tr(
            "Accuracy model 1: excluded; no usable numeric loss in the selected evidence pair.");
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
    return specialDetails + text.join(QLatin1Char('\n'));
}
} // namespace chess_review::app
