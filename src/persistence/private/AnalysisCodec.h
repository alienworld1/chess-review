#pragma once
#include "persistence/AnalysisRepository.h"
#include <QByteArray>
namespace chess_review::persistence::detail {
constexpr qsizetype maxAnalysisBytes = 16 * 1024 * 1024;
constexpr qint64 analysisBudgetBytes = 256 * 1024 * 1024;
constexpr qint64 maxAnalysisRecords = 100000;
// Canonical version-1 position encoding, reconstructing derived board fields from legal history.
QByteArray positionIdentity(const chess::PositionContext& context);
std::variant<QByteArray, CacheError> encodeAnalysis(const engine::AnalysisResult& result);
std::optional<engine::AnalysisResult> decodeAnalysis(const QByteArray& bytes);
} // namespace chess_review::persistence::detail
