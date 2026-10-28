#pragma once
#include "persistence/AnalysisRepository.h"
#include <QByteArray>
namespace chess_review::persistence::detail {
constexpr std::uint32_t analysisFormatVersion = 3;
constexpr qsizetype maxAnalysisBytes = 16 * 1024 * 1024;
constexpr qint64 analysisBudgetBytes = 256 * 1024 * 1024;
constexpr qint64 maxAnalysisRecords = 100000;
QByteArray encodeFingerprint(const engine::EngineFingerprint& fingerprint);
std::optional<engine::EngineFingerprint> decodeFingerprint(const QByteArray& bytes);
// Canonical version-1 position encoding, reconstructing derived board fields from legal history.
QByteArray positionIdentity(const chess::PositionContext& context);
std::variant<QByteArray, CacheError> encodeAnalysis(const engine::AnalysisResult& result);
std::optional<engine::AnalysisResult> decodeAnalysis(const QByteArray& bytes,
                                                     const chess::Position* knownRoot = nullptr);
} // namespace chess_review::persistence::detail
