#include "persistence/private/StorageSession.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QStringDecoder>
#include <QVariant>

namespace chess_review::persistence::detail {
namespace {
engine::Error storage() {
    return {
        engine::ErrorCode::Storage,
        QStringLiteral(
            "Engine storage failed. No registration was confirmed; retry after checking storage.")};
}
engine::Error invalid() {
    return {engine::ErrorCode::InvalidRecord,
            QStringLiteral(
                "An engine record is invalid or too large. Existing records were preserved.")};
}
// Byte-bounded projections precede Qt materialization, including for damaged databases.
const QString projection = QStringLiteral(
    "SELECT id, substr(CAST(display_name AS BLOB),1,16385), substr(CAST(executable_path AS "
    "BLOB),1,16385), "
    "substr(executable_sha256,1,33), substr(CAST(reported_name AS BLOB),1,16385), "
    "substr(CAST(reported_author AS BLOB),1,16385), detected_version IS NULL, "
    "substr(CAST(capabilities_json AS BLOB),1,1048577), substr(CAST(option_overrides_json AS "
    "BLOB),1,3), "
    "is_default, substr(CAST(created_at AS BLOB),1,65), substr(CAST(last_validated_at AS "
    "BLOB),1,65), "
    "typeof(id)='integer' AND typeof(display_name)='text' AND typeof(executable_path)='text' AND "
    "typeof(executable_sha256)='blob' AND typeof(reported_name)='text' AND "
    "(reported_author IS NULL OR typeof(reported_author)='text') AND "
    "typeof(capabilities_json)='blob' AND "
    "typeof(option_overrides_json)='blob' AND typeof(is_default)='integer' AND "
    "typeof(created_at)='text' AND typeof(last_validated_at)='text' FROM engines ");
std::optional<QString> readText(const QSqlQuery& query, int column, qsizetype cap) {
    const auto bytes = query.value(column).toByteArray();
    if (bytes.size() > cap || !bytes.isValidUtf8() || bytes.contains('\0')) {
        return std::nullopt;
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom);
    return decoder(bytes);
}
std::optional<engine::EngineProfile> readProfile(const QSqlQuery& query) {
    const auto display = readText(query, 1, 16384);
    const auto path = readText(query, 2, 16384);
    const auto name = readText(query, 4, 16384);
    const auto author = readText(query, 5, 16384);
    const auto created = readText(query, 10, 64);
    const auto validated = readText(query, 11, 64);
    const auto capabilities = engine::decodeCapabilities(query.value(7).toByteArray());
    const auto defaultValue = query.value(9).toLongLong();
    if (!display || !path || !name || !author || !created || !validated || !capabilities ||
        !query.value(6).toBool() || !query.value(12).toBool() ||
        query.value(8).toByteArray() != QByteArray("{}") ||
        (defaultValue != 0 && defaultValue != 1)) {
        return std::nullopt;
    }
    engine::EngineProfile profile;
    profile.id = query.value(0).toLongLong();
    profile.displayName = *display;
    profile.engine.executablePath = *path;
    profile.engine.executableSha256 = query.value(3).toByteArray();
    profile.engine.reportedName = *name;
    if (!query.value(5).isNull()) {
        profile.engine.reportedAuthor = *author;
    }
    profile.engine.capabilities = *capabilities;
    profile.engine.lastValidatedAt = QDateTime::fromString(*validated, Qt::ISODateWithMs);
    profile.createdAt = QDateTime::fromString(*created, Qt::ISODateWithMs);
    profile.isDefault = defaultValue == 1;
    if (profile.id <= 0 || profile.displayName != profile.engine.reportedName ||
        !created->endsWith(u'Z') || !validated->endsWith(u'Z') || !profile.createdAt.isValid() ||
        !engine::validEngine(profile.engine)) {
        return std::nullopt;
    }
    return profile;
}
class EngineTransaction final {
  public:
    EngineTransaction(QSqlDatabase& database, bool& usable)
        : database_(database), usable_(usable) {}
    ~EngineTransaction() {
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
} // namespace
RegisterEngineResult StorageSession::registerEngine(const engine::ValidatedEngine& engine) {
    if (!usable_) {
        return storage();
    }
    if (!engine::validEngine(engine)) {
        return invalid();
    }
    EngineTransaction transaction(database_, usable_);
    if (!transaction.begin()) {
        return storage();
    }
    QSqlQuery query(database_);
    if (!query.prepare(projection +
                       QStringLiteral("WHERE executable_sha256=? OR executable_path=CAST(? AS "
                                      "TEXT) ORDER BY id LIMIT 2"))) {
        return storage();
    }
    query.addBindValue(engine.executableSha256);
    query.addBindValue(engine.executablePath.toUtf8());
    if (!query.exec()) {
        return storage();
    }
    std::optional<engine::EngineProfile> duplicate;
    bool replaced = false;
    while (query.next()) {
        const auto profile = readProfile(query);
        if (!profile) {
            return invalid();
        }
        if (profile->engine.executableSha256 == engine.executableSha256) {
            duplicate = *profile;
        }
        if (profile->engine.executablePath == engine.executablePath &&
            profile->engine.executableSha256 != engine.executableSha256) {
            replaced = true;
        }
    }
    if (query.lastError().isValid()) {
        return storage();
    }
    query.finish();
    if (replaced) {
        return engine::Error{engine::ErrorCode::Replacement,
                             QStringLiteral("Different executable content is already registered at "
                                            "this path. Replacement is not supported yet.")};
    }
    if (duplicate) {
        if (!transaction.commit()) {
            return storage();
        }
        return RegisteredEngine{*duplicate, true};
    }
    if (!query.exec(QStringLiteral("SELECT count(*), coalesce(sum(is_default),0) FROM engines")) ||
        !query.next()) {
        return storage();
    }
    const auto count = query.value(0).toLongLong();
    const auto defaults = query.value(1).toLongLong();
    if (count < 0 || (count == 0 ? defaults != 0 : defaults != 1)) {
        return invalid();
    }
    query.finish();
    const auto now = QDateTime::currentDateTimeUtc();
    if (!query.prepare(QStringLiteral(
            "INSERT INTO "
            "engines(display_name,executable_path,executable_sha256,reported_name,reported_author,"
            "detected_version,capabilities_json,option_overrides_json,is_default,created_at,last_"
            "validated_at) VALUES(CAST(? AS TEXT),CAST(? AS TEXT),?,CAST(? AS TEXT),CAST(? AS "
            "TEXT),NULL,?,?,?,CAST(? AS TEXT),CAST(? AS TEXT))"))) {
        return storage();
    }
    query.addBindValue(engine.reportedName.toUtf8());
    query.addBindValue(engine.executablePath.toUtf8());
    query.addBindValue(engine.executableSha256);
    query.addBindValue(engine.reportedName.toUtf8());
    query.addBindValue(engine.reportedAuthor ? QVariant(engine.reportedAuthor->toUtf8())
                                             : QVariant());
    query.addBindValue(engine::encodeCapabilities(engine.capabilities));
    query.addBindValue(QByteArray("{}"));
    query.addBindValue(count == 0 ? 1 : 0);
    query.addBindValue(now.toString(Qt::ISODateWithMs).toUtf8());
    query.addBindValue(engine.lastValidatedAt.toUTC().toString(Qt::ISODateWithMs).toUtf8());
    if (!query.exec() || query.numRowsAffected() != 1) {
        return storage();
    }
    const auto id = query.lastInsertId().toLongLong();
    if (id <= 0 || !transaction.commit()) {
        return storage();
    }
    return RegisteredEngine{{id, engine.reportedName, engine, count == 0, now}, false};
}
ListEnginesResult StorageSession::listEngines(std::int64_t offset, int limit) {
    if (!usable_) {
        return storage();
    }
    if (offset < 0 || limit < 1 || limit > 200) {
        return invalid();
    }
    QSqlQuery query(database_);
    query.setForwardOnly(true);
    if (!query.prepare(projection + QStringLiteral("ORDER BY id LIMIT ? OFFSET ?"))) {
        return storage();
    }
    query.addBindValue(limit);
    query.addBindValue(static_cast<qlonglong>(offset));
    if (!query.exec()) {
        return storage();
    }
    std::vector<engine::EngineProfile> profiles;
    while (query.next()) {
        const auto profile = readProfile(query);
        if (!profile) {
            return invalid();
        }
        profiles.push_back(*profile);
    }
    if (query.lastError().isValid()) {
        return storage();
    }
    return profiles;
}
} // namespace chess_review::persistence::detail
