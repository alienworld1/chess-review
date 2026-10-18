#include "app/Diagnostic.h"

#include "diagnostics/Logger.h"
#include "persistence/Database.h"

#include <QCoreApplication>

namespace chess_review::app {
namespace {
QString text(const char* value) { return QCoreApplication::translate("Diagnostic", value); }
} // namespace

QString Diagnostic::safeReport() const {
    return QStringLiteral("Error: %1\n%2\n%3\n%4\n").arg(code, summary, nextSteps, safeContext);
}

Diagnostic storageDiagnostic(const persistence::DatabaseError& error) {
    using persistence::DatabaseErrorCode;
    Diagnostic result;
    // Preserve a useful failure for an unknown value while retaining compiler
    // warnings when a new known enumerator needs its own deliberate mapping.
    result.code = QStringLiteral("storage.unknown");
    result.summary = text("The local library could not be initialized.");
    result.nextSteps =
        text("Keep the library files and copy diagnostics for investigation before relaunching.");
    switch (error.code) {
    case DatabaseErrorCode::Path:
        result.code = QStringLiteral("storage.path");
        result.summary = text("The local library location could not be used.");
        result.nextSteps = text(
            "Check the database location and your access to its folder, then relaunch the app.");
        break;
    case DatabaseErrorCode::Locked:
        result.code = QStringLiteral("storage.locked");
        result.summary = text("The local library could not be locked for use.");
        result.nextSteps =
            text("Close other chess-review instances and check access to the library folder, then "
                 "relaunch. Do not delete lock files while another instance is running.");
        break;
    case DatabaseErrorCode::Open:
        result.code = QStringLiteral("storage.open");
        result.summary = text("The local library could not be opened.");
        result.nextSteps = text("Check access to the library file and available disk space, then "
                                "relaunch. Keep the existing file for investigation.");
        break;
    case DatabaseErrorCode::Configuration:
        result.code = QStringLiteral("storage.configuration");
        result.summary = text("The library could not be prepared safely.");
        result.nextSteps =
            text("Check that the library is on a local drive with write access. Keep its files "
                 "and copy diagnostics if the problem continues.");
        break;
    case DatabaseErrorCode::Schema:
        result.code = QStringLiteral("storage.schema");
        result.summary = text("This app could not read the library's database format.");
        result.nextSteps = text("Keep the existing file. Check that you are using the intended "
                                "library and app version; copy diagnostics for investigation.");
        break;
    case DatabaseErrorCode::Backup:
        result.code = QStringLiteral("storage.backup");
        result.summary =
            text("A verified backup could not be prepared before upgrading the library.");
        result.nextSteps =
            text("Check available disk space and access to the library's backup folder, then "
                 "relaunch. Keep the existing library and backup files.");
        break;
    case DatabaseErrorCode::Migration:
        result.code = QStringLiteral("storage.migration");
        result.summary = text("The library upgrade could not be completed.");
        result.nextSteps =
            text("Keep the library and any verified pre-upgrade backup. Copy diagnostics for "
                 "investigation before making changes to these files.");
        break;
    }
    // Recovery locations precede arbitrary-length native output so they remain
    // immediately visible when technical details are expanded.
    result.sensitiveDetails =
        QStringLiteral("Database: %1\nNative code: %2").arg(error.databasePath, error.nativeCode);
    if (error.migrationVersion) {
        result.sensitiveDetails += QStringLiteral("\nMigration: %1").arg(*error.migrationVersion);
        result.safeContext += QStringLiteral("Migration: %1\n").arg(*error.migrationVersion);
    }
    if (!error.backupPath.isEmpty()) {
        result.sensitiveDetails +=
            QStringLiteral("\nVerified pre-upgrade backup: %1").arg(error.backupPath);
        result.safeContext += QStringLiteral("Verified pre-upgrade backup available\n");
    }
    result.sensitiveDetails +=
        QStringLiteral("\n\nOperation: %1\nDetails: %2").arg(error.operation, error.detail);
    return result;
}
Diagnostic loggingDiagnostic(diagnostics::LogFailure failure) {
    using diagnostics::LogFailure;
    Diagnostic result;
    result.code = diagnostics::loggingFailureCode(failure);
    result.summary = text("Local logs could not be saved.");
    result.nextSteps =
        text("You can continue using the app and copy recent diagnostics from Help → Diagnostics.");
    switch (failure) {
    case LogFailure::None:
        return {};
    case LogFailure::Location:
        result.summary = text("The local log folder could not be created or used.");
        result.nextSteps += text(" Check access to the cache folder, then relaunch.");
        break;
    case LogFailure::UnsafeTarget:
        result.summary = text("An existing log or lock file could not be used safely.");
        result.nextSteps += text(" Check the log folder for unexpected files. Do not remove files "
                                 "while another instance is running.");
        break;
    case LogFailure::Busy:
        result.summary = text("The local log folder could not be reserved.");
        result.nextSteps +=
            text(" Close other instances before relaunching. Do not remove live lock files.");
        break;
    case LogFailure::Open:
        result.summary = text("The local log files could not be opened or protected.");
        result.nextSteps +=
            text(" Check access to the cache folder and available disk space, then relaunch.");
        break;
    case LogFailure::Write:
        result.summary = text("Writing local logs failed.");
        result.nextSteps +=
            text(" Check available disk space and access to the cache folder, then relaunch.");
        break;
    case LogFailure::Rotation:
        result.summary = text("Local log rotation could not be completed.");
        result.nextSteps +=
            text(" Check access to the log folder and available disk space, then relaunch.");
        break;
    }
    return result;
}

} // namespace chess_review::app
