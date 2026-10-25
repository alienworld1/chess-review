#include "persistence/private/StorageSession.h"
#include <QCryptographicHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace chess_review::persistence::detail {
namespace {
CacheError storage() {
    return {CacheErrorCode::Storage, QStringLiteral("Analysis cache storage failed.")};
}
class CacheTransaction final {
  public:
    CacheTransaction(QSqlDatabase& database, bool& usable) : database_(database), usable_(usable) {}
    ~CacheTransaction() {
        if (active_ && !database_.rollback()) {
            usable_ = false;
        }
    }
    bool begin() {
        QSqlQuery query(database_);
        active_ = query.exec(QStringLiteral("BEGIN IMMEDIATE"));
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
// No unbounded database value is materialized, including malformed TEXT masquerading as BLOB.
const QString projection = QStringLiteral(
    "SELECT id, substr(CAST(session AS BLOB),1,17), substr(CAST(attempt AS BLOB),1,17), "
    "substr(CAST(position_key AS BLOB),1,33), substr(CAST(engine_fingerprint AS BLOB),1,33), "
    "substr(CAST(position_identity AS BLOB),1,1048577), substr(CAST(raw_result_blob AS "
    "BLOB),1,16777217), "
    "result_format_version=1 AND typeof(result_format_version)='integer' AND "
    "typeof(session)='blob' AND typeof(attempt)='blob' "
    "AND typeof(position_key)='blob' AND typeof(engine_fingerprint)='blob' AND "
    "typeof(position_identity)='blob' AND typeof(raw_result_blob)='blob' "
    "FROM analysis_results ");
struct Record {
    qint64 id;
    QByteArray payload;
    engine::AnalysisResult result;
};
std::optional<Record> readRecord(const QSqlQuery& query) {
    if (!query.value(7).toBool()) {
        return std::nullopt;
    }
    const auto bytes = query.value(6).toByteArray();
    auto result = decodeAnalysis(bytes);
    if (!result) {
        return std::nullopt;
    }
    const auto& s = result->evidence;
    const auto identity = positionIdentity(s.provenance->position);
    if (query.value(0).toLongLong() <= 0 || query.value(1).toByteArray() != s.session.toRfc4122() ||
        query.value(2).toByteArray() != s.request.toRfc4122() ||
        query.value(5).toByteArray() != identity ||
        query.value(3).toByteArray() !=
            QCryptographicHash::hash(identity, QCryptographicHash::Sha256) ||
        query.value(4).toByteArray() != s.provenance->fingerprint.digest) {
        return std::nullopt;
    }
    return Record{query.value(0).toLongLong(), bytes, std::move(*result)};
}
bool removeRecord(QSqlDatabase& database, qint64 id) {
    QSqlQuery query(database);
    if (!query.prepare(QStringLiteral("DELETE FROM analysis_results WHERE id=?"))) {
        return false;
    }
    query.addBindValue(id);
    return query.exec() && query.numRowsAffected() == 1;
}
} // namespace
StoreAnalysisResult StorageSession::storeAnalysis(const engine::AnalysisResult& result,
                                                  qint64 budget, qint64 recordLimit) {
    if (!usable_ || budget <= 0 || budget > analysisBudgetBytes || recordLimit <= 0 ||
        recordLimit > maxAnalysisRecords) {
        return storage();
    }
    const auto encoded = encodeAnalysis(result);
    const auto* payload = std::get_if<QByteArray>(&encoded);
    if (!payload) {
        return std::get<CacheError>(encoded);
    }
    const auto& s = result.evidence;
    const auto identity = positionIdentity(s.provenance->position);
    if (identity.size() + payload->size() > budget) {
        return CacheError{CacheErrorCode::InvalidInput,
                          QStringLiteral("Analysis exceeds the cache budget.")};
    }
    const auto positionKey = QCryptographicHash::hash(identity, QCryptographicHash::Sha256);
    CacheTransaction transaction(database_, usable_);
    if (!transaction.begin()) {
        return storage();
    }
    QSqlQuery existing(database_);
    if (!existing.prepare(projection + QStringLiteral("WHERE session=? AND attempt=?"))) {
        return storage();
    }
    existing.addBindValue(s.session.toRfc4122());
    existing.addBindValue(s.request.toRfc4122());
    if (!existing.exec()) {
        return storage();
    }
    if (existing.next()) {
        auto record = readRecord(existing);
        existing.finish();
        if (!record || record->payload != *payload) {
            return CacheError{CacheErrorCode::Conflict,
                              QStringLiteral("Conflicting analysis for an existing attempt.")};
        }
        if (!transaction.commit()) {
            return storage();
        }
        return AnalysisStored{record->id, true};
    }
    if (existing.lastError().isValid()) {
        return storage();
    }
    existing.finish();
    QSqlQuery insert(database_);
    if (!insert.prepare(QStringLiteral(
            "INSERT INTO "
            "analysis_results(session,attempt,position_key,engine_fingerprint,position_identity,"
            "raw_result_blob,result_format_version) VALUES(?,?,?,?,?,?,1)"))) {
        return storage();
    }
    insert.addBindValue(s.session.toRfc4122());
    insert.addBindValue(s.request.toRfc4122());
    insert.addBindValue(positionKey);
    insert.addBindValue(s.provenance->fingerprint.digest);
    insert.addBindValue(identity);
    insert.addBindValue(*payload);
    if (!insert.exec() || insert.numRowsAffected() != 1) {
        return storage();
    }
    const qint64 id = insert.lastInsertId().toLongLong();
    insert.finish();
    // Read back through the validating reader before acknowledging a committed write.
    QSqlQuery verify(database_);
    if (!verify.prepare(projection + QStringLiteral("WHERE id=?"))) {
        return storage();
    }
    verify.addBindValue(id);
    if (!verify.exec() || !verify.next()) {
        return storage();
    }
    const auto saved = readRecord(verify);
    if (!saved || saved->payload != *payload) {
        return storage();
    }
    verify.finish();
    QSqlQuery totals(database_);
    if (!totals.exec(QStringLiteral(
            "SELECT count(*), coalesce(sum(length(CAST(position_identity AS "
            "BLOB))+length(CAST(raw_result_blob AS BLOB))),0) FROM analysis_results")) ||
        !totals.next()) {
        return storage();
    }
    qint64 count = totals.value(0).toLongLong();
    qint64 bytes = totals.value(1).toLongLong();
    totals.finish();
    while (count > recordLimit || bytes > budget) {
        QSqlQuery oldest(database_);
        if (!oldest.exec(QStringLiteral(
                "SELECT id,length(CAST(position_identity AS BLOB))+length(CAST(raw_result_blob AS "
                "BLOB)) FROM analysis_results ORDER BY id LIMIT 1")) ||
            !oldest.next()) {
            return storage();
        }
        const qint64 victim = oldest.value(0).toLongLong();
        const qint64 size = oldest.value(1).toLongLong();
        oldest.finish();
        if (victim == id || !removeRecord(database_, victim)) {
            return storage();
        }
        --count;
        bytes -= size;
    }
    if (!transaction.commit()) {
        return storage();
    }
    return AnalysisStored{id, false};
}
FindAnalysisResult StorageSession::findAnalysis(const engine::CacheQuery& query) {
    if (!usable_) {
        return storage();
    }
    if (engine::validateCacheQuery(query)) {
        return CacheError{CacheErrorCode::InvalidInput,
                          QStringLiteral("Invalid analysis cache query.")};
    }
    CacheLookup lookup;
    if (!engine::verifiedResources(query.fingerprint)) {
        return lookup;
    }
    const auto identity = positionIdentity(query.position);
    const auto positionKey = QCryptographicHash::hash(identity, QCryptographicHash::Sha256);
    CacheTransaction transaction(database_, usable_);
    if (!transaction.begin()) {
        return storage();
    }
    qint64 after = 0;
    std::uint64_t bestStrength = 0;
    // Keyset iteration bounds live memory without a candidate cap that could hide a valid hit.
    while (true) {
        QSqlQuery candidate(database_);
        candidate.setForwardOnly(true);
        if (!candidate.prepare(projection +
                               QStringLiteral("WHERE position_key=? AND engine_fingerprint=? AND "
                                              "id>? ORDER BY id LIMIT 1"))) {
            return storage();
        }
        candidate.addBindValue(positionKey);
        candidate.addBindValue(query.fingerprint.digest);
        candidate.addBindValue(after);
        if (!candidate.exec()) {
            return storage();
        }
        if (!candidate.next()) {
            if (candidate.lastError().isValid()) {
                return storage();
            }
            break;
        }
        after = candidate.value(0).toLongLong();
        auto record = readRecord(candidate);
        candidate.finish();
        if (!record) {
            if (!removeRecord(database_, after)) {
                return storage();
            }
            ++lookup.discardedRecords;
            continue;
        }
        if (engine::cacheDominates(record->result, query)) {
            const auto strength = engine::cacheStrength(record->result);
            if (!lookup.hit || strength >= bestStrength) {
                lookup.hit = std::move(record->result);
                bestStrength = strength;
            }
        }
    }
    if (!transaction.commit()) {
        return storage();
    }
    return lookup;
}
} // namespace chess_review::persistence::detail
