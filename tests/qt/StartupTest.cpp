#include "app/AppPaths.h"
#include "app/Diagnostic.h"
#include "app/StartupCoordinator.h"
#include "diagnostics/Logger.h"
#include "ui/StartupWindow.h"
#include <QAction>
#include <QClipboard>
#include <QDialog>
#include <QPlainTextEdit>
#include <QPushButton>

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <optional>

using chess_review::app::AppPaths;
using chess_review::app::StartupCoordinator;
using chess_review::ui::StartupWindow;

class StartupTest final : public QObject {
    Q_OBJECT

  private slots:
    void init();
    void diagnosticMapping_data();
    void diagnosticMapping();
    void failureDetailsAndSafeCopy();
    void loggedLifecycle_data();
    void loggedLifecycle();
    void initializesAndCloses();
    void failureShowsDiagnostic();
    void closeDuringStartup();
    void quitDuringStartup();
    void resolvedPathsInitializeLazily();
    void unusableDataDirectoryFails();

  private:
    std::optional<QTemporaryDir> storageRoot_;
};

void StartupTest::diagnosticMapping_data() {
    using chess_review::persistence::DatabaseErrorCode;
    QTest::addColumn<int>("code");
    QTest::addColumn<QString>("expected");
    QTest::newRow("path") << static_cast<int>(DatabaseErrorCode::Path)
                          << QStringLiteral("storage.path");
    QTest::newRow("locked") << static_cast<int>(DatabaseErrorCode::Locked)
                            << QStringLiteral("storage.locked");
    QTest::newRow("open") << static_cast<int>(DatabaseErrorCode::Open)
                          << QStringLiteral("storage.open");
    QTest::newRow("configuration") << static_cast<int>(DatabaseErrorCode::Configuration)
                                   << QStringLiteral("storage.configuration");
    QTest::newRow("schema") << static_cast<int>(DatabaseErrorCode::Schema)
                            << QStringLiteral("storage.schema");
    QTest::newRow("backup") << static_cast<int>(DatabaseErrorCode::Backup)
                            << QStringLiteral("storage.backup");
    QTest::newRow("migration") << static_cast<int>(DatabaseErrorCode::Migration)
                               << QStringLiteral("storage.migration");
}

void StartupTest::diagnosticMapping() {
    QFETCH(int, code);
    QFETCH(QString, expected);
    chess_review::persistence::DatabaseError error;
    error.code = static_cast<chess_review::persistence::DatabaseErrorCode>(code);
    error.operation = QStringLiteral("private-operation");
    error.detail = QStringLiteral("private-native-error https://personal.example [Event PGN]");
    error.databasePath = QStringLiteral("/home/private/library.sqlite3");
    error.backupPath = QStringLiteral("/home/private/backup.sqlite3");
    error.nativeCode = QStringLiteral("private-native-code");
    error.migrationVersion = 2;
    const auto diagnostic = chess_review::app::storageDiagnostic(error);
    QCOMPARE(diagnostic.code, expected);
    QVERIFY(!diagnostic.summary.isEmpty());
    QVERIFY(!diagnostic.nextSteps.isEmpty());
    QVERIFY(diagnostic.sensitiveDetails.contains(error.databasePath));
    QVERIFY(diagnostic.sensitiveDetails.contains(error.backupPath));
    QVERIFY(diagnostic.sensitiveDetails.contains(error.nativeCode));
    QVERIFY(!diagnostic.safeReport().contains(QStringLiteral("private")));
    QVERIFY(!diagnostic.safeReport().contains(QStringLiteral("PGN")));
    QVERIFY(diagnostic.safeReport().contains(QStringLiteral("Migration: 2")));
}

void StartupTest::failureDetailsAndSafeCopy() {
    StartupWindow window;
    chess_review::persistence::DatabaseError error;
    error.code = chess_review::persistence::DatabaseErrorCode::Open;
    error.databasePath = QStringLiteral("/private/<b>棋</b>/library.sqlite3");
    error.detail = QStringLiteral("private native detail\nhttps://personal.example");
    window.showFailure(chess_review::app::storageDiagnostic(error));
    window.show();
    auto* reveal = window.findChild<QPushButton*>(QStringLiteral("revealDetails"));
    auto* copy = window.findChild<QPushButton*>(QStringLiteral("copyDiagnostics"));
    auto* detail = window.findChild<QLabel*>(QStringLiteral("startupDiagnostic"));
    QVERIFY(reveal && copy && detail);
    QVERIFY(!detail->isVisible());
    reveal->setFocus();
    QTest::keyPress(reveal, Qt::Key_Space);
    QTest::keyRelease(reveal, Qt::Key_Space);
    QVERIFY(detail->isVisible());
    QCOMPARE(detail->textFormat(), Qt::PlainText);
    QVERIFY(detail->text().contains(error.databasePath));
    copy->click();
    const auto report = QApplication::clipboard()->text();
    QVERIFY(report.contains(QStringLiteral("storage.open")));
    QVERIFY(!report.contains(QStringLiteral("private")));
    QVERIFY(!report.contains(QStringLiteral("personal.example")));
    auto* action = window.findChild<QAction*>(QStringLiteral("diagnosticsAction"));
    QVERIFY(action);
    action->trigger();
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("diagnosticsDialog"));
    QVERIFY(dialog);
    QVERIFY(!dialog->isModal());
    const auto* safe = dialog->findChild<QPlainTextEdit*>(QStringLiteral("safeDiagnosticsReport"));
    QVERIFY(safe && safe->isReadOnly());
    QVERIFY(!safe->toPlainText().contains(QStringLiteral("private")));
    QTest::keyPress(dialog, Qt::Key_Escape);
    window.close();
}

