#include "persistence/private/SchemaHistory.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <array>
#include <cstddef>

namespace chess_review::persistence::detail {

std::variant<int, DatabaseError> readSchemaVersion(QSqlDatabase& database, const QString& path,
                                                   std::span<const Migration> registry) {
    const auto failure = [&](const QString& detail, const QSqlError& sqlError = {}) {
        return DatabaseError{DatabaseErrorCode::Schema,
                             QStringLiteral("Validate schema history"),
                             detail,
                             sqlError.nativeErrorCode(),
                             path,
                             {},
                             std::nullopt};
    };
    int expectedVersion = 1;
    for (const auto& migration : registry) {
        if (migration.version != expectedVersion || migration.name.isEmpty() ||
            migration.name.size() > 128 || migration.statements.isEmpty()) {
            return failure(QStringLiteral("The migration registry is not contiguous and valid."));
        }
        ++expectedVersion;
    }
    if (registry.empty()) {
        return failure(QStringLiteral("No schema migrations are registered."));
    }

    QSqlQuery query(database);
    query.setForwardOnly(true);
    if (!query.exec(QStringLiteral(
            "SELECT EXISTS(SELECT 1 FROM main.sqlite_schema WHERE name NOT GLOB 'sqlite_*'), "
            "EXISTS(SELECT 1 FROM main.sqlite_schema WHERE name='schema_migrations' AND "
            "type='table')")) ||
        !query.next()) {
        return failure(query.lastError().text(), query.lastError());
    }
    const bool hasObjects = query.value(0).toBool();
    const bool hasLedger = query.value(1).toBool();
    query.finish();
    if (!hasObjects) {
        return 0;
    }
    if (!hasLedger) {
        return failure(QStringLiteral("The database has no recognized migration history."));
    }

    // Matching rows alone is insufficient: a forged ledger or trigger can suppress history writes.
    if (!query.exec(QStringLiteral("PRAGMA main.table_xinfo('schema_migrations')"))) {
        return failure(query.lastError().text(), query.lastError());
    }
    struct Column {
        const char* name;
        const char* type;
        int notNull;
        int primaryKey;
    };
    constexpr std::array columns{Column{"version", "INTEGER", 0, 1}, Column{"name", "TEXT", 1, 0},
                                 Column{"applied_at", "TEXT", 1, 0}};
    for (std::size_t index = 0; index < columns.size(); ++index) {
        const auto& column = columns[index];
        if (!query.next() || query.value(0).toULongLong() != index ||
            query.value(1).toString() != QLatin1StringView(column.name) ||
            query.value(2).toString().toUpper() != QLatin1StringView(column.type) ||
            query.value(3).toInt() != column.notNull || !query.value(4).isNull() ||
            query.value(5).toInt() != column.primaryKey || query.value(6).toInt() != 0) {
            return failure(
                QStringLiteral("The migration ledger has an unexpected table definition."));
        }
    }
    if (query.next() || query.lastError().isValid()) {
        return failure(QStringLiteral("The migration ledger has unexpected columns."),
                       query.lastError());
    }
    query.finish();
    if (!query.exec(QStringLiteral("SELECT EXISTS(SELECT 1 FROM main.sqlite_schema "
                                   "WHERE tbl_name='schema_migrations' COLLATE NOCASE AND type IN "
                                   "('trigger','index'))")) ||
        !query.next()) {
        return failure(query.lastError().text(), query.lastError());
    }
    if (query.value(0).toBool()) {
        return failure(
            QStringLiteral("The migration ledger contains unexpected triggers or indexes."));
    }
    query.finish();
    if (!query.exec(QStringLiteral("PRAGMA main.foreign_key_list('schema_migrations')"))) {
        return failure(query.lastError().text(), query.lastError());
    }
    if (query.next() || query.lastError().isValid()) {
        return failure(QStringLiteral("The migration ledger contains unexpected foreign keys."),
                       query.lastError());
    }
    query.finish();

    // Bound both returned text and row count before Qt materializes untrusted values.
    if (!query.prepare(
            QStringLiteral("SELECT version, CAST(substr(CAST(name AS BLOB),1,513) AS TEXT), "
                           "CAST(substr(CAST(applied_at AS BLOB),1,65) AS TEXT), "
                           "typeof(version), typeof(name), typeof(applied_at) "
                           "FROM main.schema_migrations ORDER BY version LIMIT ?"))) {
        return failure(query.lastError().text(), query.lastError());
    }
    query.addBindValue(static_cast<qlonglong>(registry.size()) + 1);
    if (!query.exec()) {
        return failure(query.lastError().text(), query.lastError());
    }
    int currentVersion = 0;
    while (query.next()) {
        bool validVersion = false;
        const qlonglong version = query.value(0).toLongLong(&validVersion);
        if (!validVersion || query.value(3).toString() != QStringLiteral("integer") ||
            version != static_cast<qlonglong>(currentVersion) + 1) {
            return failure(QStringLiteral("Migration history contains invalid versions or gaps."));
        }
        if (version > registry.back().version) {
            return failure(QStringLiteral("This database requires a newer application version."));
        }
        const auto& migration = registry[static_cast<std::size_t>(version - 1)];
        const QString timestamp = query.value(2).toString();
        if (query.value(4).toString() != QStringLiteral("text") ||
            query.value(5).toString() != QStringLiteral("text") ||
            query.value(1).toString() != migration.name || timestamp.size() > 64 ||
            !QDateTime::fromString(timestamp, Qt::ISODateWithMs).isValid()) {
            return failure(QStringLiteral("Migration history has invalid names or timestamps."));
        }
        currentVersion = static_cast<int>(version); // Bounded by the compiled registry above.
    }
    if (query.lastError().isValid()) {
        return failure(query.lastError().text(), query.lastError());
    }
    if (currentVersion == 0) {
        return failure(QStringLiteral("The migration ledger is empty."));
    }
    return currentVersion;
}

} // namespace chess_review::persistence::detail
