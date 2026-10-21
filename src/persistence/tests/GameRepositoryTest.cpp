#include "TestEnvironment.h"
#include "app/GameImportService.h"
#include "app/ImportController.h"
#include "persistence/private/GameCodec.h"
#include "persistence/private/StorageSession.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <array>

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
    bool insertLegacy(GameId id, const pgn::PgnDocument& document) {
        const auto encoded = encodeGame(document, 1);
        const auto* game = std::get_if<EncodedGame>(&encoded);
        if (!game) {
            return false;
        }
        const auto metadata = gameMetadata(document, 1);
        QSqlQuery query(database_);
        if (!query.prepare(
                QStringLiteral("INSERT INTO games VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"))) {
            return false;
        }
        for (const auto& value : QVariantList{
                 QVariant::fromValue(static_cast<qlonglong>(id)), 1, game->hash, game->identity,
                 game->original, game->snapshot, 1, metadata.white, metadata.black, metadata.result,
                 metadata.playedAt, metadata.timeControl, QStringLiteral("fixture"),
                 QStringLiteral("original source"), QStringLiteral("2026-10-20T00:00:00.000Z")}) {
            query.addBindValue(value);
        }
        return query.exec();
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
    void metadataRejectsHiddenBytes();
    void summaryBoundsAndIsolation();
    void parsedSnapshotsRemainPersistable();
    void duplicateLargeSnapshots();
    void reportWarningBudget();
    void leadingBomInsideTagIsContent();
    void legacyTextUpgradePreservesGames();
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
    const auto frozen = std::get<EncodedGame>(encodeGame(*document("1. e4 e5 *"), 1));
    QCOMPARE(frozen.hash.toHex(),
             QByteArray("ce76558e5d88f05ead2e6ca4f21ca60ffbe0c7b0b2e8f4b8c90e2406fd1ea35c"));
    const auto currentIdentity = std::get<EncodedGame>(encodeGame(*document("1. e4 e5 *")));
    QCOMPARE(currentIdentity.hash.toHex(),
             QByteArray("51629077a7bd8ffac667cca613a21e72b56f28af86b6a020de07f250f86a9991"));
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
    app::ImportController batch(storage.importer);
    bool batchClosed = false;
    connect(&batch, &app::ImportController::closed, &batch, [&] { batchClosed = true; });
    const auto closeBatch = qScopeGuard([&] {
        if (!batchClosed) {
            QEventLoop loop;
            QTimer deadline;
            deadline.setSingleShot(true);
            connect(&batch, &app::ImportController::closed, &loop, &QEventLoop::quit);
            connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
            deadline.start(10'000);
            batch.close();
            if (!batchClosed) {
                loop.exec();
            }
            if (!batchClosed) {
                qFatal("Batch did not close after storage failure");
            }
        }
    });
    const auto source = directory.filePath(QStringLiteral("input.pgn"));
    QFile input(source);
    QVERIFY(input.open(QIODevice::WriteOnly));
    QCOMPARE(input.write("1.Nf3 * 1.c4 *"), qint64(14));
    input.close();
    batch.setReady(true);
    QVERIFY(batch.importFiles({QUrl::fromLocalFile(source), QUrl::fromLocalFile(source)}));
    QTRY_VERIFY(!batch.busy());
    QCOMPARE(batch.failures(), std::size_t(1));
    QCOMPARE(batch.inserted(), std::size_t(0));
    QCOMPARE(batch.files()[0].items.size(), std::size_t(1));
    QVERIFY(std::holds_alternative<GameError>(batch.files()[0].items[0].outcome));
    QCOMPARE(batch.files()[1].state, app::ImportFileState::Unprocessed);
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
void GameRepositoryTest::metadataRejectsHiddenBytes() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
    StorageSession session;
    QVERIFY(!session.initialize(path, migrations(), ignoreProgress));
    const auto id =
        std::get<Inserted>(session.insertGame(*document("[White \"Alice\"]\n\n*"), {})).id;
    Fixture fixture(path);
    QVERIFY(fixture.exec(QStringLiteral("UPDATE games SET source_uri='a'||char(0)||'hidden'")));
    QVERIFY2(std::holds_alternative<GameError>(session.loadGame(id)),
             "Embedded NUL bytes must not disappear during SQL text projection");
    QVERIFY(fixture.update(QStringLiteral("source_uri"), QStringLiteral(""), id));
    QVERIFY(fixture.exec(QStringLiteral("UPDATE games SET white_name='Alice'||char(0)||'hidden'")));
    QVERIFY(std::holds_alternative<GameError>(session.loadGame(id)));
    const auto unicode =
        std::get<Inserted>(session.insertGame(*document("[White \"�\"]\n\n*"), {})).id;
    QVERIFY(
        fixture.exec(QStringLiteral("UPDATE games SET white_name=CAST(X'FF' AS TEXT) WHERE id=2")));
    QVERIFY(std::holds_alternative<GameError>(session.loadGame(unicode)));
}
void GameRepositoryTest::summaryBoundsAndIsolation() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
    StorageSession session;
    QVERIFY(!session.initialize(path, migrations(), ignoreProgress));
    const auto first =
        std::get<Inserted>(
            session.insertGame(*document("[White \"" + std::string(60000, 'A') + "\"]\n\n1. e4 *"),
                               {}))
            .id;
    const auto second = std::get<Inserted>(session.insertGame(*document("1. d4 *"), {})).id;
    auto result = session.listGames(0, 200);
    QVERIFY(std::holds_alternative<std::vector<GameSummary>>(result));
    for (const auto& value : std::get<std::vector<GameSummary>>(result)) {
        QVERIFY(value.white.capacity() <= 1024);
        QVERIFY2(value.white.size() <= 256,
                 "Page results must not retain full-size PGN tag values");
    }
    const auto full = session.loadGame(first);
    QCOMPARE(std::get<StoredGame>(full).summary.white.size(), 60000);
    QVERIFY(!std::get<StoredGame>(full).summary.metadataTruncated);
    Fixture fixture(path);
    QVERIFY(fixture.update(QStringLiteral("white_name"), QString(100000, u'x'), first));
    result = session.listGames(0, 200);
    QVERIFY2(std::holds_alternative<std::vector<GameSummary>>(result),
             "One malformed row must not hide a healthy game on the same page");
    const auto& values = std::get<std::vector<GameSummary>>(result);
    QVERIFY(
        std::ranges::any_of(values, [second](const auto& value) { return value.id == second; }));
    const auto damaged =
        std::ranges::find_if(values, [first](const auto& value) { return value.id == first; });
    QVERIFY(damaged != values.end());
    QVERIFY(damaged->metadataError.has_value());
    QVERIFY(damaged->white.isEmpty() && damaged->result.isEmpty());
    const auto emoji = document("[White \"" + std::string(255, 'a') + "😀x\"]\n\n1. c4 *");
    const auto emojiId = std::get<Inserted>(session.insertGame(*emoji, {})).id;
    const auto page = std::get<std::vector<GameSummary>>(session.listGames(0, 200));
    const auto clipped =
        std::ranges::find_if(page, [emojiId](const auto& value) { return value.id == emojiId; });
    QVERIFY(clipped != page.end());
    QVERIFY(clipped->metadataTruncated && clipped->white.isValidUtf16());
    QCOMPARE(clipped->white.size(), 255);
}
void GameRepositoryTest::parsedSnapshotsRemainPersistable() {
    const std::string source =
        "[White \"A\"]\n[Result \"*\"]\n\n1. e4 $1 {note} (1. d4 d5) e5 *\n\n1. Nf3 *";
    for (std::size_t index = 0; index < source.size(); ++index) {
        for (const auto byte : {'[', ']', '{', '}', '"', '\\', '\0', '\r', '\n', '%', '*', '?'}) {
            auto mutated = source;
            mutated[index] = byte;
            pgn::PgnReader reader(mutated);
            for (int gameIndex = 0; gameIndex < 10; ++gameIndex) {
                auto outcome = reader.next();
                if (const auto* value = std::get_if<pgn::PgnDocument>(&outcome)) {
                    const auto encoded = encodeGame(*value);
                    QVERIFY2(std::holds_alternative<EncodedGame>(encoded),
                             "Every default-budget accepted import must remain persistable");
                    const auto& saved = std::get<EncodedGame>(encoded);
                    QVERIFY(std::holds_alternative<StoredGame>(
                        decodeGame(saved.original, saved.snapshot)));
                } else if (std::holds_alternative<pgn::EndOfInput>(outcome) ||
                           std::holds_alternative<pgn::Cancelled>(outcome)) {
                    break;
                }
            }
        }
    }
}
void GameRepositoryTest::duplicateLargeSnapshots() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
    StorageSession session;
    QVERIFY(!session.initialize(path, migrations(), ignoreProgress));
    std::string annotated;
    std::string plain;
    for (int index = 0; index < 16; ++index) {
        const std::array moves{"Nf3", "Nf6", "Ng1", "Ng8"};
        const std::string move = std::string(moves[static_cast<std::size_t>(index % 4)]) + " ";
        plain += move;
        annotated += move + "{" + std::string(60000, 'x') + "} ";
    }
    plain += "*";
    annotated += "*";
    const auto id = std::get<Inserted>(session.insertGame(*document(annotated), {})).id;
    const auto incoming = document(plain);
    QElapsedTimer timer;
    timer.start();
    for (int index = 0; index < 50; ++index) {
        const auto outcome = session.insertGame(*incoming, {});
        QVERIFY(std::holds_alternative<Duplicate>(outcome));
        QCOMPARE(std::get<Duplicate>(outcome).id, id);
    }
    qInfo("50 repeated duplicate checks against a 960 KB annotated game: %lld ms",
          static_cast<long long>(timer.elapsed()));
    const auto names = QSqlDatabase::connectionNames();
    QCOMPARE(names.size(), 1);
    {
        QSqlQuery query(QSqlDatabase::database(names.front()));
        QVERIFY(query.exec(QStringLiteral("UPDATE games SET source_uri='x'||char(0)||'hidden'")));
        QVERIFY(std::holds_alternative<GameError>(session.insertGame(*incoming, {})));
        QVERIFY(query.exec(QStringLiteral("UPDATE games SET source_uri=''")));
        QVERIFY(std::holds_alternative<Duplicate>(session.insertGame(*incoming, {})));
    }
    // External changes must invalidate any validation shortcut.
    Fixture fixture(path);
    const auto encoded = std::get<EncodedGame>(encodeGame(*document(annotated)));
    QVERIFY(fixture.update(QStringLiteral("snapshot"), QByteArray("invalid"), id));
    QVERIFY(std::holds_alternative<GameError>(session.insertGame(*incoming, {})));
    QVERIFY(fixture.update(QStringLiteral("snapshot"), encoded.snapshot, id));
    QVERIFY(std::holds_alternative<Duplicate>(session.insertGame(*incoming, {})));
}
void GameRepositoryTest::reportWarningBudget() {
    QTemporaryDir directory;
    AsyncStorage storage;
    QSignalSpy ready(&storage.database, &Database::ready);
    storage.database.initialize(directory.filePath(QStringLiteral("library.sqlite3")));
    QTRY_COMPARE(ready.count(), 1);
    QSignalSpy finished(&storage.importer, &app::GameImportService::finished);
    std::size_t retainedCapacity = 0;
    connect(&storage.importer, &app::GameImportService::progress, &storage.importer,
            [&](const app::ImportItem& item) { retainedCapacity += item.warnings.capacity(); });
    std::string source;
    for (int index = 0; index < 11; ++index) {
        for (int tagIndex = 0; tagIndex < 101; ++tagIndex) {
            source += R"([White "A"])";
        }
        source += " *\n";
    }
    storage.importer.importText(QUuid::createUuid(), std::move(source));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 10000);
    const auto report = qvariant_cast<app::ImportReport>(finished.at(0).at(0));
    QCOMPARE(report.items.size(), std::size_t(11));
    std::size_t retained = 0;
    std::size_t suppressed = 0;
    for (const auto& item : report.items) {
        retained += item.warnings.size();
        suppressed += item.suppressedWarnings;
    }
    QCOMPARE(retained + suppressed, std::size_t(1100));
    QVERIFY(retainedCapacity <= 2000);
    QVERIFY2(retained <= 1000,
             "The report must bound aggregate warning retention, not only warnings per game");
}
void GameRepositoryTest::leadingBomInsideTagIsContent() {
    QTemporaryDir directory;
    StorageSession session;
    QVERIFY(!session.initialize(directory.filePath(QStringLiteral("library.sqlite3")), migrations(),
                                ignoreProgress));
    QVERIFY(std::holds_alternative<Inserted>(
        session.insertGame(*document("[White \"Alice\"]\n\n*"), {})));
    const auto input = document(std::string("[White \"") + "\xEF\xBB\xBF" + "Alice\"]\n\n*");
    const auto result =
        session.insertGame(*input, {QString(QChar(0xFEFF)) + QStringLiteral("paste"),
                                    QString(QChar(0xFEFF)) + QStringLiteral("source")});
    QVERIFY2(std::holds_alternative<Inserted>(result),
             "U+FEFF inside a tag value is content, not a source BOM or whitespace normalization");
    const auto loaded = session.loadGame(std::get<Inserted>(result).id);
    QVERIFY(std::holds_alternative<StoredGame>(loaded));
    QVERIFY(std::get<StoredGame>(loaded).summary.white.startsWith(QChar(0xFEFF)));
    QVERIFY(std::get<StoredGame>(loaded).source.uri.startsWith(QChar(0xFEFF)));
    QVERIFY(std::get<StoredGame>(loaded).source.type.startsWith(QChar(0xFEFF)));
    const auto encoded = std::get<EncodedGame>(encodeGame(*input));
    QVERIFY(std::holds_alternative<GameError>(decodeGame(encoded.original, encoded.snapshot, 1)));
}
void GameRepositoryTest::legacyTextUpgradePreservesGames() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("library.sqlite3"));
    const auto original = document(std::string("[White \"") + "\xEF\xBB\xBF" +
                                   "Alice\"]\n\n{\xEF\xBB\xBF"
                                   "comment} *");
    {
        StorageSession old;
        QVERIFY(!old.initialize(path, std::span(migrations()).first(2), ignoreProgress));
        Fixture fixture(path);
        QVERIFY(fixture.insertLegacy(41, *original));
        QVERIFY(fixture.insertLegacy(42, *document("[White \"Bob\"]\n\n*")));
        QVERIFY(fixture.update(QStringLiteral("snapshot"), QByteArray("broken"), 42));
        QVERIFY(fixture.insertLegacy(43, *document("[White \"Charlie\"]\n\n*")));
    }
    StorageSession current;
    QVERIFY(!current.initialize(path, migrations(), ignoreProgress));
    const auto loaded = current.loadGame(41);
    QVERIFY(std::holds_alternative<StoredGame>(loaded));
    const auto& game = std::get<StoredGame>(loaded);
    QCOMPARE(game.summary.id, GameId(41));
    QCOMPARE(game.document->originalText, original->originalText);
    QCOMPARE(game.source.uri, QStringLiteral("original source"));
    QCOMPARE(game.summary.importedAt, QStringLiteral("2026-10-20T00:00:00.000Z"));
    QVERIFY(game.summary.white.startsWith(QChar(0xFEFF)));
    QVERIFY(std::holds_alternative<StoredGame>(current.loadGame(43)));
    QVERIFY(std::holds_alternative<GameError>(current.loadGame(42)));
    const auto duplicate = current.insertGame(*original, {});
    QVERIFY(std::holds_alternative<Duplicate>(duplicate));
    QCOMPARE(std::get<Duplicate>(duplicate).id, GameId(41));
    QVERIFY(std::holds_alternative<Inserted>(
        current.insertGame(*document("[White \"Alice\"]\n\n*"), {})));
    const QDir backups(directory.filePath(QStringLiteral("backups")));
    QCOMPARE(backups.entryList({QStringLiteral("*.sqlite3")}, QDir::Files).size(), 1);
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
