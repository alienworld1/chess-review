#include "ui/chess/ChessBoardWidget.h"
#include "ui/shell/MainWindow.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QMessageBox>
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
        QObject::connect(timer, &QTimer::timeout, timer, [timer, mode, phase = 0]() mutable {
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
                if (mode.startsWith(QStringLiteral("analysis-"))) {
                    if (phase == 0) {
                        window->findChild<QAction*>(QStringLiteral("analyzeAction"))->trigger();
                        ++phase;
                        return;
                    }
                    auto* board = window->findChild<chess_review::ui::ChessBoardWidget*>();
                    if (!board || !board->state().interactive) {
                        return;
                    }
                    if (phase == 1) {
                        board->activateSquare(*chess_review::chess::Square::fromName("e2"));
                        board->activateSquare(*chess_review::chess::Square::fromName("e4"));
                        ++phase;
                        return;
                    }
                    if (board->state().sideToMove != chess_review::chess::Color::Black) {
                        return;
                    }
                    timer->stop();
                    const auto quit = [window, mode] {
                        if (mode.endsWith(QStringLiteral("application"))) {
                            QCoreApplication::quit();
                        } else if (mode.endsWith(QStringLiteral("menu"))) {
                            window->findChild<QAction*>(QStringLiteral("quitAction"))->trigger();
                        } else {
                            window->close();
                        }
                    };
                    const auto answer = [](QMessageBox::StandardButton button) {
                        QTimer::singleShot(0, qApp, [button] {
                            auto* dialog =
                                qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                            if (!dialog) {
                                qFatal("Discard prompt was not shown");
                            }
                            dialog->button(button)->click();
                        });
                    };
                    if (mode.contains(QStringLiteral("cancel"))) {
                        answer(QMessageBox::Cancel);
                        quit();
                        QTimer::singleShot(0, window, [window, board, quit, answer] {
                            if (!window->isVisible() || !board->state().interactive ||
                                board->state().sideToMove != chess_review::chess::Color::Black) {
                                qFatal("Discard cancellation lost the live analysis session");
                            }
                            std::fputs("discard-cancelled\n", stdout);
                            std::fflush(stdout);
                            answer(QMessageBox::Discard);
                            quit();
                        });
                    } else {
                        answer(QMessageBox::Discard);
                        quit();
                    }
                    return;
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
