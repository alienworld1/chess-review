#include "engine/AnalysisCache.h"
#include "persistence/private/ReviewCodec.h"
#include "persistence/private/StorageSession.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QTimeZone>
#include <QVariant>
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <utility>
namespace chess_review::persistence::detail {
namespace {
using review::JobFailure;
using review::JobStatus;
ReviewError error(ReviewErrorCode code) {
    switch (code) {
    case ReviewErrorCode::Missing:
        return {code, QStringLiteral("The requested review record is missing.")};
    case ReviewErrorCode::InvalidInput:
        return {code, QStringLiteral("Invalid review storage request.")};
    case ReviewErrorCode::InvalidRecord:
        return {code, QStringLiteral("The saved review record is damaged.")};
    case ReviewErrorCode::UnsupportedVersion:
        return {code, QStringLiteral("The saved review requires an unsupported format or model.")};
    case ReviewErrorCode::Conflict:
        return {code,
                QStringLiteral("Review progress or configuration conflicts with this request.")};
    default:
        return {code, QStringLiteral("Review storage could not complete the request.")};
    }
}
ReviewError storage() { return error(ReviewErrorCode::Storage); }
ReviewError invalid() { return error(ReviewErrorCode::InvalidRecord); }
class ReviewTransaction {
  public:
    ReviewTransaction(QSqlDatabase& database, bool& usable)
        : database_(database), usable_(usable) {}
    ReviewTransaction(const ReviewTransaction&) = delete;
    ReviewTransaction& operator=(const ReviewTransaction&) = delete;
    ~ReviewTransaction() {
        if (active_ && !database_.rollback()) {
            usable_ = false;
        }
    }
    bool begin() {
        QSqlQuery q(database_);
        active_ = q.exec(QStringLiteral("BEGIN IMMEDIATE"));
        return active_;
    }
    bool commit() {
        if (!database_.commit()) {
            return false;
        }
        active_ = false;
        return true;
    }

