#include "TestEnvironment.h"

#include "persistence/Database.h"
#include "persistence/private/Migrations.h"
#include "persistence/private/PrivateFile.h"
#include "persistence/private/StorageSession.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVariant>

#include <new>
#include <optional>
#include <span>
#include <vector>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace chess_review::persistence;
using namespace chess_review::persistence::detail;

namespace {

class FixtureConnection {
  public:
    explicit FixtureConnection(const QString& path) {
        name_ = QUuid::createUuid().toString();
        database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
        database_.setDatabaseName(path);
        if (!database_.open()) {
            qFatal("Cannot open test fixture: %s", qPrintable(database_.lastError().text()));
        }
    }

    ~FixtureConnection() {
        database_.close();
        database_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(name_);
    }

    bool exec(const QString& statement) {
        QSqlQuery query(database_);
        return query.exec(statement);
    }

    QVariant scalar(const QString& statement) {
        QSqlQuery query(database_);
        if (!query.exec(statement) || !query.next()) {
            return {};
        }
        return query.value(0);
    }

  private:
    QString name_;
    QSqlDatabase database_;
};

const Progress ignoreProgress = [](const QString&) {};

std::vector<Migration> fixtureMigrations() {
    return {
        initialMigration(),
        {2,
         QStringLiteral("fixture_data"),
         {QStringLiteral("CREATE TABLE fixture_data (id INTEGER PRIMARY KEY, value TEXT NOT NULL)"),
          QStringLiteral("INSERT INTO fixture_data VALUES (1, 'preserved')")}},
        {3,
         QStringLiteral("fixture_index"),
         {QStringLiteral("CREATE INDEX fixture_value ON fixture_data(value)")}}};
}

std::optional<DatabaseError> initialize(const QString& path, std::span<const Migration> registry) {
    StorageSession session;
    return session.initialize(path, registry, ignoreProgress);
}

DatabaseError signalError(const QSignalSpy& spy) {
    return qvariant_cast<DatabaseError>(spy.at(0).at(0));
}

} // namespace

class DatabaseTest final : public QObject {
    Q_OBJECT

  private slots:
    void init() { QTest::failOnWarning(); }
    void storageEventsAreTypedAndTruthful();
    void freshAndRepeatInitialization();
    void sequentialUpgradeAndWalBackup();
    void migrationFailureRollsBackAndRetainsBackup();
    void invalidHistory_data();
    void invalidHistory();
    void invalidRegistry();
    void rejectedDatabaseRemainsUnchanged();
    void forgedLedgerIsRejected();
    void privateFilesAndAliases();
    void specialFilesAreRejected();
    void unsafeStorageFiles_data();
    void unsafeStorageFiles();
    void boundValuesCannotInjectSql();
    void privateFileCannotOverwriteExistingData();
    void legacyPermissionsAreRestricted();
    void unrecognizedDatabaseIsPreserved();
    void initialMigrationFailureIsAtomic();
    void backupFailurePreventsUpgrade();
    void invalidPathsAndCorruption();
    void lockContention();
    void asynchronousLifecycle();
    void busyTimeoutKeepsGuiResponsive();
    void closeDuringInitialization();
    void closeBeforeInitialization();
    void foreignEncodingIsRejectedWithoutChangingData();
    void transformFailureRollsBack();
};

