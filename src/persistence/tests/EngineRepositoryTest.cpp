#include "TestEnvironment.h"
#include "persistence/private/StorageSession.h"
#include <QCryptographicHash>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>
using namespace chess_review;
using namespace chess_review::persistence;
using namespace chess_review::persistence::detail;
namespace {
engine::ValidatedEngine candidate(const QString& path,
                                  const QByteArray& content = QByteArray("binary")) {
    engine::ValidatedEngine value;
    value.executablePath = path;
    value.executableSha256 = QCryptographicHash::hash(content, QCryptographicHash::Sha256);
    value.reportedName = QStringLiteral("\uFEFFName <b> '; DROP TABLE engines;--");
    value.reportedAuthor = QStringLiteral("Author");
    value.lastValidatedAt = QDateTime::currentDateTimeUtc();
    return value;
}
class Fixture final {
  public:
    explicit Fixture(const QString& path) : name_(QUuid::createUuid().toString()) {
        database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
        database_.setDatabaseName(path);
        if (!database_.open()) {
            qFatal("Cannot open fixture");
        }
    }
    ~Fixture() {
        database_.close();
        database_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(name_);
    }
    bool exec(const QString& sql) {
        QSqlQuery query(database_);
        return query.exec(sql);
    }
    QVariant scalar(const QString& sql) {
        QSqlQuery query(database_);
        if (!query.exec(sql) || !query.next()) {
            return {};
        }
        return query.value(0);
    }

