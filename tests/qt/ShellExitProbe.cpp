#include "ui/shell/MainWindow.h"
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QObject>
#include <QString>
#include <QTimer>
#include <cstdio>

namespace {
void installExitDriver() {
    // Schedule after QApplication construction. No test hook is compiled into the real app.
    QTimer::singleShot(0, QCoreApplication::instance(), [] {
        auto* timer = new QTimer(QCoreApplication::instance());
        const auto mode = qEnvironmentVariable("CHESS_REVIEW_EXIT_MODE");
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, timer, [] {
            std::fputs("event-loop-quitting\n", stdout);
            std::fflush(stdout);
        });
        QObject::connect(timer, &QTimer::timeout, timer, [timer, mode] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* window = qobject_cast<chess_review::ui::MainWindow*>(widget);
                if (!window || !window->isVisible()) {
                    continue;
                }
                auto* status = window->findChild<QLabel*>(QStringLiteral("startupStatus"));
                if (!status) {
                    qFatal("Exit probe has no startup status");
                }
                const bool failure =
                    status->text() == QStringLiteral("Could not initialize local storage");
                const bool ready = !status->isVisible();
                if (mode != QStringLiteral("startup") && !ready && !failure) {
                    continue;
                }
                timer->stop();
                window->findChild<QAction*>(QStringLiteral("puzzlesAction"))->trigger();
                if (mode == QStringLiteral("menu")) {
                    window->findChild<QAction*>(QStringLiteral("quitAction"))->trigger();
                } else if (mode == QStringLiteral("application")) {
                    QCoreApplication::quit();
                } else {
                    if (mode == QStringLiteral("diagnostics")) {
                        window->findChild<QAction*>(QStringLiteral("diagnosticsAction"))->trigger();
                        if (!window->findChild<QWidget*>(QStringLiteral("diagnosticsDialog"))) {
                            qFatal("Exit probe did not open diagnostics");
                        }
                    }
                    window->close();
                }
                return;
            }
        });
        timer->start(0);
    });
}
} // namespace
Q_COREAPP_STARTUP_FUNCTION(installExitDriver)