void DatabaseTest::storageEventsAreTypedAndTruthful() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("library.sqlite3"));
    auto registry = fixtureMigrations();
    QVERIFY(!initialize(path, std::span(registry).first(1)));
    std::vector<std::pair<StorageEvent, int>> events;
    const Observe observe = [&](StorageEvent event, int version) {
        events.emplace_back(event, version);
    };
    {
        StorageSession session;
        QVERIFY(!session.initialize(path, registry, ignoreProgress, observe));
    }
    const std::vector<std::pair<StorageEvent, int>> expected{
        {StorageEvent::SchemaChecked, 1},      {StorageEvent::BackupStarted, 1},
        {StorageEvent::BackupVerified, 1},     {StorageEvent::MigrationStarted, 2},
        {StorageEvent::MigrationCompleted, 2}, {StorageEvent::MigrationStarted, 3},
        {StorageEvent::MigrationCompleted, 3}};
    QVERIFY(events == expected);
    events.clear();
    registry.push_back(
        {4, QStringLiteral("broken"), {QStringLiteral("INSERT INTO missing_table VALUES (1)")}});
    {
        StorageSession session;
        const auto error = session.initialize(path, registry, ignoreProgress, observe);
        QVERIFY(error);
        QCOMPARE(error->code, DatabaseErrorCode::Migration);
    }
    const std::vector<std::pair<StorageEvent, int>> failureExpected{
        {StorageEvent::SchemaChecked, 3},
        {StorageEvent::BackupStarted, 3},
        {StorageEvent::BackupVerified, 3},
        {StorageEvent::MigrationStarted, 4}};
    QVERIFY(events == failureExpected);
    // A backup failure must never advertise a verified snapshot or a migration.
    const QString second = directory.filePath(QStringLiteral("second/library.sqlite3"));
    QVERIFY(!initialize(second, std::span(registry).first(1)));
    QFile obstruction(directory.filePath(QStringLiteral("second/backups")));
    QVERIFY(obstruction.open(QIODevice::WriteOnly));
    obstruction.close();
    events.clear();
    StorageSession session;
    const auto error = session.initialize(second, registry, ignoreProgress, observe);
    QVERIFY(error);
    QCOMPARE(error->code, DatabaseErrorCode::Backup);
    QVERIFY(error->backupPath.isEmpty());
    const std::vector<std::pair<StorageEvent, int>> backupExpected{
        {StorageEvent::SchemaChecked, 1}, {StorageEvent::BackupStarted, 1}};
    QVERIFY(events == backupExpected);
}

void DatabaseTest::freshAndRepeatInitialization() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("nested/library.sqlite3"));
    const auto connectionsBefore = QSqlDatabase::connectionNames();
    {
        StorageSession session;
        QVERIFY(!session.initialize(path, migrations(), ignoreProgress));
        QStringList names = QSqlDatabase::connectionNames();
        for (const auto& existing : connectionsBefore) {
            names.removeAll(existing);
        }
        QCOMPARE(names.size(), 1);
        {
            auto database = QSqlDatabase::database(names.front());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral("PRAGMA foreign_keys")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 1);
            query.finish();
            QVERIFY(query.exec(QStringLiteral("PRAGMA busy_timeout")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 5000);
            query.finish();
            QVERIFY(query.exec(QStringLiteral("PRAGMA trusted_schema")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 0);
            query.finish();
            QVERIFY(query.exec(QStringLiteral("PRAGMA synchronous")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 2);
        }
    }
    QCOMPARE(QSqlDatabase::connectionNames(), connectionsBefore);
    QVERIFY(!initialize(path, migrations()));
    FixtureConnection fixture(path);
    QCOMPARE(fixture.scalar(QStringLiteral("SELECT COUNT(*) FROM schema_migrations")).toInt(), 4);
    QCOMPARE(fixture.scalar(QStringLiteral("SELECT MAX(version) FROM schema_migrations")).toInt(),
             4);
    QCOMPARE(fixture.scalar(QStringLiteral("PRAGMA journal_mode")).toString(),
             QStringLiteral("wal"));
    QCOMPARE(fixture
                 .scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_schema "
                                        "WHERE type='table' AND name NOT GLOB 'sqlite_*'"))
                 .toInt(),
             3);
    QVERIFY(!QDir(directory.filePath(QStringLiteral("nested/backups"))).exists());
}

