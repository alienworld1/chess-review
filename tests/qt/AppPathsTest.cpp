#include "app/AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

using chess_review::app::AppPaths;

class AppPathsTest final : public QObject {
    Q_OBJECT

  private slots:
    void standardLocations_data();
    void standardLocations();
    void emptyDirectoryDoesNotBecomeRelative();
    void invalidDirectories_data();
    void invalidDirectories();
};

void AppPathsTest::standardLocations_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("unset") << QStringLiteral("unset");
    QTest::newRow("empty") << QStringLiteral("empty");
    QTest::newRow("overrides-with-spaces-and-unicode") << QStringLiteral("override");
    QTest::newRow("relative-overrides") << QStringLiteral("relative");
}

void AppPathsTest::standardLocations() {
#ifndef Q_OS_LINUX
    QSKIP("Linux XDG layout test");
#else
    QFETCH(QString, mode);
    const QString home = QDir::homePath();
    const bool overridden = mode == QStringLiteral("override");
    const QString dataRoot =
        home + (overridden ? QStringLiteral("/data space/棋") : QStringLiteral("/.local/share"));
    const QString configRoot =
        home + (overridden ? QStringLiteral("/config space/棋") : QStringLiteral("/.config"));
    const QString cacheRoot =
        home + (overridden ? QStringLiteral("/cache space/棋") : QStringLiteral("/.cache"));
    if (mode == QStringLiteral("unset")) {
        qunsetenv("XDG_DATA_HOME");
        qunsetenv("XDG_CONFIG_HOME");
        qunsetenv("XDG_CACHE_HOME");
    } else {
        const QByteArray fallback =
            mode == QStringLiteral("relative") ? QByteArray("relative") : QByteArray{};
        qputenv("XDG_DATA_HOME", overridden ? QFile::encodeName(dataRoot) : fallback);
        qputenv("XDG_CONFIG_HOME", overridden ? QFile::encodeName(configRoot) : fallback);
        qputenv("XDG_CACHE_HOME", overridden ? QFile::encodeName(cacheRoot) : fallback);
    }

    const auto paths = AppPaths::resolve();
    QCOMPARE(paths.dataDirectory, dataRoot + QStringLiteral("/chess-review"));
    QCOMPARE(paths.configDirectory, configRoot + QStringLiteral("/chess-review"));
    QCOMPARE(paths.cacheDirectory, cacheRoot + QStringLiteral("/chess-review"));
    QCOMPARE(paths.databasePath(), paths.dataDirectory + QStringLiteral("/chess-review.sqlite3"));
    QCOMPARE(paths.configFilePath(), paths.configDirectory + QStringLiteral("/config.toml"));
    QVERIFY(!QFileInfo::exists(dataRoot));
    QVERIFY(!QFileInfo::exists(configRoot));
    QVERIFY(!QFileInfo::exists(cacheRoot));
    QVERIFY(QDir(home)
                .entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)
                .isEmpty());
#endif
}

void AppPathsTest::emptyDirectoryDoesNotBecomeRelative() {
    const AppPaths paths;
    QVERIFY(paths.databasePath().isEmpty());
    QVERIFY(paths.configFilePath().isEmpty());
}

void AppPathsTest::invalidDirectories_data() {
    QTest::addColumn<QString>("directory");
    QTest::newRow("relative") << QStringLiteral("relative");
    QTest::newRow("current-directory") << QStringLiteral(".");
    QTest::newRow("embedded-null") << (QDir::tempPath() + QChar::Null + QStringLiteral("/hidden"));
}

void AppPathsTest::invalidDirectories() {
    QFETCH(QString, directory);
    const AppPaths paths{directory, directory, directory};
    QVERIFY(paths.databasePath().isEmpty());
    QVERIFY(paths.configFilePath().isEmpty());
}

int main(int argc, char* argv[]) {
    // Isolate HOME before Qt can cache it; never inspect or write the user's paths.
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
#endif
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("chess-review"));
    application.setOrganizationName(QString{});
    AppPathsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "AppPathsTest.moc"
