#include "persistence/private/StorageSession.h"

#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace chess_review::persistence::detail {

StorageSession::StorageSession() = default;

StorageSession::~StorageSession() {
    if (!connectionName_.isEmpty()) {
        database_.close();
        database_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName_);
    }
}

std::optional<DatabaseError> StorageSession::initialize(const QString& path,
                                                        std::span<const Migration> registry,
                                                        const Progress& progress) {
    const auto failure = [&path](DatabaseErrorCode code, const QString& operation,
                                 const QString& detail, const QString& nativeCode = {}) {
        return DatabaseError{code, operation, detail, nativeCode, path, {}, std::nullopt};
    };
    progress(QStringLiteral("Opening local database…"));
    if (path.isEmpty() || !QFileInfo(path).isAbsolute() || path.contains(QChar::Null)) {
        return failure(DatabaseErrorCode::Path, QStringLiteral("Locate database"),
                       QStringLiteral("An absolute database file path is required."));
    }
    const QFileInfo file(path);
    if (file.isSymLink() && !file.exists()) {
        return failure(DatabaseErrorCode::Path, QStringLiteral("Locate database"),
                       QStringLiteral("The database path is a dangling symbolic link."));
    }
    if (!QDir().mkpath(file.absolutePath())) {
        return failure(DatabaseErrorCode::Path, QStringLiteral("Create data directory"),
                       QStringLiteral("Cannot create the database directory."));
    }
    // Resolve directory aliases before choosing the lock, so symlinked paths share it.
    const QString canonicalDirectory = QFileInfo(file.absolutePath()).canonicalFilePath();
    const QString canonicalPath = file.exists()
                                      ? file.canonicalFilePath()
                                      : QDir(canonicalDirectory).filePath(file.fileName());
    lock_ = std::make_unique<QLockFile>(canonicalPath + QStringLiteral(".lock"));
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock(0)) {
        return failure(DatabaseErrorCode::Locked, QStringLiteral("Lock database"),
                       lock_->error() == QLockFile::LockFailedError
                           ? QStringLiteral("This database is already in use by another instance.")
                           : QStringLiteral("Cannot create the database lock file."));
    }

    connectionName_ = QStringLiteral("chess-review-") + QUuid::createUuid().toString(QUuid::Id128);
    database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    database_.setDatabaseName(canonicalPath);
    database_.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!database_.open()) {
        return failure(DatabaseErrorCode::Open, QStringLiteral("Open database"),
                       database_.lastError().text(), database_.lastError().nativeErrorCode());
    }

    const auto configure = [&](const QString& statement, const QVariant& expected) {
        QSqlQuery query(database_);
        if (!query.exec(statement) || !query.next() || query.value(0) != expected) {
            return std::optional<DatabaseError>(
                failure(DatabaseErrorCode::Configuration, QStringLiteral("Configure SQLite"),
                        query.lastError().isValid()
                            ? query.lastError().text()
                            : QStringLiteral("SQLite configuration was not applied."),
                        query.lastError().nativeErrorCode()));
        }
        return std::optional<DatabaseError>{};
    };
    {
        QSqlQuery query(database_);
        if (!query.exec(QStringLiteral("PRAGMA foreign_keys = ON"))) {
            return failure(DatabaseErrorCode::Configuration, QStringLiteral("Enable foreign keys"),
                           query.lastError().text(), query.lastError().nativeErrorCode());
        }
    }
    if (auto error = configure(QStringLiteral("PRAGMA foreign_keys"), 1)) {
        return error;
    }
    if (auto error = configure(QStringLiteral("PRAGMA busy_timeout"), 5000)) {
        return error;
    }
    if (auto error =
            configure(QStringLiteral("PRAGMA journal_mode = WAL"), QStringLiteral("wal"))) {
        return error;
    }
    return migrate(database_, canonicalPath, registry, progress);
}

} // namespace chess_review::persistence::detail