void DatabaseTest::sequentialUpgradeAndWalBackup() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("player's library.sqlite3"));
    const auto registry = fixtureMigrations();
    QVERIFY(!initialize(path, std::span(registry).first(2)));
    FixtureConnection writer(path);
    QVERIFY(writer.exec(QStringLiteral("PRAGMA wal_autocheckpoint = 0")));
    QVERIFY(writer.exec(QStringLiteral("INSERT INTO fixture_data VALUES (2, 'from WAL')")));
    QVERIFY(QFileInfo(path + QStringLiteral("-wal")).size() > 0);
    QVERIFY(!initialize(path, registry));
    QCOMPARE(writer.scalar(QStringLiteral("SELECT MAX(version) FROM schema_migrations")).toInt(),
             3);
    QCOMPARE(writer.scalar(QStringLiteral("SELECT COUNT(*) FROM fixture_data")).toInt(), 2);
    const QDir backups(directory.filePath(QStringLiteral("backups")));
    const auto files = backups.entryList({QStringLiteral("*.sqlite3")}, QDir::Files);
    QCOMPARE(files.size(), 1);
    FixtureConnection backup(backups.filePath(files.front()));
    QCOMPARE(backup.scalar(QStringLiteral("SELECT MAX(version) FROM schema_migrations")).toInt(),
             2);
    QCOMPARE(backup.scalar(QStringLiteral("SELECT value FROM fixture_data WHERE id=2")).toString(),
             QStringLiteral("from WAL"));
    QCOMPARE(backup.scalar(QStringLiteral("PRAGMA integrity_check")).toString(),
             QStringLiteral("ok"));
    QVERIFY(!initialize(path, registry));
    QCOMPARE(backups.entryList({QStringLiteral("*.sqlite3")}, QDir::Files).size(), 1);
}

void DatabaseTest::migrationFailureRollsBackAndRetainsBackup() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("library.sqlite3"));
    auto registry = fixtureMigrations();
    QVERIFY(!initialize(path, std::span(registry).first(2)));
    registry.push_back({4,
                        QStringLiteral("broken"),
                        {QStringLiteral("CREATE TABLE rolled_back (id INTEGER PRIMARY KEY)"),
                         QStringLiteral("INSERT INTO missing_table VALUES (1)")}});
    const auto error = initialize(path, registry);
    QVERIFY(error);
    QCOMPARE(error->code, DatabaseErrorCode::Migration);
    QCOMPARE(error->migrationVersion, std::optional<int>(4));
    QVERIFY(!error->nativeCode.isEmpty());
    QVERIFY(QFileInfo::exists(error->backupPath));
    FixtureConnection fixture(path);
    QCOMPARE(fixture.scalar(QStringLiteral("SELECT MAX(version) FROM schema_migrations")).toInt(),
             3);
    QCOMPARE(fixture.scalar(QStringLiteral("SELECT COUNT(*) FROM fixture_data")).toInt(), 1);
    QCOMPARE(
        fixture
            .scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_schema WHERE name='rolled_back'"))
            .toInt(),
        0);
    FixtureConnection backup(error->backupPath);
    QCOMPARE(backup.scalar(QStringLiteral("SELECT MAX(version) FROM schema_migrations")).toInt(),
             2);
    registry.pop_back();
    QVERIFY(!initialize(path, registry));
}

void DatabaseTest::invalidHistory_data() {
    QTest::addColumn<QString>("mutation");
    QTest::newRow("newer") << QStringLiteral(
        "INSERT INTO schema_migrations VALUES (2, 'games', '2026-10-19T00:00:00.000Z'),(3, "
        "'game_text', '2026-10-19T00:00:00.000Z'),(4, 'future', '2026-10-19T00:00:00.000Z')");
    QTest::newRow("gap") << QStringLiteral("UPDATE schema_migrations SET version=3");
    QTest::newRow("integer-overflow")
        << QStringLiteral("UPDATE schema_migrations SET version=4294967297");
    QTest::newRow("negative-overflow")
        << QStringLiteral("UPDATE schema_migrations SET version=9223372032559808513");
    QTest::newRow("oversized-name")
        << QStringLiteral("UPDATE schema_migrations SET name=zeroblob(1048576)");
    QTest::newRow("embedded-null")
        << QStringLiteral("UPDATE schema_migrations SET name='initial'||char(0)||'hidden'");
    QTest::newRow("ledger-trigger")
        << QStringLiteral("CREATE TRIGGER skip_history BEFORE INSERT ON schema_migrations BEGIN "
                          "SELECT RAISE(IGNORE); END");
    QTest::newRow("ledger-index") << QStringLiteral(
        "CREATE INDEX extra_ledger_index ON schema_migrations(name)");
    QTest::newRow("extra-column") << QStringLiteral(
        "ALTER TABLE schema_migrations ADD COLUMN extra TEXT");
    QTest::newRow("renamed") << QStringLiteral("UPDATE schema_migrations SET name='modified'");
    QTest::newRow("timestamp") << QStringLiteral(
        "UPDATE schema_migrations SET applied_at='invalid'");
    QTest::newRow("empty") << QStringLiteral("DELETE FROM schema_migrations");
    QTest::newRow("unrecognized") << QStringLiteral(
        "ALTER TABLE schema_migrations RENAME TO other_data");
    QTest::newRow("malformed") << QStringLiteral(
        "ALTER TABLE schema_migrations RENAME COLUMN version TO wrong_column");
}

