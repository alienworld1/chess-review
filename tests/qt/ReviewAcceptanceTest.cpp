#include "ReviewAcceptanceSupport.h"
#include "TestEnvironment.h"
#include "engine/AnalysisCache.h"
#include "persistence/Database.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <cstddef>
#include <memory>
#include <variant>
#include <vector>

namespace {
QString fixture(const char* name) {
    return QDir(QString::fromUtf8(REVIEW_ACCEPTANCE_FIXTURES)).filePath(QString::fromLatin1(name));
}
struct Result {
    bool normal = false;
    QByteArray output;
    QByteArray errors;
    std::vector<QJsonObject> events;
    QJsonObject event(const QString& name) const {
        for (const auto& value : events) {
            if (value.value(QStringLiteral("event")).toString() == name) {
                return value;
            }
        }
        return {};
    }
};
Result launch(const QTemporaryDir& root, const QString& mode, const QString& pgn,
              const QString& engine = {}) {
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("HOME"), root.path());
    environment.insert(QStringLiteral("XDG_DATA_HOME"), root.filePath(QStringLiteral("data")));
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), root.filePath(QStringLiteral("config")));
    environment.insert(QStringLiteral("XDG_CACHE_HOME"), root.filePath(QStringLiteral("cache")));
    environment.insert(QStringLiteral("CHESS_REVIEW_ACCEPTANCE_MODE"), mode);
    environment.insert(QStringLiteral("CHESS_REVIEW_ACCEPTANCE_PGN"), pgn);
    environment.insert(QStringLiteral("CHESS_REVIEW_ACCEPTANCE_SCRIPT"),
                       root.filePath(QStringLiteral("scenario.json")));
    environment.insert(QStringLiteral("CHESS_REVIEW_STOCKFISH"), engine);
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"),
                       QStringLiteral("offscreen:configfile=") +
                           QString::fromUtf8(REVIEW_ACCEPTANCE_SCREEN));
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(root.path());
    const auto cleanup = qScopeGuard([&] {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            if (!process.waitForFinished(5000)) {
                qFatal("Acceptance child could not be reaped");
            }
        }
    });
    process.start(QString::fromUtf8(REVIEW_ACCEPTANCE_PROBE), {});
    Result result;
    if (!process.waitForStarted(5000)) {
        result.errors = process.errorString().toUtf8();
        return result;
    }
    if (mode == QStringLiteral("interrupt")) {
        QElapsedTimer deadline;
        deadline.start();
        while (!result.output.contains("\"event\":\"checkpoint\"") && deadline.elapsed() < 30000 &&
               process.state() != QProcess::NotRunning) {
            process.waitForReadyRead(1000);
            result.output += process.readAllStandardOutput();
        }
        if (result.output.contains("\"event\":\"checkpoint\"")) {
            process.kill();
            result.normal =
                process.waitForFinished(5000) && process.exitStatus() == QProcess::CrashExit;
        }
    } else {
        result.normal = process.waitForFinished(engine.isEmpty() ? 120000 : 360000) &&
                        process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    }
    result.output += process.readAllStandardOutput();
    result.errors += process.readAllStandardError();
    for (const auto& line : result.output.split('\n')) {
        const auto document = QJsonDocument::fromJson(line);
        if (document.isObject()) {
            result.events.push_back(document.object());
        }
    }
    return result;
}
QByteArray logs(const QTemporaryDir& root) {
    QFile file(root.filePath(QStringLiteral("cache/chess-review/logs/events-0.jsonl")));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}
} // namespace

class ReviewAcceptanceTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { QTest::failOnWarning(); }
    void fixtures_data() {
        QTest::addColumn<QString>("path");
        QTest::addColumn<int>("plies");
        QTest::newRow("tactical") << fixture("tactical.pgn") << 7;
        QTest::newRow("complete") << fixture("complete.pgn") << 60;
        QTest::newRow("terminal") << fixture("terminal.pgn") << 0;
        QTest::newRow("stress") << fixture("stress.pgn") << 512;
    }
    void fixtures() {
        QFETCH(QString, path);
        QFETCH(int, plies);
        const auto game = review_acceptance::readGame(path);
        QCOMPARE(game->originalMainline().size(), static_cast<std::size_t>(plies));
        auto position =
            std::get<chess_review::chess::Position>(game->tree.position(game->tree.rootId()));
        for (const auto move : game->originalMainline()) {
            QVERIFY(std::holds_alternative<std::monostate>(position.play(move)));
        }
        if (plies == 0) {
            QVERIFY(position.legalMoves().empty());
        }
    }

  private:
    void verifyEvidence(const QTemporaryDir& root, const QJsonObject& completed,
                        const QString& path) {
        using namespace chess_review;
        persistence::Database database;
        const auto cleanup = qScopeGuard([&] {
            QEventLoop loop;
            QTimer deadline;
            deadline.setSingleShot(true);
            bool closed = false;
            connect(&database, &persistence::Database::closed, &loop, [&] {
                closed = true;
                loop.quit();
            });
            connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
            database.close();
            deadline.start(10000);
            if (!closed) {
                loop.exec();
            }
            if (!closed) {
                qFatal("Acceptance evidence reader did not drain");
            }
        });
        QSignalSpy ready(&database, &persistence::Database::ready);
        database.initialize(
            root.filePath(QStringLiteral("data/chess-review/chess-review.sqlite3")));
        QVERIFY(ready.wait(10000));
        const auto id = completed.value(QStringLiteral("run")).toInteger();
        QSignalSpy loaded(&database, &persistence::Database::reviewLoaded);
        database.loadReview(QUuid::createUuid(), id);
        QVERIFY(loaded.wait());
        const auto record = qvariant_cast<persistence::LoadReviewResult>(loaded.front()[1]);
        QVERIFY(std::holds_alternative<persistence::StoredReview>(record));
        const auto saved = std::get<persistence::StoredReview>(record);
        QVERIFY(!engine::verifiedResources(saved.settings.fingerprint));
        QCOMPARE(saved.summary.selectedSide, chess::Color::Black);
        QCOMPARE(saved.summary.quality, review::Quality::Standard);
        const auto document = review_acceptance::readGame(path);
        auto position = std::get<chess::Position>(document->tree.position(document->tree.rootId()));
        const auto moves = document->originalMainline();
        QCOMPARE(saved.summary.totalPositions, static_cast<int>(moves.size()) + 1);
        QSignalSpy evidence(&database, &persistence::Database::reviewEvidenceLoaded);
        QSignalSpy rows(&database, &persistence::Database::reviewMovesLoaded);
        for (int index = 0; index < saved.summary.totalPositions; ++index) {
            evidence.clear();
            database.loadReviewEvidence(QUuid::createUuid(), id, index);
            QVERIFY(evidence.wait());
            const auto result =
                qvariant_cast<persistence::LoadReviewEvidenceResult>(evidence.front()[1]);
            QVERIFY(std::holds_alternative<persistence::StoredReviewEvidence>(result));
            const auto& stored = std::get<persistence::StoredReviewEvidence>(result);
            QCOMPARE(stored.evidence.position, position.context());
            QVERIFY(stored.evidenceId > 0);
            if (!position.legalMoves().empty()) {
                QVERIFY(stored.analysis);
                QVERIFY(!stored.evidence.attemptIdentity.empty());
                QVERIFY(!stored.evidence.engineIdentity.empty());
            } else {
                QVERIFY(stored.evidence.terminal);
                QVERIFY(!stored.analysis);
            }
            if (index < static_cast<int>(moves.size())) {
                rows.clear();
                database.deriveReviewMoves(QUuid::createUuid(), id, saved.summary.revision, index,
                                           1);
                QVERIFY(rows.wait());
                const auto selected =
                    qvariant_cast<persistence::LoadReviewMovesResult>(rows.front()[1]);
                QVERIFY(std::holds_alternative<persistence::ReviewMovePage>(selected));
                const auto& page = std::get<persistence::ReviewMovePage>(selected);
                QCOMPARE(page.moves.size(), std::size_t{1});
                const auto& row = page.moves.front();
                QVERIFY(row.classification);
                evidence.clear();
                database.loadReviewEvidence(QUuid::createUuid(), id, index + 1, row.stage);
                QVERIFY(evidence.wait());
                const auto after =
                    qvariant_cast<persistence::LoadReviewEvidenceResult>(evidence.front()[1]);
                QVERIFY(std::holds_alternative<persistence::StoredReviewEvidence>(after));
                const auto& selectedEvidence = std::get<persistence::StoredReviewEvidence>(after);
                const auto afterValue = selectedEvidence.evidence;
                evidence.clear();
                database.loadReviewEvidence(QUuid::createUuid(), id, index, row.stage);
                QVERIFY(evidence.wait());
                const auto before =
                    qvariant_cast<persistence::LoadReviewEvidenceResult>(evidence.front()[1]);
                QVERIFY(std::holds_alternative<persistence::StoredReviewEvidence>(before));
                const auto& beforeValue = std::get<persistence::StoredReviewEvidence>(before);
                const auto pair = review::pairEvidence(
                    position, moves[static_cast<std::size_t>(index)], beforeValue.evidence,
                    afterValue, saved.settings.configuration.values.conversion);
                QCOMPARE(pair.before.position, position.context());
                QCOMPARE(row.rawBefore, pair.before.evaluation);
                QCOMPARE(row.rawAfter, pair.after.evaluation);
                QCOMPARE(row.afterEvidenceId, selectedEvidence.evidenceId);
                const auto derived = review::classify(
                    position, pair, saved.settings.configuration.values.classification,
                    saved.settings.configuration.values.conversion,
                    row.classification->modelVersion);
                QCOMPARE(row.classification->label, derived.label);
                QCOMPARE(row.classification->ruleId, derived.ruleId);
                QCOMPARE(row.classification->loss, derived.loss);
                QCOMPARE(row.classification->thresholds,
                         saved.settings.configuration.values.classification);
                QVERIFY(row.beforeEvidenceId > 0 && row.afterEvidenceId > 0);
                QVERIFY(std::holds_alternative<std::monostate>(
                    position.play(moves[static_cast<std::size_t>(index)])));
            }
        }
    }
  private slots:
    void importReviewExploreRestart() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto first = launch(root, QStringLiteral("journey"), fixture("tactical.pgn"));
        QVERIFY2(first.normal, (first.output + first.errors).constData());
        verifyEvidence(root, first.event(QStringLiteral("reviewed")), fixture("tactical.pgn"));
        const auto explored = first.event(QStringLiteral("explored"));
        QVERIFY(!explored.isEmpty());
        QVERIFY(!first.event(QStringLiteral("quit")).isEmpty());
        // User-selected executables are untouched. Reopening is checked after making this
        // isolated profile's scenario unavailable; any accidental fake-engine launch fails.
        QVERIFY(QFile::remove(root.filePath(QStringLiteral("scenario.json"))));
        const auto second = launch(root, QStringLiteral("reopen"), fixture("tactical.pgn"));
        QVERIFY2(second.normal, (second.output + second.errors).constData());
        const auto restored = second.event(QStringLiteral("restored"));
        for (const auto* key : {"game", "run", "moment", "side"}) {
            QCOMPARE(restored.value(QString::fromLatin1(key)),
                     explored.value(QString::fromLatin1(key)));
        }
        const auto records = logs(root);
        QVERIFY(records.contains("session.stopped"));
        QVERIFY(!records.contains("engine.process.started"));
    }
    void completeGames_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("normal-length") << fixture("complete.pgn");
        QTest::newRow("terminal-fen") << fixture("terminal.pgn");
    }
    void completeGames() {
        QFETCH(QString, path);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto result = launch(root, QStringLiteral("complete"), path);
        QVERIFY2(result.normal, (result.output + result.errors).constData());
        const auto reviewed = result.event(QStringLiteral("reviewed"));
        QVERIFY(!reviewed.isEmpty());
        verifyEvidence(root, reviewed, path);
        if (review_acceptance::readGame(path)->originalMainline().empty()) {
            QCOMPARE(reviewed.value(QStringLiteral("searches")).toInt(), 0);
        }
        QCOMPARE(reviewed.value(QStringLiteral("completed")),
                 reviewed.value(QStringLiteral("total")));
        QVERIFY(!result.event(QStringLiteral("quit")).isEmpty());
    }
    void interruptedRequiresExplicitResume() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto interrupted = launch(root, QStringLiteral("interrupt"), fixture("tactical.pgn"));
        QVERIFY2(interrupted.normal, (interrupted.output + interrupted.errors).constData());
        const auto checkpoint = interrupted.event(QStringLiteral("checkpoint"));
        QVERIFY(!checkpoint.isEmpty());
        const auto recovered = launch(root, QStringLiteral("recover"), fixture("tactical.pgn"));
        QVERIFY2(recovered.normal, (recovered.output + recovered.errors).constData());
        const auto restored = recovered.event(QStringLiteral("restored"));
        QCOMPARE(restored.value(QStringLiteral("run")), checkpoint.value(QStringLiteral("run")));
        QVERIFY(restored.value(QStringLiteral("completed")).toInt() >=
                checkpoint.value(QStringLiteral("completed")).toInt());
        QVERIFY(!logs(root).contains("engine.process.started"));
        const auto resumed = launch(root, QStringLiteral("resume"), fixture("tactical.pgn"));
        QVERIFY2(resumed.normal, (resumed.output + resumed.errors).constData());
        QCOMPARE(resumed.event(QStringLiteral("resumed")).value(QStringLiteral("run")),
                 checkpoint.value(QStringLiteral("run")));
        QCOMPARE(
            resumed.event(QStringLiteral("resumed")).value(QStringLiteral("completed")).toInt(), 8);
    }
    void failures_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("cancel") << QStringLiteral("cancel");
        QTest::newRow("engine-crash") << QStringLiteral("crash");
        QTest::newRow("responsive-512-ply") << QStringLiteral("stress");
    }
    void failures() {
        QFETCH(QString, mode);
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto result = launch(
            root, mode, fixture(mode == QStringLiteral("stress") ? "stress.pgn" : "tactical.pgn"));
        QVERIFY2(result.normal, (result.output + result.errors).constData());
        const auto reviewed = result.event(QStringLiteral("reviewed"));
        QVERIFY(!reviewed.isEmpty());
        qInfo().noquote() << QJsonDocument(reviewed).toJson(QJsonDocument::Compact);
        QVERIFY(!result.event(QStringLiteral("quit")).isEmpty());
    }
    void renderJourney() {
        if (qEnvironmentVariableIsEmpty("CHESS_REVIEW_ACCEPTANCE_RENDER_DIR")) {
            QSKIP("Set CHESS_REVIEW_ACCEPTANCE_RENDER_DIR to an existing output directory.");
        }
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto result = launch(root, QStringLiteral("journey"), fixture("tactical.pgn"));
        QVERIFY2(result.normal, (result.output + result.errors).constData());
        QVERIFY(!result.event(QStringLiteral("explored")).isEmpty());
    }
    void realEngineJourney() {
        const auto engine = qEnvironmentVariable("CHESS_REVIEW_STOCKFISH");
        if (engine.isEmpty()) {
            QSKIP("Select a local engine with CHESS_REVIEW_STOCKFISH for real-engine acceptance.");
        }
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto result = launch(root, QStringLiteral("real"), fixture("tactical.pgn"), engine);
        QVERIFY2(result.normal, (result.output + result.errors).constData());
        QVERIFY(!result.event(QStringLiteral("reviewed")).isEmpty());
        const auto reopened = launch(root, QStringLiteral("reopen"), fixture("tactical.pgn"));
        QVERIFY2(reopened.normal, (reopened.output + reopened.errors).constData());
        QVERIFY(!logs(root).contains("engine.process.started"));
        const auto explored = result.event(QStringLiteral("explored"));
        const auto restored = reopened.event(QStringLiteral("restored"));
        QVERIFY(!explored.isEmpty());
        for (const auto* key : {"game", "run", "moment", "side"}) {
            QCOMPARE(restored.value(QString::fromLatin1(key)),
                     explored.value(QString::fromLatin1(key)));
        }
    }
};

int main(int argc, char** argv) {
    TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QCoreApplication application(argc, argv);
    ReviewAcceptanceTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ReviewAcceptanceTest.moc"
