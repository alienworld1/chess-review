#pragma once
#include "persistence/ReviewRepository.h"
#include <QString>
#include <optional>
namespace chess_review::app {
struct ReviewEvaluation {
    std::optional<double> whiteExpected;
    bool approximate = false;
    QString description;
};
ReviewEvaluation reviewPositionEvaluation(const persistence::ReviewMove* move, bool initial);
QString reviewMoveMarker(const persistence::ReviewMove* move);
QString reviewMomentHeadline(const persistence::ReviewMove* move, int position);
QString reviewMomentExplanation(const persistence::ReviewMove* move, int position);
QString classificationText(const std::optional<review::ClassificationResult>& result);
QString reviewOverviewText(const persistence::ReviewOverview& overview, chess::Color selectedSide);
QString reviewMoveDetails(const persistence::ReviewMove& move);
} // namespace chess_review::app
