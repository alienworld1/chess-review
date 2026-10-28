#pragma once
#include "persistence/ReviewRepository.h"
#include <QString>
#include <optional>
namespace chess_review::app {
QString classificationText(const std::optional<review::ClassificationResult>& result);
QString reviewMoveDetails(const persistence::ReviewMove& move);
} // namespace chess_review::app
