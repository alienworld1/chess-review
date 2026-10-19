#include "TestEnvironment.h"
#include "app/NavigationController.h"
#include "app/StartupCoordinator.h"
#include "app/UiStateStore.h"
#include "diagnostics/Logger.h"
#include "ui/shell/MainWindow.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <array>

using namespace chess_review;

namespace {
void awaitClose(app::UiStateStore& store, QSignalSpy& closed) {
    store.saveAndClose();
    if (closed.isEmpty() && !closed.wait(5000)) {
        qFatal("UI state worker did not close");
    }
}
} // namespace
class ShellTest final : public QObject {
    Q_OBJECT
  private slots:
    void init();
    void navigation();
    void diagnosticsSurviveReadiness();
    void geometry_data();
    void geometry();
    void restoreAndReset();
    void preferencesRoundTrip();
    void preferencesSaveFailure();
    void preferencesFailure_data();
    void preferencesFailure();
    void shutdownAndPersistence_data();
    void shutdownAndPersistence();
    void quitBeforePreferencesLoad();
    void layout_data();
    void layout();
};
void ShellTest::init() {
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        QTest::failOnWarning(QRegularExpression(
            QStringLiteral("^(?!This plugin does not support propagateSizeHints\\(\\)$)")));
    } else {
        QTest::failOnWarning();
    }
}
void ShellTest::navigation() {
    app::NavigationController navigation;
    ui::MainWindow window(navigation);
    window.show();
    auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("destinationPages"));
    QVERIFY(pages);
    QCOMPARE(pages->count(), 5);
    QSignalSpy changed(&navigation, &app::NavigationController::destinationChanged);
    const std::array destinations{app::Destination::Games, app::Destination::Puzzles,
                                  app::Destination::Analyze, app::Destination::Settings,
                                  app::Destination::Home};
    for (const auto destination : destinations) {
        const auto id = app::destinationId(destination);
        auto* button = window.findChild<QToolButton*>(id + QStringLiteral("Button"));
        auto* action = window.findChild<QAction*>(id + QStringLiteral("Action"));
        QVERIFY(button && action);
        QCOMPARE(button->defaultAction(), action);
        bool inMenu = false;
        for (auto* object : action->associatedObjects()) {
            inMenu = inMenu || qobject_cast<QMenu*>(object) != nullptr;
        }
        QVERIFY(inMenu);
        button->setFocus();
        QTest::keyPress(button, Qt::Key_Space);
        QTest::keyRelease(button, Qt::Key_Space);
        QCOMPARE(navigation.current(), destination);
        QVERIFY(action->isChecked());
        QCOMPARE(pages->currentWidget()->objectName(), id + QStringLiteral("Page"));
        const auto count = changed.count();
        action->trigger();
        QCOMPARE(changed.count(), count);
    }
    window.showFailure({QStringLiteral("storage.open"),
                        QStringLiteral("Storage unavailable"),
                        QStringLiteral("Relaunch to retry"),
                        {},
                        {}});
    window.findChild<QAction*>(QStringLiteral("gamesAction"))->trigger();
    QCOMPARE(navigation.current(), app::Destination::Games);
    QVERIFY(window.findChild<QLabel*>(QStringLiteral("failureSummary"))->isVisible());
    window.showClosing();
    window.findChild<QAction*>(QStringLiteral("homeAction"))->trigger();
    QCOMPARE(navigation.current(), app::Destination::Games);
    QVERIFY(!window.findChild<QAction*>(QStringLiteral("resetLayoutAction"))->isEnabled());
}
void ShellTest::diagnosticsSurviveReadiness() {
    app::NavigationController navigation;
    ui::MainWindow window(navigation);
    window.show();
    auto* action = window.findChild<QAction*>(QStringLiteral("diagnosticsAction"));
    action->trigger();
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("diagnosticsDialog"));
    QVERIFY(dialog && dialog->isVisible());
    window.showReady();
    QVERIFY(dialog->isVisible());
    QVERIFY(!window.findChild<QLabel*>(QStringLiteral("startupStatus"))->isVisible());
    dialog->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    action->trigger();
    dialog = window.findChild<QDialog*>(QStringLiteral("diagnosticsDialog"));
    QVERIFY(dialog && dialog->isVisible());
}
void ShellTest::geometry_data() {
    QTest::addColumn<QRect>("desired");
    QTest::addColumn<QRect>("available");
    QTest::addColumn<QRect>("expected");
    QTest::newRow("removed-monitor")
        << QRect(2200, 500, 1000, 700) << QRect(0, 30, 1280, 770) << QRect(280, 100, 1000, 700);
    QTest::newRow("small-desktop")
        << QRect(0, 0, 1360, 850) << QRect(0, 24, 800, 576) << QRect(0, 24, 800, 576);
    QTest::newRow("negative-monitor") << QRect(-1900, 10, 1000, 700) << QRect(-1920, 0, 1920, 1080)
                                      << QRect(-1900, 10, 1000, 700);
}
void ShellTest::geometry() {
    QFETCH(QRect, desired);
    QFETCH(QRect, available);
    QFETCH(QRect, expected);
    QCOMPARE(ui::fittedWindowGeometry(desired, available), expected);
}
void ShellTest::restoreAndReset() {
    app::NavigationController firstNavigation;
    ui::MainWindow first(firstNavigation);
    first.resize(720, 500);
    firstNavigation.navigate(app::Destination::Analyze);
    const auto snapshot = first.uiState();
    app::NavigationController navigation;
    ui::MainWindow window(navigation);
    window.restoreUiState(snapshot);
    window.show();
    QCOMPARE(navigation.current(), app::Destination::Analyze);
    QCOMPARE(window.size(), first.size());
    window.setWindowState(Qt::WindowMaximized);
    QVERIFY(window.uiState().maximized);
    const auto maximized = window.uiState();
    app::NavigationController maximizedNavigation;
    ui::MainWindow maximizedWindow(maximizedNavigation);
    maximizedWindow.restoreUiState(maximized);
    QVERIFY(maximizedWindow.isMaximized());
    maximizedWindow.showMinimized();
    const auto minimizedSnapshot = maximizedWindow.uiState();
    app::NavigationController resumedNavigation;
    ui::MainWindow resumed(resumedNavigation);
    resumed.restoreUiState(minimizedSnapshot);
    QVERIFY(!resumed.isMinimized());
    window.findChild<QAction*>(QStringLiteral("resetLayoutAction"))->trigger();
    QVERIFY(!window.isMaximized());
    QCOMPARE(navigation.current(), app::Destination::Home);
    app::NavigationController invalidNavigation;
    ui::MainWindow invalid(invalidNavigation);
    QSignalSpy invalidState(&invalid, &ui::MainWindow::invalidUiState);
    invalid.restoreUiState({QByteArray("bad geometry"), false, app::Destination::Home});
    QCOMPARE(invalidState.count(), 1);
    QVERIFY(!invalid.findChild<QLabel*>(QStringLiteral("uiStateNotice"))->isHidden());
}
void ShellTest::preferencesRoundTrip() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath(QStringLiteral("config/ui-state.ini"));
    app::UiStateStore writer;
    QSignalSpy loaded(&writer, &app::UiStateStore::loaded);
    QSignalSpy failed(&writer, &app::UiStateStore::failed);
    QSignalSpy closed(&writer, &app::UiStateStore::closed);
    const auto cleanup = qScopeGuard([&] { awaitClose(writer, closed); });
    writer.load(path);
    QVERIFY(loaded.wait(5000));
    QVERIFY(!QFileInfo::exists(path));
    writer.saveAndClose(
        app::UiState{QByteArray("geometry bytes"), true, app::Destination::Settings});
    QTRY_COMPARE(closed.count(), 1);
    QCOMPARE(failed.count(), 0);
    app::UiStateStore reader;
    QSignalSpy restored(&reader, &app::UiStateStore::loaded);
    QSignalSpy readerClosed(&reader, &app::UiStateStore::closed);
    const auto readerCleanup = qScopeGuard([&] { awaitClose(reader, readerClosed); });
    reader.load(path);
    QVERIFY(restored.wait(5000));
    const auto state = qvariant_cast<app::UiState>(restored.front().front());
    QCOMPARE(state.geometry, QByteArray("geometry bytes"));
    QVERIFY(state.maximized);
    QCOMPARE(state.destination, app::Destination::Settings);
}
void ShellTest::preferencesSaveFailure() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath(QStringLiteral("ui-state.ini"));
    app::UiStateStore store;
    QSignalSpy loaded(&store, &app::UiStateStore::loaded);
    QSignalSpy failed(&store, &app::UiStateStore::failed);
    QSignalSpy closed(&store, &app::UiStateStore::closed);
    const auto cleanup = qScopeGuard([&] { awaitClose(store, closed); });
    store.load(path);
    QVERIFY(loaded.wait(5000));
    QCOMPARE(failed.count(), 0);
    QVERIFY(QDir().mkdir(path));
    store.saveAndClose(app::UiState{{}, false, app::Destination::Games});
    QTRY_COMPARE(closed.count(), 1);
    QCOMPARE(failed.count(), 1);
    QVERIFY(QFileInfo(path).isDir());
}
void ShellTest::preferencesFailure_data() {
    QTest::addColumn<QString>("kind");
    for (const auto* kind : {"directory", "relative", "oversized", "malformed",
                             "unknown-destination", "invalid-boolean"}) {
        QTest::newRow(kind) << QString::fromLatin1(kind);
    }
}
void ShellTest::preferencesFailure() {
    QFETCH(QString, kind);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QString path = root.filePath(QStringLiteral("ui-state.ini"));
    if (kind == QStringLiteral("directory")) {
        path = root.path();
    } else if (kind == QStringLiteral("relative")) {
        path = QStringLiteral("relative/ui-state.ini");
    } else {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QByteArray bytes("[shell]\nv1\\destination=unknown\n");
        if (kind == QStringLiteral("oversized")) {
            bytes = QByteArray(65537, 'x');
        }
        if (kind == QStringLiteral("malformed")) {
            bytes = QByteArray("[bad section\n");
        }
        if (kind == QStringLiteral("invalid-boolean")) {
            bytes = QByteArray("[shell]\nv1\\maximized=garbage\n");
        }
        QCOMPARE(file.write(bytes), bytes.size());
    }
    app::UiStateStore store;
    QSignalSpy loaded(&store, &app::UiStateStore::loaded);
    QSignalSpy failed(&store, &app::UiStateStore::failed);
    QSignalSpy closed(&store, &app::UiStateStore::closed);
    const auto cleanup = qScopeGuard([&] { awaitClose(store, closed); });
    store.load(path);
    QVERIFY(loaded.wait(5000));
    QCOMPARE(failed.count(), 1);
    const auto state = qvariant_cast<app::UiState>(loaded.front().front());
    QCOMPARE(state.destination, app::Destination::Home);
    QVERIFY(state.geometry.isEmpty());
}
void ShellTest::shutdownAndPersistence_data() {
    QTest::addColumn<bool>("badStorage");
    QTest::addColumn<bool>("badPreferences");
    QTest::newRow("normal") << false << false;
    QTest::newRow("storage-failed") << true << false;
    QTest::newRow("preferences-failed") << false << true;
    QTest::newRow("both-failed") << true << true;
}
void ShellTest::shutdownAndPersistence() {
    QFETCH(bool, badStorage);
    QFETCH(bool, badPreferences);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto database = root.filePath(QStringLiteral("library.sqlite3"));
    if (badStorage) {
        QFile file(database);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("not a database") > 0);
    }
    const auto preferences =
        badPreferences ? root.path() : root.filePath(QStringLiteral("ui-state.ini"));
    diagnostics::Logger logger(root.filePath(QStringLiteral("logs")));
    app::NavigationController navigation;
    ui::MainWindow window(navigation);
    window.setDiagnostics(logger);
    app::StartupCoordinator startup(logger);
    QSignalSpy finished(&startup, &app::StartupCoordinator::finished);
    const auto cleanup = qScopeGuard([&] {
        startup.shutdown();
        if (finished.isEmpty() && !finished.wait(5000)) {
            qFatal("Startup failed to close");
        }
    });
    connect(&startup, &app::StartupCoordinator::initialUiState, &window,
            [&](const app::UiState& state) {
                window.restoreUiState(state);
                window.show();
            });
    connect(&startup, &app::StartupCoordinator::ready, &window, &ui::MainWindow::showReady);
    connect(&startup, &app::StartupCoordinator::failed, &window, &ui::MainWindow::showFailure);
    connect(&startup, &app::StartupCoordinator::uiStateFailed, &window,
            &ui::MainWindow::showUiStateFailure);
    connect(&window, &ui::MainWindow::closeRequested, &startup, &app::StartupCoordinator::shutdown);
    connect(&startup, &app::StartupCoordinator::stopping, &window, [&] {
        startup.setFinalUiState(window.uiState());
        window.showClosing();
    });
    startup.start(database, preferences);
    QTRY_COMPARE(startup.state(), badStorage ? app::Lifecycle::Failed : app::Lifecycle::Ready);
    window.findChild<QAction*>(QStringLiteral("puzzlesAction"))->trigger();
    if (badPreferences) {
        QVERIFY(window.findChild<QLabel*>(QStringLiteral("uiStateNotice"))->isVisible());
    }
    window.close();
    QVERIFY(window.isVisible());
    startup.shutdown();
    window.close();
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(logger.health(), diagnostics::Health::Closed);
    const auto report = logger.report();
    QVERIFY(report.indexOf(QStringLiteral("storage.closed")) <
            report.indexOf(QStringLiteral("session.stopped")));
    if (!badPreferences) {
        QSettings saved(preferences, QSettings::IniFormat);
        QCOMPARE(saved.value(QStringLiteral("shell/v1/destination")).toString(),
                 QStringLiteral("puzzles"));
    } else {
        QVERIFY(report.contains(QStringLiteral("ui_state.failed")));
    }
    QVERIFY(!report.contains(root.path()));
}
void ShellTest::quitBeforePreferencesLoad() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath(QStringLiteral("ui-state.ini"));
    const QByteArray original("[shell]\nv1\\destination=settings\n");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(original), original.size());
    }
    app::StartupCoordinator startup;
    QSignalSpy finished(&startup, &app::StartupCoordinator::finished);
    QSignalSpy initial(&startup, &app::StartupCoordinator::initialUiState);
    startup.start(root.filePath(QStringLiteral("library.sqlite3")), path);
    QEvent quit(QEvent::Quit);
    QCoreApplication::sendEvent(qApp, &quit);
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(initial.count(), 0);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
    QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("library.sqlite3"))));
}
void ShellTest::layout_data() {
    QTest::addColumn<QSize>("size");
    QTest::addColumn<bool>("failure");
    QTest::newRow("default") << QSize(1360, 850) << false;
    QTest::newRow("laptop") << QSize(1100, 720) << true;
    QTest::newRow("small") << QSize(800, 600) << true;
    QTest::newRow("desktop") << QSize(1600, 1000) << false;
}
void ShellTest::layout() {
    QFETCH(QSize, size);
    QFETCH(bool, failure);
    app::NavigationController navigation;
    ui::MainWindow window(navigation);
    auto font = window.font();
    font.setPointSizeF(font.pointSizeF() * 1.25);
    window.setFont(font);
    window.resize(size);
    if (failure) {
        window.showUiStateFailure();
        window.showFailure({QStringLiteral("storage.open"),
                            QStringLiteral("Storage unavailable"),
                            QStringLiteral("Relaunch after fixing storage access."),
                            QStringLiteral("Long private output\n").repeated(5000),
                            {}});
    } else {
        window.showReady();
    }
    window.show();
    if (failure) {
        window.findChild<QPushButton*>(QStringLiteral("revealDetails"))->click();
    }
    QCoreApplication::sendPostedEvents();
    QVERIFY(window.size().width() <= size.width());
    QVERIFY(window.size().height() <= size.height());
    for (const auto* name : {"homeButton", "settingsButton"}) {
        auto* button = window.findChild<QToolButton*>(QString::fromLatin1(name));
        QVERIFY(window.rect().contains(QRect(button->mapTo(&window, QPoint()), button->size())));
    }
    if (qEnvironmentVariableIsSet("CHESS_REVIEW_SCREENSHOT_DIR")) {
        const auto filename = qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR") +
                              QLatin1Char('/') + QString::fromLatin1(QTest::currentDataTag()) +
                              QStringLiteral(".png");
        QVERIFY(window.grab().save(filename));
    }
}
int main(int argc, char* argv[]) {
    const TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("chess-review"));
    application.setQuitOnLastWindowClosed(false);
    ShellTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ShellTest.moc"