void StartupTest::loggedLifecycle_data() {
    QTest::addColumn<bool>("loggingAvailable");
    QTest::addColumn<bool>("closeEarly");
    QTest::addColumn<bool>("badDatabase");
    QTest::newRow("normal") << true << false << false;
    QTest::newRow("memory-only") << false << false << false;
    QTest::newRow("close-during-startup") << true << true << false;
    QTest::newRow("storage-failure") << true << false << true;
}

void StartupTest::loggedLifecycle() {
    QFETCH(bool, loggingAvailable);
    QFETCH(bool, closeEarly);
    QFETCH(bool, badDatabase);
    QTemporaryDir directory;
    chess_review::diagnostics::Logger logger(
        loggingAvailable ? directory.filePath(QStringLiteral("logs")) : QString());
    StartupWindow window;
    StartupCoordinator startup(window, logger);
    QSignalSpy finished(&startup, &StartupCoordinator::finished);
    // Always shut down and await the native writer join, even on assertion failure.
    const auto cleanup = qScopeGuard([&] {
        startup.shutdown();
        if (finished.isEmpty()) {
            QEventLoop loop;
            QObject::connect(&startup, &StartupCoordinator::finished, &loop, &QEventLoop::quit);
            loop.exec();
        }
    });
    const QString path = directory.filePath(QStringLiteral("private-library.sqlite3"));
    if (badDatabase) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("invalid private data") > 0);
    }
    window.show();
    startup.start(path);
    if (!closeEarly) {
        auto* status = window.findChild<QLabel*>(QStringLiteral("startupStatus"));
        QTRY_COMPARE(status->text(), badDatabase
                                         ? QStringLiteral("Could not initialize local storage")
                                         : QStringLiteral("Ready"));
        if (!loggingAvailable) {
            auto* notice = window.findChild<QLabel*>(QStringLiteral("loggingNotice"));
            QTRY_VERIFY(notice->isVisible());
        }
    }
    window.close();
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(logger.health(), chess_review::diagnostics::Health::Closed);
    const auto report = logger.report();
    QVERIFY(report.contains(QStringLiteral("storage.started")));
    QVERIFY(report.contains(QStringLiteral("storage.closed")));
    QVERIFY(report.indexOf(QStringLiteral("storage.closed")) <
            report.indexOf(QStringLiteral("session.stopped")));
    QVERIFY(!report.contains(QStringLiteral("private")));
    if (badDatabase) {
        QCOMPARE(report.count(QStringLiteral("storage.failed")), 1);
    }
}

void StartupTest::init() {
#ifdef Q_OS_LINUX
    // Each test owns fresh roots, including when selected repeatedly or reordered.
    storageRoot_.emplace();
    QVERIFY(storageRoot_->isValid());
    qputenv("XDG_DATA_HOME", QFile::encodeName(storageRoot_->filePath(QStringLiteral("data"))));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(storageRoot_->filePath(QStringLiteral("config"))));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(storageRoot_->filePath(QStringLiteral("cache"))));
#endif
}

void StartupTest::resolvedPathsInitializeLazily() {
#ifndef Q_OS_LINUX
    QSKIP("Linux XDG startup test");
#else
    const auto paths = AppPaths::resolve();
    QVERIFY(!QFileInfo::exists(paths.dataDirectory));
    StartupWindow window;
    StartupCoordinator startup(window);
    QSignalSpy finished(&startup, &StartupCoordinator::finished);
    const auto* status = window.findChild<QLabel*>(QStringLiteral("startupStatus"));
    QVERIFY(status);
    window.show();
    startup.start(paths.databasePath());
    QTRY_COMPARE(status->text(), QStringLiteral("Ready"));
    QVERIFY(QFileInfo::exists(paths.databasePath()));
    QVERIFY(!QFileInfo::exists(paths.configDirectory));
    QVERIFY(!QFileInfo::exists(paths.cacheDirectory));
    window.close();
    QTRY_COMPARE(finished.count(), 1);
#endif
}