  private:
    QSqlDatabase& database_;
    bool& usable_;
    bool active_ = false;
};
bool exec(QSqlQuery& query, const QString& sql, std::initializer_list<QVariant> values) {
    if (!query.prepare(sql)) {
        return false;
    }
    for (const auto& v : values) {
        query.addBindValue(v);
    }
    return query.exec();
}
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
QDateTime timestamp(qint64 value) { return QDateTime::fromMSecsSinceEpoch(value, QTimeZone::UTC); }
bool active(JobStatus status) {
    return status >= JobStatus::Queued && status <= JobStatus::Deriving;
}
bool resumable(JobStatus status) {
    return status >= JobStatus::Partial && status <= JobStatus::Failed;
}
bool validStatus(JobStatus status) {
    return status >= JobStatus::Queued && status <= JobStatus::Failed;
}
const QString summaryProjection = QStringLiteral(
    "SELECT r.id,j.id,CASE WHEN typeof(r.game_id)='integer' THEN r.game_id END,"
    "CASE WHEN typeof(r.selected_side)='integer' THEN r.selected_side END,"
    "CASE WHEN typeof(r.quality)='integer' THEN r.quality END,"
    "CASE WHEN typeof(j.status)='integer' THEN j.status END,"
    "CASE WHEN typeof(j.revision)='integer' THEN j.revision END,"
    "CASE WHEN typeof(j.completed_positions)='integer' THEN j.completed_positions END,"
    "CASE WHEN typeof(j.total_positions)='integer' THEN j.total_positions END,"
    "CASE WHEN typeof(r.created_at)='integer' THEN r.created_at END,"
    "CASE WHEN typeof(j.updated_at)='integer' THEN j.updated_at END,"
    "typeof(r.id)='integer' AND typeof(j.id)='integer' AND typeof(r.game_id)='integer' AND "
    "typeof(r.selected_side)='integer' AND typeof(r.quality)='integer' AND "
    "typeof(j.status)='integer' AND "
    "typeof(j.revision)='integer' AND typeof(j.completed_positions)='integer' AND "
    "typeof(j.total_positions)='integer' AND "
    "typeof(r.created_at)='integer' AND typeof(j.updated_at)='integer' "
    "FROM reviews r JOIN review_jobs j ON j.review_id=r.id ");
std::optional<ReviewSummary> summary(const QSqlQuery& q) {
    if (!q.value(11).toBool() || q.value(0).toLongLong() <= 0 || q.value(1).toLongLong() <= 0 ||
        q.value(2).toLongLong() <= 0 || q.value(3).toLongLong() < 0 ||
        q.value(3).toLongLong() > 1 || q.value(4).toLongLong() < 0 || q.value(4).toLongLong() > 2 ||
        q.value(5).toLongLong() < 0 || q.value(5).toLongLong() > 9 || q.value(6).toLongLong() < 0 ||
        q.value(7).toLongLong() < 0 || q.value(7).toLongLong() > q.value(8).toLongLong() ||
        q.value(8).toLongLong() < 1 || q.value(8).toLongLong() > 16385 ||
        q.value(9).toLongLong() < 1 || q.value(10).toLongLong() < q.value(9).toLongLong() ||
        q.value(10).toLongLong() > 253402300799999LL) {
        return std::nullopt;
    }
    return ReviewSummary{q.value(0).toLongLong(),
                         q.value(1).toLongLong(),
                         q.value(2).toLongLong(),
                         static_cast<chess::Color>(q.value(3).toInt()),
                         static_cast<review::Quality>(q.value(4).toInt()),
                         static_cast<JobStatus>(q.value(5).toInt()),
                         q.value(6).toLongLong(),
                         q.value(7).toInt(),
                         q.value(8).toInt(),
                         timestamp(q.value(9).toLongLong()),
                         timestamp(q.value(10).toLongLong()),
                         {}};
}
chess::Result<chess::Position> gamePosition(const StoredGame& game, int index) {
    auto root = game.document->tree.position(game.document->tree.rootId());
    if (!std::holds_alternative<chess::Position>(root)) {
        return chess::Error{chess::ErrorCode::InvalidPosition, "Invalid game root"};
    }
    auto position = std::get<chess::Position>(std::move(root));
    const auto moves = game.document->originalMainline();
    if (index < 0 || static_cast<std::size_t>(index) > moves.size()) {
        return chess::Error{chess::ErrorCode::InvalidPosition, "Invalid position index"};
    }
    for (int i = 0; i < index; ++i) {
        if (std::holds_alternative<chess::Error>(
                position.play(moves[static_cast<std::size_t>(i)]))) {
            return chess::Error{chess::ErrorCode::InvalidPosition, "Invalid mainline"};
        }
    }
    return position;
}
bool transition(QSqlDatabase& db, ReviewId id, std::int64_t revision,
                std::optional<JobStatus> previous, JobStatus status, JobFailure failure,
                qint64 time) {
    QSqlQuery q(db);
    return exec(q,
                QStringLiteral("INSERT INTO "
                               "review_transitions(review_id,revision,previous,status,failure,"
                               "created_at) VALUES(?,?,?,?,?,?)"),
                {static_cast<qint64>(id), static_cast<qint64>(revision),
                 previous ? QVariant(static_cast<int>(*previous))
                          : QVariant(QMetaType::fromType<int>()),
                 static_cast<int>(status), static_cast<int>(failure), time}) &&
           q.numRowsAffected() == 1;
}
std::variant<StoredReviewEvidence, ReviewError> readEvidence(QSqlDatabase& db,
                                                             const StoredReview& run,
                                                             const chess::Position& position,
                                                             int index) {
    QSqlQuery q(db);
    if (!exec(q,
              QStringLiteral("SELECT e.id,CASE WHEN typeof(e.position_index)='integer' THEN "
                             "e.position_index END,substr(CAST(e.session AS "
                             "BLOB),1,17),substr(CAST(e.attempt AS BLOB),1,17),"
                             "substr(CAST(e.analysis AS BLOB),1,16777217),substr(CAST(e.adapted AS "
                             "BLOB),1,16777217),CASE WHEN typeof(e.format_version)='integer' THEN "
                             "e.format_version END,"
                             "typeof(e.id)='integer' AND typeof(e.position_index)='integer' AND "
                             "typeof(e.format_version)='integer' AND "
                             "typeof(e.adapted)='blob' AND ((e.analysis IS NULL AND e.session IS "
                             "NULL AND e.attempt IS NULL) OR "
                             "(typeof(e.analysis)='blob' AND typeof(e.session)='blob' AND "
                             "typeof(e.attempt)='blob')) "
                             "FROM review_positions p JOIN review_evidence e ON "
                             "e.review_id=p.review_id AND e.id=p.evidence_id "
                             "WHERE p.review_id=? AND p.position_index=?"),
              {static_cast<qint64>(run.summary.id), index})) {
        return storage();
    }
    if (!q.next()) {
        return q.lastError().isValid() ? storage() : error(ReviewErrorCode::Missing);
    }
    if (!q.value(7).toBool()) {
        return invalid();
    }
    if (q.value(6).toLongLong() != reviewFormatVersion) {
        return error(ReviewErrorCode::UnsupportedVersion);
    }
    if (q.value(0).toLongLong() <= 0 || q.value(1).toLongLong() != index) {
        return invalid();
    }
    StoredReviewEvidence out;
    out.positionIndex = index;
    out.evidenceId = q.value(0).toLongLong();
    if (q.value(4).isNull()) {
        out.evidence = review::terminalEvidence(position);
        if (!out.evidence.terminal) {
            return invalid();
        }
    } else {
        auto decoded = decodeAnalysis(q.value(4).toByteArray(), &position);
        if (!decoded || decoded->evidence.provenance->position != position.context() ||
            decoded->evidence.provenance->fingerprint != run.settings.fingerprint ||
            decoded->evidence.session.toRfc4122() != q.value(2).toByteArray() ||
            decoded->evidence.request.toRfc4122() != q.value(3).toByteArray()) {
            return invalid();
        }
        auto adapted = analysis::reviewEvidence(*decoded);
        const auto* value = std::get_if<analysis::ReviewEvidenceRecord>(&adapted);
        if (!value) {
            return invalid();
        }
        out.evidence = value->evidence;
        out.analysis = std::move(*decoded);
    }
    const auto adapted = readReviewDocument(q.value(5).toByteArray(), maxAnalysisBytes);
    if (!adapted) {
        return invalid();
    }
    if ((*adapted)["formatVersion"].toInt(-1) != reviewFormatVersion ||
        (*adapted)["evidence"].toObject()["modelVersion"].toInt(-1) !=
            static_cast<int>(review::evidenceModelVersion)) {
        return error(ReviewErrorCode::UnsupportedVersion);
    }
    if (adaptedDocument(out.evidence, run.settings.configuration.values.conversion) !=
        q.value(5).toByteArray()) {
        return invalid();
    }
    out.conversion =
        review::convertEvaluation(out.evidence.evaluation, out.evidence.evaluation.perspective,
                                  run.settings.configuration.values.conversion);
    return out;
}
std::variant<review::MoveEvidence, ReviewError> readPair(QSqlDatabase& db, const StoredReview& run,
                                                         const chess::Position& beforePosition,
                                                         chess::Move move, int index,
                                                         const StoredReviewEvidence& before,
                                                         const StoredReviewEvidence& after) {
    auto paired = review::pairEvidence(beforePosition, move, before.evidence, after.evidence,
                                       run.settings.configuration.values.conversion);
    QSqlQuery q(db);
    if (!exec(q,
              QStringLiteral("SELECT CASE WHEN typeof(before_id)='integer' THEN before_id END,CASE "
                             "WHEN typeof(after_id)='integer' THEN after_id END,substr(CAST(paired "
                             "AS BLOB),1,16777217),"
                             "typeof(before_id)='integer' AND typeof(after_id)='integer' AND "
                             "typeof(paired)='blob' "
                             "FROM review_pairs WHERE review_id=? AND position_index=?"),
              {static_cast<qint64>(run.summary.id), index})) {
        return storage();
    }
    if (!q.next()) {
        return q.lastError().isValid() ? storage() : invalid();
    }
    if (!q.value(3).toBool() || q.value(0).toLongLong() != before.evidenceId ||
        q.value(1).toLongLong() != after.evidenceId) {
        return invalid();
    }
    const auto bytes = q.value(2).toByteArray();
    const auto document = readReviewDocument(bytes, maxAnalysisBytes);
    if (!document) {
        return invalid();
    }
    if ((*document)["formatVersion"].toInt(-1) != reviewFormatVersion) {
        return error(ReviewErrorCode::UnsupportedVersion);
    }
    if (bytes != pairedDocument(paired)) {
        return invalid();
    }
    return paired;
}
bool updatePair(QSqlDatabase& db, const StoredReview& run, const StoredGame& game, int index) {
    if (index <= 0 || index >= run.summary.totalPositions) {
        return true;
    }
    auto restored = gamePosition(game, index - 1);
    const auto* position = std::get_if<chess::Position>(&restored);
    if (!position) {
        return false;
    }
    auto before = readEvidence(db, run, *position, index - 1);
    auto afterPosition = *position;
    const auto move = game.document->originalMainline()[static_cast<std::size_t>(index - 1)];
    if (std::holds_alternative<chess::Error>(afterPosition.play(move))) {
        return false;
    }
    auto after = readEvidence(db, run, afterPosition, index);
    if (const auto* e = std::get_if<ReviewError>(&before)) {
        return e->code == ReviewErrorCode::Missing;
    }
    if (const auto* e = std::get_if<ReviewError>(&after)) {
        return e->code == ReviewErrorCode::Missing;
    }
    const auto& b = std::get<StoredReviewEvidence>(before);
    const auto& a = std::get<StoredReviewEvidence>(after);
    const auto paired = review::pairEvidence(*position, move, b.evidence, a.evidence,
                                             run.settings.configuration.values.conversion);
    const auto bytes = pairedDocument(paired);
    if (bytes.size() > maxAnalysisBytes) {
        return false;
    }
    QSqlQuery q(db);
    return exec(q,
                QStringLiteral(
                    "INSERT INTO review_pairs(review_id,position_index,before_id,after_id,paired) "
                    "VALUES(?,?,?,?,?) "
                    "ON CONFLICT(review_id,position_index) DO UPDATE SET "
                    "before_id=excluded.before_id,after_id=excluded.after_id,paired=excluded."
                    "paired"),
                {static_cast<qint64>(run.summary.id), index, static_cast<qint64>(b.evidenceId),
                 static_cast<qint64>(a.evidenceId), bytes}) &&
           q.numRowsAffected() == 1;
}
} // namespace
CreateReviewResult StorageSession::createReview(const CreateReview& request) {
    if (!usable_) {
        return storage();
    }
    const auto settings = encodeReviewSettings(request);
    if (settings.isEmpty() || settings.size() > maxReviewSettingsBytes) {
        return error(ReviewErrorCode::InvalidInput);
    }
    ReviewTransaction tx(database_, usable_);
    if (!tx.begin()) {
        return storage();
    }
    const auto loaded = loadGame(request.gameId);
    const auto* game = std::get_if<StoredGame>(&loaded);
    if (!game) {
        return error(ReviewErrorCode::InvalidInput);
    }
    const auto count = static_cast<int>(game->document->originalMainline().size()) + 1;
    auto final = gamePosition(*game, count - 1);
    if (!std::holds_alternative<chess::Position>(final)) {
        return error(ReviewErrorCode::InvalidInput);
    }
    const auto identity = positionIdentity(std::get<chess::Position>(final).context());
    if (identity.size() > 1024 * 1024 || count > 16385) {
        return error(ReviewErrorCode::InvalidInput);
    }
    QSqlQuery q(database_);
    if (!exec(q,
              QStringLiteral("SELECT substr(CAST(executable_sha256 AS BLOB),1,33) FROM engines "
                             "WHERE id=? AND typeof(executable_sha256)='blob'"),
              {static_cast<qint64>(request.engineProfileId)})) {
        return storage();
    }
    if (!q.next() || q.value(0).toByteArray() != request.fingerprint.executableSha256) {
        return error(ReviewErrorCode::InvalidInput);
    }
    q.finish();
    const auto time = now();
    if (!exec(
            q,
            QStringLiteral("INSERT INTO "
                           "reviews(game_id,engine_profile_id,selected_side,quality,mainline_"
                           "identity,settings,format_version,created_at) VALUES(?,?,?,?,?,?,?,?)"),
            {static_cast<qint64>(request.gameId), static_cast<qint64>(request.engineProfileId),
             static_cast<int>(request.selectedSide),
             static_cast<int>(request.configuration.values.quality), identity, settings,
             reviewFormatVersion, time}) ||
        q.numRowsAffected() != 1) {
        return storage();
    }
    const auto id = q.lastInsertId().toLongLong();
    q.finish();
    if (!exec(q,
              QStringLiteral("INSERT INTO "
                             "review_jobs(review_id,status,revision,total_positions,completed_"
                             "positions,updated_at) VALUES(?,0,0,?,0,?)"),
              {static_cast<qint64>(id), count, time}) ||
        q.numRowsAffected() != 1) {
        return storage();
    }
    const auto job = q.lastInsertId().toLongLong();
    q.finish();
    if (!transition(database_, id, 0, {}, JobStatus::Queued, JobFailure::None, time) ||
        !tx.commit()) {
        return storage();
    }
    return ReviewSummary{id,
                         job,
                         request.gameId,
                         request.selectedSide,
                         request.configuration.values.quality,
                         JobStatus::Queued,
                         0,
                         0,
                         count,
                         timestamp(time),
                         timestamp(time),
                         {}};
}
LoadReviewResult StorageSession::loadReview(ReviewId id) { return loadReviewRecord(id, nullptr); }
LoadReviewResult StorageSession::loadReviewRecord(ReviewId id, StoredGame* owningGame) {
    if (!usable_) {
        return storage();
    }
    QSqlQuery q(database_);
    if (id <= 0) {
        return error(ReviewErrorCode::InvalidInput);
    }
    if (!exec(q, summaryProjection + QStringLiteral("WHERE r.id=?"), {static_cast<qint64>(id)})) {
        return storage();
    }
    if (!q.next()) {
        return q.lastError().isValid() ? storage() : error(ReviewErrorCode::Missing);
    }
    const auto s = summary(q);
    q.finish();
    if (!s) {
        return invalid();
    }
    if (!exec(q,
              QStringLiteral(
                  "SELECT substr(CAST(settings AS BLOB),1,2097153),substr(CAST(mainline_identity "
                  "AS BLOB),1,1048577),"
                  "CASE WHEN typeof(engine_profile_id)='integer' THEN engine_profile_id END,CASE "
                  "WHEN typeof(format_version)='integer' THEN format_version "
                  "END,typeof(settings)='blob' AND "
                  "typeof(mainline_identity)='blob' AND typeof(engine_profile_id)='integer' AND "
                  "typeof(format_version)='integer' FROM reviews WHERE id=?"),
              {static_cast<qint64>(id)}) ||
        !q.next()) {
        return storage();
    }
    if (!q.value(4).toBool()) {
        return invalid();
    }
    if (q.value(3).toLongLong() != reviewFormatVersion) {
        return error(ReviewErrorCode::UnsupportedVersion);
    }
    const auto bytes = q.value(0).toByteArray();
    const auto doc = readReviewDocument(bytes, maxReviewSettingsBytes);
    if (!doc) {
        return invalid();
    }
    if ((*doc)["formatVersion"].toInt(-1) != reviewFormatVersion ||
        (*doc)["configurationVersion"].toInt(-1) != 1 ||
        (*doc)["evidenceModelVersion"].toInt(-1) !=
            static_cast<int>(review::evidenceModelVersion) ||
        (*doc)["expectedScoreModelVersion"].toInt(-1) !=
            static_cast<int>(review::expectedScoreModelVersion)) {
        return error(ReviewErrorCode::UnsupportedVersion);
    }
    auto settings = decodeReviewSettings(bytes);
    if (!settings || settings->gameId != s->gameId ||
        settings->engineProfileId != q.value(2).toLongLong() ||
        settings->selectedSide != s->selectedSide ||
        settings->configuration.values.quality != s->quality) {
        return invalid();
    }
    const auto identity = q.value(1).toByteArray();
    q.finish();
    const auto loaded = loadGame(s->gameId);
    const auto* game = std::get_if<StoredGame>(&loaded);
    if (!game ||
        static_cast<int>(game->document->originalMainline().size()) + 1 != s->totalPositions) {
        return invalid();
    }
    auto final = gamePosition(*game, s->totalPositions - 1);
    if (!std::holds_alternative<chess::Position>(final) ||
        positionIdentity(std::get<chess::Position>(final).context()) != identity) {
        return invalid();
    }
    if (!exec(
            q,
            QStringLiteral("SELECT count(*),min(CASE WHEN typeof(position_index)='integer' THEN "
                           "position_index ELSE -1 END),max(CASE WHEN "
                           "typeof(position_index)='integer' THEN position_index ELSE -1 END) FROM "
                           "review_positions WHERE review_id=?"),
            {static_cast<qint64>(id)}) ||
        !q.next()) {
        return storage();
    }
    if (q.value(0).toLongLong() != s->completedPositions ||
        (s->completedPositions > 0 &&
         (q.value(1).toLongLong() < 0 || q.value(2).toLongLong() >= s->totalPositions)) ||
        (s->status == JobStatus::Complete && s->completedPositions != s->totalPositions)) {
        return invalid();
    }
    q.finish();
    if (!exec(q,
              QStringLiteral(
                  "SELECT status,created_at FROM review_transitions WHERE review_id=? "
                  "AND revision=? AND typeof(status)='integer' AND typeof(created_at)='integer'"),
              {static_cast<qint64>(id), static_cast<qint64>(s->revision)}) ||
        !q.next()) {
        return invalid();
    }
    if (q.value(0).toLongLong() != static_cast<int>(s->status) ||
        q.value(1).toLongLong() != s->updatedAt.toMSecsSinceEpoch()) {
        return invalid();
    }
    if (owningGame) {
        *owningGame = *game;
    }
    return StoredReview{*s, std::move(*settings), identity};
}
ListReviewsResult StorageSession::listReviews(GameId gameId, std::int64_t offset, int limit) {
    if (!usable_) {
        return storage();
    }
    if (gameId <= 0 || offset < 0 || limit < 1 || limit > 200) {
        return error(ReviewErrorCode::InvalidInput);
    }
    QSqlQuery q(database_);
    if (!exec(q,
              summaryProjection +
                  QStringLiteral("WHERE r.game_id=? ORDER BY r.id DESC LIMIT ? OFFSET ?"),
              {static_cast<qint64>(gameId), limit, static_cast<qint64>(offset)})) {
        return storage();
    }
    std::vector<ReviewSummary> rows;
    while (q.next()) {
        auto value = summary(q);
        if (!value) {
            ReviewSummary damaged;
            damaged.id = q.value(0).toLongLong();
            damaged.metadataError = ReviewErrorCode::InvalidRecord;
            rows.push_back(std::move(damaged));
        } else {
            rows.push_back(*value);
        }
    }
    if (q.lastError().isValid()) {
        return storage();
    }
    return rows;
}
CheckpointReviewResult StorageSession::checkpointReview(const ReviewCheckpoint& request) {
    if (!usable_) {
        return storage();
    }
    if (request.expectedRevision < 0 ||
        request.expectedRevision == std::numeric_limits<std::int64_t>::max() ||
        !validStatus(request.status) || request.status == JobStatus::Queued ||
        request.status == JobStatus::Interrupted || request.failure < JobFailure::None ||
        request.failure > JobFailure::Configuration ||
        (request.status == JobStatus::Failed) != (request.failure != JobFailure::None) ||
        request.positionIndex.has_value() != (request.analysis.has_value() || request.terminal) ||
        (request.analysis && request.terminal)) {
        return error(ReviewErrorCode::InvalidInput);
    }
    ReviewTransaction tx(database_, usable_);
    if (!tx.begin()) {
        return storage();
    }
    StoredGame owningGame;
    auto loaded = loadReviewRecord(request.reviewId, &owningGame);
    if (const auto* failure = std::get_if<ReviewError>(&loaded)) {
        return *failure;
    }
    auto* saved = std::get_if<StoredReview>(&loaded);
    if (!saved) {
        return invalid();
    }
    auto& run = *saved;
    if (run.summary.revision != request.expectedRevision || !active(run.summary.status)) {
        return error(ReviewErrorCode::Conflict);
    }
    if (active(request.status) && request.status < run.summary.status) {
        return error(ReviewErrorCode::Conflict);
    }
    QSqlQuery q(database_);
    if (request.positionIndex) {
        if (*request.positionIndex < 0 || *request.positionIndex >= run.summary.totalPositions) {
            return error(ReviewErrorCode::InvalidInput);
        }
        const auto* game = &owningGame;
        const auto positionResult = gamePosition(*game, *request.positionIndex);
        const auto* position = std::get_if<chess::Position>(&positionResult);
        if (!position) {
            return invalid();
        }
        review::PositionEvidence evidence;
        QByteArray raw;
        QVariant session(QMetaType::fromType<QByteArray>()),
            attempt(QMetaType::fromType<QByteArray>());
        if (request.analysis) {
            if (!request.analysis->evidence.provenance ||
                request.analysis->evidence.provenance->position != position->context() ||
                request.analysis->evidence.provenance->fingerprint != run.settings.fingerprint) {
                return error(ReviewErrorCode::InvalidInput);
            }
            auto encoded = encodeAnalysis(*request.analysis);
            const auto* value = std::get_if<QByteArray>(&encoded);
            auto adapted = analysis::reviewEvidence(*request.analysis);
            const auto* record = std::get_if<analysis::ReviewEvidenceRecord>(&adapted);
            if (!value || !record) {
                return error(ReviewErrorCode::InvalidInput);
            }
            raw = *value;
            evidence = record->evidence;
            session = request.analysis->evidence.session.toRfc4122();
            attempt = request.analysis->evidence.request.toRfc4122();
        } else {
            evidence = review::terminalEvidence(*position);
            if (!evidence.terminal) {
                return error(ReviewErrorCode::InvalidInput);
            }
        }
        const auto adapted =
            adaptedDocument(evidence, run.settings.configuration.values.conversion);
        if (adapted.size() > maxAnalysisBytes) {
            return error(ReviewErrorCode::InvalidInput);
        }
        if (!exec(q,
                  QStringLiteral("SELECT id,CASE WHEN typeof(position_index)='integer' THEN "
                                 "position_index END,substr(CAST(analysis AS "
                                 "BLOB),1,16777217),substr(CAST(adapted AS "
                                 "BLOB),1,16777217),CASE WHEN typeof(format_version)='integer' "
                                 "THEN format_version END FROM review_evidence "
                                 "WHERE review_id=? AND ((session=? AND attempt=?) OR (session IS "
                                 "NULL AND attempt IS NULL AND position_index=?))"),
                  {static_cast<qint64>(request.reviewId), session, attempt,
                   *request.positionIndex})) {
            return storage();
        }
        qint64 evidenceId = 0;
        if (q.next()) {
            evidenceId = q.value(0).toLongLong();
            if (q.value(1).toLongLong() != *request.positionIndex ||
                q.value(2).toByteArray() != raw || q.value(3).toByteArray() != adapted ||
                q.value(4).toLongLong() != reviewFormatVersion) {
                return error(ReviewErrorCode::Conflict);
            }
            if (q.next() || q.lastError().isValid()) {
                return invalid();
            }
        } else if (q.lastError().isValid()) {
            return storage();
        }
        q.finish();
        if (evidenceId == 0) {
            if (!exec(
                    q,
                    QStringLiteral("INSERT INTO "
                                   "review_evidence(review_id,position_index,session,attempt,"
                                   "analysis,adapted,format_version) VALUES(?,?,?,?,?,?,?)"),
                    {static_cast<qint64>(request.reviewId), *request.positionIndex, session,
                     attempt,
                     request.analysis ? QVariant(raw) : QVariant(QMetaType::fromType<QByteArray>()),
                     adapted, reviewFormatVersion}) ||
                q.numRowsAffected() != 1) {
                return storage();
            }
            evidenceId = q.lastInsertId().toLongLong();
            q.finish();
        }
        if (!exec(
                q,
                QStringLiteral("INSERT INTO review_positions(review_id,position_index,evidence_id) "
                               "VALUES(?,?,?) ON CONFLICT(review_id,position_index) DO UPDATE SET "
                               "evidence_id=excluded.evidence_id"),
                {static_cast<qint64>(request.reviewId), *request.positionIndex, evidenceId}) ||
            q.numRowsAffected() != 1) {
            return storage();
        }
        q.finish();
        // Validate persisted bytes before committing; triggers cannot silently substitute evidence.
        if (!std::holds_alternative<StoredReviewEvidence>(
                readEvidence(database_, run, *position, *request.positionIndex)) ||
            !updatePair(database_, run, *game, *request.positionIndex) ||
            !updatePair(database_, run, *game, *request.positionIndex + 1)) {
            return storage();
        }
    }
    if (!exec(q, QStringLiteral("SELECT count(*) FROM review_positions WHERE review_id=?"),
              {static_cast<qint64>(request.reviewId)}) ||
        !q.next()) {
        return storage();
    }
    const auto completedCount = q.value(0).toLongLong();
    if (completedCount < 0 || completedCount > run.summary.totalPositions) {
        return invalid();
    }
    const auto completed = static_cast<int>(completedCount);
    q.finish();
    if (request.status == JobStatus::Complete && completed != run.summary.totalPositions) {
        return error(ReviewErrorCode::InvalidInput);
    }
    const auto time = std::max(now(), run.summary.updatedAt.toMSecsSinceEpoch());
    if (!exec(q,
              QStringLiteral("UPDATE review_jobs SET "
                             "status=?,revision=revision+1,completed_positions=?,updated_at=? "
                             "WHERE review_id=? AND revision=?"),
              {static_cast<int>(request.status), completed, time,
               static_cast<qint64>(request.reviewId),
               static_cast<qint64>(request.expectedRevision)}) ||
        q.numRowsAffected() != 1) {
        return storage();
    }
    q.finish();
    if (!transition(database_, request.reviewId, request.expectedRevision + 1, run.summary.status,
                    request.status, request.failure, time)) {
        return storage();
    }
    // Stream completion validation with one game and two adjacent records, not a full-run load per
    // position.
    if (request.status == JobStatus::Complete) {
        const auto* game = &owningGame;
        auto root = gamePosition(*game, 0);
        auto* position = std::get_if<chess::Position>(&root);
        if (!position) {
            return invalid();
        }
        auto previous = readEvidence(database_, run, *position, 0);
        if (const auto* e = std::get_if<ReviewError>(&previous)) {
            return *e;
        }
        for (int index = 1; index < run.summary.totalPositions; ++index) {
            const auto beforePosition = *position;
            const auto move =
                game->document->originalMainline()[static_cast<std::size_t>(index - 1)];
            if (std::holds_alternative<chess::Error>(position->play(move))) {
                return invalid();
            }
            auto current = readEvidence(database_, run, *position, index);
            if (const auto* e = std::get_if<ReviewError>(&current)) {
                return *e;
            }
            const auto pair = readPair(database_, run, beforePosition, move, index,
                                       std::get<StoredReviewEvidence>(previous),
                                       std::get<StoredReviewEvidence>(current));
            if (const auto* e = std::get_if<ReviewError>(&pair)) {
                return *e;
            }
            previous = std::move(current);
        }
    }
    if (!tx.commit()) {
        return storage();
    }
    run.summary.revision++;
    run.summary.status = request.status;
    run.summary.completedPositions = completed;
    run.summary.updatedAt = timestamp(time);
    return run.summary;
}
ContinueReviewResult StorageSession::continueReview(const ContinueReview& request) {
    if (!usable_) {
        return storage();
    }
    ReviewTransaction tx(database_, usable_);
    if (!tx.begin()) {
        return storage();
    }
    auto loaded = loadReview(request.reviewId);
    if (const auto* failure = std::get_if<ReviewError>(&loaded)) {
        return *failure;
    }
    auto* saved = std::get_if<StoredReview>(&loaded);
    if (!saved) {
        return invalid();
    }
    auto& run = *saved;
    if (request.expectedRevision < 0 ||
        request.expectedRevision == std::numeric_limits<std::int64_t>::max() ||
        run.summary.revision != request.expectedRevision || !resumable(run.summary.status) ||
        request.fingerprint != run.settings.fingerprint ||
        !sameConfiguration(request.configuration, run.settings.configuration)) {
        return error(ReviewErrorCode::Conflict);
    }
    const auto time = std::max(now(), run.summary.updatedAt.toMSecsSinceEpoch());
    QSqlQuery q(database_);
    if (!exec(q,
              QStringLiteral("UPDATE review_jobs SET status=0,revision=revision+1,updated_at=? "
                             "WHERE review_id=? AND revision=?"),
              {time, static_cast<qint64>(request.reviewId),
               static_cast<qint64>(request.expectedRevision)}) ||
        q.numRowsAffected() != 1) {
        return storage();
    }
    q.finish();
    if (!transition(database_, request.reviewId, request.expectedRevision + 1, run.summary.status,
                    JobStatus::Queued, JobFailure::None, time) ||
        !tx.commit()) {
        return storage();
    }
    ++run.summary.revision;
    run.summary.status = JobStatus::Queued;
    run.summary.updatedAt = timestamp(time);
    return run.summary;
}
LoadReviewEvidenceResult StorageSession::loadReviewEvidence(ReviewId id, int index) {
    StoredGame owningGame;
    const auto loaded = loadReviewRecord(id, &owningGame);
    if (const auto* e = std::get_if<ReviewError>(&loaded)) {
        return *e;
    }
    const auto& run = std::get<StoredReview>(loaded);
    if (index < 0 || index >= run.summary.totalPositions) {
        return error(ReviewErrorCode::InvalidInput);
    }
    const auto* game = &owningGame;
    auto restored = gamePosition(*game, index);
    auto* position = std::get_if<chess::Position>(&restored);
    if (!position) {
        return invalid();
    }
    auto result = readEvidence(database_, run, *position, index);
    auto* value = std::get_if<StoredReviewEvidence>(&result);
    if (!value || index == 0) {
        return result;
    }
    if (!position->undo()) {
        return invalid();
    }
    auto before = readEvidence(database_, run, *position, index - 1);
    if (const auto* e = std::get_if<ReviewError>(&before)) {
        if (e->code == ReviewErrorCode::Missing) {
            return result;
        }
        return *e;
    }
    const auto& previous = std::get<StoredReviewEvidence>(before);
    auto paired = readPair(database_, run, *position,
                           game->document->originalMainline()[static_cast<std::size_t>(index - 1)],
                           index, previous, *value);
    if (const auto* e = std::get_if<ReviewError>(&paired)) {
        return *e;
    }
    value->precedingMove = std::get<review::MoveEvidence>(std::move(paired));
    return result;
}
ListReviewTransitionsResult StorageSession::listReviewTransitions(ReviewId id, std::int64_t offset,
                                                                  int limit) {
    if (!usable_) {
        return storage();
    }
    if (id <= 0 || offset < 0 || limit < 1 || limit > 200) {
        return error(ReviewErrorCode::InvalidInput);
    }
    QSqlQuery q(database_);
    if (!exec(
            q,
            QStringLiteral(
                "SELECT CASE WHEN typeof(revision)='integer' THEN revision END,"
                "CASE WHEN typeof(previous)='integer' THEN previous END,"
                "CASE WHEN typeof(status)='integer' THEN status END,"
                "CASE WHEN typeof(failure)='integer' THEN failure END,"
                "CASE WHEN typeof(created_at)='integer' THEN created_at "
                "END,typeof(revision)='integer' AND "
                "(previous IS NULL OR typeof(previous)='integer') AND typeof(status)='integer' AND "
                "typeof(failure)='integer' AND typeof(created_at)='integer' FROM "
                "review_transitions WHERE review_id=? ORDER BY revision LIMIT ? OFFSET ?"),
            {static_cast<qint64>(id), limit, static_cast<qint64>(offset)})) {
        return storage();
    }
    std::vector<ReviewTransition> rows;
    while (q.next()) {
        if (!q.value(5).toBool() || q.value(0).toLongLong() < 0 ||
            (!q.value(1).isNull() &&
             (q.value(1).toLongLong() < 0 || q.value(1).toLongLong() > 9)) ||
            q.value(2).toLongLong() < 0 || q.value(2).toLongLong() > 9 ||
            q.value(3).toLongLong() < 0 || q.value(3).toLongLong() > 4 ||
            q.value(4).toLongLong() < 1 || q.value(4).toLongLong() > 253402300799999LL) {
            return invalid();
        }
        rows.push_back(
            {q.value(0).toLongLong(),
             q.value(1).isNull() ? std::nullopt
                                 : std::optional(static_cast<JobStatus>(q.value(1).toInt())),
             static_cast<JobStatus>(q.value(2).toInt()),
             static_cast<JobFailure>(q.value(3).toInt()), timestamp(q.value(4).toLongLong())});
    }
    if (q.lastError().isValid()) {
        return storage();
    }
    return rows;
}
std::optional<DatabaseError> StorageSession::interruptReviewJobs(const QString& path) {
    const auto failure = [&]() {
        return DatabaseError{
            DatabaseErrorCode::Open,
            QStringLiteral("Recover review jobs"),
            QStringLiteral("Unfinished review jobs could not be marked interrupted."),
            {},
            path,
            {},
            {}};
    };
    ReviewTransaction tx(database_, usable_);
    if (!tx.begin()) {
        return failure();
    }
    // Keyset iteration bounds memory independently of retained job count.
    qint64 after = 0;
    while (true) {
        QSqlQuery q(database_);
        if (!exec(q,
                  summaryProjection +
                      QStringLiteral(
                          "WHERE r.id>? AND j.status BETWEEN 0 AND 4 ORDER BY r.id LIMIT 1"),
                  {after})) {
            return failure();
        }
        if (!q.next()) {
            if (q.lastError().isValid()) {
                return failure();
            }
            break;
        }
        const auto s = summary(q);
        q.finish();
        if (!s || s->revision == std::numeric_limits<std::int64_t>::max()) {
            return failure();
        }
        after = s->id;
        const auto time = std::max(now(), s->updatedAt.toMSecsSinceEpoch());
        if (!exec(q,
                  QStringLiteral("UPDATE review_jobs SET status=8,revision=revision+1,updated_at=? "
                                 "WHERE review_id=? AND revision=?"),
                  {time, static_cast<qint64>(s->id), static_cast<qint64>(s->revision)}) ||
            q.numRowsAffected() != 1) {
            return failure();
        }
        q.finish();
        if (!transition(database_, s->id, s->revision + 1, s->status, JobStatus::Interrupted,
                        JobFailure::None, time)) {
            return failure();
        }
    }
    if (!tx.commit()) {
        return failure();
    }
    return std::nullopt;
}
} // namespace chess_review::persistence::detail
