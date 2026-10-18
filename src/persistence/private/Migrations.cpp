#include "persistence/private/Migrations.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace chess_review::persistence::detail {
namespace {

std::optional<QString> verifyBackup(const QString& path) {
    const QString name =
        QStringLiteral("backup-check-") + QUuid::createUuid().toString(QUuid::Id128);
    std::optional<QString> error;
    {
        auto backup = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        backup.setDatabaseName(path);
        backup.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
        if (!backup.open()) {
            error = backup.lastError().text();
        } else {
            QSqlQuery query(backup);
            if (!query.exec(QStringLiteral("PRAGMA integrity_check")) || !query.next() ||
                query.value(0).toString() != QStringLiteral("ok")) {
                error = query.lastError().isValid()
                            ? query.lastError().text()
                            : QStringLiteral("Backup integrity check failed.");
            } else if (query.next() || query.lastError().isValid()) {
                error = QStringLiteral("Backup integrity check returned unexpected results.");
            }
        }
        backup.close();
    }
    QSqlDatabase::removeDatabase(name);
    return error;
}

} // namespace

const std::vector<Migration>& migrations() {
    static const std::vector<Migration> registry{initialMigration()};
    return registry;
}

std::optional<DatabaseError> migrate(QSqlDatabase& database, const QString& path,
                                     std::span<const Migration> registry,
                                     const Progress& progress) {
    QString backupPath;
    const auto failure = [&](DatabaseErrorCode code, const QString& operation,
                             const QString& detail, const QSqlError& sqlError = {},
                             std::optional<int> version = std::nullopt) {
        return DatabaseError{code, operation,  detail, sqlError.nativeErrorCode(),
                             path, backupPath, version};
    };
    const auto schemaFailure = [&](const QString& detail) {
        return failure(DatabaseErrorCode::Schema, QStringLiteral("Validate schema history"),
                       detail);
    };
    if (registry.empty()) {
        return schemaFailure(QStringLiteral("No schema migrations are registered."));
    }
    int expectedVersion = 1;
    for (const auto& migration : registry) {
        if (migration.version != expectedVersion || migration.name.isEmpty() ||
            migration.statements.isEmpty()) {
            return schemaFailure(
                QStringLiteral("The migration registry is not contiguous and valid."));
        }
        ++expectedVersion;
    }

    progress(QStringLiteral("Checking database schema…"));
    int currentVersion = 0;
    {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("SELECT name, type FROM sqlite_schema "
                                       "WHERE name NOT GLOB 'sqlite_*'"))) {
            return failure(DatabaseErrorCode::Schema, QStringLiteral("Read database schema"),
                           query.lastError().text(), query.lastError());
        }
        bool hasObjects = false;
        bool hasLedger = false;
        while (query.next()) {
            hasObjects = true;
            hasLedger =
                hasLedger || (query.value(0).toString() == QStringLiteral("schema_migrations") &&
                              query.value(1).toString() == QStringLiteral("table"));
        }
        if (query.lastError().isValid()) {
            return schemaFailure(query.lastError().text());
        }
        query.finish();
        if (hasObjects && !hasLedger) {
            return schemaFailure(
                QStringLiteral("The database has no recognized migration history."));
        }
        if (hasLedger) {
            if (!query.exec(QStringLiteral("SELECT version, name, applied_at, typeof(version) "
                                           "FROM schema_migrations ORDER BY version"))) {
                return schemaFailure(query.lastError().text());
            }
            while (query.next()) {
                bool validVersion = false;
                const int version = query.value(0).toInt(&validVersion);
                if (!validVersion || query.value(3).toString() != QStringLiteral("integer") ||
                    version != currentVersion + 1) {
                    return schemaFailure(
                        QStringLiteral("Migration history contains invalid versions or gaps."));
                }
                if (version > registry.back().version) {
                    return schemaFailure(
                        QStringLiteral("This database requires a newer application version."));
                }
                const auto& migration = registry[static_cast<std::size_t>(version - 1)];
                if (query.value(1).toString() != migration.name ||
                    !QDateTime::fromString(query.value(2).toString(), Qt::ISODateWithMs)
                         .isValid()) {
                    return schemaFailure(
                        QStringLiteral("Migration history has invalid names or timestamps."));
                }
                currentVersion = version;
            }
            if (query.lastError().isValid()) {
                return schemaFailure(query.lastError().text());
            }
            if (currentVersion == 0) {
                return schemaFailure(QStringLiteral("The migration ledger is empty."));
            }
        }
    }
    if (currentVersion == registry.back().version) {
        return std::nullopt;
    }

    if (currentVersion > 0) {
        progress(QStringLiteral("Backing up database before upgrade…"));
        const QDir directory(QFileInfo(path).absolutePath());
        if (!directory.mkpath(QStringLiteral("backups"))) {
            return failure(DatabaseErrorCode::Backup, QStringLiteral("Create backup directory"),
                           QStringLiteral("Cannot create the pre-migration backup directory."));
        }
        const QString candidate =
            directory.filePath(QStringLiteral("backups/%1-v%2-%3-%4.sqlite3")
                                   .arg(QFileInfo(path).completeBaseName())
                                   .arg(currentVersion)
                                   .arg(QDateTime::currentDateTimeUtc().toString(
                                            QStringLiteral("yyyyMMddTHHmmsszzzZ")),
                                        QUuid::createUuid().toString(QUuid::Id128)));
        QSqlQuery query(database);
        if (!query.prepare(QStringLiteral("VACUUM INTO ?"))) {
            return failure(DatabaseErrorCode::Backup, QStringLiteral("Prepare backup"),
                           query.lastError().text(), query.lastError());
        }
        query.addBindValue(candidate);
        if (!query.exec()) {
            return failure(DatabaseErrorCode::Backup, QStringLiteral("Create backup"),
                           query.lastError().text(), query.lastError());
        }
        query.finish();
        backupPath = candidate;
        if (const auto error = verifyBackup(candidate)) {
            return failure(DatabaseErrorCode::Backup, QStringLiteral("Verify backup"), *error);
        }
    }

    for (const auto& migration : registry) {
        if (migration.version <= currentVersion) {
            continue;
        }
        progress(QStringLiteral("Applying database migration %1…").arg(migration.version));
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
            return failure(DatabaseErrorCode::Migration, QStringLiteral("Begin migration"),
                           query.lastError().text(), query.lastError(), migration.version);
        }
        const auto rollbackFailure = [&](const QString& operation, const QSqlError& sqlError) {
            auto error = failure(DatabaseErrorCode::Migration, operation, sqlError.text(), sqlError,
                                 migration.version);
            if (!database.rollback()) {
                error.detail += QStringLiteral("; rollback failed: ") + database.lastError().text();
            }
            return error;
        };
        for (const auto& statement : migration.statements) {
            if (!query.exec(statement)) {
                return rollbackFailure(QStringLiteral("Apply migration"), query.lastError());
            }
            query.finish();
        }
        if (!query.prepare(
                QStringLiteral("INSERT INTO schema_migrations (version, name, applied_at) "
                               "VALUES (?, ?, ?)"))) {
            return rollbackFailure(QStringLiteral("Prepare migration history"), query.lastError());
        }
        query.addBindValue(migration.version);
        query.addBindValue(migration.name);
        query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        if (!query.exec()) {
            return rollbackFailure(QStringLiteral("Record migration"), query.lastError());
        }
        query.finish();
        if (!database.commit()) {
            return rollbackFailure(QStringLiteral("Commit migration"), database.lastError());
        }
    }
    return std::nullopt;
}

} // namespace chess_review::persistence::detail
