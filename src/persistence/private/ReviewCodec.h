#pragma once
#include "persistence/ReviewRepository.h"
#include <QJsonObject>
namespace chess_review::persistence::detail {
inline constexpr int reviewFormatVersion = 1;
inline constexpr qsizetype maxReviewSettingsBytes = 2 * 1024 * 1024;
// Checksum framing detects corruption, not deliberate rewriting by the database owner.
QByteArray reviewDocument(const QJsonObject& object);
std::optional<QJsonObject> readReviewDocument(const QByteArray& bytes, qsizetype maximum);
QByteArray encodeReviewSettings(const CreateReview& settings);
std::optional<CreateReview> decodeReviewSettings(const QByteArray& bytes);
bool sameConfiguration(const config::ValidatedConfiguration& a,
                       const config::ValidatedConfiguration& b);
QByteArray adaptedDocument(const review::PositionEvidence& evidence,
                           const review::ConversionConfig& config);
QByteArray classificationDocument(const review::ClassificationResult& result, std::int64_t beforeId,
                                  std::int64_t afterId, const QByteArray& configurationIdentity);
QByteArray pairedDocument(const review::MoveEvidence& evidence);
} // namespace chess_review::persistence::detail
