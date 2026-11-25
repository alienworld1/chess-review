#include "persistence/private/StorageSession.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringDecoder>
#include <QVariant>
#include <algorithm>

namespace chess_review::persistence::detail {
namespace {
engine::Error storage() {
    return {engine::ErrorCode::Storage,
            QStringLiteral("Engine storage failed. Try again after checking storage.")};
}
engine::Error invalid() {
    return {engine::ErrorCode::InvalidRecord,
            QStringLiteral(
                "An engine record is invalid or too large. Existing records were preserved.")};
}
// Byte-bounded projections precede Qt materialization, including for damaged databases.
const QString profileColumns = QStringLiteral(
    "SELECT id, substr(CAST(display_name AS BLOB),1,16385), substr(CAST(%1 AS BLOB),1,16385), "
    "substr(executable_sha256,1,33), substr(CAST(reported_name AS BLOB),1,16385), "
    "substr(CAST(reported_author AS BLOB),1,16385), detected_version IS NULL, "
    "substr(CAST(capabilities_json AS BLOB),1,1048577), substr(CAST(option_overrides_json AS "
    "BLOB),1,65537), "
    "is_default, substr(CAST(created_at AS BLOB),1,65), substr(CAST(last_validated_at AS "
    "BLOB),1,65), "
    "typeof(id)='integer' AND typeof(display_name)='text' AND typeof(executable_path)='text' AND "
    "typeof(executable_sha256)='blob' AND typeof(reported_name)='text' AND "
    "(reported_author IS NULL OR typeof(reported_author)='text') AND "
    "typeof(capabilities_json)='blob' AND "
    "typeof(option_overrides_json)='blob' AND typeof(is_default)='integer' AND "
    "typeof(created_at)='text' AND typeof(last_validated_at)='text', "
    "%2 FROM engines ");
const QString& projection(bool canArchive) {
    static const auto current =
        profileColumns.arg(QStringLiteral("coalesce(archived_path, executable_path)"),
                           QStringLiteral("archived_path IS NOT NULL"));
    static const auto previous =
        profileColumns.arg(QStringLiteral("executable_path"), QStringLiteral("0"));
    return canArchive ? current : previous;
}
std::optional<QString> readText(const QSqlQuery& query, int column, qsizetype cap) {
    const auto bytes = query.value(column).toByteArray();
    if (bytes.size() > cap || !bytes.isValidUtf8() || bytes.contains('\0')) {
        return std::nullopt;
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom);
    return decoder(bytes);
}
bool validDisplayName(const QString& name) {
    return !name.trimmed().isEmpty() && name.size() <= 4096 && name.isValidUtf16() &&
           !name.contains(u'\n') && !name.contains(u'\r') && !name.contains(QChar::Null);
}
QByteArray encodeOverrides(const engine::OptionOverrides& overrides) {
    QJsonObject object;
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        object.insert(it.key(), it.value());
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
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
    const auto json = query.value(8).toByteArray();
    const auto parsed = json.size() <= 65536 && json.isValidUtf8() ? QJsonDocument::fromJson(json)
                                                                   : QJsonDocument{};
    if (!parsed.isObject()) {
        return std::nullopt;
    }
    const auto fields = parsed.object();
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        if (!it.value().isString()) {
            return std::nullopt;
        }
        profile.optionOverrides.insert(it.key(), it.value().toString());
    }
    if (profile.id <= 0 || !validDisplayName(profile.displayName) ||
        !engine::validOverrides(profile.engine.capabilities, profile.optionOverrides) ||
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
    const bool canArchive = schemaVersion_ >= 15;
    // Check the registry invariant before either the duplicate or insertion path.
    // Count valid defaults instead of summing untrusted integer values.
    if (!query.exec(QStringLiteral(
                        "SELECT count(*), count(CASE WHEN is_default=1 THEN 1 END), "
                        "count(CASE WHEN typeof(is_default)!='integer' OR is_default NOT IN (0,1) "
                        "THEN 1 END) FROM engines") +
                    (canArchive ? QStringLiteral(" WHERE archived_path IS NULL") : QString{})) ||
        !query.next()) {
        return storage();
    }
    const auto count = query.value(0).toLongLong();
    const auto defaults = query.value(1).toLongLong();
    if (count < 0 || query.value(2).toLongLong() != 0 ||
        (count == 0 ? defaults != 0 : defaults != 1)) {
        return invalid();
    }
    query.finish();
    if (!query.prepare(projection(canArchive) +
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
    bool archivedDuplicate = false;
    while (query.next()) {
        const auto profile = readProfile(query);
        if (!profile) {
            return invalid();
        }
        if (profile->engine.executableSha256 == engine.executableSha256) {
            duplicate = *profile;
            archivedDuplicate = query.value(13).toBool();
        }
        if (!query.value(13).toBool() && profile->engine.executablePath == engine.executablePath &&
            profile->engine.executableSha256 != engine.executableSha256) {
            replaced = true;
        }
    }
    if (query.lastError().isValid()) {
        return storage();
    }
    query.finish();
    if (replaced) {
        return engine::Error{
            engine::ErrorCode::Replacement,
            QStringLiteral("Different executable content is already registered at "
                           "this path. Remove that profile before adding the new executable.")};
    }
    if (archivedDuplicate) {
        if (!engine::validOverrides(engine.capabilities, duplicate->optionOverrides)) {
            duplicate->optionOverrides.clear();
        }
        if (!query.prepare(QStringLiteral(
                "UPDATE engines SET archived_path=NULL, executable_path=?, is_default=?, "
                "reported_name=?, reported_author=?, capabilities_json=?, option_overrides_json=?, "
                "last_validated_at=? WHERE id=? AND archived_path IS NOT NULL"))) {
            return storage();
        }
        query.addBindValue(engine.executablePath);
        query.addBindValue(count == 0 ? 1 : 0);
        query.addBindValue(engine.reportedName);
        query.addBindValue(engine.reportedAuthor ? QVariant(*engine.reportedAuthor) : QVariant());
        query.addBindValue(engine::encodeCapabilities(engine.capabilities));
        query.addBindValue(encodeOverrides(duplicate->optionOverrides));
        query.addBindValue(engine.lastValidatedAt.toUTC().toString(Qt::ISODateWithMs));
        query.addBindValue(static_cast<qlonglong>(duplicate->id));
        if (!query.exec() || query.numRowsAffected() != 1 || !transaction.commit()) {
            return storage();
        }
        duplicate->engine = engine;
        duplicate->isDefault = count == 0;
        return RegisteredEngine{*duplicate, false};
    }
    if (duplicate) {
        if (!transaction.commit()) {
            return storage();
        }
        return RegisteredEngine{*duplicate, true};
    }
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
    query.finish();
    const engine::EngineProfile expected{id, engine.reportedName, engine, count == 0, now};
    if (id <= 0 || !query.prepare(projection(canArchive) + QStringLiteral("WHERE id=?"))) {
        return storage();
    }
    query.addBindValue(id);
    if (!query.exec()) {
        return storage();
    }
    if (!query.next()) {
        return invalid();
    }
    const auto stored = readProfile(query);
    if (!stored || *stored != expected) {
        return invalid();
    }
    query.finish();
    if (!transaction.commit()) {
        return storage();
    }
    return RegisteredEngine{*stored, false};
}
EditEngineResult StorageSession::editEngine(std::int64_t id, const EngineProfileEdit& edit) {
    EngineTransaction transaction(database_, usable_);
    if (schemaVersion_ < 15 || !usable_ || !transaction.begin()) {
        return storage();
    }
    QSqlQuery query(database_);
    if (!query.prepare(projection(true) + QStringLiteral("WHERE id=? AND archived_path IS NULL"))) {
        return storage();
    }
    const auto key = static_cast<qlonglong>(id);
    query.addBindValue(key);
    if (!query.exec()) {
        return storage();
    }
    if (!query.next()) {
        return query.lastError().isValid()
                   ? storage()
                   : engine::Error{
                         engine::ErrorCode::InvalidRequest,
                         QStringLiteral("This engine is no longer registered. Refresh the list.")};
    }
    const auto profile = readProfile(query);
    query.finish();
    if (!profile) {
        return invalid();
    }
    QString statement;
    QVariant parameter;
    if (const auto* rename = std::get_if<RenameEngine>(&edit)) {
        const auto name = rename->displayName.trimmed();
        if (name.size() > 120 || !validDisplayName(name)) {
            return engine::Error{engine::ErrorCode::InvalidRequest,
                                 QStringLiteral("Enter a display name of 1 to 120 characters.")};
        }
        statement = QStringLiteral("UPDATE engines SET display_name=? WHERE id=?");
        parameter = name;
    } else if (std::holds_alternative<MakeDefaultEngine>(edit)) {
        if (!query.exec(QStringLiteral("UPDATE engines SET is_default=0 WHERE is_default=1"))) {
            return storage();
        }
        statement = QStringLiteral("UPDATE engines SET is_default=1 WHERE id=?");
    } else if (std::holds_alternative<RemoveEngine>(edit)) {
        if (profile->isDefault) {
            if (!query.exec(QStringLiteral("SELECT EXISTS(SELECT 1 FROM engines WHERE "
                                           "archived_path IS NULL AND is_default=0)")) ||
                !query.next()) {
                return storage();
            }
            if (query.value(0).toBool()) {
                return engine::Error{
                    engine::ErrorCode::InvalidRequest,
                    QStringLiteral("Set another engine as default before removing this one.")};
            }
            query.finish();
        }
        // Retain the engine ID for historical reviews. Release the path so a new
        // binary installed at the same location can be registered.
        statement = QStringLiteral("UPDATE engines SET archived_path=executable_path, "
                                   "executable_path='archived:' || id, is_default=0 WHERE id=?");
    } else if (const auto* relocate = std::get_if<RelocateEngine>(&edit)) {
        if (!engine::validEngine(relocate->engine)) {
            return invalid();
        }
        if (relocate->engine.executableSha256 != profile->engine.executableSha256) {
            return engine::Error{
                engine::ErrorCode::Replacement,
                QStringLiteral("The selected executable is a different engine binary. Remove the "
                               "missing profile or add this executable as a new engine.")};
        }
        if (!query.prepare(QStringLiteral("SELECT EXISTS(SELECT 1 FROM engines WHERE id!=? AND "
                                          "executable_path=CAST(? AS TEXT))"))) {
            return storage();
        }
        query.addBindValue(key);
        query.addBindValue(relocate->engine.executablePath.toUtf8());
        if (!query.exec() || !query.next()) {
            return storage();
        }
        if (query.value(0).toBool()) {
            return engine::Error{
                engine::ErrorCode::Replacement,
                QStringLiteral("Another registered engine already uses this executable path.")};
        }
        query.finish();
        if (!query.prepare(QStringLiteral(
                "UPDATE engines SET executable_path=CAST(? AS TEXT), last_validated_at=CAST(? AS "
                "TEXT) WHERE id=? AND archived_path IS NULL"))) {
            return storage();
        }
        query.addBindValue(relocate->engine.executablePath.toUtf8());
        query.addBindValue(
            relocate->engine.lastValidatedAt.toUTC().toString(Qt::ISODateWithMs).toUtf8());
        query.addBindValue(key);
        if (!query.exec() || query.numRowsAffected() != 1 || !transaction.commit()) {
            return storage();
        }
        return {};
    } else {
        const auto& setting = std::get<SetEngineOption>(edit);
        const auto& capabilities = profile->engine.capabilities;
        const auto option =
            std::ranges::find_if(capabilities.options, [&setting](const auto& value) {
                return value.name.compare(setting.name, Qt::CaseInsensitive) == 0;
            });
        if (option == capabilities.options.end() || !engine::editableOption(*option)) {
            return engine::Error{engine::ErrorCode::InvalidRequest,
                                 QStringLiteral("This engine option cannot be edited.")};
        }
        auto overrides = profile->optionOverrides;
        const auto name = option->name.toCaseFolded();
        if (setting.value) {
            const auto value = engine::normalizedOverride(*option, *setting.value);
            if (!value) {
                return engine::Error{
                    engine::ErrorCode::InvalidRequest,
                    QStringLiteral("Enter a supported value within the displayed limits.")};
            }
            overrides.insert(name, *value);
        } else {
            overrides.remove(name);
        }
        if (!engine::validOverrides(capabilities, overrides)) {
            return engine::Error{
                engine::ErrorCode::InvalidRequest,
                QStringLiteral("The saved engine options exceed the allowed size.")};
        }
        parameter = encodeOverrides(overrides);
        statement = QStringLiteral("UPDATE engines SET option_overrides_json=? WHERE id=?");
    }
    if (!query.prepare(statement)) {
        return storage();
    }
    if (parameter.isValid()) {
        query.addBindValue(parameter);
    }
    query.addBindValue(key);
    if (!query.exec() || query.numRowsAffected() != 1 || !transaction.commit()) {
        return storage();
    }
    return {};
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
    const bool canArchive = schemaVersion_ >= 15;
    const auto order =
        canArchive ? QStringLiteral("WHERE archived_path IS NULL ORDER BY is_default DESC, id")
                   : QStringLiteral("ORDER BY id");
    if (!query.prepare(projection(canArchive) + order + QStringLiteral(" LIMIT ? OFFSET ?"))) {
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
