#include "ui/chess/ChessBoardWidget.h"
#include "ui/games/GamesPage.h"
#include "ui/shell/MainWindow.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
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
                if (mode.startsWith(QStringLiteral("import-"))) {
                    if (phase == 0) {
                        QFile input(QDir::current().filePath(QStringLiteral("exit-import.pgn")));
                        if (!input.open(QIODevice::WriteOnly)) {
                            qFatal("Could not create import exit fixture");
                        }
                        QByteArray bytes;
                        for (int i = 0; i < 10'000; ++i) {
                            bytes += "1.e4 *\n";
                        }
                        if (input.write(bytes) != bytes.size()) {
                            qFatal("Could not write import exit fixture");
                        }
                        if (mode == QStringLiteral("import-reading") &&
                            !input.resize(64 * 1024 * 1024)) {
                            qFatal("Could not resize import exit fixture");
                        }
                        input.close();
                        window->findChild<QAction*>(QStringLiteral("gamesAction"))->trigger();
                        auto* page = window->findChild<chess_review::ui::GamesPage*>();
                        QMimeData mime;
                        mime.setUrls({QUrl::fromLocalFile(input.fileName())});
                        QDragEnterEvent drag(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton,
                                             Qt::NoModifier);
                        QApplication::sendEvent(page, &drag);
                        QDropEvent drop(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton,
                                        Qt::NoModifier);
                        QApplication::sendEvent(page, &drop);
                        if (!drop.isAccepted()) {
                            qFatal("Import exit probe did not submit its batch");
                        }
                        std::fputs("import-started\n", stdout);
                        std::fflush(stdout);
                        ++phase;
                        if (mode == QStringLiteral("import-reading")) {
                            timer->stop();
                            window->close();
                        }
                        return;
                    }
                    auto* progress = window->findChild<QLabel*>(QStringLiteral("importStatus"));
                    if (!progress || !progress->text().startsWith(QStringLiteral("Importing")) ||
                        progress->text().contains(QStringLiteral(" 0 imported"))) {
                        return;
                    }
                    timer->stop();
                    window->close();
                    return;
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
