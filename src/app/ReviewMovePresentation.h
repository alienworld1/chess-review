#pragma once
#include "app/BoardState.h"
#include "persistence/ReviewRepository.h"
#include <QString>
#include <optional>
namespace chess_review::app {
struct ReviewMomentPresentation {
    QString move;
    QString classification;
    QString symbol;
    QString explanation;
    QString availability;
    QString alternative;
    QString impact;
};
struct ReviewComparisonPresentation {
    QString move;
    QString score;
    QString detail;
    bool operator==(const ReviewComparisonPresentation&) const = default;
};
ReviewComparisonPresentation presentReviewComparison(const review::MoveComparison& row,
                                                     const QString& san);
ReviewMomentPresentation presentReviewMoment(const persistence::ReviewMove* move, int position,
                                             bool analyzing);
QString classificationSymbol(std::optional<review::Classification> classification);
struct ReviewEvaluation {
    std::optional<double> whiteExpected;
    bool approximate = false;
    QString description;
};
ReviewEvaluation reviewPositionEvaluation(const persistence::ReviewMove* move, bool initial);
std::vector<BoardAnnotation> reviewTacticalAnnotations(const persistence::ReviewMove& move);
QString reviewMoveMarker(const persistence::ReviewMove* move);
QString reviewMomentHeadline(const persistence::ReviewMove* move, int position);
QString classificationText(const std::optional<review::ClassificationResult>& result);
QString reviewOverviewText(const persistence::ReviewOverview& overview, chess::Color selectedSide);
QString comparisonReasonText(review::ComparisonReason reason);
QString reviewComparisonRow(const review::MoveComparison& row, const QString& san);
QString reviewMoveDetails(const persistence::ReviewMove& move);
QString reviewRetryHint(const review::RetryHint& hint, review::RetryHintLevel level);
} // namespace chess_review::app
