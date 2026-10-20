#include "TestEnvironment.h"
#include "app/GameImportService.h"
#include "persistence/private/GameCodec.h"
#include "persistence/private/StorageSession.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace chess_review;
using namespace chess_review::persistence;
using namespace chess_review::persistence::detail;
namespace {
const Progress ignoreProgress = [](const QString&) {};
std::shared_ptr<const pgn::PgnDocument> document(const std::string& bytes) {
    pgn::PgnReader reader(bytes);
    auto outcome = reader.next();
    auto* value = std::get_if<pgn::PgnDocument>(&outcome);
    if (!value) {
        qFatal("Invalid test PGN");
    }
    return std::make_shared<const pgn::PgnDocument>(std::move(*value));
}
class Fixture {
  public:
    explicit Fixture(const QString& path) {
        name_ = QUuid::createUuid().toString();
        database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name_);
        database_.setDatabaseName(path);
        if (!database_.open()) {
            qFatal("Cannot open fixture");
        }
    }
    ~Fixture() {
        database_.close();
        database_ = {};
        QSqlDatabase::removeDatabase(name_);
    }
    bool exec(const QString& sql) {
        QSqlQuery query(database_);
        return query.exec(sql);
    }
    bool update(const QString& column, const QVariant& value, GameId id) {
        // Column names are internal test constants, never user input.
        QSqlQuery query(database_);
        if (!query.prepare(QStringLiteral("UPDATE games SET ") + column +
                           QStringLiteral("=? WHERE id=?"))) {
            return false;
        }
        query.addBindValue(value);
        query.addBindValue(static_cast<qlonglong>(id));
        return query.exec();
    }

