#include "app/StartupCoordinator.h"

#include "app/Diagnostic.h"
#include "diagnostics/Logger.h"
#include "ui/StartupWindow.h"

#include <QCoreApplication>
#include <QEvent>

namespace chess_review::app {

StartupCoordinator::StartupCoordinator(ui::StartupWindow& window, QObject* parent)
    : QObject(parent) {
    connectWindow(window);
}

StartupCoordinator::StartupCoordinator(ui::StartupWindow& window, diagnostics::Logger& logger,
                                       QObject* parent)
    : QObject(parent), logger_(&logger), database_(logger) {
    window.setDiagnostics(logger);
    connect(&logger, &diagnostics::Logger::closed, this, &StartupCoordinator::finish);
    connectWindow(window);
}

void StartupCoordinator::connectWindow(ui::StartupWindow& window) {
    QCoreApplication::instance()->installEventFilter(this);
    connect(&database_, &persistence::Database::progress, &window,
            &ui::StartupWindow::showProgress);
    connect(&database_, &persistence::Database::ready, &window, &ui::StartupWindow::showReady);
    connect(&database_, &persistence::Database::failed, &window,
            [&window](const persistence::DatabaseError& error) {
                window.showFailure(storageDiagnostic(error));
            });
    connect(&window, &ui::StartupWindow::closeRequested, this, &StartupCoordinator::shutdown);
    connect(&database_, &persistence::Database::closed, this, [this] {
        if (logger_) {
            logger_->close();
        } else {
            finish();
        }
    });
}

void StartupCoordinator::finish() {
    stopped_ = true;
    emit finished();
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
