#include "persistence/private/ReviewCodec.h"
#include "persistence/private/AnalysisCodec.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
namespace chess_review::persistence::detail {
namespace {
// These numeric tags are part of durable document format 1.
static_assert(static_cast<int>(chess::Color::White) == 0 &&
              static_cast<int>(chess::Color::Black) == 1);
static_assert(static_cast<int>(review::Quality::Fast) == 0 &&
              static_cast<int>(review::Quality::Thorough) == 2);
static_assert(static_cast<int>(review::ScoreKind::Centipawns) == 0 &&
              static_cast<int>(review::ScoreKind::Mate) == 1);
static_assert(static_cast<int>(review::Bound::Exact) == 0 &&
              static_cast<int>(review::Bound::Upper) == 2);
static_assert(static_cast<int>(review::ConversionSource::NativeWdl) == 0 &&
              static_cast<int>(review::ConversionSource::Terminal) == 2);
static_assert(static_cast<int>(review::EvidenceIssue::Invalid) == 0 &&
              static_cast<int>(review::EvidenceIssue::ContradictoryCandidates) == 11);
static_assert(static_cast<int>(review::TerminalOutcome::Checkmate) == 0 &&
              static_cast<int>(review::TerminalOutcome::Stalemate) == 1);
QJsonValue integer(std::uint64_t value) { return QString::number(value); }
QJsonValue optionalInteger(const std::optional<std::uint64_t>& value) {
    return value ? integer(*value) : QJsonValue();
}
QJsonObject strength(const review::AnalysisStrength& value) {
    return {{"depth", value.depth ? QJsonValue(*value.depth) : QJsonValue()},
            {"nodes", optionalInteger(value.nodes)},
            {"timeMs", optionalInteger(value.timeMs)}};
}
QJsonObject raw(const review::RawEvaluation& value) {
    QJsonObject out{{"perspective", static_cast<int>(value.perspective)}};
    out["score"] = value.score
                       ? QJsonValue(QJsonObject{{"kind", static_cast<int>(value.score->kind)},
                                                {"value", QString::number(value.score->value)},
                                                {"bound", static_cast<int>(value.score->bound)}})
                       : QJsonValue();
    out["wdl"] = value.wdl
                     ? QJsonValue(QJsonArray{value.wdl->wins, value.wdl->draws, value.wdl->losses})
                     : QJsonValue();
    return out;
}
QJsonValue mate(const std::optional<review::RawScore>& score) {
    if (!score) {
        return {};
    }
    return QJsonObject{{"kind", static_cast<int>(score->kind)},
                       {"value", QString::number(score->value)},
                       {"bound", static_cast<int>(score->bound)}};
}
QJsonArray move(const chess::Move& value) {
    return {value.from.index(), value.to.index(),
            value.promotion ? static_cast<int>(*value.promotion) : -1};
}
QJsonObject evaluation(const review::EvaluationResult& value) {
    QJsonObject out{
        {"issue", value.issue ? QJsonValue(static_cast<int>(*value.issue)) : QJsonValue()}};
    out["expected"] = value.expected
                          ? QJsonValue(QJsonObject{
                                {"value", value.expected->value},
                                {"perspective", static_cast<int>(value.expected->perspective)},
                                {"source", static_cast<int>(value.expected->source)},
                                {"modelVersion", static_cast<int>(value.expected->modelVersion)},
                                {"fallbackScaleCp", value.expected->config.fallbackScaleCp},
                                {"preferWdl", value.expected->config.preferWdl}})
                          : QJsonValue();
    return out;
}
QJsonObject position(const review::PositionEvidence& value) {
    QJsonArray candidates;
    for (const auto& candidate : value.candidates) {
        QJsonArray line;
        for (const auto& m : candidate.continuation) {
            line.append(move(m));
        }
        candidates.append(QJsonObject{{"rank", candidate.rank},
                                      {"evaluation", raw(candidate.evaluation)},
                                      {"line", line},
                                      {"strength", strength(candidate.strength)}});
    }
    return {{"modelVersion", static_cast<int>(value.modelVersion)},
            {"position", QString::fromLatin1(positionIdentity(value.position).toBase64())},
            {"evaluation", raw(value.evaluation)},
            {"candidates", candidates},
            {"strength", strength(value.strength)},
            {"engine", QString::fromStdString(value.engineIdentity)},
            {"session", QString::fromStdString(value.sessionIdentity)},
            {"attempt", QString::fromStdString(value.attemptIdentity)},
            {"createdAt", QString::fromStdString(value.createdAtUtc)},
            {"complete", value.complete},
            {"restricted", value.restricted},
            {"terminal",
             value.terminal ? QJsonValue(static_cast<int>(*value.terminal)) : QJsonValue()}};
}
} // namespace
QByteArray reviewDocument(const QJsonObject& object) {
    auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    bytes += QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    return bytes;
}
std::optional<QJsonObject> readReviewDocument(const QByteArray& bytes, qsizetype maximum) {
    if (bytes.size() < 34 || bytes.size() > maximum) {
        return std::nullopt;
    }
    const auto body = bytes.first(bytes.size() - 32);
    if (QCryptographicHash::hash(body, QCryptographicHash::Sha256) != bytes.last(32)) {
        return std::nullopt;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}
bool sameConfiguration(const config::ValidatedConfiguration& a,
                       const config::ValidatedConfiguration& b) {
    return a.classificationIdentityVersion == b.classificationIdentityVersion &&
           a.values == b.values && a.identities.classification == b.identities.classification &&
           a.identities.conversion == b.identities.conversion &&
           a.identities.analysis == b.identities.analysis;
}
QByteArray encodeReviewSettings(const CreateReview& settings) {
    const auto validated = config::validate(settings.configuration.values,
                                            settings.configuration.classificationIdentityVersion);
    const auto* config =
        std::get_if<std::shared_ptr<const config::ValidatedConfiguration>>(&validated);
    if (!config || !sameConfiguration(**config, settings.configuration)) {
        return {};
    }
    const auto fingerprint = encodeFingerprint(settings.fingerprint);
    if (fingerprint.isEmpty() || fingerprint.size() > 1024 * 1024 || settings.gameId <= 0 ||
        settings.engineProfileId <= 0 ||
        (settings.selectedSide != chess::Color::White &&
         settings.selectedSide != chess::Color::Black)) {
        return {};
    }
    QJsonArray budgets;
    for (const auto& b : settings.configuration.values.budgets) {
        budgets.append(QJsonArray{integer(b.quickScanNodes), integer(b.deepeningNodes),
                                  integer(b.verificationNodes)});
    }
    const auto& c = settings.configuration.values;
    const auto& ids = settings.configuration.identities;
    auto document = QJsonObject{
        {"formatVersion", settings.configuration.classificationIdentityVersion == 1 ? 1 : 2},
        {"evidenceModelVersion", static_cast<int>(review::evidenceModelVersion)},
        {"expectedScoreModelVersion", static_cast<int>(review::expectedScoreModelVersion)},
        {"gameId", QString::number(settings.gameId)},
        {"engineProfileId", QString::number(settings.engineProfileId)},
        {"selectedSide", static_cast<int>(settings.selectedSide)},
        {"fingerprint", QString::fromLatin1(fingerprint.toBase64())},
        {"configurationVersion", static_cast<int>(c.version)},
        {"quality", static_cast<int>(c.quality)},
        {"budgets", budgets},
        {"thresholds", QJsonArray{c.classification.inaccuracyLoss, c.classification.mistakeLoss,
                                  c.classification.blunderLoss}},
        {"fallbackScaleCp", c.conversion.fallbackScaleCp},
        {"preferWdl", c.conversion.preferWdl},
        {"classificationIdentity", QString::fromLatin1(ids.classification.toHex())},
        {"conversionIdentity", QString::fromLatin1(ids.conversion.toHex())},
        {"analysisIdentity", QString::fromLatin1(ids.analysis.toHex())},
        {"reviewModelVersion", QJsonValue()},
        {"accuracyModelVersion", QJsonValue()},
        {"motifModelVersion", QJsonValue()},
        {"explanationModelVersion", QJsonValue()},
        {"openingDatasetVersion", QJsonValue()}};
    if (settings.configuration.classificationIdentityVersion == 2) {
        document["excellentLoss"] = c.classification.excellentLoss;
    }
    return reviewDocument(document);
}
std::optional<CreateReview> decodeReviewSettings(const QByteArray& bytes) {
    const auto parsed = readReviewDocument(bytes, maxReviewSettingsBytes);
    if (!parsed) {
        return std::nullopt;
    }
    const auto& o = *parsed;
    CreateReview out;
    const int format = o["formatVersion"].toInt(-1);
    if (format != 1 && format != 2) {
        return std::nullopt;
    }
    out.configuration.classificationIdentityVersion = static_cast<std::uint32_t>(format);
    out.gameId = o["gameId"].toString().toLongLong();
    out.engineProfileId = o["engineProfileId"].toString().toLongLong();
    out.selectedSide = static_cast<chess::Color>(o["selectedSide"].toInt(-1));
    const auto fp =
        decodeFingerprint(QByteArray::fromBase64(o["fingerprint"].toString().toLatin1()));
    if (!fp) {
        return std::nullopt;
    }
    out.fingerprint = *fp;
    auto& c = out.configuration.values;
    c.version = static_cast<std::uint32_t>(o["configurationVersion"].toInt(-1));
    c.quality = static_cast<review::Quality>(o["quality"].toInt(-1));
    const auto budgets = o["budgets"].toArray();
    if (budgets.size() != 3) {
        return std::nullopt;
    }
    for (qsizetype i = 0; i < 3; ++i) {
        const auto b = budgets[i].toArray();
        if (b.size() != 3) {
            return std::nullopt;
        }
        c.budgets[static_cast<std::size_t>(i)] = {b[0].toString().toULongLong(),
                                                  b[1].toString().toULongLong(),
                                                  b[2].toString().toULongLong()};
    }
    const auto thresholds = o["thresholds"].toArray();
    if (thresholds.size() != 3) {
        return std::nullopt;
    }
    c.classification = {thresholds[0].toDouble(), thresholds[1].toDouble(),
                        thresholds[2].toDouble()};
    if (format == 2) {
        c.classification.excellentLoss = o["excellentLoss"].toDouble(-1);
    }
    c.conversion = {o["fallbackScaleCp"].toDouble(), o["preferWdl"].toBool()};
    out.configuration.identities = {
        QByteArray::fromHex(o["classificationIdentity"].toString().toLatin1()),
        QByteArray::fromHex(o["conversionIdentity"].toString().toLatin1()),
        QByteArray::fromHex(o["analysisIdentity"].toString().toLatin1())};
    // Canonical agreement validates all wire types, versions, keys, identities and null metadata.
    const auto encoded = encodeReviewSettings(out);
    if (encoded.isEmpty() || encoded != bytes) {
        return std::nullopt;
    }
    return out;
}
QByteArray adaptedDocument(const review::PositionEvidence& evidence,
                           const review::ConversionConfig& config) {
    return reviewDocument(
        {{"formatVersion", reviewFormatVersion},
         {"evidence", position(evidence)},
         {"rawConversion", evaluation(review::convertEvaluation(
                               evidence.evaluation, evidence.evaluation.perspective, config))}});
}
QByteArray classificationDocument(const review::ClassificationResult& result, std::int64_t beforeId,
                                  std::int64_t afterId, const QByteArray& configurationIdentity) {
    QJsonArray alternatives;
    for (const auto& a : result.alternatives) {
        alternatives.append(QJsonObject{{"move", move(a.move)},
                                        {"rank", a.rank},
                                        {"gap", a.gap},
                                        {"nearlyEquivalent", a.nearlyEquivalent}});
    }
    return reviewDocument(
        {{"formatVersion", 1},
         {"modelVersion", static_cast<int>(result.modelVersion)},
         {"beforeId", QString::number(beforeId)},
         {"afterId", QString::number(afterId)},
         {"configurationIdentity", QString::fromLatin1(configurationIdentity.toHex())},
         {"label", result.label ? QJsonValue(static_cast<int>(*result.label)) : QJsonValue()},
         {"issue", result.issue ? QJsonValue(static_cast<int>(*result.issue)) : QJsonValue()},
         {"evidenceIssue", result.evidenceIssue
                               ? QJsonValue(static_cast<int>(*result.evidenceIssue))
                               : QJsonValue()},
         {"ruleId", QString::fromStdString(result.ruleId)},
         {"stable", result.stable},
         {"mover", static_cast<int>(result.mover)},
         {"thresholds", QJsonArray{result.thresholds.inaccuracyLoss, result.thresholds.mistakeLoss,
                                   result.thresholds.blunderLoss, result.thresholds.excellentLoss}},
         {"before", evaluation(result.before)},
         {"after", evaluation(result.after)},
         {"loss", result.loss ? QJsonValue(*result.loss) : QJsonValue()},
         {"signedDelta", result.signedDelta ? QJsonValue(*result.signedDelta) : QJsonValue()},
         {"bestMove", result.bestMove ? QJsonValue(move(*result.bestMove)) : QJsonValue()},
         {"playedMoveRank",
          result.playedMoveRank ? QJsonValue(*result.playedMoveRank) : QJsonValue()},
         {"alternatives", alternatives}});
}
QByteArray pairedDocument(const review::MoveEvidence& evidence) {
    const auto optionalDouble = [](const auto& v) { return v ? QJsonValue(*v) : QJsonValue(); };
    return reviewDocument(
        {{"formatVersion", reviewFormatVersion},
         {"playedMove", move(evidence.playedMove)},
         {"mover", static_cast<int>(evidence.mover)},
         {"mate", QJsonObject{{"before", mate(evidence.mate.before)},
                              {"after", mate(evidence.mate.after)},
                              {"perspective", static_cast<int>(evidence.mate.perspective)}}},
         {"beforeEvaluation", evaluation(evidence.beforeEvaluation)},
         {"afterEvaluation", evaluation(evidence.afterEvaluation)},
         {"signedDelta", optionalDouble(evidence.signedDelta)},
         {"loss", optionalDouble(evidence.loss)},
         {"playedMoveRank",
          evidence.playedMoveRank ? QJsonValue(*evidence.playedMoveRank) : QJsonValue()},
         {"issue", evidence.issue ? QJsonValue(static_cast<int>(*evidence.issue)) : QJsonValue()},
         {"beforeIdentity",
          QString::fromLatin1(QCryptographicHash::hash(reviewDocument(position(evidence.before)),
                                                       QCryptographicHash::Sha256)
                                  .toHex())},
         {"afterIdentity",
          QString::fromLatin1(QCryptographicHash::hash(reviewDocument(position(evidence.after)),
                                                       QCryptographicHash::Sha256)
                                  .toHex())}});
}
} // namespace chess_review::persistence::detail
