#include "app/AppPaths.h"
#include "app/StartupCoordinator.h"
#include "ui/StartupWindow.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
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
    void initializesAndCloses();
    void failureShowsDiagnostic();
    void closeDuringStartup();
    void quitDuringStartup();
    void resolvedPathsInitializeLazily();
    void unusableDataDirectoryFails();

  private:
    std::optional<QTemporaryDir> storageRoot_;
};

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
