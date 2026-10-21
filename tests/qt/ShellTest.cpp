#include "TestEnvironment.h"
#include "app/NavigationController.h"
#include "app/StartupCoordinator.h"
#include "app/UiStateStore.h"
#include "diagnostics/Logger.h"
#include "ui/shell/MainWindow.h"
#include "ui/shell/StartupPanel.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLockFile>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <array>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace chess_review;

namespace {
void awaitClose(app::UiStateStore& store, QSignalSpy& closed) {
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&store, &app::UiStateStore::closed, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    store.saveAndClose();
    if (closed.isEmpty()) {
        timeout.start(5000);
        // Qt Test's signal wait skips its loop once an assertion has failed.
        loop.exec();
    }
    if (closed.isEmpty()) {
        qFatal("UI state worker did not close");
    }
}
} // namespace
class ShellTest final : public QObject {
    Q_OBJECT
  private slots:
    void init();
    void processExitsAfterShutdown_data();
    void processExitsAfterShutdown();
    void navigation();
    void navigationConstructionPreservesState();
    void navigationRedirectStaysSynchronized();
    void diagnosticsSurviveReadiness();
    void diagnosticsOutlivePanel();
    void preferencesLiveLock();
    void preferencesLockMetadata_data();
    void preferencesLockMetadata();
    void preferencesPreserveUnknownKeys();
    void preferencesUnsafeTargets_data();
    void preferencesUnsafeTargets();
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
void ShellTest::processExitsAfterShutdown_data() {
    QTest::addColumn<QString>("mode");
    for (const auto* mode :
         {"window", "menu", "application", "startup", "failure", "diagnostics", "analysis-window",
          "analysis-menu", "analysis-application", "analysis-cancel-window", "analysis-cancel-menu",
          "analysis-cancel-application", "import-reading", "import-saving"}) {
        QTest::newRow(mode) << QString::fromLatin1(mode);
    }
}
void ShellTest::processExitsAfterShutdown() {
#ifndef Q_OS_LINUX
    QSKIP("Full application profile isolation uses Linux XDG locations");
#else
    QFETCH(QString, mode);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto data = root.filePath(QStringLiteral("data/chess-review"));
    if (mode == QStringLiteral("failure")) {
        QVERIFY(QDir().mkpath(data));
        QFile invalid(QDir(data).filePath(QStringLiteral("chess-review.sqlite3")));
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        QVERIFY(invalid.write("invalid database") > 0);
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("XDG_DATA_HOME"), root.filePath(QStringLiteral("data")));
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), root.filePath(QStringLiteral("config")));
    environment.insert(QStringLiteral("XDG_CACHE_HOME"), root.filePath(QStringLiteral("cache")));
    environment.insert(QStringLiteral("CHESS_REVIEW_EXIT_MODE"), mode);
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(root.path());
    // This executable compiles the actual production main.cpp. Only its UI driver is test-only.
    process.start(QString::fromUtf8(CHESS_REVIEW_EXIT_PROBE), QStringList{});
    const auto cleanup = qScopeGuard([&] {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            if (!process.waitForFinished(5000)) {
                qFatal("Exit probe could not be reaped");
            }
        }
    });
    QVERIFY(process.waitForStarted(5000));
    const bool exited = process.waitForFinished(5000);
    if (!exited) {
        process.kill();
        QVERIFY(process.waitForFinished(5000));
    }
    QFile events(root.filePath(QStringLiteral("cache/chess-review/logs/events-0.jsonl")));
    QVERIFY(events.open(QIODevice::ReadOnly));
    const auto records = events.readAll();
    QVERIFY2(records.contains("session.stopped"), "Probe never finished orderly service shutdown");
    if (mode == QStringLiteral("failure")) {
        QVERIFY(records.contains("storage.failed"));
    } else if (mode != QStringLiteral("startup")) {
        QVERIFY(records.contains("storage.ready"));
    }
    QVERIFY2(exited, "Services finished, but QApplication::exec() did not exit");
    const auto output = process.readAllStandardOutput();
    QVERIFY(output.contains("event-loop-quitting"));
    if (mode.startsWith(QStringLiteral("import-"))) {
        QVERIFY(output.contains("import-started"));
    }
    if (mode.contains(QStringLiteral("cancel"))) {
        QVERIFY(output.contains("discard-cancelled"));
    }
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);
    if (mode != QStringLiteral("startup")) {
        QVERIFY(records.indexOf("storage.closed") < records.indexOf("session.stopped"));
    }
    QSettings saved(root.filePath(QStringLiteral("config/chess-review/ui-state.ini")),
                    QSettings::IniFormat);
    QCOMPARE(saved.value(QStringLiteral("shell/v1/destination")).toString(),
             mode.startsWith(QStringLiteral("analysis-")) ? QStringLiteral("analyze")
             : mode.startsWith(QStringLiteral("import-")) ? QStringLiteral("games")
                                                          : QStringLiteral("puzzles"));
