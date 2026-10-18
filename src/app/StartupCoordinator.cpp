#include "app/StartupCoordinator.h"

#include "ui/StartupWindow.h"

#include <QDir>
#include <QStandardPaths>

namespace chess_review::app {

StartupCoordinator::StartupCoordinator(ui::StartupWindow& window, QObject* parent)
    : QObject(parent) {
    connect(&database_, &persistence::Database::progress, &window,
            &ui::StartupWindow::showProgress);
    connect(&database_, &persistence::Database::ready, &window, &ui::StartupWindow::showReady);
    connect(&database_, &persistence::Database::failed, &window,
            [&window](const persistence::DatabaseError& error) {
                QString diagnostic = QStringLiteral("%1: %2\n\nDatabase: %3")
                                         .arg(error.operation, error.detail, error.databasePath);
                if (error.migrationVersion) {
                    diagnostic += QStringLiteral("\nMigration: %1").arg(*error.migrationVersion);
                }
                if (!error.backupPath.isEmpty()) {
                    diagnostic += QStringLiteral("\nPre-upgrade backup: %1").arg(error.backupPath);
                }
                window.showFailure(diagnostic);
            });
    connect(&window, &ui::StartupWindow::closeRequested, this, &StartupCoordinator::shutdown);
    connect(&database_, &persistence::Database::closed, this, &StartupCoordinator::finished);
}

QString StartupCoordinator::defaultDatabasePath() {
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return directory.isEmpty() ? QString{}
                               : QDir(directory).filePath(QStringLiteral("chess-review.sqlite3"));
}

void StartupCoordinator::start(const QString& databasePath) { database_.initialize(databasePath); }

void StartupCoordinator::shutdown() {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    database_.close();
}

} // namespace chess_review::app