void DatabaseTest::invalidHistory() {
    QFETCH(QString, mutation);
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("library.sqlite3"));
    QVERIFY(!initialize(path, std::span(migrations()).first(1)));
    {
        FixtureConnection fixture(path);
        QVERIFY(fixture.exec(mutation));
    }
    const auto error = initialize(path, migrations());
    QVERIFY(error);
    QCOMPARE(error->code, DatabaseErrorCode::Schema);
    QVERIFY(error->backupPath.isEmpty());
    QVERIFY(QFileInfo::exists(path));
}

void DatabaseTest::invalidRegistry() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("library.sqlite3"));
    auto registry = fixtureMigrations();
    registry[1].version = 3;
    const auto error = initialize(path, registry);
    QVERIFY(error);
    QCOMPARE(error->code, DatabaseErrorCode::Schema);
    QVERIFY(!initialize(path, migrations()));
}

void DatabaseTest::rejectedDatabaseRemainsUnchanged() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("other.sqlite3"));
    {
        FixtureConnection fixture(path);
        QVERIFY(fixture.exec(QStringLiteral("CREATE TABLE other_data(value TEXT)")));
        QCOMPARE(fixture.scalar(QStringLiteral("PRAGMA journal_mode")).toString(),
                 QStringLiteral("delete"));
    }
    QFile original(path);
    QVERIFY(original.open(QIODevice::ReadOnly));
    const auto bytes = original.readAll();
    original.close();
    QVERIFY(initialize(path, migrations()));
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), bytes);
}

void DatabaseTest::forgedLedgerIsRejected() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("forged.sqlite3"));
    {
        FixtureConnection fixture(path);
        QVERIFY(fixture.exec(QStringLiteral(
            "CREATE TABLE schema_migrations(version INTEGER, name TEXT, applied_at TEXT)")));
        QVERIFY(fixture.exec(QStringLiteral(
            "INSERT INTO schema_migrations VALUES(1, 'initial', '2026-10-19T00:00:00.000Z')")));
    }
    const auto error = initialize(path, migrations());
    QVERIFY(error);
    QCOMPARE(error->code, DatabaseErrorCode::Schema);
}

void DatabaseTest::privateFilesAndAliases() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("library.sqlite3"));
    const auto registry = fixtureMigrations();
    QVERIFY(!initialize(path, std::span(registry).first(1)));
    QVERIFY(!initialize(path, registry));
#ifdef Q_OS_UNIX
    const auto publicPermissions = QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                   QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                   QFileDevice::WriteOther | QFileDevice::ExeOther;
    QCOMPARE(QFileInfo(path).permissions() & publicPermissions, QFileDevice::Permissions{});
    const QDir backups(directory.filePath(QStringLiteral("backups")));
    for (const auto& file : backups.entryList(QDir::Files)) {
        QCOMPARE(QFileInfo(backups.filePath(file)).permissions() & publicPermissions,
                 QFileDevice::Permissions{});
    }
    const QString hardLink = directory.filePath(QStringLiteral("hard.sqlite3"));
    QCOMPARE(::link(QFile::encodeName(path).constData(), QFile::encodeName(hardLink).constData()),
             0);
    const auto linkedError = initialize(hardLink, registry);
    QVERIFY(linkedError);
    QCOMPARE(linkedError->code, DatabaseErrorCode::Path);
    QVERIFY(QFile::remove(hardLink));