#endif
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
void ShellTest::navigationConstructionPreservesState() {
    app::NavigationController navigation;
    navigation.navigate(app::Destination::Analyze);
    ui::MainWindow window(navigation);
    QCOMPARE(navigation.current(), app::Destination::Analyze);
    QVERIFY(window.findChild<QAction*>(QStringLiteral("analyzeAction"))->isChecked());
}
void ShellTest::navigationRedirectStaysSynchronized() {
    app::NavigationController navigation;
    connect(&navigation, &app::NavigationController::destinationChanged, &navigation,
            [&](app::Destination destination) {
                if (destination == app::Destination::Games) {
                    navigation.navigate(app::Destination::Settings);
                }
            });
    ui::MainWindow window(navigation);
    window.findChild<QAction*>(QStringLiteral("gamesAction"))->trigger();
    QCOMPARE(navigation.current(), app::Destination::Settings);
    auto* pages = window.findChild<QStackedWidget*>(QStringLiteral("destinationPages"));
    QCOMPARE(pages->currentWidget()->objectName(), QStringLiteral("settingsPage"));
    QVERIFY(window.findChild<QAction*>(QStringLiteral("settingsAction"))->isChecked());
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
void ShellTest::diagnosticsOutlivePanel() {
    QWidget owner;
    {
        ui::StartupPanel panel(&owner);
        panel.showDiagnostics();
    }
    auto* dialog = owner.findChild<QDialog*>(QStringLiteral("diagnosticsDialog"));
    QVERIFY(dialog);
    auto* refresh = dialog->findChild<QPushButton*>(QStringLiteral("refreshDiagnostics"));
    auto* report = dialog->findChild<QPlainTextEdit*>(QStringLiteral("safeDiagnosticsReport"));
    QVERIFY(refresh && report);
    refresh->click();
    QVERIFY(report->toPlainText().contains(QStringLiteral("no longer available")));
}
void ShellTest::preferencesLiveLock() {
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
    QLockFile lock(path + QStringLiteral(".lock"));
    QVERIFY(lock.tryLock(0));
    // Release after a bounded interval so the original indefinite lock path can finish.
    QTimer release;
    release.setSingleShot(true);
    connect(&release, &QTimer::timeout, &release, [&] { lock.unlock(); });
    release.start(700);
    QElapsedTimer elapsed;
    elapsed.start();
    store.saveAndClose(app::UiState{});
    QTRY_COMPARE(closed.count(), 1);
    QVERIFY2(elapsed.elapsed() < 500,
             "Settings must fail promptly instead of waiting for another writer");
    QCOMPARE(failed.count(), 1);
    QCOMPARE(qvariant_cast<app::UiStateError>(failed.front().front()), app::UiStateError::Busy);
    QVERIFY(!QFileInfo::exists(path));
}
void ShellTest::preferencesLockMetadata_data() {
    QTest::addColumn<QString>("suffix");
    QTest::addColumn<int>("bytes");
    QTest::newRow("oversized") << QStringLiteral(".lock") << 16385;
    QTest::newRow("cleanup-lock") << QStringLiteral(".lock.rmlock") << 1;
}
void ShellTest::preferencesLockMetadata() {
    QFETCH(QString, suffix);
    QFETCH(int, bytes);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath(QStringLiteral("ui-state.ini"));
    const auto lockPath = path + suffix;
    const QByteArray contents(bytes, 'x');
    {
        QFile file(lockPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), contents.size());
    }
    app::UiStateStore store;
    QSignalSpy loaded(&store, &app::UiStateStore::loaded);
    QSignalSpy failed(&store, &app::UiStateStore::failed);
    QSignalSpy closed(&store, &app::UiStateStore::closed);
    const auto cleanup = qScopeGuard([&] { awaitClose(store, closed); });
    store.load(path);
    QVERIFY(loaded.wait(5000));
    store.saveAndClose(app::UiState{});
    QTRY_COMPARE(closed.count(), 1);
    QCOMPARE(failed.count(), 1);
    QVERIFY(!QFileInfo::exists(path));
    QFile file(lockPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), contents);
}
void ShellTest::preferencesPreserveUnknownKeys() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath(QStringLiteral("ui-state.ini"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setValue(QStringLiteral("future/value"), QStringLiteral("preserve me"));
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
    }
    app::UiStateStore store;
    QSignalSpy loaded(&store, &app::UiStateStore::loaded);
    QSignalSpy failed(&store, &app::UiStateStore::failed);
    QSignalSpy closed(&store, &app::UiStateStore::closed);
    const auto cleanup = qScopeGuard([&] { awaitClose(store, closed); });
    store.load(path);
    QVERIFY(loaded.wait(5000));
    store.saveAndClose(app::UiState{{}, false, app::Destination::Games});
    QTRY_COMPARE(closed.count(), 1);
    QCOMPARE(failed.count(), 0);
    QSettings settings(path, QSettings::IniFormat);
    QCOMPARE(settings.value(QStringLiteral("future/value")).toString(),
             QStringLiteral("preserve me"));
    QCOMPARE(settings.value(QStringLiteral("shell/v1/destination")).toString(),
             QStringLiteral("games"));
    QCOMPARE(QDir(root.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot),
             QStringList{QStringLiteral("ui-state.ini")});
}
void ShellTest::preferencesUnsafeTargets_data() {
    QTest::addColumn<bool>("lockTarget");
    QTest::addColumn<bool>("fifo");
    QTest::newRow("state-symlink") << false << false;
    QTest::newRow("lock-symlink") << true << false;
    QTest::newRow("state-fifo") << false << true;
    QTest::newRow("lock-fifo") << true << true;
}
void ShellTest::preferencesUnsafeTargets() {
#ifndef Q_OS_UNIX
    QSKIP("Unix symlink/FIFO fixtures");
#else
    QFETCH(bool, lockTarget);
    QFETCH(bool, fifo);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath(QStringLiteral("ui-state.ini"));
    const auto unsafe = lockTarget ? path + QStringLiteral(".lock") : path;
    const auto sentinel = root.filePath(QStringLiteral("sentinel"));
    const QByteArray contents("preserve this unrelated file");
    {
        QFile file(sentinel);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), contents.size());
    }
    if (fifo) {
        QCOMPARE(::mkfifo(QFile::encodeName(unsafe).constData(), 0600), 0);
    } else {
        QVERIFY(QFile::link(sentinel, unsafe));
    }
    app::UiStateStore store;
    QSignalSpy loaded(&store, &app::UiStateStore::loaded);
    QSignalSpy failed(&store, &app::UiStateStore::failed);
    QSignalSpy closed(&store, &app::UiStateStore::closed);
    const auto cleanup = qScopeGuard([&] { awaitClose(store, closed); });
    store.load(path);
    QVERIFY(loaded.wait(5000));
    store.saveAndClose(app::UiState{});
    QTRY_COMPARE(closed.count(), 1);
    QVERIFY(!failed.isEmpty());
    QFile file(sentinel);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), contents);
    QVERIFY(fifo ? QFileInfo(unsafe).exists() : QFileInfo(unsafe).isSymLink());
