#include "persistence/private/StorageSession.h"

#include "persistence/private/PrivateFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace chess_review::persistence::detail {
namespace {

// Check before Qt/SQLite can read, recover, truncate, or chmod an existing file.
bool safeFile(const QString& path) {
    const QFileInfo file(path);
    if (file.isSymLink() || (file.exists() && !file.isFile())) {
        return false;
    }
#ifdef Q_OS_UNIX
    if (file.exists()) {
        struct stat information {};
        const auto encoded = QFile::encodeName(path);
        if (::lstat(encoded.constData(), &information) != 0 || information.st_nlink != 1) {
            return false;
        }
    }
#endif
    return true;
}

} // namespace

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
                                                        const Progress& progress,
                                                        const Observe& observe) {
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
    if (file.exists() && !file.isFile()) {
        return failure(DatabaseErrorCode::Path, QStringLiteral("Locate database"),
                       QStringLiteral("The database path must be a regular file."));
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
    if (canonicalDirectory.isEmpty() || canonicalPath.isEmpty()) {
        return failure(DatabaseErrorCode::Path, QStringLiteral("Locate database"),
                       QStringLiteral("Cannot resolve the database location."));
    }
    const QString lockPath = canonicalPath + QStringLiteral(".lock");
    // QLockFile reads metadata with unbounded readLine(); stale cleanup also opens
    // a separate .rmlock file. Do not let either touch untrusted special files.
    if (!safeFile(lockPath) || QFileInfo(lockPath).size() > 16 * 1024) {
        return failure(DatabaseErrorCode::Locked, QStringLiteral("Validate database lock"),
                       QStringLiteral("The database lock file is unsafe or oversized."));
    }
    const QFileInfo cleanupLock(lockPath + QStringLiteral(".rmlock"));
    if (cleanupLock.exists() || cleanupLock.isSymLink()) {
        return failure(DatabaseErrorCode::Locked, QStringLiteral("Validate database lock"),
                       QStringLiteral("A database lock cleanup file is already present."));
    }
    lock_ = std::make_unique<QLockFile>(lockPath);
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock(0)) {
        return failure(DatabaseErrorCode::Locked, QStringLiteral("Lock database"),
                       lock_->error() == QLockFile::LockFailedError
                           ? QStringLiteral("This database is already in use by another instance.")
                           : QStringLiteral("Cannot create the database lock file."));
    }
    // SQLite may recover journals or open WAL/SHM files during open/schema reads,
    // before migration validation or permission repair has a chance to run.
    for (const auto& suffix :
         {QString(), QStringLiteral("-wal"), QStringLiteral("-shm"), QStringLiteral("-journal")}) {
        if (!safeFile(canonicalPath + suffix)) {
            return failure(DatabaseErrorCode::Path, QStringLiteral("Validate database files"),
                           QStringLiteral("A database file or sidecar is not a regular, singly "
                                          "linked file with readable metadata."));
        }
    }
    if (!QFileInfo::exists(canonicalPath)) {
        if (const auto error = createPrivateFile(canonicalPath)) {
            return failure(DatabaseErrorCode::Path, QStringLiteral("Create database file"), *error);
        }
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
        for (const auto& statement : {QStringLiteral("PRAGMA trusted_schema = OFF"),
                                      QStringLiteral("PRAGMA foreign_keys = ON"),
                                      QStringLiteral("PRAGMA synchronous = FULL")}) {
            if (!query.exec(statement)) {
                return failure(DatabaseErrorCode::Configuration, QStringLiteral("Configure SQLite"),
                               query.lastError().text(), query.lastError().nativeErrorCode());
            }
        }
    }
    if (auto error = configure(QStringLiteral("PRAGMA trusted_schema"), 0)) {
        return error;
    }
    if (auto error = configure(QStringLiteral("PRAGMA synchronous"), 2)) {
        return error;
    }
    if (auto error = configure(QStringLiteral("PRAGMA foreign_keys"), 1)) {
        return error;
    }
    if (auto error = configure(QStringLiteral("PRAGMA busy_timeout"), 5000)) {
        return error;
    }
    // Text-to-BLOB projections validate exact UTF-8 bytes. Application databases
    // use SQLite's default encoding; reject foreign encodings before any migration/write.
    QSqlQuery encoding(database_);
    if (!encoding.exec(QStringLiteral("PRAGMA main.encoding")) || !encoding.next() ||
        encoding.value(0).toString() != QStringLiteral("UTF-8")) {
        return failure(DatabaseErrorCode::Configuration, QStringLiteral("Verify database encoding"),
                       QStringLiteral("The application database must use UTF-8 text encoding."));
    }
    encoding.finish();
    if (auto error = migrate(database_, canonicalPath, registry, progress, observe)) {
        return error;
    }
    // Custom historical registries are used only by persistence migration fixtures.
    if (registry.back().version >= 6) {
        progress(QStringLiteral("Recovering unfinished reviews…"));
        return interruptReviewJobs(canonicalPath);
    }
    return std::nullopt;
}

} // namespace chess_review::persistence::detail