#endif
    const QString alias = directory.filePath(QStringLiteral("alias.sqlite3"));
    if (!QFile::link(path, alias)) {
        QSKIP("Cannot create symbolic links on this platform.");
    }
    StorageSession owner;
    QVERIFY(!owner.initialize(path, registry, ignoreProgress));
    const auto error = initialize(alias, registry);
    QVERIFY(error);
    QCOMPARE(error->code, DatabaseErrorCode::Locked);
}

void DatabaseTest::specialFilesAreRejected() {
#ifdef Q_OS_UNIX
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString pipe = directory.filePath(QStringLiteral("pipe.sqlite3"));
    QCOMPARE(::mkfifo(QFile::encodeName(pipe).constData(), 0600), 0);
    const auto error = initialize(pipe, migrations());
    QVERIFY(error);
    QCOMPARE(error->code, DatabaseErrorCode::Path);
#else
    QSKIP("Named pipe fixture is Unix-specific.");
#endif
}

void DatabaseTest::unsafeStorageFiles_data() {
    QTest::addColumn<QString>("suffix");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("existingDatabase");
    for (const auto& suffix :
         {QStringLiteral("-wal"), QStringLiteral("-shm"), QStringLiteral("-journal"),
          QStringLiteral(".lock"), QStringLiteral(".lock.rmlock")}) {
        for (const auto& kind :
             {QStringLiteral("symlink"), QStringLiteral("dangling"), QStringLiteral("hardlink"),
              QStringLiteral("fifo"), QStringLiteral("directory")}) {
            for (const bool existing : {false, true}) {
                const auto name =
                    suffix + QLatin1Char('-') + kind +
                    (existing ? QStringLiteral("-existing") : QStringLiteral("-fresh"));
                QTest::newRow(qPrintable(name)) << suffix << kind << existing;
            }
        }
    }
    QTest::newRow("oversized-lock")
        << QStringLiteral(".lock") << QStringLiteral("oversized") << false;
    QTest::newRow("cleanup-metadata")
        << QStringLiteral(".lock.rmlock") << QStringLiteral("regular") << false;
}

void DatabaseTest::unsafeStorageFiles() {
#ifdef Q_OS_UNIX
    QFETCH(QString, suffix);
    QFETCH(QString, kind);
    QFETCH(bool, existingDatabase);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
    QByteArray original;
    if (existingDatabase) {
        QVERIFY(!initialize(path, migrations()));
        QFile database(path);
        QVERIFY(database.open(QIODevice::ReadOnly));
        original = database.readAll();
    }
    const auto sentinel = directory.filePath(QStringLiteral("sentinel"));
    const QByteArray contents("preserve unrelated data and permissions");
    {
        QFile file(sentinel);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(contents), contents.size());
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ReadGroup));
    }
    const auto permissions = QFileInfo(sentinel).permissions();
    const auto unsafe = path + suffix;
    if (kind == QStringLiteral("symlink") || kind == QStringLiteral("dangling")) {
        QVERIFY(QFile::link(kind == QStringLiteral("symlink")
                                ? sentinel
                                : directory.filePath(QStringLiteral("missing")),
                            unsafe));
    } else if (kind == QStringLiteral("hardlink")) {
        QCOMPARE(
            ::link(QFile::encodeName(sentinel).constData(), QFile::encodeName(unsafe).constData()),
            0);
    } else if (kind == QStringLiteral("fifo")) {
        QCOMPARE(::mkfifo(QFile::encodeName(unsafe).constData(), 0600), 0);
    } else if (kind == QStringLiteral("directory")) {
        QVERIFY(QDir().mkdir(unsafe));
    } else {
        QFile file(unsafe);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray metadata(kind == QStringLiteral("oversized") ? 16385 : 1, 'x');
        QCOMPARE(file.write(metadata), metadata.size());
    }
    QElapsedTimer elapsed;
    elapsed.start();
    const auto error = initialize(path, migrations());
    QVERIFY(error);
    QCOMPARE(error->code, suffix.startsWith(QLatin1Char('.')) ? DatabaseErrorCode::Locked
                                                              : DatabaseErrorCode::Path);
    QVERIFY(elapsed.elapsed() < 1000);
    QVERIFY(QFileInfo(unsafe).exists() || QFileInfo(unsafe).isSymLink());
    QFile file(sentinel);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), contents);
    QCOMPARE(QFileInfo(sentinel).permissions(), permissions);
    if (existingDatabase) {
        QFile database(path);
        QVERIFY(database.open(QIODevice::ReadOnly));
        QCOMPARE(database.readAll(), original);
    } else {
        QVERIFY(!QFileInfo::exists(path));
    }
