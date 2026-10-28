#include "engine/AnalysisCache.h"
#include "engine/EffectiveSearch.h"
#include "persistence/private/ReviewCodec.h"
#include "persistence/private/StorageSession.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QTimeZone>
#include <QVariant>
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <set>
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
    "FROM reviews r LEFT JOIN review_jobs j ON j.review_id=r.id ");
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
std::variant<StoredReview, ReviewError> readReviewHeader(QSqlDatabase& db,
                                                         const ReviewSummary& snapshot) {
    QSqlQuery q(db);
    const auto* s = &snapshot;
    const auto id = s->id;
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
    if (((*doc)["formatVersion"].toInt(-1) != 1 && (*doc)["formatVersion"].toInt(-1) != 2) ||
        (*doc)["configurationVersion"].toInt(-1) != 1 ||
        (*doc)["evidenceModelVersion"].toInt(-1) !=
            static_cast<int>(review::evidenceModelVersion) ||
        (*doc)["expectedScoreModelVersion"].toInt(-1) !=
            static_cast<int>(review::expectedScoreModelVersion)) {
        return error(ReviewErrorCode::UnsupportedVersion);
    }
    if (doc->contains("pipelineVersion") && ((*doc)["pipelineVersion"].toInt(-1) != 1 ||
                                             (*doc)["accuracyModelVersion"].toInt(-1) != 1 ||
                                             (*doc)["stabilityModelVersion"].toInt(-1) != 1 ||
                                             (*doc)["reviewModelVersion"].toInt(-1) != 1)) {
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
    if (identity.isEmpty() || identity.size() > 1024 * 1024) {
        return invalid();
    }
    q.finish();
    return StoredReview{snapshot, std::move(*settings), identity};
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
bool validTransition(JobStatus previous, JobStatus status) {
    if (active(previous)) {
        return validStatus(status) && status != JobStatus::Queued &&
               (!active(status) || status >= previous);
    }
    return resumable(previous) && status == JobStatus::Queued;
}
const QString transitionProjection = QStringLiteral(
    "SELECT CASE WHEN typeof(t.revision)='integer' THEN t.revision END,"
    "CASE WHEN typeof(t.previous)='integer' THEN t.previous END,"
    "CASE WHEN typeof(t.status)='integer' THEN t.status END,"
    "CASE WHEN typeof(t.failure)='integer' THEN t.failure END,"
    "CASE WHEN typeof(t.created_at)='integer' THEN t.created_at END,"
    "typeof(t.revision)='integer' AND (t.previous IS NULL OR typeof(t.previous)='integer') AND "
    "typeof(t.status)='integer' AND typeof(t.failure)='integer' AND typeof(t.created_at)='integer',"
    "CASE WHEN typeof(p.status)='integer' THEN p.status END,"
    "CASE WHEN typeof(p.created_at)='integer' THEN p.created_at END,"
    "typeof(p.revision)='integer' AND typeof(p.status)='integer' AND "
    "typeof(p.created_at)='integer' "
    "FROM review_transitions t LEFT JOIN review_transitions p ON p.review_id=t.review_id AND "
    "p.revision=t.revision-1 ");
std::optional<ReviewTransition> readTransition(const QSqlQuery& q) {
    if (!q.value(5).toBool() || q.value(0).toLongLong() < 0 ||
        (!q.value(1).isNull() && (q.value(1).toLongLong() < 0 || q.value(1).toLongLong() > 9)) ||
        q.value(2).toLongLong() < 0 || q.value(2).toLongLong() > 9 || q.value(3).toLongLong() < 0 ||
        q.value(3).toLongLong() > 4 || q.value(4).toLongLong() < 1 ||
        q.value(4).toLongLong() > 253402300799999LL) {
        return std::nullopt;
    }
    ReviewTransition out{
        q.value(0).toLongLong(),
        q.value(1).isNull() ? std::nullopt
                            : std::optional(static_cast<JobStatus>(q.value(1).toInt())),
        static_cast<JobStatus>(q.value(2).toInt()), static_cast<JobFailure>(q.value(3).toInt()),
        timestamp(q.value(4).toLongLong())};
    if ((out.status == JobStatus::Failed) != (out.failure != JobFailure::None)) {
        return std::nullopt;
    }
    if (out.revision == 0) {
        if (out.previous || out.status != JobStatus::Queued) {
            return std::nullopt;
        }
    } else if (!out.previous || !q.value(8).toBool() ||
               q.value(6).toLongLong() != static_cast<int>(*out.previous) ||
               q.value(7).toLongLong() < 1 || q.value(7).toLongLong() > q.value(4).toLongLong() ||
               !validTransition(*out.previous, out.status)) {
        return std::nullopt;
    }
    return out;
}
std::optional<ReviewError> checkCurrentTransition(QSqlDatabase& db, const ReviewSummary& snapshot) {
    QSqlQuery q(db);
    // Descending key lookup also rejects unexpected future revisions without scanning history.
    if (!exec(q,
              transitionProjection +
                  QStringLiteral("WHERE t.review_id=? ORDER BY t.revision DESC LIMIT 1"),
              {static_cast<qint64>(snapshot.id)})) {
        return storage();
    }
    if (!q.next()) {
        return q.lastError().isValid() ? storage() : invalid();
    }
    const auto current = readTransition(q);
    if (!current || current->revision != snapshot.revision || current->status != snapshot.status ||
        current->createdAt != snapshot.updatedAt ||
        (current->revision == 0 && current->createdAt != snapshot.createdAt)) {
        return invalid();
    }
    return std::nullopt;
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
bool stageAnalysisValid(const StoredReview& run, const chess::Position& position,
                        const engine::AnalysisResult& result, review::EvidenceStage stage) {
    if (!result.evidence.provenance) {
        return false;
    }
    if (stage == review::EvidenceStage::Quick) {
        return result.evidence.provenance->fingerprint == run.settings.fingerprint;
    }
    const int width = result.evidence.requestedMultiPv;
    const auto& policy = run.settings.configuration.values;
    const auto& budget = policy.budgets[static_cast<std::size_t>(policy.quality)];
    const auto* nodes = std::get_if<engine::NodeLimit>(&result.evidence.limit);
    return run.settings.refinement && width >= 1 && width <= policy.refinement.multiPv &&
           width <= static_cast<int>(position.legalMoves().size()) &&
           result.evidence.effectiveMultiPv == width && !result.evidence.provenance->searchMoves &&
           nodes &&
           nodes->nodes >= (stage == review::EvidenceStage::Deepening ? budget.deepeningNodes
                                                                      : budget.verificationNodes) &&
           engine::reviewWidthCompatible(run.settings.fingerprint,
                                         result.evidence.provenance->fingerprint, width);
}
std::variant<StoredReviewEvidence, ReviewError>
readEvidence(QSqlDatabase& db, const StoredReview& run, const chess::Position& position, int index,
             review::EvidenceStage stage = review::EvidenceStage::Quick) {
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
                             "FROM %1 p LEFT JOIN review_evidence e ON "
                             "e.review_id=p.review_id AND e.id=p.evidence_id "
                             "WHERE p.review_id=? AND p.position_index=?")
                  .arg(stage == review::EvidenceStage::Quick
                           ? QStringLiteral("review_positions")
                           : QStringLiteral("(SELECT * FROM review_stage_positions WHERE stage=%1)")
                                 .arg(static_cast<int>(stage))),
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
    const auto adaptedSnapshot = readReviewDocument(q.value(5).toByteArray(), maxAnalysisBytes);
    if (!adaptedSnapshot) {
        return invalid();
    }
    if ((*adaptedSnapshot)["formatVersion"].toInt(-1) != reviewFormatVersion ||
        (*adaptedSnapshot)["evidence"].toObject()["modelVersion"].toInt(-1) !=
            static_cast<int>(review::evidenceModelVersion)) {
        return error(ReviewErrorCode::UnsupportedVersion);
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
            !stageAnalysisValid(run, position, *decoded, stage) ||
            decoded->evidence.session.toRfc4122() != q.value(2).toByteArray() ||
            decoded->evidence.request.toRfc4122() != q.value(3).toByteArray()) {
            return invalid();
        }
        auto adapted = analysis::reviewEvidence(*decoded, &position);
        const auto* value = std::get_if<analysis::ReviewEvidenceRecord>(&adapted);
        if (!value) {
            return invalid();
        }
        out.evidence = value->evidence;
        out.analysis = std::move(*decoded);
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
    auto paired = review::pairEvidence(beforePosition, move, before.evidence, after.evidence,
                                       run.settings.configuration.values.conversion);
    if (bytes != pairedDocument(paired)) {
        return invalid();
    }
    return paired;
}
struct RefinedPair {
    StoredReviewEvidence before;
    StoredReviewEvidence after;
    review::MoveEvidence pair;
    review::ClassificationResult classification;
};
std::variant<std::optional<RefinedPair>, ReviewError>
refinedPair(QSqlDatabase& db, const StoredReview& run, const chess::Position& position,
            chess::Move move, int index, review::EvidenceStage stage) {
    auto afterPosition = position;
    if (std::holds_alternative<chess::Error>(afterPosition.play(move))) {
        return invalid();
    }
    auto before = readEvidence(db, run, position, index - 1, stage);
    auto after = readEvidence(db, run, afterPosition, index, stage);
    for (const auto* value : {&before, &after}) {
        if (const auto* e = std::get_if<ReviewError>(value);
            e && e->code != ReviewErrorCode::Missing) {
            return *e;
        }
    }
    if (!std::holds_alternative<StoredReviewEvidence>(before) ||
        !std::holds_alternative<StoredReviewEvidence>(after)) {
        return std::optional<RefinedPair>{};
    }
    RefinedPair out;
    out.before = std::get<StoredReviewEvidence>(std::move(before));
    out.after = std::get<StoredReviewEvidence>(std::move(after));
    auto b = out.before.evidence;
    auto a = out.after.evidence;
    // Each exact fingerprint was validated above. Only MultiPV may differ between roots.
    if (!b.terminal) {
        b.engineIdentity = run.settings.fingerprint.digest.toHex().toStdString();
    }
    if (!a.terminal) {
        a.engineIdentity = run.settings.fingerprint.digest.toHex().toStdString();
    }
    out.pair = review::pairEvidence(position, move, std::move(b), std::move(a),
                                    run.settings.configuration.values.conversion);
    out.classification =
        review::classify(position, out.pair, run.settings.configuration.values.classification,
                         run.settings.configuration.values.conversion);
    return std::optional<RefinedPair>{std::move(out)};
}
// A record is derived only from a validated pair. Existing records are never silently repaired.
std::optional<ReviewError> retainClassification(QSqlDatabase& db, const StoredReview& run,
                                                const chess::Position& position,
                                                const review::MoveEvidence& pair, int index,
                                                std::int64_t beforeId, std::int64_t afterId,
                                                bool replace, review::ClassificationResult& out) {
    QSqlQuery q(db);
    if (!exec(q,
              QStringLiteral("SELECT CASE WHEN typeof(before_id)='integer' THEN before_id END, "
                             "CASE WHEN typeof(after_id)='integer' THEN after_id END, "
                             "substr(CAST(document AS BLOB),1,65537), typeof(document)='blob' "
                             "FROM review_classifications WHERE review_id=? AND position_index=?"),
              {static_cast<qint64>(run.summary.id), index})) {
        return storage();
    }
    bool present = false;
    bool sameEvidence = false;
    QByteArray previousBytes;
    auto modelVersion = review::classificationModelVersion;
    if (q.next()) {
        present = true;
        previousBytes = q.value(2).toByteArray();
        const auto document = readReviewDocument(previousBytes, 65536);
        if (!q.value(3).toBool() || !document) {
            return invalid();
        }
        if ((*document)["formatVersion"].toInt(-1) != 1 ||
            !review::supportsClassificationModel(
                static_cast<std::uint32_t>((*document)["modelVersion"].toInt(-1)))) {
            return error(ReviewErrorCode::UnsupportedVersion);
        }
        sameEvidence = q.value(0).toLongLong() == beforeId && q.value(1).toLongLong() == afterId;
        if (!replace && !sameEvidence) {
            return invalid();
        }
        if (sameEvidence) {
            modelVersion = static_cast<std::uint32_t>((*document)["modelVersion"].toInt());
        }
    } else if (q.lastError().isValid()) {
        return storage();
    }
    q.finish();
    out = review::classify(position, pair, run.settings.configuration.values.classification,
                           run.settings.configuration.values.conversion, modelVersion);
    // Legacy settings retain their released identity; interpretation explicitly includes the new
    // default.
    const auto validated = config::validate(run.settings.configuration.values);
    const auto* configuration =
        std::get_if<std::shared_ptr<const config::ValidatedConfiguration>>(&validated);
    if (!configuration) {
        return invalid();
    }
    const auto bytes =
        classificationDocument(out, beforeId, afterId, (*configuration)->identities.classification);
    if (sameEvidence) {
        return previousBytes == bytes ? std::nullopt : std::optional{invalid()};
    }
    if (!exec(q,
              present ? QStringLiteral(
                            "UPDATE review_classifications SET before_id=?,after_id=?,document=? "
                            "WHERE review_id=? AND position_index=?")
                      : QStringLiteral("INSERT INTO "
                                       "review_classifications(before_id,after_id,document,review_"
                                       "id,position_index) VALUES(?,?,?,?,?)"),
              {static_cast<qint64>(beforeId), static_cast<qint64>(afterId), bytes,
               static_cast<qint64>(run.summary.id), index}) ||
        q.numRowsAffected() != 1) {
        return storage();
    }
    // Detect triggers substituting a document before authorizing a committed label.
    q.finish();
    if (!exec(q,
              QStringLiteral("SELECT before_id,after_id,substr(CAST(document AS "
                             "BLOB),1,65537),typeof(document)='blob' FROM review_classifications "
                             "WHERE review_id=? AND position_index=?"),
              {static_cast<qint64>(run.summary.id), index}) ||
        !q.next() || !q.value(3).toBool() || q.value(0).toLongLong() != beforeId ||
        q.value(1).toLongLong() != afterId || q.value(2).toByteArray() != bytes) {
        return storage();
    }
    return std::nullopt;
}
bool updatePair(QSqlDatabase& db, const StoredReview& run, const StoredGame& game, int index) {
    if (index <= 0 || index >= run.summary.totalPositions) {
        return true;
    }
    QSqlQuery coverage(db);
    if (!exec(coverage,
              QStringLiteral("SELECT count(*) FROM review_positions WHERE review_id=? AND "
                             "position_index IN (?,?)"),
              {static_cast<qint64>(run.summary.id), index - 1, index}) ||
        !coverage.next()) {
        return false;
    }
    // One member is the newly validated checkpoint. Do not replay history or decode it again
    // until the neighbour is actually present; an orphaned reference still counts and is rejected
    // below.
    if (coverage.value(0).toInt() != 2) {
        return true;
    }
    coverage.finish();
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
    const bool saved =
        exec(q,
             QStringLiteral(
                 "INSERT INTO review_pairs(review_id,position_index,before_id,after_id,paired) "
                 "VALUES(?,?,?,?,?) "
                 "ON CONFLICT(review_id,position_index) DO UPDATE SET "
                 "before_id=excluded.before_id,after_id=excluded.after_id,paired=excluded."
                 "paired"),
             {static_cast<qint64>(run.summary.id), index, static_cast<qint64>(b.evidenceId),
              static_cast<qint64>(a.evidenceId), bytes}) &&
        q.numRowsAffected() == 1;
    if (!saved) {
        return false;
    }
    q.finish();
    review::ClassificationResult classification;
    return !retainClassification(db, run, *position, paired, index, b.evidenceId, a.evidenceId,
                                 true, classification);
}
// Validate any existing interpretation while its original position mapping is still intact.
// A replacement must not conceal damaged derived records from the previous attempts.
std::optional<ReviewError> validateReplacedClassifications(QSqlDatabase& db,
                                                           const StoredReview& run,
                                                           const StoredGame& game, int index) {
    for (const int end : {index, index + 1}) {
        if (end <= 0 || end >= run.summary.totalPositions) {
            continue;
        }
        QSqlQuery q(db);
        if (!exec(
                q,
                QStringLiteral(
                    "SELECT 1 FROM review_classifications WHERE review_id=? AND position_index=?"),
                {static_cast<qint64>(run.summary.id), end})) {
            return storage();
        }
        if (!q.next()) {
            if (q.lastError().isValid()) {
                return storage();
            }
            continue;
        }
        q.finish();
        auto restored = gamePosition(game, end - 1);
        const auto* position = std::get_if<chess::Position>(&restored);
        if (!position) {
            return invalid();
        }
        auto before = readEvidence(db, run, *position, end - 1);
        auto next = *position;
        const auto move = game.document->originalMainline()[static_cast<std::size_t>(end - 1)];
        if (std::holds_alternative<chess::Error>(next.play(move))) {
            return invalid();
        }
        auto after = readEvidence(db, run, next, end);
        const auto* b = std::get_if<StoredReviewEvidence>(&before);
        const auto* a = std::get_if<StoredReviewEvidence>(&after);
        if (!b || !a) {
            return invalid();
        }
        const auto pair = readPair(db, run, *position, move, end, *b, *a);
        if (const auto* e = std::get_if<ReviewError>(&pair)) {
            return *e;
        }
        review::ClassificationResult result;
        if (const auto e =
                retainClassification(db, run, *position, std::get<review::MoveEvidence>(pair), end,
                                     b->evidenceId, a->evidenceId, false, result)) {
            return e;
        }
    }
    return std::nullopt;
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
    auto header = readReviewHeader(database_, *s);
    if (const auto* issue = std::get_if<ReviewError>(&header)) {
        return *issue;
    }
    auto* run = std::get_if<StoredReview>(&header);
    if (!run) {
        return invalid();
    }
    const auto loaded = loadGame(s->gameId);
    const auto* game = std::get_if<StoredGame>(&loaded);
    if (!game ||
        static_cast<int>(game->document->originalMainline().size()) + 1 != s->totalPositions) {
        return invalid();
    }
    auto final = gamePosition(*game, s->totalPositions - 1);
    if (!std::holds_alternative<chess::Position>(final) ||
        positionIdentity(std::get<chess::Position>(final).context()) != run->mainlineIdentity) {
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
    // Stage mappings can only follow a complete quick scan. Validate their shape
    // even when the game has no moves, so a summary cannot hide orphaned roots.
    if (!exec(q,
              QStringLiteral(
                  "SELECT 1 FROM review_stage_positions p LEFT JOIN review_evidence e "
                  "ON e.review_id=p.review_id AND e.id=p.evidence_id WHERE p.review_id=? "
                  "AND (?=0 OR ?<>? OR typeof(p.stage)<>'integer' OR p.stage NOT IN (1,2) "
                  "OR typeof(p.position_index)<>'integer' OR p.position_index<0 OR "
                  "p.position_index>=? OR typeof(p.evidence_id)<>'integer' OR e.id IS NULL "
                  "OR typeof(e.position_index)<>'integer' OR e.position_index<>p.position_index) "
                  "LIMIT 1"),
              {static_cast<qint64>(id), run->settings.refinement ? 1 : 0, s->completedPositions,
               s->totalPositions, s->totalPositions})) {
        return storage();
    }
    if (q.next()) {
        return invalid();
    }
    if (q.lastError().isValid()) {
        return storage();
    }
    q.finish();
    if (const auto failure = checkCurrentTransition(database_, *s)) {
        return *failure;
    }
    if (owningGame) {
        *owningGame = *game;
    }
    return std::move(*run);
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
    if (request.stage < review::EvidenceStage::Quick ||
        request.stage > review::EvidenceStage::Verification || request.expectedRevision < 0 ||
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
    if (request.stage != review::EvidenceStage::Quick &&
        (!run.settings.refinement || request.status != JobStatus::Deepening ||
         run.summary.completedPositions != run.summary.totalPositions)) {
        return error(ReviewErrorCode::InvalidInput);
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
                !stageAnalysisValid(run, *position, *request.analysis, request.stage)) {
                return error(ReviewErrorCode::InvalidInput);
            }
            auto encoded = encodeAnalysis(*request.analysis);
            const auto* value = std::get_if<QByteArray>(&encoded);
            auto adapted = analysis::reviewEvidence(*request.analysis, position);
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
        if (request.stage != review::EvidenceStage::Quick) {
            if (!exec(q,
                      QStringLiteral(
                          "INSERT INTO "
                          "review_stage_positions(review_id,stage,position_index,evidence_id) "
                          "VALUES(?,?,?,?) ON CONFLICT(review_id,stage,position_index) DO NOTHING"),
                      {static_cast<qint64>(request.reviewId), static_cast<int>(request.stage),
                       *request.positionIndex, evidenceId})) {
                return storage();
            }
            const auto checked =
                readEvidence(database_, run, *position, *request.positionIndex, request.stage);
            const auto* savedEvidence = std::get_if<StoredReviewEvidence>(&checked);
            if (const auto* e = std::get_if<ReviewError>(&checked)) {
                return *e;
            }
            if (!savedEvidence || savedEvidence->evidenceId != evidenceId) {
                return error(ReviewErrorCode::Conflict);
            }
        } else {
            if (const auto e = validateReplacedClassifications(database_, run, owningGame,
                                                               *request.positionIndex)) {
                return *e;
            }
            if (!exec(
                    q,
                    QStringLiteral(
                        "INSERT INTO review_positions(review_id,position_index,evidence_id) "
                        "VALUES(?,?,?) ON CONFLICT(review_id,position_index) DO UPDATE SET "
                        "evidence_id=excluded.evidence_id"),
                    {static_cast<qint64>(request.reviewId), *request.positionIndex, evidenceId}) ||
                q.numRowsAffected() != 1) {
                return storage();
            }
            q.finish();
            // Validate persisted bytes before committing; triggers cannot silently substitute
            // evidence.
            if (!std::holds_alternative<StoredReviewEvidence>(
                    readEvidence(database_, run, *position, *request.positionIndex)) ||
                !updatePair(database_, run, *game, *request.positionIndex) ||
                !updatePair(database_, run, *game, *request.positionIndex + 1)) {
                return storage();
            }
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
            if (run.settings.refinement) {
                const auto& quickPair = std::get<review::MoveEvidence>(pair);
                const auto classified = review::classify(
                    beforePosition, quickPair, run.settings.configuration.values.classification,
                    run.settings.configuration.values.conversion);
                if (review::needsRefinement(classified, quickPair,
                                            run.settings.configuration.values.refinement)) {
                    for (const auto stage :
                         {review::EvidenceStage::Deepening, review::EvidenceStage::Verification}) {
                        const auto refined =
                            refinedPair(database_, run, beforePosition, move, index, stage);
                        if (const auto* e = std::get_if<ReviewError>(&refined)) {
                            return *e;
                        }
                        if (!std::get<std::optional<RefinedPair>>(refined)) {
                            return error(ReviewErrorCode::InvalidInput);
                        }
                    }
                }
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
namespace {
LoadReviewMovesResult readMovePage(QSqlDatabase& db, const StoredReview& run,
                                   const StoredGame& game, chess::Position& cursor, int offset,
                                   int limit) {
    const int total = run.summary.totalPositions - 1;
    auto* position = &cursor;
    ReviewMovePage page{run.summary.id, run.summary.revision, offset, total, {}};
    auto before = readEvidence(db, run, *position, offset);
    if (const auto* e = std::get_if<ReviewError>(&before);
        e && e->code != ReviewErrorCode::Missing) {
        return *e;
    }
    for (int ply = offset; ply < total && ply - offset < limit; ++ply) {
        const auto move = game.document->originalMainline()[static_cast<std::size_t>(ply)];
        const auto san = position->san(move);
        const auto* text = std::get_if<std::string>(&san);
        if (!text) {
            return invalid();
        }
        ReviewMove row;
        row.positionIndex = ply + 1;
        row.moveNumber = position->fullmoveNumber();
        row.mover = position->sideToMove();
        row.san = QString::fromStdString(*text);
        auto afterPosition = *position;
        if (std::holds_alternative<chess::Error>(afterPosition.play(move))) {
            return invalid();
        }
        auto after = readEvidence(db, run, afterPosition, ply + 1);
        for (const auto* value : {&before, &after}) {
            if (const auto* e = std::get_if<ReviewError>(value);
                e && e->code != ReviewErrorCode::Missing) {
                return *e;
            }
        }
        const auto* b = std::get_if<StoredReviewEvidence>(&before);
        const auto* a = std::get_if<StoredReviewEvidence>(&after);
        if (b && a) {
            const auto pair = readPair(db, run, *position, move, ply + 1, *b, *a);
            if (const auto* e = std::get_if<ReviewError>(&pair)) {
                return *e;
            }
            review::ClassificationResult result;
            if (const auto e =
                    retainClassification(db, run, *position, std::get<review::MoveEvidence>(pair),
                                         ply + 1, b->evidenceId, a->evidenceId, false, result)) {
                return *e;
            }
            const auto quickPair = std::get<review::MoveEvidence>(pair);
            row.refinementTarget = review::needsRefinement(
                result, quickPair, run.settings.configuration.values.refinement);
            std::optional<RefinedPair> chosen;
            if (run.settings.refinement) {
                auto deepResult = refinedPair(db, run, *position, move, ply + 1,
                                              review::EvidenceStage::Deepening);
                auto verifiedResult = refinedPair(db, run, *position, move, ply + 1,
                                                  review::EvidenceStage::Verification);
                for (const auto* value : {&deepResult, &verifiedResult}) {
                    if (const auto* e = std::get_if<ReviewError>(value)) {
                        return *e;
                    }
                }
                auto deep = std::get<std::optional<RefinedPair>>(std::move(deepResult));
                auto verified = std::get<std::optional<RefinedPair>>(std::move(verifiedResult));
                if (row.refinementTarget) {
                    row.stability.reason = review::StabilityReason::Pending;
                }
                if (deep) {
                    row.stabilityEvidenceIds[0] = deep->before.evidenceId;
                    row.stabilityEvidenceIds[1] = deep->after.evidenceId;
                    row.stabilityStrength[0] = deep->before.evidence.strength;
                    row.stabilityStrength[1] = deep->after.evidence.strength;
                }
                if (verified) {
                    row.stabilityEvidenceIds[2] = verified->before.evidenceId;
                    row.stabilityEvidenceIds[3] = verified->after.evidenceId;
                    row.stabilityStrength[2] = verified->before.evidence.strength;
                    row.stabilityStrength[3] = verified->after.evidence.strength;
                }
                if (deep && verified) {
                    row.stability = review::assessStability(
                        deep->classification, verified->classification, deep->pair, verified->pair,
                        run.settings.configuration.values.refinement);
                }
                if (verified && (verified->classification.loss || verified->pair.mate.before ||
                                 verified->pair.mate.after)) {
                    chosen = std::move(verified);
                    row.stage = review::EvidenceStage::Verification;
                } else if (deep && (deep->classification.loss || deep->pair.mate.before ||
                                    deep->pair.mate.after)) {
                    chosen = std::move(deep);
                    row.stage = review::EvidenceStage::Deepening;
                }
                if (chosen) {
                    result = chosen->classification;
                    result.stable = row.stability.stable;
                    b = &chosen->before;
                    a = &chosen->after;
                }
            }
            row.accuracyLoss = review::accuracyLoss(chosen ? chosen->pair : quickPair);
            row.classification = std::move(result);
            row.beforeEvidenceId = b->evidenceId;
            row.afterEvidenceId = a->evidenceId;
            row.rawBefore = b->evidence.evaluation;
            row.rawAfter = a->evidence.evaluation;
            row.beforeStrength = b->evidence.strength;
            row.afterStrength = a->evidence.strength;
            row.beforeAttempt = QString::fromStdString(b->evidence.attemptIdentity);
            row.afterAttempt = QString::fromStdString(a->evidence.attemptIdentity);
            row.beforeSession = QString::fromStdString(b->evidence.sessionIdentity);
            row.afterSession = QString::fromStdString(a->evidence.sessionIdentity);
            row.engineIdentity = QString::fromStdString(b->evidence.engineIdentity);
            if (row.classification->bestMove) {
                const auto best = position->san(*row.classification->bestMove);
                if (const auto* t = std::get_if<std::string>(&best)) {
                    row.bestSan = QString::fromStdString(*t);
                } else {
                    return invalid();
                }
            }
            for (const auto& alternative : row.classification->alternatives) {
                const auto alternativeSan = position->san(alternative.move);
                if (const auto* t = std::get_if<std::string>(&alternativeSan)) {
                    row.alternativeSans.push_back(QString::fromStdString(*t));
                } else {
                    return invalid();
                }
            }
        } else {
            QSqlQuery q(db);
            if (!exec(q,
                      QStringLiteral("SELECT 1 FROM review_classifications WHERE review_id=? AND "
                                     "position_index=?"),
                      {static_cast<qint64>(run.summary.id), ply + 1})) {
                return storage();
            }
            if (q.next()) {
                return invalid();
            }
            if (q.lastError().isValid()) {
                return storage();
            }
        }
        page.moves.push_back(std::move(row));
        *position = std::move(afterPosition);
        before = std::move(after);
    }
    return page;
}
} // namespace
LoadReviewMovesResult StorageSession::deriveReviewMoves(ReviewId id, std::int64_t expectedRevision,
                                                        int offset, int limit) {
    if (!usable_) {
        return storage();
    }
    if (offset < 0 || limit < 1 || limit > 50) {
        return error(ReviewErrorCode::InvalidInput);
    }
    ReviewTransaction tx(database_, usable_);
    if (!tx.begin()) {
        return storage();
    }
    StoredGame game;
    const auto loaded = loadReviewRecord(id, &game);
    if (const auto* e = std::get_if<ReviewError>(&loaded)) {
        return *e;
    }
    const auto& run = std::get<StoredReview>(loaded);
    if (run.summary.revision != expectedRevision) {
        return error(ReviewErrorCode::Conflict);
    }
    const int total = run.summary.totalPositions - 1;
    if (offset > total) {
        return error(ReviewErrorCode::InvalidInput);
    }
    auto restored = gamePosition(game, offset);
    auto* position = std::get_if<chess::Position>(&restored);
    if (!position) {
        return invalid();
    }
    auto result = readMovePage(database_, run, game, *position, offset, limit);
    if (std::holds_alternative<ReviewError>(result)) {
        return result;
    }
    if (!tx.commit()) {
        return storage();
    }
    return result;
}
LoadReviewOverviewResult StorageSession::loadReviewOverview(ReviewId id, std::int64_t revision,
                                                            int keyOffset) {
    if (keyOffset < 0 || keyOffset % 50 != 0) {
        return error(ReviewErrorCode::InvalidInput);
    }
    ReviewTransaction tx(database_, usable_);
    if (!usable_ || !tx.begin()) {
        return storage();
    }
    StoredGame game;
    const auto loaded = loadReviewRecord(id, &game);
    if (const auto* e = std::get_if<ReviewError>(&loaded)) {
        return *e;
    }
    const auto& run = std::get<StoredReview>(loaded);
    if (run.summary.revision != revision) {
        return error(ReviewErrorCode::Conflict);
    }
    ReviewOverview out;
    out.reviewId = id;
    out.revision = revision;
    out.keyOffset = keyOffset;
    out.result = game.summary.result.left(32);
    review::SummaryBuilder summaryBuilder(run.summary.selectedSide);
    std::set<int> positions;
    int keyMoments = 0;
    auto restored = gamePosition(game, 0);
    auto* position = std::get_if<chess::Position>(&restored);
    if (!position) {
        return invalid();
    }
    // Validate the root even for a zero-move game.
    const auto root = readEvidence(database_, run, *position, 0);
    if (const auto* e = std::get_if<ReviewError>(&root); e && e->code != ReviewErrorCode::Missing) {
        return *e;
    }
    for (int offset = 0; offset < run.summary.totalPositions - 1; offset += 50) {
        const auto page = readMovePage(database_, run, game, *position, offset, 50);
        if (const auto* e = std::get_if<ReviewError>(&page)) {
            return *e;
        }
        for (const auto& move : std::get<ReviewMovePage>(page).moves) {
            const auto& c = move.classification;
            const auto selection = summaryBuilder.add(
                {move.positionIndex, move.mover, c.has_value(), c && c->stable,
                 c ? c->label : std::nullopt, move.accuracyLoss, move.rawBefore, move.rawAfter});
            if (move.refinementTarget) {
                positions.insert(move.positionIndex - 1);
                positions.insert(move.positionIndex);
            }
            if (selection.selected) {
                out.keyPositions.push_back(move.positionIndex);
                if (selection.opponentContext) {
                    out.opponentContexts.push_back(move.positionIndex);
                }
                if (keyMoments >= keyOffset && out.keyMoments.size() < 50) {
                    auto moment = move;
                    moment.contextPosition = selection.opponentContext;
                    out.keyMoments.push_back(std::move(moment));
                }
                ++keyMoments;
            }
        }
    }
    out.refinementPositions.assign(positions.begin(), positions.end());
    const auto facts = summaryBuilder.finish();
    out.sides = facts.sides;
    out.totalKeyMoments = facts.keyMoments;
    out.largestLoss = facts.largestLoss;
    out.largestLossPosition = facts.largestLossPosition;
    QSqlQuery q(database_);
    if (!exec(q,
              QStringLiteral("SELECT stage,count(*) FROM review_stage_positions WHERE review_id=? "
                             "GROUP BY stage"),
              {static_cast<qint64>(id)})) {
        return storage();
    }
    while (q.next()) {
        const int stage = q.value(0).toInt();
        if (stage == 1) {
            out.deepenedPositions = q.value(1).toInt();
        } else if (stage == 2) {
            out.verifiedPositions = q.value(1).toInt();
        } else {
            return invalid();
        }
    }
    if (q.lastError().isValid()) {
        return storage();
    }
    q.finish();
    if (!tx.commit()) {
        return storage();
    }
    return out;
}
LoadReviewEvidenceResult StorageSession::loadReviewEvidence(ReviewId id, int index,
                                                            review::EvidenceStage stage) {
    StoredGame owningGame;
    const auto loaded = loadReviewRecord(id, &owningGame);
    if (const auto* e = std::get_if<ReviewError>(&loaded)) {
        return *e;
    }
    const auto& run = std::get<StoredReview>(loaded);
    if (stage < review::EvidenceStage::Quick || stage > review::EvidenceStage::Verification ||
        index < 0 || index >= run.summary.totalPositions) {
        return error(ReviewErrorCode::InvalidInput);
    }
    const auto* game = &owningGame;
    auto restored = gamePosition(*game, index);
    auto* position = std::get_if<chess::Position>(&restored);
    if (!position) {
        return invalid();
    }
    auto result = readEvidence(database_, run, *position, index, stage);
    auto* value = std::get_if<StoredReviewEvidence>(&result);
    if (!value || index == 0 || stage != review::EvidenceStage::Quick) {
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
    if (!exec(q,
              transitionProjection +
                  QStringLiteral("WHERE t.review_id=? ORDER BY t.revision LIMIT ? OFFSET ?"),
              {static_cast<qint64>(id), limit, static_cast<qint64>(offset)})) {
        return storage();
    }
    std::vector<ReviewTransition> rows;
    while (q.next()) {
        auto value = readTransition(q);
        if (!value) {
            return invalid();
        }
        rows.push_back(*value);
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
        // Recovery must not hide an unsupported header or inconsistent history beneath a fresh
        // valid tail.
        const auto header = readReviewHeader(database_, *s);
        std::optional<ReviewError> metadataFailure;
        if (const auto* issue = std::get_if<ReviewError>(&header)) {
            metadataFailure = *issue;
        } else {
            metadataFailure = checkCurrentTransition(database_, *s);
        }
        if (metadataFailure) {
            auto result = failure();
            result.operation = QStringLiteral("Validate unfinished review metadata");
            result.detail = metadataFailure->message + QStringLiteral(" Saved data is preserved.");
            return result;
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