#endif
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
    for (const auto* kind :
         {"directory", "relative", "oversized", "malformed", "unknown-destination",
          "invalid-boolean", "invalid-geometry-type", "blocked-parent"}) {
        QTest::newRow(kind) << QString::fromLatin1(kind);
    }
}
void ShellTest::preferencesFailure() {
    QFETCH(QString, kind);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QString path = root.filePath(QStringLiteral("ui-state.ini"));
    if (kind == QStringLiteral("blocked-parent")) {
        QFile obstruction(root.filePath(QStringLiteral("blocked")));
        QVERIFY(obstruction.open(QIODevice::WriteOnly));
        obstruction.close();
        path = obstruction.fileName() + QStringLiteral("/ui-state.ini");
    } else if (kind == QStringLiteral("directory")) {
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
        if (kind == QStringLiteral("invalid-geometry-type")) {
            bytes = QByteArray("[shell]\nv1\\geometry=@Rect(0 0 5 5)\n");
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
    if (kind == QStringLiteral("malformed")) {
        QFile original(path);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const auto contents = original.readAll();
        original.close();
        store.saveAndClose(app::UiState{});
        QTRY_COMPARE(closed.count(), 1);
        QCOMPARE(failed.count(), 2);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), contents);
    }
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
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&startup, &app::StartupCoordinator::finished, &loop, &QEventLoop::quit);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        startup.shutdown();
        if (finished.isEmpty()) {
            timeout.start(5000);
            loop.exec();
        }
        if (finished.isEmpty()) {
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
    connect(&startup, &app::StartupCoordinator::finished, &window,
            &ui::MainWindow::completeShutdown);
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
    QVERIFY(!window.isVisible());
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
    QTest::newRow("constrained") << QSize(640, 480) << true;
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
    auto* scroll = window.findChild<QScrollArea*>(QStringLiteral("shellScrollArea"));
    QVERIFY(scroll);
    for (const auto* name : {"homeButton", "settingsButton"}) {
        auto* button = window.findChild<QToolButton*>(QString::fromLatin1(name));
        scroll->ensureWidgetVisible(button);
        QVERIFY(scroll->viewport()->rect().contains(
            QRect(button->mapTo(scroll->viewport(), QPoint()), button->size())));
    }
    if (failure) {
        auto* copy = window.findChild<QPushButton*>(QStringLiteral("copyDiagnostics"));
        scroll->ensureWidgetVisible(copy);
        QVERIFY(scroll->viewport()->rect().contains(
            QRect(copy->mapTo(scroll->viewport(), QPoint()), copy->size())));
    }
    scroll->verticalScrollBar()->setValue(0);
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