#else
    QSKIP("Unix filesystem alias and special-file fixtures.");
#endif
}

void DatabaseTest::boundValuesCannotInjectSql() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    // Quotes and SQL punctuation are ordinary filename/name data, never executable SQL.
    const QString path =
        directory.filePath(QStringLiteral("library'; DROP TABLE schema_migrations;--.sqlite3"));
    auto registry = fixtureMigrations();
    registry[1].name = QStringLiteral("second'); DROP TABLE schema_migrations;--");
    QVERIFY(!initialize(path, std::span(registry).first(1)));
    QVERIFY(!initialize(path, registry));
    QVERIFY(!initialize(path, registry));
    FixtureConnection fixture(path);
    QCOMPARE(fixture.scalar(QStringLiteral("SELECT COUNT(*) FROM schema_migrations")).toInt(), 3);
    QCOMPARE(fixture.scalar(QStringLiteral("SELECT COUNT(*) FROM fixture_data")).toInt(), 1);
}

void DatabaseTest::privateFileCannotOverwriteExistingData() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("existing"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("preserved"), 9);
    file.close();
    QVERIFY(createPrivateFile(path));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("preserved"));
}

void DatabaseTest::legacyPermissionsAreRestricted() {
#ifdef Q_OS_UNIX
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("legacy.sqlite3"));
    QVERIFY(!initialize(path, migrations()));
    FixtureConnection writer(path);
    QVERIFY(writer.exec(QStringLiteral("PRAGMA wal_autocheckpoint=0")));
    QVERIFY(
        writer.exec(QStringLiteral("UPDATE schema_migrations SET name='initial' WHERE version=1")));
    for (const auto& file : {path, path + QStringLiteral("-wal"), path + QStringLiteral("-shm")}) {
        QVERIFY(QFile::setPermissions(file, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                QFileDevice::ReadGroup | QFileDevice::ReadOther));
    }
    QVERIFY(!initialize(path, migrations()));
    for (const auto& file : {path, path + QStringLiteral("-wal"), path + QStringLiteral("-shm")}) {
        QCOMPARE(QFileInfo(file).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
                 QFileDevice::Permissions{});
    }
#else
    QSKIP("Unix permission regression fixture.");
#endif
}

void DatabaseTest::unrecognizedDatabaseIsPreserved() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("other.sqlite3"));
    {
        FixtureConnection fixture(path);
        // The underscore in SQLite's reserved prefix must be matched literally.
        QVERIFY(fixture.exec(QStringLiteral("CREATE TABLE sqliteXuser_data (value TEXT)")));
        QVERIFY(fixture.exec(QStringLiteral("INSERT INTO sqliteXuser_data VALUES ('preserved')")));
    }
    const auto error = initialize(path, migrations());
    QVERIFY(error);
    QCOMPARE(error->code, DatabaseErrorCode::Schema);
    FixtureConnection fixture(path);
    QCOMPARE(fixture.scalar(QStringLiteral("SELECT value FROM sqliteXuser_data")).toString(),
             QStringLiteral("preserved"));
    QCOMPARE(fixture
                 .scalar(QStringLiteral(
                     "SELECT COUNT(*) FROM sqlite_schema WHERE name='schema_migrations'"))
                 .toInt(),
             0);
}

void DatabaseTest::initialMigrationFailureIsAtomic() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("library.sqlite3"));
    auto migration = initialMigration();
    migration.statements.push_back(QStringLiteral("INSERT INTO missing_table VALUES (1)"));
    const std::vector<Migration> registry{migration};
    const auto error = initialize(path, registry);
    QVERIFY(error);
    QCOMPARE(error->migrationVersion, std::optional<int>(1));
    QVERIFY(error->backupPath.isEmpty());
    {
        FixtureConnection fixture(path);
        QCOMPARE(fixture
                     .scalar(QStringLiteral(
                         "SELECT COUNT(*) FROM sqlite_schema WHERE name='schema_migrations'"))
                     .toInt(),
                 0);
    }
    QVERIFY(!initialize(path, migrations()));
}