void StartupTest::unusableDataDirectoryFails() {
#ifndef Q_OS_LINUX
    QSKIP("Linux XDG startup test");
#else
    const auto paths = AppPaths::resolve();
    QVERIFY(QDir().mkpath(QFileInfo(paths.dataDirectory).absolutePath()));
    QFile obstruction(paths.dataDirectory);
    QVERIFY(obstruction.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    obstruction.close();
    StartupWindow window;
    StartupCoordinator startup(window);
    QSignalSpy finished(&startup, &StartupCoordinator::finished);
    const auto* status = window.findChild<QLabel*>(QStringLiteral("startupStatus"));
    const auto* diagnostic = window.findChild<QLabel*>(QStringLiteral("startupDiagnostic"));
    QVERIFY(status);
    QVERIFY(diagnostic);
    window.show();
    startup.start(paths.databasePath());
    QTRY_COMPARE(status->text(), QStringLiteral("Could not initialize local storage"));
    QVERIFY(diagnostic->text().contains(paths.databasePath()));
    QVERIFY(!QFileInfo::exists(paths.databasePath()));
    QVERIFY(!QFileInfo::exists(paths.configDirectory));
    QVERIFY(!QFileInfo::exists(paths.cacheDirectory));
    QVERIFY(!QFileInfo::exists(QDir::homePath() + QStringLiteral("/.local/share/chess-review")));
    QVERIFY(!QFileInfo::exists(QStringLiteral("chess-review.sqlite3")));
    window.close();
    QTRY_COMPARE(finished.count(), 1);
#endif
}

void StartupTest::initializesAndCloses() {
    QTemporaryDir directory;
    StartupWindow window;
    StartupCoordinator startup(window);
    QSignalSpy finished(&startup, &StartupCoordinator::finished);
    const auto* status = window.findChild<QLabel*>(QStringLiteral("startupStatus"));
    QVERIFY(status);
    window.show();
    startup.start(directory.filePath(QStringLiteral("library.sqlite3")));
    QTRY_COMPARE(status->text(), QStringLiteral("Ready"));
    window.close();
    QTRY_COMPARE(finished.count(), 1);
}

void StartupTest::failureShowsDiagnostic() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("broken.sqlite3"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("invalid database") > 0);
    file.close();
    StartupWindow window;
    StartupCoordinator startup(window);
    QSignalSpy finished(&startup, &StartupCoordinator::finished);
    const auto* status = window.findChild<QLabel*>(QStringLiteral("startupStatus"));
    const auto* diagnostic = window.findChild<QLabel*>(QStringLiteral("startupDiagnostic"));
    QVERIFY(status);
    QVERIFY(diagnostic);
    window.show();
    startup.start(path);
    QTRY_COMPARE(status->text(), QStringLiteral("Could not initialize local storage"));
    QVERIFY(diagnostic->text().contains(path));
    QCOMPARE(diagnostic->textFormat(), Qt::PlainText);
    window.close();
    QTRY_COMPARE(finished.count(), 1);
}

void StartupTest::closeDuringStartup() {
    QTemporaryDir directory;
    StartupWindow window;
    StartupCoordinator startup(window);
    QSignalSpy finished(&startup, &StartupCoordinator::finished);
    window.show();
    startup.start(directory.filePath(QStringLiteral("library.sqlite3")));
    window.close();
    QTRY_COMPARE(finished.count(), 1);
}

void StartupTest::quitDuringStartup() {
    QTemporaryDir directory;
    StartupWindow window;
    StartupCoordinator startup(window);
    QSignalSpy finished(&startup, &StartupCoordinator::finished);
    window.show();
    startup.start(directory.filePath(QStringLiteral("library.sqlite3")));
    QEvent quit(QEvent::Quit);
    QCoreApplication::sendEvent(QCoreApplication::instance(), &quit);
    QTRY_COMPARE(finished.count(), 1);
    window.close();
}

int main(int argc, char* argv[]) {
    QTemporaryDir home;
    if (!home.isValid()) {
        return 1;
    }
    const QString previousDirectory = QDir::currentPath();
    if (!QDir::setCurrent(home.path())) {
        return 1;
    }
    const auto restoreDirectory = qScopeGuard([&previousDirectory] {
        if (!QDir::setCurrent(previousDirectory)) {
            qFatal("Cannot restore test working directory");
        }
    });
#ifdef Q_OS_LINUX
    qputenv("HOME", QFile::encodeName(home.path()));
    qputenv("XDG_DATA_HOME", QFile::encodeName(home.filePath(QStringLiteral("data"))));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(home.filePath(QStringLiteral("config"))));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(home.filePath(QStringLiteral("cache"))));
#endif
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("chess-review"));
    application.setQuitOnLastWindowClosed(false);
    StartupTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "StartupTest.moc"
