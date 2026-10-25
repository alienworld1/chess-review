#include "EngineTestSupport.h"
#include "TestEnvironment.h"
#include "engine/private/Fingerprint.h"
#include "engine/private/SearchProtocol.h"
#include "persistence/Database.h"
#include "persistence/private/AnalysisCodec.h"
#include "persistence/private/Migrations.h"
#include "persistence/private/StorageSession.h"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QVariant>
#include <algorithm>
#include <functional>
#include <limits>

using namespace chess_review;
using namespace chess_review::engine;
using namespace chess_review::persistence;
using namespace chess_review::persistence::detail;
namespace {
EngineFingerprint supportedFingerprint(int width = 1, bool wdl = false) {
    ValidatedEngine engine;
    engine.executableSha256 =
        QByteArray::fromHex("0f83d24cc46d2c66c60f16001af5444873bc112b7d028594513426894c12da19");
    engine.reportedName = QStringLiteral("Stockfish 19");
    engine.reportedAuthor = QStringLiteral("the Stockfish developers (see AUTHORS file)");
    std::vector<EffectiveOption> options;
    for (const auto* declaration :
         {"option name Debug Log File type string default <empty>",
          "option name EvalFile type string default nn-1a298aa575a0.nnue",
          "option name Hash type spin default 16 min 1 max 33554432",
          "option name Move Overhead type spin default 10 min 0 max 5000",
          "option name MultiPV type spin default 1 min 1 max 256",
          "option name nodestime type spin default 0 min 0 max 10000",
          "option name NumaPolicy type string default auto",
          "option name Ponder type check default false",
          "option name Skill Level type spin default 20 min 0 max 20",
          "option name Syzygy50MoveRule type check default true",
          "option name SyzygyPath type string default <empty>",
          "option name SyzygyProbeDepth type spin default 1 min 1 max 100",
          "option name SyzygyProbeLimit type spin default 7 min 0 max 7",
          "option name Threads type spin default 1 min 1 max 1024",
          "option name UCI_Chess960 type check default false",
          "option name UCI_Elo type spin default 1320 min 1320 max 3190",
          "option name UCI_LimitStrength type check default false",
          "option name UCI_ShowWDL type check default false"}) {
        const auto o = std::get<UciOption>(parseOption(QString::fromLatin1(declaration)));
        options.push_back({o.name.toCaseFolded(), o.type, o.defaultValue});
    }
    for (auto& o : options) {
        if (o.name == QStringLiteral("multipv")) {
            o.value = QString::number(width);
        }
        if (o.name == QStringLiteral("uci_showwdl")) {
            o.value = wdl ? QStringLiteral("true") : QStringLiteral("false");
        }
    }
    return makeFingerprint(engine, std::move(options));
}
AnalysisResult sample(int width = 1, bool wdl = false) {
    AnalysisResult result;
    result.outcome = AnalysisOutcome::Completed;
    auto& s = result.evidence;
    auto position = chess::Position::initial();
    s.session = QUuid::createUuid();
    s.request = QUuid::createUuid();
    s.limit = NodeLimit{1000};
    s.requestedMultiPv = width;
    s.effectiveMultiPv = width;
    s.wdlRequested = wdl;
    s.wdlSupported = true;
    s.searchStarted = true;
    s.provenance = std::make_shared<const AnalysisProvenance>(
        AnalysisProvenance{position.context(), supportedFingerprint(width, wdl), std::nullopt,
                           QDateTime::fromMSecsSinceEpoch(1700000000000, QTimeZone::UTC)});
    for (int i = 1; i <= width; ++i) {
        AnalysisLine line;
        line.rank = i;
        line.score = AnalysisScore{ScoreType::Centipawns, i * 5, ScoreBound::Exact};
        line.pv = {position.legalMoves()[static_cast<std::size_t>(i - 1)]};
        line.statistics.depth = 8;
        line.statistics.nodes = 1100;
        if (wdl) {
            line.wdl = Wdl{300, 500, 200};
        }
        s.lines.push_back(std::move(line));
    }
    s.achievedWidth = width;
    s.commonDepth = 8;
    s.statistics.depth = 8;
    s.statistics.nodes = 1100;
    s.bestMove = s.lines.front().pv.front();
    return result;
}
CacheQuery queryFor(const AnalysisResult& result) {
    const auto& s = result.evidence;
    const auto& p = *s.provenance;
    return {p.position, p.fingerprint, s.limit, s.requestedMultiPv, s.wdlRequested, p.searchMoves};
}
void changeProvenance(AnalysisResult& result,
                      const std::function<void(AnalysisProvenance&)>& change) {
    auto p = *result.evidence.provenance;
    change(p);
    result.evidence.provenance = std::make_shared<const AnalysisProvenance>(std::move(p));
}
class Fixture {
  public:
    QTemporaryDir directory;
    QString path = directory.filePath(QStringLiteral("cache.sqlite3"));
    StorageSession session;
    Fixture() {
        if (session.initialize(path, migrations(), [](const QString&) {})) {
            qFatal("Cannot open fixture");
        }
    }
    bool exec(const QString& sql) {
        // Session and fixture execute on this test thread; the connection never crosses threads.
        QSqlQuery query(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
        return query.exec(sql);
    }
};
class AsyncDatabase {
  public:
    Database value;
    bool closed = false;
    AsyncDatabase() {
        QObject::connect(&value, &Database::closed, &value, [this] { closed = true; });
    }
    ~AsyncDatabase() {
        if (closed) {
            return;
        }
        QEventLoop loop;
        QObject::connect(&value, &Database::closed, &loop, &QEventLoop::quit);
        value.close();
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }
};
} // namespace
class AnalysisRepositoryTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { QTest::failOnWarning(); }
    void compatibility() {
        const auto good = sample(2, true);
        QVERIFY(!validateCacheResult(good));
        auto q = queryFor(good);
        QVERIFY(validFingerprint(q.fingerprint));
        QVERIFY(verifiedResources(q.fingerprint));
        QVERIFY(cacheDominates(good, q));
        q.limit = NodeLimit{500};
        QVERIFY(cacheDominates(good, q));
        q.limit = NodeLimit{1001};
        QVERIFY(!cacheDominates(good, q)); // Incidental overshoot.
        q.limit = DepthLimit{1};
        QVERIFY(!cacheDominates(good, q));
        q = queryFor(good);
        q.multiPv = 3;
        QVERIFY(!cacheDominates(good, q));
        q = queryFor(good);
        q.position.halfmoveClock++;
        QVERIFY(!cacheDominates(good, q));
        q = queryFor(good);
        q.position =
            std::get<chess::Position>(chess::Position::fromFen(chess::Position::initial().fen()))
                .context();
        QVERIFY(!cacheDominates(good, q)); // Same board, different root provenance.
        auto depth = good;
        depth.evidence.limit = DepthLimit{8};
        q = queryFor(depth);
        q.limit = DepthLimit{7};
        QVERIFY(cacheDominates(depth, q));
        q.limit = DepthLimit{9};
        QVERIFY(!cacheDominates(depth, q));
        const std::vector<std::function<void(AnalysisResult&)>> changes{
            [](auto& r) { r.outcome = AnalysisOutcome::Stopped; },
            [](auto& r) { r.evidence.searchStarted = false; },
            [](auto& r) { r.error = Error{ErrorCode::Protocol, QStringLiteral("failure")}; },
            [](auto& r) { r.evidence.statistics.nodes.reset(); },
            [](auto& r) { r.evidence.statistics.nodes = 999; },
            [](auto& r) { r.evidence.lines[0].score->bound = ScoreBound::Lower; },
            [](auto& r) { r.evidence.lines[0].wdl.reset(); },
            [](auto& r) {
                r.evidence.lines[0].statistics.depth = 7;
                r.evidence.commonDepth.reset();
            },
            [](auto& r) { r.evidence.lines[0].pv.clear(); },
            [](auto& r) { r.evidence.lines[1].pv = r.evidence.lines[0].pv; },
            [](auto& r) { changeProvenance(r, [](auto& p) { p.fingerprint.digest[0] ^= 1; }); },
        };
        for (std::size_t i = 0; i < changes.size(); ++i) {
            auto changed = good;
            changes[i](changed);
            QVERIFY2(!cacheDominates(changed, queryFor(good)), qPrintable(QString::number(i)));
        }
        auto downgrade = good;
        downgrade.evidence.requestedMultiPv = 3;
        auto wider = queryFor(downgrade);
        QVERIFY(!cacheDominates(downgrade, wider));
        auto timed = good;
        timed.evidence.limit = TimeLimit{1000};
        QVERIFY(!cacheDominates(timed, queryFor(timed)));
        auto infinite = good;
        infinite.evidence.limit = InfiniteLimit{};
        QVERIFY(!cacheDominates(infinite, queryFor(infinite)));
    }
    void terminalAndInsufficientEvidenceRemainStorable() {
        auto terminal = sample();
        const auto position =
            std::get<chess::Position>(chess::Position::fromFen("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1"));
        changeProvenance(terminal, [&](auto& p) { p.position = position.context(); });
        terminal.evidence.lines.clear();
        terminal.evidence.achievedWidth = 0;
        terminal.evidence.commonDepth.reset();
        terminal.evidence.bestMove.reset();
        QVERIFY(!validateCacheResult(terminal));
        QVERIFY(!cacheDominates(terminal, queryFor(terminal)));
        QVERIFY(decodeAnalysis(std::get<QByteArray>(encodeAnalysis(terminal))));
        auto bounded = sample();
        bounded.evidence.lines[0].score->bound = ScoreBound::Upper;
        Fixture database;
        QVERIFY(std::holds_alternative<AnalysisStored>(database.session.storeAnalysis(bounded)));
        QVERIFY(!std::get<CacheLookup>(database.session.findAnalysis(queryFor(bounded))).hit);
        auto missing = sample();
        missing.evidence.bestMove.reset();
        QVERIFY(validateCacheResult(missing));
    }
    void resourcesAndOptions() {
        auto f = supportedFingerprint();
        QVERIFY(validFingerprint(f));
        QVERIFY(verifiedResources(f));
        for (const auto* name : {"evalfile", "syzygypath", "debug log file", "threads", "nodestime",
                                 "uci_chess960", "ponder"}) {
            auto changed = f;
            for (auto& o : changed.options) {
                if (o.name == QLatin1String(name)) {
                    o.value += u'x';
                }
            }
            changed.digest = fingerprintDigest(changed);
            QVERIFY(!verifiedResources(changed));
            QVERIFY(!validFingerprint(changed));
        }
        auto altered = f;
        altered.executableSha256[0] ^= 1;
        altered.digest = fingerprintDigest(altered);
        QVERIFY(!verifiedResources(altered));
        altered = f;
        altered.resourcePolicyVersion++;
        altered.digest = fingerprintDigest(altered);
        QVERIFY(!validFingerprint(altered));
        altered = f;
        altered.options.push_back(
            {QStringLiteral("zzz"), OptionType::String, QStringLiteral("external")});
        altered.digest = fingerprintDigest(altered);
        QVERIFY(!verifiedResources(altered));
        auto r = sample();
        auto q = queryFor(r);
        q.fingerprint.options[2].value = QStringLiteral("32");
        q.fingerprint.digest = fingerprintDigest(q.fingerprint);
        QVERIFY(validFingerprint(q.fingerprint));
        QVERIFY(!cacheDominates(r, q));
        changeProvenance(r, [](auto& p) {
            p.fingerprint.schemaVersion = 1;
            p.fingerprint.resourcePolicyVersion = 0;
            p.fingerprint.resources = ResourceVerification::ExternalResourcesUnverified;
            p.fingerprint.digest = fingerprintDigest(p.fingerprint);
        });
        QVERIFY(!validateCacheResult(r));
        QVERIFY(!cacheDominates(r, queryFor(r)));
    }
    void unsupportedOptionsDoNotPreventEvidenceStorage() {
        auto result = sample();
        changeProvenance(result, [](auto& p) {
            p.fingerprint.executableSha256 = QByteArray(32, 'x');
            p.fingerprint.resources = ResourceVerification::ExternalResourcesUnverified;
            p.fingerprint.resourcePolicyVersion = 0;
            for (auto& option : p.fingerprint.options) {
                if (option.name == QStringLiteral("multipv")) {
                    option.value =
                        QStringLiteral("0"); // Valid unsupported spin declaration: min=0/max=1.
                }
                if (option.name == QStringLiteral("uci_showwdl")) {
                    option.type = OptionType::String;
                    option.value = QStringLiteral("unsupported");
                }
            }
            p.fingerprint.digest = fingerprintDigest(p.fingerprint);
        });
        result.evidence.wdlSupported = false;
        QVERIFY(!validateCacheResult(result));
        Fixture database;
        QVERIFY(std::holds_alternative<AnalysisStored>(database.session.storeAnalysis(result)));
        QVERIFY(!std::get<CacheLookup>(database.session.findAnalysis(queryFor(result))).hit);
    }
    void rootSetsAndHistory() {
        auto r = sample(2);
        const auto a = r.evidence.lines[0].pv[0];
        const auto b = r.evidence.lines[1].pv[0];
        changeProvenance(r, [&](auto& p) { p.searchMoves = std::vector{a, b}; });
        auto q = queryFor(r);
        std::reverse(q.searchMoves->begin(), q.searchMoves->end());
        QVERIFY(cacheDominates(r, q));
        q.searchMoves.reset();
        QVERIFY(!cacheDominates(r, q));
        q = queryFor(r);
        q.searchMoves->pop_back();
        QVERIFY(!cacheDominates(r, q));
        auto position = chess::Position::initial();
        for (int i = 0; i < 1000; ++i) {
            for (const auto* uci : {"g1f3", "g8f6", "f3g1", "f6g8"}) {
                QVERIFY(std::holds_alternative<std::monostate>(
                    position.play(std::get<chess::Move>(position.parseUci(uci)))));
            }
        }
        const auto context = position.context();
        QVERIFY(std::holds_alternative<chess::Position>(restorePosition(context)));
        const auto fen =
            std::get<chess::Position>(chess::Position::fromFen(position.fen())).context();
        QVERIFY(positionIdentity(context) != positionIdentity(fen));
    }
    void codec() {
        auto r = sample(2, true);
        r.evidence.statistics.nps = std::numeric_limits<std::uint64_t>::max();
        r.evidence.lines[0].score->value = std::numeric_limits<std::int64_t>::min();
        r.evidence.lines[0].score->type = ScoreType::Mate;
        const auto bytes = std::get<QByteArray>(encodeAnalysis(r));
        auto decoded = decodeAnalysis(bytes);
        QVERIFY(decoded);
        QVERIFY(cacheDominates(*decoded, queryFor(r)));
        QCOMPARE(std::get<QByteArray>(encodeAnalysis(*decoded)), bytes);
        for (qsizetype i = 0; i < bytes.size(); ++i) {
            QVERIFY(!decodeAnalysis(bytes.first(i)));
        }
        QVERIFY(!decodeAnalysis(bytes + 'x'));
        auto corrupt = bytes;
        corrupt[3] = 2;
        QVERIFY(!decodeAnalysis(corrupt));
        corrupt = bytes;
        for (int i = 4; i < 8; ++i) {
            corrupt[i] = static_cast<char>(0xff);
        }
        QVERIFY(!decodeAnalysis(corrupt));
        auto bad = r;
        bad.evidence.lines[0].wdl = Wdl{1001, 0, 0};
        QVERIFY(validateCacheResult(bad));
        bad = r;
        bad.evidence.lines[0].pv[0].promotion = chess::PieceType::King;
        QVERIFY(validateCacheResult(bad));
    }
    void repository() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("cache.sqlite3"));
        auto first = sample();
        {
            StorageSession s;
            QVERIFY(!s.initialize(path, migrations(), [](const QString&) {}));
            auto stored = std::get<AnalysisStored>(s.storeAnalysis(first));
            QVERIFY(!stored.duplicate);
            const auto again = std::get<AnalysisStored>(s.storeAnalysis(first));
            QVERIFY(again.duplicate);
            QCOMPARE(again.id, stored.id);
            auto conflict = first;
            conflict.evidence.lines[0].score->value++;
            QCOMPARE(std::get<CacheError>(s.storeAnalysis(conflict)).code,
                     CacheErrorCode::Conflict);
            QVERIFY(std::get<CacheLookup>(s.findAnalysis(queryFor(first))).hit);
            auto stronger = first;
            stronger.evidence.request = QUuid::createUuid();
            stronger.evidence.statistics.nodes = 2000;
            QVERIFY(std::holds_alternative<AnalysisStored>(s.storeAnalysis(stronger)));
            auto hit = std::get<CacheLookup>(s.findAnalysis(queryFor(first))).hit;
            QVERIFY(hit);
            QCOMPARE(hit->evidence.request, stronger.evidence.request);
            auto newest = stronger;
            newest.evidence.request = QUuid::createUuid();
            QVERIFY(std::holds_alternative<AnalysisStored>(s.storeAnalysis(newest)));
            hit = std::get<CacheLookup>(s.findAnalysis(queryFor(first))).hit;
            QCOMPARE(hit->evidence.request, newest.evidence.request);
        }
        StorageSession reopened;
        QVERIFY(!reopened.initialize(path, migrations(), [](const QString&) {}));
        QVERIFY(std::get<CacheLookup>(reopened.findAnalysis(queryFor(first))).hit);
    }
    void corruptFallbackAndRollback() {
        Fixture f;
        auto r = sample();
        QVERIFY(std::holds_alternative<AnalysisStored>(f.session.storeAnalysis(r)));
        auto second = r;
        second.evidence.request = QUuid::createUuid();
        QVERIFY(std::holds_alternative<AnalysisStored>(f.session.storeAnalysis(second)));
        QVERIFY(
            f.exec(QStringLiteral("UPDATE analysis_results SET raw_result_blob=x'00' WHERE id=2")));
        auto found = std::get<CacheLookup>(f.session.findAnalysis(queryFor(r)));
        QVERIFY(found.hit);
        QCOMPARE(found.discardedRecords, 1);
        QCOMPARE(found.hit->evidence.request, r.evidence.request);
        QVERIFY(f.exec(QStringLiteral("UPDATE analysis_results SET result_format_version=99")));
        found = std::get<CacheLookup>(f.session.findAnalysis(queryFor(r)));
        QVERIFY(!found.hit);
        QCOMPARE(found.discardedRecords, 1);
        QVERIFY(
            f.exec(QStringLiteral("CREATE TRIGGER reject_cache BEFORE INSERT ON analysis_results "
                                  "BEGIN SELECT RAISE(ABORT,'fixture'); END")));
        QVERIFY(std::holds_alternative<CacheError>(f.session.storeAnalysis(r)));
        QVERIFY(f.exec(QStringLiteral("DROP TRIGGER reject_cache")));
        QVERIFY(std::holds_alternative<AnalysisStored>(f.session.storeAnalysis(r)));
        QVERIFY(f.exec(QStringLiteral(
            "CREATE TRIGGER alter_cache AFTER INSERT ON analysis_results BEGIN UPDATE "
            "analysis_results SET result_format_version=99 WHERE id=new.id; END")));
        QVERIFY(std::holds_alternative<CacheError>(f.session.storeAnalysis(second)));
        found = std::get<CacheLookup>(f.session.findAnalysis(queryFor(r)));
        QVERIFY(found.hit);
        QCOMPARE(found.discardedRecords, 0);
    }
    void cacheTransactionsRollbackOnCommitAndCleanupFailure() {
        Fixture f;
        auto first = sample();
        QVERIFY(std::holds_alternative<AnalysisStored>(f.session.storeAnalysis(first)));
        QVERIFY(f.exec(QStringLiteral("CREATE TABLE cache_guard(parent INTEGER REFERENCES "
                                      "engines(id) DEFERRABLE INITIALLY DEFERRED)")));
        QVERIFY(
            f.exec(QStringLiteral("CREATE TRIGGER reject_commit AFTER INSERT ON analysis_results "
                                  "BEGIN INSERT INTO cache_guard VALUES(-1); END")));
        auto next = first;
        next.evidence.request = QUuid::createUuid();
        QVERIFY(std::holds_alternative<CacheError>(f.session.storeAnalysis(next)));
        QVERIFY(f.exec(QStringLiteral("DROP TRIGGER reject_commit")));
        const auto retry = std::get<AnalysisStored>(f.session.storeAnalysis(next));
        QVERIFY(!retry.duplicate);
        QVERIFY(f.exec(QStringLiteral("UPDATE analysis_results SET position_identity=x'00'")));
        QVERIFY(
            f.exec(QStringLiteral("CREATE TRIGGER reject_cleanup BEFORE DELETE ON analysis_results "
                                  "WHEN old.id=2 BEGIN SELECT RAISE(ABORT,'fixture'); END")));
        QVERIFY(std::holds_alternative<CacheError>(f.session.findAnalysis(queryFor(first))));
        QVERIFY(f.exec(QStringLiteral("DROP TRIGGER reject_cleanup")));
        const auto found = std::get<CacheLookup>(f.session.findAnalysis(queryFor(first)));
        QCOMPARE(found.discardedRecords, 2);
        QVERIFY(!found.hit);
    }
    void failedCacheMigrationPreservesOldSchema() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("migration.sqlite3"));
        {
            StorageSession old;
            QVERIFY(!old.initialize(path, std::span(migrations()).first(4), [](const QString&) {}));
        }
        auto broken = migrations();
        broken.back().statements.push_back(
            QStringLiteral("INSERT INTO nonexistent_cache_table VALUES(1)"));
        {
            StorageSession upgrade;
            const auto failure = upgrade.initialize(path, broken, [](const QString&) {});
            QVERIFY(failure);
            QCOMPARE(failure->code, DatabaseErrorCode::Migration);
            QCOMPARE(failure->migrationVersion, std::optional<int>(5));
            QVERIFY(QFileInfo::exists(failure->backupPath));
        }
        StorageSession original;
        QVERIFY(
            !original.initialize(path, std::span(migrations()).first(4), [](const QString&) {}));
    }
    void retention_data() {
        QTest::addColumn<bool>("byBytes");
        QTest::newRow("bytes") << true;
        QTest::newRow("records") << false;
    }
    void retention() {
        QFETCH(bool, byBytes);
        Fixture f;
        auto r = sample();
        const auto size = std::get<QByteArray>(encodeAnalysis(r)).size() +
                          positionIdentity(r.evidence.provenance->position).size();
        const qint64 budget = byBytes ? size * 2 : analysisBudgetBytes;
        const qint64 count = byBytes ? maxAnalysisRecords : 2;
        QVERIFY(std::holds_alternative<AnalysisStored>(f.session.storeAnalysis(r, budget, count)));
        auto second = r;
        second.evidence.request = QUuid::createUuid();
        QVERIFY(
            std::holds_alternative<AnalysisStored>(f.session.storeAnalysis(second, budget, count)));
        auto third = r;
        third.evidence.request = QUuid::createUuid();
        QVERIFY(
            std::holds_alternative<AnalysisStored>(f.session.storeAnalysis(third, budget, count)));
        const auto again = std::get<AnalysisStored>(f.session.storeAnalysis(r, budget, count));
        QVERIFY(!again.duplicate);
        QVERIFY(std::holds_alternative<CacheError>(f.session.storeAnalysis(r, size - 1, count)));
        // Failed eviction rolls back the new insert and leaves the surviving old rows.
        QVERIFY(
            f.exec(QStringLiteral("CREATE TRIGGER reject_eviction BEFORE DELETE ON "
                                  "analysis_results BEGIN SELECT RAISE(ABORT,'fixture'); END")));
        auto fourth = r;
        fourth.evidence.request = QUuid::createUuid();
        QVERIFY(std::holds_alternative<CacheError>(f.session.storeAnalysis(fourth, budget, count)));
        QVERIFY(f.exec(QStringLiteral("DROP TRIGGER reject_eviction")));
        QVERIFY(std::get<AnalysisStored>(f.session.storeAnalysis(r, budget, count)).duplicate);
        QVERIFY(
            !std::get<AnalysisStored>(f.session.storeAnalysis(fourth, budget, count)).duplicate);
    }
    void migration() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("upgrade.sqlite3"));
        {
            StorageSession old;
            QVERIFY(!old.initialize(path, std::span(migrations()).first(4), [](const QString&) {}));
        }
        StorageSession current;
        QVERIFY(!current.initialize(path, migrations(), [](const QString&) {}));
        QCOMPARE(QDir(dir.filePath(QStringLiteral("backups")))
                     .entryList({QStringLiteral("*.sqlite3")}, QDir::Files)
                     .size(),
                 1);
        QVERIFY(std::holds_alternative<AnalysisStored>(current.storeAnalysis(sample())));
    }
    void asynchronousDrainAndAdmission() {
        QTemporaryDir dir;
        AsyncDatabase owner;
        auto& db = owner.value;
        QSignalSpy ready(&db, &Database::ready), stored(&db, &Database::analysisStored),
            closed(&db, &Database::closed);
        db.initialize(dir.filePath(QStringLiteral("async.sqlite3")));
        QTRY_COMPARE(ready.size(), 1);
        QThread* delivery = nullptr;
        connect(&db, &Database::analysisStored, this, [&] { delivery = QThread::currentThread(); });
        const auto r = sample();
        for (std::uint64_t i = 1; i <= 17; ++i) {
            db.storeAnalysis(QUuid::createUuid(), r);
        }
        db.close();
        db.storeAnalysis(QUuid::createUuid(), r);
        QTRY_COMPARE(closed.size(), 1);
        QTRY_COMPARE(stored.size(), 18);
        int busy = 0;
        int closing = 0;
        int success = 0;
        for (const auto& event : stored) {
            const auto value = qvariant_cast<StoreAnalysisResult>(event[1]);
            if (const auto* error = std::get_if<CacheError>(&value)) {
                busy += error->code == CacheErrorCode::Busy;
                closing += error->code == CacheErrorCode::Closing;
            } else {
                ++success;
            }
        }
        QCOMPARE(success, 16);
        QCOMPARE(busy, 1);
        QCOMPARE(closing, 1);
        QCOMPARE(delivery, QThread::currentThread());
    }
    void pinnedEngineRoundTrip() {
        const auto path = qEnvironmentVariable("CHESS_REVIEW_STOCKFISH");
        if (path.isEmpty()) {
            QSKIP("Set CHESS_REVIEW_STOCKFISH for the optional pinned-engine probe.");
        }
        engine_test::Validator fixture;
        QSignalSpy validation(&fixture.service, &EngineSupervisor::completed);
        fixture.service.validate(QUuid::createUuid(), path);
        QTRY_COMPARE_WITH_TIMEOUT(validation.size(), 1, 15000);
        const auto validated = qvariant_cast<ValidationResult>(validation[0][1]);
        QVERIFY(std::holds_alternative<ValidatedEngine>(validated));
        EngineProfile profile;
        profile.id = 1;
        profile.engine = std::get<ValidatedEngine>(validated);
        const auto session = QUuid::createUuid();
        QSignalSpy runtime(&fixture.service, &EngineSupervisor::runtimeChanged);
        fixture.service.connectEngine(session, profile);
        QTRY_VERIFY_WITH_TIMEOUT(!runtime.empty() &&
                                     qvariant_cast<RuntimeState>(runtime.back()[0]).phase ==
                                         RuntimePhase::Ready,
                                 15000);
        AnalysisRequest request;
        request.id = QUuid::createUuid();
        request.limit = DepthLimit{4};
        QSignalSpy done(&fixture.service, &EngineSupervisor::analysisFinished);
        fixture.service.analyze(session, request);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
        const auto result = qvariant_cast<AnalysisResult>(done[0][0]);
        QCOMPARE(result.outcome, AnalysisOutcome::Completed);
        QVERIFY(result.evidence.provenance);
        QVERIFY(verifiedResources(result.evidence.provenance->fingerprint));
        QVERIFY(!validateCacheResult(result));
        Fixture database;
        QVERIFY(std::holds_alternative<AnalysisStored>(database.session.storeAnalysis(result)));
        const auto found = std::get<CacheLookup>(database.session.findAnalysis(queryFor(result)));
        QVERIFY(found.hit);
        QCOMPARE(found.hit->evidence.request, request.id);
    }
    void cacheWaitKeepsOwnerResponsive() {
        QTemporaryDir dir;
        AsyncDatabase owner;
        auto& db = owner.value;
        const auto path = dir.filePath(QStringLiteral("responsive.sqlite3"));
        QSignalSpy ready(&db, &Database::ready), saved(&db, &Database::analysisStored);
        db.initialize(path);
        QTRY_COMPARE(ready.size(), 1);
        const auto connectionName = QStringLiteral("cache-test-lock");
        {
            auto connection = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            connection.setDatabaseName(path);
            QVERIFY(connection.open());
            QSqlQuery lock(connection);
            QVERIFY(lock.exec(QStringLiteral("BEGIN IMMEDIATE")));
            int beats = 0;
            QTimer heartbeat;
            connect(&heartbeat, &QTimer::timeout, this, [&] { ++beats; });
            heartbeat.start(5);
            QTimer::singleShot(100, &db, [&] {
                if (!connection.rollback()) {
                    qFatal("Cannot release fixture lock");
                }
            });
            const auto id = QUuid::createUuid();
            db.storeAnalysis(id, sample());
            QTRY_COMPARE(saved.size(), 1);
            QVERIFY(beats >= 2);
            QCOMPARE(saved[0][0].toUuid(), id);
            QVERIFY(std::holds_alternative<AnalysisStored>(
                qvariant_cast<StoreAnalysisResult>(saved[0][1])));
            connection.close();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }
#ifdef FAKE_UCI_ENGINE
    void fakeEngineRoundTrip() {
        QTemporaryDir dir;
        engine_test::Validator fixture;
        const auto registration = engine_test::scenario(
            dir.filePath(QStringLiteral("register.json")), engine_test::handshake());
        ValidatorTestAccess::configure(fixture.service, registration, 1000);
        QSignalSpy completed(&fixture.service, &EngineSupervisor::completed);
        fixture.service.validate(QUuid::createUuid(), QString::fromUtf8(FAKE_UCI_ENGINE));
        QTRY_COMPARE(completed.size(), 1);
        EngineProfile profile;
        profile.id = 1;
        profile.engine =
            std::get<ValidatedEngine>(qvariant_cast<ValidationResult>(completed[0][1]));
        const auto ready = engine_test::step(QStringLiteral("isready"),
                                             {engine_test::write(QStringLiteral("readyok\n"))});
        QJsonArray steps{
            engine_test::step(QStringLiteral("uci"),
                              {engine_test::write(QStringLiteral(
                                  "id name Test engine\nid author Tests\nuciok\n"))}),
            ready,
            engine_test::step(QStringLiteral("ucinewgame"), {}),
            ready,
            engine_test::step(QStringLiteral("position startpos"), {}),
            engine_test::step(
                QStringLiteral("go nodes 1000"),
                {engine_test::write(QStringLiteral(
                    "info depth 8 score cp 20 nodes 1100 pv e2e4\nbestmove e2e4\n"))}),
            ready,
            engine_test::step(QStringLiteral("quit"),
                              {QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}})};
        ValidatorTestAccess::configure(
            fixture.service,
            engine_test::scenario(dir.filePath(QStringLiteral("search.json")), steps), 1000);
        QSignalSpy runtime(&fixture.service, &EngineSupervisor::runtimeChanged);
        const auto session = QUuid::createUuid();
        fixture.service.connectEngine(session, profile);
        QTRY_VERIFY(!runtime.empty() &&
                    qvariant_cast<RuntimeState>(runtime.back()[0]).phase == RuntimePhase::Ready);
        QSignalSpy done(&fixture.service, &EngineSupervisor::analysisFinished);
        AnalysisRequest request;
        request.id = QUuid::createUuid();
        request.limit = NodeLimit{1000};
        fixture.service.analyze(session, request);
        QTRY_COMPARE(done.size(), 1);
        const auto result = qvariant_cast<AnalysisResult>(done[0][0]);
        QCOMPARE(result.outcome, AnalysisOutcome::Completed);
        QVERIFY(!validateCacheResult(result));
        AsyncDatabase database;
        QSignalSpy dbReady(&database.value, &Database::ready),
            saved(&database.value, &Database::analysisStored),
            found(&database.value, &Database::analysisFound);
        database.value.initialize(dir.filePath(QStringLiteral("evidence.sqlite3")));
        QTRY_COMPARE(dbReady.size(), 1);
        database.value.storeAnalysis(QUuid::createUuid(), result);
        QTRY_COMPARE(saved.size(), 1);
        QVERIFY(std::holds_alternative<AnalysisStored>(
            qvariant_cast<StoreAnalysisResult>(saved[0][1])));
        database.value.findAnalysis(QUuid::createUuid(), queryFor(result));
        QTRY_COMPARE(found.size(), 1);
        QVERIFY(!std::get<CacheLookup>(qvariant_cast<FindAnalysisResult>(found[0][1]))
                     .hit); // Fake binary is correctly unverified.
    }
#endif
};
int main(int argc, char** argv) {
    TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AnalysisRepositoryTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "AnalysisRepositoryTest.moc"
