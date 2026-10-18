#include "app/StartupCoordinator.h"

#include "ui/StartupWindow.h"

#include <QCoreApplication>
#include <QEvent>

namespace chess_review::app {

StartupCoordinator::StartupCoordinator(ui::StartupWindow& window, QObject* parent)
    : QObject(parent) {
    QCoreApplication::instance()->installEventFilter(this);
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
    connect(&database_, &persistence::Database::closed, this, [this] {
        stopped_ = true;
        emit finished();
    });
}

bool StartupCoordinator::eventFilter(QObject* watched, QEvent* event) {
    if (watched == QCoreApplication::instance() && event->type() == QEvent::Quit && !stopped_) {
        shutdown();
        return true;
    }
    return QObject::eventFilter(watched, event);
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