  private:
    QString name_;
    QSqlDatabase database_;
};
const Progress ignore = [](const QString&) {};
} // namespace
class EngineRepositoryTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { QTest::failOnWarning(); }
    void durableIdentityAndDefaults() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
        engine::EngineProfile first;
        {
            StorageSession session;
            QVERIFY(!session.initialize(path, migrations(), ignore));
            const auto value = candidate(QStringLiteral("/tmp/棋 engine ';$()"));
            const auto result = session.registerEngine(value);
            QVERIFY(std::holds_alternative<RegisteredEngine>(result));
            first = std::get<RegisteredEngine>(result).profile;
            QVERIFY(first.isDefault);
            QCOMPARE(first.displayName, value.reportedName);
            auto duplicateValue = value;
            duplicateValue.executablePath = QStringLiteral("/another/path");
            duplicateValue.lastValidatedAt = value.lastValidatedAt.addDays(1);
            const auto duplicate = session.registerEngine(duplicateValue);
            QVERIFY(std::holds_alternative<RegisteredEngine>(duplicate));
            QVERIFY(std::get<RegisteredEngine>(duplicate).duplicate);
            QCOMPARE(std::get<RegisteredEngine>(duplicate).profile.engine.executablePath,
                     value.executablePath);
            QCOMPARE(std::get<RegisteredEngine>(duplicate).profile.engine.lastValidatedAt,
                     value.lastValidatedAt);
            const auto replacement =
                session.registerEngine(candidate(value.executablePath, QByteArray("changed")));
            QVERIFY(std::holds_alternative<engine::Error>(replacement));
            QCOMPARE(std::get<engine::Error>(replacement).code, engine::ErrorCode::Replacement);
            const auto second = session.registerEngine(
                candidate(QStringLiteral("/different/path"), QByteArray("second")));
            QVERIFY(std::holds_alternative<RegisteredEngine>(second));
            QVERIFY(!std::get<RegisteredEngine>(second).profile.isDefault);
        }
        StorageSession reopened;
        QVERIFY(!reopened.initialize(path, migrations(), ignore));
        const auto listed = reopened.listEngines(0, 50);
        QVERIFY(std::holds_alternative<std::vector<engine::EngineProfile>>(listed));
        const auto profiles = std::get<std::vector<engine::EngineProfile>>(listed);
        QCOMPARE(profiles.size(), std::size_t(2));
        QCOMPARE(profiles[0].id, first.id);
        QCOMPARE(profiles[0].engine.reportedName, first.engine.reportedName);
        QCOMPARE(profiles[0].createdAt, first.createdAt);
        QVERIFY(std::holds_alternative<engine::Error>(reopened.listEngines(-1, 50)));
        QVERIFY(std::holds_alternative<engine::Error>(reopened.listEngines(0, 201)));
        QCOMPARE(std::get<std::vector<engine::EngineProfile>>(reopened.listEngines(1, 1)).size(),
                 std::size_t(1));
    }
    void rollbackAndDamage() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
        StorageSession session;
        QVERIFY(!session.initialize(path, migrations(), ignore));
        QVERIFY(std::holds_alternative<RegisteredEngine>(
            session.registerEngine(candidate(QStringLiteral("/first")))));
        Fixture fixture(path);
        QVERIFY(fixture.exec(QStringLiteral("CREATE TRIGGER reject_engine BEFORE INSERT ON engines "
                                            "BEGIN SELECT RAISE(ABORT,'fixture'); END")));
        QVERIFY(std::holds_alternative<engine::Error>(
            session.registerEngine(candidate(QStringLiteral("/second"), QByteArray("second")))));
        QCOMPARE(fixture.scalar(QStringLiteral("SELECT count(*) FROM engines")).toInt(), 1);
        QVERIFY(fixture.exec(QStringLiteral("DROP TRIGGER reject_engine")));
        // A deferred foreign-key violation fails COMMIT, not the insert itself.
        QVERIFY(
            fixture.exec(QStringLiteral("CREATE TABLE engine_commit_guard(id INTEGER REFERENCES "
                                        "engines(id) DEFERRABLE INITIALLY DEFERRED)")));
        QVERIFY(
            fixture.exec(QStringLiteral("CREATE TRIGGER fail_engine_commit AFTER INSERT ON engines "
                                        "BEGIN INSERT INTO engine_commit_guard VALUES(-1); END")));
        QVERIFY(std::holds_alternative<engine::Error>(
            session.registerEngine(candidate(QStringLiteral("/second"), QByteArray("second")))));
        QCOMPARE(fixture.scalar(QStringLiteral("SELECT count(*) FROM engines")).toInt(), 1);
        QVERIFY(fixture.exec(QStringLiteral("DROP TRIGGER fail_engine_commit")));
        QVERIFY(std::holds_alternative<RegisteredEngine>(
            session.registerEngine(candidate(QStringLiteral("/second"), QByteArray("second")))));
        QVERIFY(fixture.exec(
            QStringLiteral("UPDATE engines SET capabilities_json=zeroblob(1048577) WHERE id=1")));
        QVERIFY(std::holds_alternative<engine::Error>(session.listEngines(0, 50)));
        QVERIFY(std::holds_alternative<engine::Error>(
            session.registerEngine(candidate(QStringLiteral("/first")))));
        QCOMPARE(fixture.scalar(QStringLiteral("SELECT count(*) FROM engines")).toInt(), 2);
        // A damaged row does not prevent a healthy page from loading.
        QVERIFY(
            std::holds_alternative<std::vector<engine::EngineProfile>>(session.listEngines(1, 1)));
    }
    void damagedDefaultRejectsDuplicates() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
        StorageSession session;
        QVERIFY(!session.initialize(path, migrations(), ignore));
        const auto value = candidate(QStringLiteral("/first"));
        QVERIFY(std::holds_alternative<RegisteredEngine>(session.registerEngine(value)));
        Fixture fixture(path);
        QVERIFY(fixture.exec(QStringLiteral("UPDATE engines SET is_default=0")));
        QVERIFY(std::holds_alternative<engine::Error>(session.registerEngine(value)));
        QCOMPARE(fixture.scalar(QStringLiteral("SELECT count(*) FROM engines")).toInt(), 1);
    }
    void insertMustProduceTheConfirmedProfile() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
        StorageSession session;
        QVERIFY(!session.initialize(path, migrations(), ignore));
        Fixture fixture(path);
        QVERIFY(fixture.exec(QStringLiteral("CREATE TRIGGER lose_engine AFTER INSERT ON engines "
                                            "BEGIN DELETE FROM engines WHERE id=new.id; END")));
        QVERIFY(std::holds_alternative<engine::Error>(
            session.registerEngine(candidate(QStringLiteral("/first")))));
        QCOMPARE(fixture.scalar(QStringLiteral("SELECT count(*) FROM engines")).toInt(), 0);
    }
    void upgradeAndBackup() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
        {
            StorageSession old;
            QVERIFY(!old.initialize(path, std::span(migrations()).first(3), ignore));
        }
        {
            Fixture fixture(path);
            QVERIFY(fixture.exec(QStringLiteral("CREATE TABLE engine_upgrade_marker(value TEXT)")));
            QVERIFY(fixture.exec(
                QStringLiteral("INSERT INTO engine_upgrade_marker VALUES('preserved')")));
        }
        StorageSession session;
        QVERIFY(!session.initialize(path, migrations(), ignore));
        Fixture fixture(path);
        QCOMPARE(
            fixture.scalar(QStringLiteral("SELECT value FROM engine_upgrade_marker")).toString(),
            QStringLiteral("preserved"));
        QCOMPARE(
            fixture.scalar(QStringLiteral("SELECT max(version) FROM schema_migrations")).toInt(),
            9);
        QCOMPARE(QDir(directory.filePath(QStringLiteral("backups")))
                     .entryList({QStringLiteral("*.sqlite3")}, QDir::Files)
                     .size(),
                 1);
        QVERIFY(std::holds_alternative<RegisteredEngine>(
            session.registerEngine(candidate(QStringLiteral("/first")))));
        QVERIFY(!fixture.exec(
            QStringLiteral("INSERT INTO engines SELECT "
                           "id+1,display_name,'/"
                           "other',zeroblob(32),reported_name,reported_author,detected_version,"
                           "capabilities_json,option_overrides_json,is_default,created_at,last_"
                           "validated_at FROM engines WHERE id=1")));
    }
};
QTEST_GUILESS_MAIN(EngineRepositoryTest)
#include "EngineRepositoryTest.moc"