void DatabaseTest::backupFailurePreventsUpgrade() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("library.sqlite3"));
    QVERIFY(!initialize(path, std::span(migrations()).first(1)));
    QFile obstruction(directory.filePath(QStringLiteral("backups")));
    QVERIFY(obstruction.open(QIODevice::WriteOnly));
    obstruction.close();
    const auto error = initialize(path, fixtureMigrations());
    QVERIFY(error);
    QCOMPARE(error->code, DatabaseErrorCode::Backup);
    FixtureConnection fixture(path);
    QCOMPARE(fixture.scalar(QStringLiteral("SELECT MAX(version) FROM schema_migrations")).toInt(),
             1);
    QCOMPARE(
        fixture
            .scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_schema WHERE name='fixture_data'"))
            .toInt(),
        0);
}

void DatabaseTest::invalidPathsAndCorruption() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const auto empty = initialize({}, migrations());
    QVERIFY(empty);
    QCOMPARE(empty->code, DatabaseErrorCode::Path);
    const auto relative = initialize(QStringLiteral("relative.sqlite3"), migrations());
    QVERIFY(relative);
    QCOMPARE(relative->code, DatabaseErrorCode::Path);
    const QString path = directory.filePath(QStringLiteral("broken.sqlite3"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("not a SQLite database"), 21);
    file.close();
    const auto blocked = initialize(path + QStringLiteral("/child.sqlite3"), migrations());
    QVERIFY(blocked);
    QCOMPARE(blocked->code, DatabaseErrorCode::Path);
    const auto corrupt = initialize(path, migrations());
    QVERIFY(corrupt);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("not a SQLite database"));
    file.close();
    QVERIFY(initialize(directory.path(), migrations()));
}

void DatabaseTest::lockContention() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("library.sqlite3"));
    StorageSession owner;
    QVERIFY(!owner.initialize(path, migrations(), ignoreProgress));
    const auto error = initialize(path, migrations());
    QVERIFY(error);
    QCOMPARE(error->code, DatabaseErrorCode::Locked);
}

void DatabaseTest::asynchronousLifecycle() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const auto before = QSqlDatabase::connectionNames();
    for (int iteration = 0; iteration < 3; ++iteration) {
        Database database;
        QSignalSpy ready(&database, &Database::ready);
        QSignalSpy failed(&database, &Database::failed);
        QSignalSpy closed(&database, &Database::closed);
        QThread* callbackThread = nullptr;
        connect(&database, &Database::ready, this,
                [&] { callbackThread = QThread::currentThread(); });
        database.initialize(directory.filePath(QStringLiteral("library.sqlite3")));
        QTRY_COMPARE(ready.count() + failed.count(), 1);
        // Always close before assertions that might return from this test.
        database.close();
        QTRY_COMPARE(closed.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(ready.front().front().toInt(), 4);
        QCOMPARE(callbackThread, QThread::currentThread());
        QCOMPARE(QSqlDatabase::connectionNames(), before);
        database.close();
        QCOMPARE(closed.count(), 1);
    }
}

void DatabaseTest::busyTimeoutKeepsGuiResponsive() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("library.sqlite3"));
    FixtureConnection writer(path);
    QVERIFY(writer.exec(QStringLiteral("PRAGMA journal_mode=WAL")));
    QVERIFY(writer.exec(QStringLiteral("BEGIN IMMEDIATE")));
    Database database;
    QSignalSpy ready(&database, &Database::ready);
    QSignalSpy failed(&database, &Database::failed);
    QSignalSpy closed(&database, &Database::closed);
    int ticks = 0;
    QTimer heartbeat;
    heartbeat.setInterval(20);
    connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
    heartbeat.start();
    QElapsedTimer elapsed;
    elapsed.start();
    database.initialize(path);
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 8000);
    database.close();
    QTRY_COMPARE(closed.count(), 1);
    QCOMPARE(ready.count(), 0);
    const auto error = signalError(failed);
    QCOMPARE(error.code, DatabaseErrorCode::Migration);
    QCOMPARE(error.migrationVersion, std::optional<int>(1));
    QVERIFY(elapsed.elapsed() >= 4500);
    QVERIFY(elapsed.elapsed() < 8000);
    QVERIFY(ticks > 20);
    QVERIFY(writer.exec(QStringLiteral("ROLLBACK")));
    QCOMPARE(writer
                 .scalar(QStringLiteral(
                     "SELECT COUNT(*) FROM sqlite_schema WHERE name='schema_migrations'"))
                 .toInt(),
             0);
}