  private:
    QString name_;
    QSqlDatabase database_;
};
// Ensures asynchronous closure even when a QVERIFY returns early.
class AsyncStorage {
  public:
    Database database;
    app::GameImportService importer{database};
    bool importClosed = false;
    bool databaseClosed = false;
    AsyncStorage() {
        QObject::connect(&importer, &app::GameImportService::closed, &importer,
                         [this] { importClosed = true; });
        QObject::connect(&database, &Database::closed, &database,
                         [this] { databaseClosed = true; });
    }
    ~AsyncStorage() {
        if (!importClosed) {
            QSignalSpy done(&importer, &app::GameImportService::closed);
            importer.close();
            if (done.empty()) {
                done.wait(10000);
            }
        }
        if (!databaseClosed) {
            QSignalSpy done(&database, &Database::closed);
            database.close();
            if (done.empty()) {
                done.wait(10000);
            }
        }
    }
};
} // namespace
class GameRepositoryTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { QTest::failOnWarning(); }
    void roundTripAndReopen();
    void duplicatePolicy();
    void atomicFailureAndRetry();
    void corruptRecords();
    void migrationFromOne();
    void importOutcomesAndCancellation();
    void requestLifecycleAndBounds();
    void cancellationDuringCommitAndShutdown();
    void storageFailureStopsImport();
    void cancelWhileParsing();
};
void GameRepositoryTest::roundTripAndReopen() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
    const auto input = document("[White \"A ' ); DROP TABLE games;\"]\n[Unknown \"λ\"]\n\n{before} "
                                "1. e4 $1 $1 {hello 世界} (1. d4 d5 (1... Nf6)) e5 *");
    GameId id = 0;
    QByteArray snapshot;
    {
        StorageSession session;
        QVERIFY(!session.initialize(path, migrations(), ignoreProgress));
        const auto inserted = session.insertGame(
            *input, {QStringLiteral("paste"), QStringLiteral("https://example.invalid/'?q=;")});
        QVERIFY(std::holds_alternative<Inserted>(inserted));
        id = std::get<Inserted>(inserted).id;
        auto encoded = encodeGame(*input);
        snapshot = std::get<EncodedGame>(encoded).snapshot;
    }
    StorageSession session;
    QVERIFY(!session.initialize(path, migrations(), ignoreProgress));
    auto loaded = session.loadGame(id);
    QVERIFY(std::holds_alternative<StoredGame>(loaded));
    const auto& game = std::get<StoredGame>(loaded);
    QCOMPARE(std::get<EncodedGame>(encodeGame(*game.document)).snapshot, snapshot);
    QCOMPARE(game.source.type, QStringLiteral("paste"));
    QCOMPARE(game.document->originalText, input->originalText);
    QVERIFY(std::holds_alternative<Duplicate>(session.insertGame(*input, {})));
    // A parser-merged continuation is not the original played line.
    const auto merged = document("1. e4 (1. e4 e5) *");
    QCOMPARE(merged->originalMainline().size(), std::size_t(1));
    const auto other = session.insertGame(*merged, {});
    QVERIFY(std::holds_alternative<Inserted>(other));
    const auto reopened = session.loadGame(std::get<Inserted>(other).id);
    QCOMPARE(std::get<StoredGame>(reopened).document->originalMainline().size(), std::size_t(1));
    const auto fen = document("[SetUp \"1\"]\n[FEN \"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w "
                              "KQkq - 0 1\"]\n\n1. Nf3 *");
    const auto savedFen = session.insertGame(*fen, {});
    QVERIFY(std::holds_alternative<Inserted>(savedFen));
    const auto restoredFen = session.loadGame(std::get<Inserted>(savedFen).id);
    const auto root = std::get<StoredGame>(restoredFen).document->tree.position(chess::NodeId{0});
    QCOMPARE(std::get<chess::Position>(root).context().provenance,
             chess::RootProvenance::FenAnalysisRoot);
    auto changed = *input;
    QVERIFY(std::holds_alternative<std::monostate>(
        changed.tree.setAnnotations(chess::NodeId{1}, {"changed", {}, {}})));
    QVERIFY(std::holds_alternative<GameError>(session.insertGame(changed, {})));
}
void GameRepositoryTest::duplicatePolicy() {
    QTemporaryDir directory;
    StorageSession session;
    QVERIFY(!session.initialize(directory.filePath(QStringLiteral("library.sqlite3")), migrations(),
                                ignoreProgress));
    // Frozen v1 identity: changing the encoding requires a migration, not a new hash.
    const auto frozen = std::get<EncodedGame>(encodeGame(*document("1. e4 e5 *")));
    QCOMPARE(frozen.hash.toHex(),
             QByteArray("ce76558e5d88f05ead2e6ca4f21ca60ffbe0c7b0b2e8f4b8c90e2406fd1ea35c"));
    const auto first = session.insertGame(*document("1. e4 e5 *"), {});
    QVERIFY(std::holds_alternative<Inserted>(first));
    const auto id = std::get<Inserted>(first).id;
    for (const auto& text : {"1.e4 {annotation} e5 *", "[White \"?\"]\n\n1. e4 e5 *",
                             "[Other \"ignored\"]\n\n1. e4 (1. d4) e5 *"}) {
        const auto result = session.insertGame(*document(text), {});
        QVERIFY(std::holds_alternative<Duplicate>(result));
        QCOMPARE(std::get<Duplicate>(result).id, id);
    }
    for (const auto* name : {"White", "Black", "Date", "Event", "Site", "Round", "TimeControl"}) {
        const auto game = document(std::string("[") + name + " \"different\"]\n\n1. e4 e5 *");
        QVERIFY(std::holds_alternative<Inserted>(session.insertGame(*game, {})));
    }
    for (const auto* text : {"1. e4 e5 1-0", "1. d4 d5 *", "[White \"a\"]\n\n1. e4 e5 *",
                             "[White \"A\"]\n\n1. e4 e5 *", "[White \" A\"]\n\n1. e4 e5 *"}) {
        QVERIFY(std::holds_alternative<Inserted>(session.insertGame(*document(text), {})));
    }
    auto games = session.listGames(0, 1);
    QCOMPARE(std::get<std::vector<GameSummary>>(games).size(), std::size_t(1));
    const auto loaded = session.loadGame(id);
    QCOMPARE(std::get<StoredGame>(loaded).document->originalText, std::string("1. e4 e5 *"));
}
void GameRepositoryTest::atomicFailureAndRetry() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
    StorageSession session;
    QVERIFY(!session.initialize(path, migrations(), ignoreProgress));
    Fixture fixture(path);
    const auto first = session.insertGame(*document("1. e4 *"), {});
    QVERIFY(std::holds_alternative<Inserted>(first));
    QVERIFY(fixture.exec(QStringLiteral("CREATE TRIGGER reject_game BEFORE INSERT ON games BEGIN "
                                        "SELECT RAISE(ABORT,'fixture'); END")));
    QVERIFY(std::holds_alternative<GameError>(session.insertGame(*document("1. d4 *"), {})));
    QCOMPARE(std::get<std::vector<GameSummary>>(session.listGames(0, 50)).size(), std::size_t(1));
    QVERIFY(fixture.exec(QStringLiteral("DROP TRIGGER reject_game")));
    // Deferred FK violation allows INSERT but fails at COMMIT.
    QVERIFY(fixture.exec(
        QStringLiteral("CREATE TABLE commit_failure (game_id INTEGER REFERENCES games(id), "
                       "missing_id INTEGER REFERENCES games(id) DEFERRABLE INITIALLY DEFERRED)")));
    QVERIFY(fixture.exec(QStringLiteral("CREATE TRIGGER fail_commit AFTER INSERT ON games BEGIN "
                                        "INSERT INTO commit_failure VALUES(new.id,-1); END")));
    const auto failed = session.insertGame(*document("1. d4 *"), {});
    QVERIFY(std::holds_alternative<GameError>(failed));
    QCOMPARE(std::get<GameError>(failed).code, GameErrorCode::Storage);
    QCOMPARE(std::get<std::vector<GameSummary>>(session.listGames(0, 50)).size(), std::size_t(1));
    QVERIFY(fixture.exec(QStringLiteral("DROP TRIGGER fail_commit")));
    QVERIFY(std::holds_alternative<Inserted>(session.insertGame(*document("1. d4 *"), {})));
    QVERIFY(std::holds_alternative<StoredGame>(session.loadGame(std::get<Inserted>(first).id)));
}
void GameRepositoryTest::corruptRecords() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
    StorageSession session;
    QVERIFY(!session.initialize(path, migrations(), ignoreProgress));
    const auto input = document("1. e4 *");
    const auto id = std::get<Inserted>(session.insertGame(*input, {})).id;
    const auto other = std::get<Inserted>(session.insertGame(*document("1. d4 *"), {})).id;
    Fixture fixture(path);
    const auto encoded = std::get<EncodedGame>(encodeGame(*input));
    for (const auto& bytes :
         {QByteArray("{}"), QByteArray("{invalid"), QByteArray("{\"formatVersion\":2}"),
          QByteArray("{\"formatVersion\":1,\"nodes\":[]}"), QByteArray(1, char(0xff))}) {
        QVERIFY(fixture.update(QStringLiteral("snapshot"), bytes, id));
        QVERIFY(std::holds_alternative<GameError>(session.loadGame(id)));
        QVERIFY(std::holds_alternative<StoredGame>(session.loadGame(other)));
    }
    QVERIFY(fixture.update(QStringLiteral("snapshot"), encoded.snapshot, id));
    QVERIFY(fixture.update(QStringLiteral("identity"), QByteArray("wrong"), id));
    QVERIFY(std::holds_alternative<GameError>(session.insertGame(*input, {})));
    QVERIFY(fixture.update(QStringLiteral("identity"), encoded.identity, id));
    QVERIFY(
        fixture.exec(QStringLiteral("UPDATE games SET snapshot=zeroblob(67108865) WHERE id=1")));
    QVERIFY(std::holds_alternative<GameError>(session.loadGame(id)));
    QVERIFY(fixture.update(QStringLiteral("snapshot"), encoded.snapshot, id));
    QVERIFY(fixture.update(QStringLiteral("original_pgn"), QByteArray("1. e5 *"), id));
    QVERIFY(std::holds_alternative<GameError>(session.loadGame(id)));
    QCOMPARE(std::get<GameError>(session.loadGame(-1)).code, GameErrorCode::Missing);
}
void GameRepositoryTest::migrationFromOne() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
    {
        StorageSession session;
        QVERIFY(!session.initialize(path, std::span(migrations()).first(1), ignoreProgress));
    }
    {
        StorageSession session;
        QVERIFY(!session.initialize(path, migrations(), ignoreProgress));
        QVERIFY(std::holds_alternative<Inserted>(session.insertGame(*document("*"), {})));
    }
    const QDir backups(directory.filePath(QStringLiteral("backups")));
    QCOMPARE(backups.entryList({QStringLiteral("*.sqlite3")}, QDir::Files).size(), 1);
    StorageSession session;
    QVERIFY(!session.initialize(path, migrations(), ignoreProgress));
    QCOMPARE(std::get<std::vector<GameSummary>>(session.listGames(0, 50)).size(), std::size_t(1));
}
void GameRepositoryTest::importOutcomesAndCancellation() {
    QTemporaryDir directory;
    AsyncStorage storage;
    QSignalSpy ready(&storage.database, &Database::ready);
    storage.database.initialize(directory.filePath(QStringLiteral("library.sqlite3")));
    QTRY_COMPARE(ready.count(), 1);
    QSignalSpy finished(&storage.importer, &app::GameImportService::finished);
    QSignalSpy rejected(&storage.importer, &app::GameImportService::rejected);
    const auto request = QUuid::createUuid();
    storage.importer.importText(request, "1. e4 *\n\n1. e4 *\n\n1. e5 *\n\n1. d4 *");
    storage.importer.importText(QUuid::createUuid(), "*");
    QTRY_COMPARE(finished.count(), 1);
    QTRY_COMPARE(rejected.count(), 1);
    auto report = qvariant_cast<app::ImportReport>(finished.at(0).at(0));
    QCOMPARE(report.request, request);
    QCOMPARE(report.status, app::ImportStatus::Completed);
    QCOMPARE(report.items.size(), std::size_t(4));
    QVERIFY(std::holds_alternative<Inserted>(report.items[0].outcome));
    QVERIFY(std::holds_alternative<Duplicate>(report.items[1].outcome));
    QVERIFY(std::holds_alternative<pgn::PgnFailure>(report.items[2].outcome));
    QVERIFY(std::holds_alternative<Inserted>(report.items[3].outcome));
    connect(&storage.importer, &app::GameImportService::progress, &storage.importer,
            [&] { storage.importer.cancel(); });
    storage.importer.importText(QUuid::createUuid(), "1. Nf3 *\n\n1. c4 *");
    QTRY_COMPARE(finished.count(), 2);
    report = qvariant_cast<app::ImportReport>(finished.at(1).at(0));
    QCOMPARE(report.status, app::ImportStatus::Cancelled);
    QCOMPARE(report.items.size(), std::size_t(1));
    QVERIFY(std::holds_alternative<Inserted>(report.items.front().outcome));
}
void GameRepositoryTest::requestLifecycleAndBounds() {
    QTemporaryDir directory;
    AsyncStorage storage;
    QSignalSpy loaded(&storage.database, &Database::gameLoaded);
    storage.database.loadGame(QUuid::createUuid(), 1);
    QTRY_COMPARE(loaded.count(), 1);
    QCOMPARE(std::get<GameError>(qvariant_cast<LoadGameResult>(loaded.at(0).at(1))).code,
             GameErrorCode::Unavailable);
    QSignalSpy ready(&storage.database, &Database::ready);
    storage.database.initialize(directory.filePath(QStringLiteral("library.sqlite3")));
    QTRY_COMPARE(ready.count(), 1);
    QSignalSpy listed(&storage.database, &Database::gamesListed);
    for (int index = 0; index < 17; ++index) {
        storage.database.listGames(QUuid::createUuid());
    }
    QTRY_COMPARE(listed.count(), 17);
    int busy = 0;
    for (const auto& args : listed) {
        const auto result = qvariant_cast<ListGamesResult>(args.at(1));
        if (const auto* error = std::get_if<GameError>(&result)) {
            QCOMPARE(error->code, GameErrorCode::Busy);
            ++busy;
        }
    }
    QCOMPARE(busy, 1);
    listed.clear();
    storage.database.listGames(QUuid::createUuid(), -1, 201);
    QTRY_COMPARE(listed.count(), 1);
    QVERIFY(std::holds_alternative<GameError>(qvariant_cast<ListGamesResult>(listed.at(0).at(1))));
}
void GameRepositoryTest::cancellationDuringCommitAndShutdown() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
    AsyncStorage storage;
    auto& database = storage.database;
    auto& importer = storage.importer;
    QSignalSpy ready(&database, &Database::ready);
    database.initialize(path);
    QTRY_COMPARE(ready.count(), 1);
    QSignalSpy finished(&importer, &app::GameImportService::finished);
    QSignalSpy importClosed(&importer, &app::GameImportService::closed);
    QSignalSpy storageClosed(&database, &Database::closed);
    connect(&importer, &app::GameImportService::closed, &database, &Database::close);
    {
        Fixture fixture(path);
        QVERIFY(fixture.exec(QStringLiteral("BEGIN IMMEDIATE")));
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++ticks; });
        heartbeat.start(5);
        QSignalSpy inserted(&database, &Database::gameInserted);
        connect(&importer, &app::GameImportService::savingGame, &importer, [&] {
            importer.close();
            QTimer::singleShot(100, &importer, [&] {
                if (!fixture.exec(QStringLiteral("ROLLBACK"))) {
                    qFatal("Cannot release fixture lock");
                }
            });
        });
        importer.importText(QUuid::createUuid(), "1. e4 *\n\n1. d4 *");
        QTRY_COMPARE(finished.count(), 1);
        QTRY_COMPARE(importClosed.count(), 1);
        QTRY_COMPARE(storageClosed.count(), 1);
        QVERIFY(ticks > 0);
        QCOMPARE(inserted.count(), 1);
        const auto report = qvariant_cast<app::ImportReport>(finished.at(0).at(0));
        QCOMPARE(report.status, app::ImportStatus::Cancelled);
        QCOMPARE(report.items.size(), std::size_t(1));
        QVERIFY(std::holds_alternative<Inserted>(report.items[0].outcome));
    }
    StorageSession session;
    QVERIFY(!session.initialize(path, migrations(), ignoreProgress));
    QCOMPARE(std::get<std::vector<GameSummary>>(session.listGames(0, 50)).size(), std::size_t(1));
}
void GameRepositoryTest::storageFailureStopsImport() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
    AsyncStorage storage;
    QSignalSpy ready(&storage.database, &Database::ready);
    storage.database.initialize(path);
    QTRY_COMPARE(ready.count(), 1);
    Fixture fixture(path);
    QVERIFY(fixture.exec(
        QStringLiteral("CREATE TRIGGER reject_second BEFORE INSERT ON games WHEN (SELECT count(*) "
                       "FROM games)>0 BEGIN SELECT RAISE(ABORT,'fixture'); END")));
    QSignalSpy finished(&storage.importer, &app::GameImportService::finished);
    storage.importer.importText(QUuid::createUuid(), "1. e4 *\n\n1. d4 *\n\n1. c4 *");
    QTRY_COMPARE(finished.count(), 1);
    const auto report = qvariant_cast<app::ImportReport>(finished.at(0).at(0));
    QCOMPARE(report.status, app::ImportStatus::Failed);
    QCOMPARE(report.items.size(), std::size_t(2));
    QVERIFY(std::holds_alternative<Inserted>(report.items[0].outcome));
    QVERIFY(std::holds_alternative<GameError>(report.items[1].outcome));
    QCOMPARE(report.items[1].ordinal, std::size_t(2));
    QSignalSpy listed(&storage.database, &Database::gamesListed);
    storage.database.listGames(QUuid::createUuid());
    QTRY_COMPARE(listed.count(), 1);
    QCOMPARE(std::get<std::vector<GameSummary>>(qvariant_cast<ListGamesResult>(listed.at(0).at(1)))
                 .size(),
             std::size_t(1));
}
void GameRepositoryTest::cancelWhileParsing() {
    QTemporaryDir directory;
    AsyncStorage storage;
    QSignalSpy ready(&storage.database, &Database::ready);
    storage.database.initialize(directory.filePath(QStringLiteral("library.sqlite3")));
    QTRY_COMPARE(ready.count(), 1);
    QSignalSpy finished(&storage.importer, &app::GameImportService::finished);
    QSignalSpy inserted(&storage.database, &Database::gameInserted);
    // No owner-thread parse result can be consumed before this cancellation.
    storage.importer.importText(QUuid::createUuid(), "1. e4 *\n\n1. d4 *");
    storage.importer.cancel();
    QTRY_COMPARE(finished.count(), 1);
    const auto report = qvariant_cast<app::ImportReport>(finished.at(0).at(0));
    QCOMPARE(report.status, app::ImportStatus::Cancelled);
    QVERIFY(report.items.empty());
    QCOMPARE(inserted.count(), 0);
}
int main(int argc, char* argv[]) {
    const TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QCoreApplication application(argc, argv);
    GameRepositoryTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "GameRepositoryTest.moc"
