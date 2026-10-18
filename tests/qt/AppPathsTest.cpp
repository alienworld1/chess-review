#include "app/AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using chess_review::app::AppPaths;

class AppPathsTest final : public QObject {
    Q_OBJECT

  private slots:
    void standardLocations_data();
    void standardLocations();
    void emptyDirectoryDoesNotBecomeRelative();
};

void AppPathsTest::standardLocations_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("unset") << QStringLiteral("unset");
    QTest::newRow("empty") << QStringLiteral("empty");
    QTest::newRow("overrides-with-spaces-and-unicode") << QStringLiteral("override");
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
        qputenv("XDG_DATA_HOME", overridden ? QFile::encodeName(dataRoot) : QByteArray{});
        qputenv("XDG_CONFIG_HOME", overridden ? QFile::encodeName(configRoot) : QByteArray{});
        qputenv("XDG_CACHE_HOME", overridden ? QFile::encodeName(cacheRoot) : QByteArray{});
    }

    const auto paths = AppPaths::resolve();
    QCOMPARE(paths.dataDirectory, dataRoot + QStringLiteral("/chess-review"));
    QCOMPARE(paths.configDirectory, configRoot + QStringLiteral("/chess-review"));
    QCOMPARE(paths.cacheDirectory, cacheRoot + QStringLiteral("/chess-review"));
    QCOMPARE(paths.databasePath(), paths.dataDirectory + QStringLiteral("/chess-review.sqlite3"));
    QCOMPARE(paths.configFilePath(), paths.configDirectory + QStringLiteral("/config.toml"));
    QVERIFY(QDir(home).isEmpty());
#endif
}

void AppPathsTest::emptyDirectoryDoesNotBecomeRelative() {
    const AppPaths paths;
    QVERIFY(paths.databasePath().isEmpty());
    QVERIFY(paths.configFilePath().isEmpty());
}

int main(int argc, char* argv[]) {
    // Isolate HOME before Qt can cache it; never inspect or write the user's paths.
    QTemporaryDir home;
    if (!home.isValid()) {
        return 1;
    }
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