void DatabaseTest::closeDuringInitialization() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    Database database;
    QSignalSpy ready(&database, &Database::ready);
    QSignalSpy failed(&database, &Database::failed);
    QSignalSpy closed(&database, &Database::closed);
    const auto before = QSqlDatabase::connectionNames();
    database.initialize(directory.filePath(QStringLiteral("library.sqlite3")));
    database.close();
    QTRY_COMPARE(closed.count(), 1);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(QSqlDatabase::connectionNames(), before);
    QVERIFY(!initialize(directory.filePath(QStringLiteral("library.sqlite3")), migrations()));
}

void DatabaseTest::closeBeforeInitialization() {
    Database database;
    QSignalSpy closed(&database, &Database::closed);
    database.close();
    database.close();
    QTRY_COMPARE(closed.count(), 1);
}

void DatabaseTest::foreignEncodingIsRejectedWithoutChangingData() {
    for (const auto* encoding : {"UTF-16le", "UTF-16be"}) {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
        {
            FixtureConnection fixture(path);
            QVERIFY(fixture.exec(QStringLiteral("PRAGMA encoding='") + QLatin1StringView(encoding) +
                                 QStringLiteral("'")));
            QVERIFY(fixture.exec(initialMigration().statements.front()));
            QVERIFY(fixture.exec(QStringLiteral(
                "INSERT INTO schema_migrations VALUES(1,'initial','2026-10-21T00:00:00.000Z')")));
        }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto before = file.readAll();
        file.close();
        const auto error = initialize(path, migrations());
        QVERIFY(error);
        QCOMPARE(error->code, DatabaseErrorCode::Configuration);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), before);
    }
}

void DatabaseTest::transformFailureRollsBack() {
    for (bool allocationFailure : {false, true}) {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
        auto registry = fixtureMigrations();
        registry.resize(2);
        QVERIFY(!initialize(path, registry));
        registry.push_back(
            {3,
             QStringLiteral("failing_transform"),
             {QStringLiteral("CREATE TABLE transform_marker(value TEXT)")},
             [allocationFailure](QSqlDatabase& database) -> std::optional<QString> {
                 QSqlQuery query(database);
                 if (!query.exec(QStringLiteral("UPDATE fixture_data SET value='modified'"))) {
                     return QStringLiteral("Fixture update failed.");
                 }
                 if (allocationFailure) {
                     throw std::bad_alloc();
                 }
                 return QStringLiteral("Fixture transformation failed.");
             }});
        const auto error = initialize(path, registry);
        QVERIFY(error);
        QCOMPARE(error->code, DatabaseErrorCode::Migration);
        QCOMPARE(error->migrationVersion, std::optional<int>(3));
        QVERIFY(!error->backupPath.isEmpty());
        FixtureConnection fixture(path);
        QCOMPARE(
            fixture.scalar(QStringLiteral("SELECT MAX(version) FROM schema_migrations")).toInt(),
            2);
        QCOMPARE(fixture.scalar(QStringLiteral("SELECT value FROM fixture_data")).toString(),
                 QStringLiteral("preserved"));
        QCOMPARE(fixture
                     .scalar(QStringLiteral(
                         "SELECT COUNT(*) FROM sqlite_schema WHERE name='transform_marker'"))
                     .toInt(),
                 0);
    }
}

int main(int argc, char* argv[]) {
    const TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QCoreApplication application(argc, argv);
    application.setAttribute(Qt::AA_Use96Dpi, true);
    DatabaseTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "DatabaseTest.moc"
