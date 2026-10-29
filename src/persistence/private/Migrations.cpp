#include "persistence/private/Migrations.h"

#include "persistence/private/PrivateFile.h"
#include "persistence/private/SchemaHistory.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <exception>

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
            if (!query.exec(QStringLiteral("PRAGMA trusted_schema = OFF")) ||
                !query.exec(QStringLiteral("PRAGMA integrity_check(1)")) || !query.next() ||
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
    static const std::vector<Migration> registry{
        initialMigration(),         gamesMigration(),      gameTextMigration(),
        enginesMigration(),         analysisMigration(),   reviewsMigration(),
        classificationsMigration(), refinementMigration(), reviewWorkspaceMigration()};
    return registry;
}

std::optional<DatabaseError> migrate(QSqlDatabase& database, const QString& path,
                                     std::span<const Migration> registry, const Progress& progress,
                                     const Observe& observe) {
    QString backupPath;
    const auto failure = [&](DatabaseErrorCode code, const QString& operation,
                             const QString& detail, const QSqlError& sqlError = {},
                             std::optional<int> version = std::nullopt) {
        return DatabaseError{code, operation,  detail, sqlError.nativeErrorCode(),
                             path, backupPath, version};
    };
    progress(QStringLiteral("Checking database schema…"));
    const auto history = readSchemaVersion(database, path, registry);
    if (const auto* error = std::get_if<DatabaseError>(&history)) {
        return *error;
    }
    const int currentVersion = std::get<int>(history);
    if (observe) {
        observe(StorageEvent::SchemaChecked, currentVersion);
    }

    // Repair permissive files created by earlier versions, including live WAL contents.
    for (const auto& file : {path, path + QStringLiteral("-wal"), path + QStringLiteral("-shm")}) {
        if (QFileInfo::exists(file)) {
            if (const auto error = restrictFilePermissions(file)) {
                return failure(DatabaseErrorCode::Path, QStringLiteral("Protect database file"),
                               *error);
            }
        }
    }

    // Change persistent settings only after recognizing and validating the database.
    QSqlQuery configuration(database);
    if (!configuration.exec(QStringLiteral("PRAGMA journal_mode = WAL")) || !configuration.next() ||
        configuration.value(0).toString() != QStringLiteral("wal")) {
        return failure(DatabaseErrorCode::Configuration, QStringLiteral("Enable WAL"),
                       configuration.lastError().isValid()
                           ? configuration.lastError().text()
                           : QStringLiteral("SQLite did not enable WAL mode."),
                       configuration.lastError());
    }
    configuration.finish();
    if (currentVersion == registry.back().version) {
        return std::nullopt;
    }

    if (currentVersion > 0) {
        if (observe) {
            observe(StorageEvent::BackupStarted, currentVersion);
        }
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
        if (const auto error = createPrivateFile(candidate)) {
            return failure(DatabaseErrorCode::Backup, QStringLiteral("Reserve backup file"),
                           *error);
        }
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
        if (const auto error = verifyBackup(candidate)) {
            return failure(DatabaseErrorCode::Backup, QStringLiteral("Verify backup"), *error);
        }
        // Only a successfully verified snapshot is advertised as a recovery backup.
        backupPath = candidate;
        if (observe) {
            observe(StorageEvent::BackupVerified, currentVersion);
        }
    }

    for (const auto& migration : registry) {
        if (migration.version <= currentVersion) {
            continue;
        }
        if (observe) {
            observe(StorageEvent::MigrationStarted, migration.version);
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
        if (migration.transform) {
            std::optional<QString> error;
            try {
                error = migration.transform(database);
            } catch (const std::exception&) {
                error = QStringLiteral("Migration transform could not complete.");
            }
            if (error) {
                return rollbackFailure(
                    QStringLiteral("Transform migration"),
                    QSqlError(QStringLiteral("Data migration"), *error, QSqlError::StatementError));
            }
        }
        if (!query.prepare(
                QStringLiteral("INSERT INTO schema_migrations (version, name, applied_at) "
                               "VALUES (?, ?, ?)"))) {
            return rollbackFailure(QStringLiteral("Prepare migration history"), query.lastError());
        }
        query.addBindValue(migration.version);
        query.addBindValue(migration.name);
        query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        if (!query.exec() || query.numRowsAffected() != 1) {
            return rollbackFailure(QStringLiteral("Record migration"), query.lastError());
        }
        query.finish();
        if (!database.commit()) {
            return rollbackFailure(QStringLiteral("Commit migration"), database.lastError());
        }
        if (observe) {
            observe(StorageEvent::MigrationCompleted, migration.version);
        }
    }
    return std::nullopt;
}

} // namespace chess_review::persistence::detail
