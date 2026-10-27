#include "EngineControllerTestAccess.h"
#include "EngineTestSupport.h"
#include "TestEnvironment.h"
#include "app/StartupCoordinator.h"
#include "engine/EffectiveSearch.h"
#include "engine/private/SearchProtocol.h"
#include "ui/shell/MainWindow.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
using namespace chess_review;
namespace {
QJsonObject ready() {
    return engine_test::step(QStringLiteral("isready"),
                             {engine_test::write(QStringLiteral("readyok\n"))});
}
QJsonObject quit() {
    return engine_test::step(QStringLiteral("quit"),
                             {QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}});
}
class Fixture {
  public:
    explicit Fixture(const QString& path = {},
                     const std::optional<QString>& configuration = std::nullopt)
        : window(navigation, startup.analysis()) {
        window.installEngines(startup.engines());
        window.installEngineAnalysis(startup.engineAnalysis());
        window.installImports(startup.localImports(), startup.library());
        window.installReview(startup.reviewGame());
        QObject::connect(&startup, &app::StartupCoordinator::ready, &window,
                         &ui::MainWindow::showReady);
        startup.start(path.isEmpty() ? directory.filePath(QStringLiteral("review.sqlite3")) : path,
                      std::nullopt, configuration);
    }
    ~Fixture() {
        shutdown();
        window.completeShutdown();
    }
    void shutdown() {
        if (startup.state() == app::Lifecycle::Stopped) {
            return;
        }
        startup.shutdown();
        if (!QTest::qWaitFor([this] { return startup.state() == app::Lifecycle::Stopped; },
                             10000)) {
            qFatal("Quick review did not close");
        }
    }
    bool seed(const std::string& text = "1. e4 e5 *") {
        if (!QTest::qWaitFor([this] {
                return startup.state() == app::Lifecycle::Ready &&
                       startup.engines().state().ready && !startup.engines().state().listing;
            })) {
            return false;
        }
        pgn::PgnReader reader(text);
        auto parsed = reader.next();
        if (!std::holds_alternative<pgn::PgnDocument>(parsed)) {
            return false;
        }
        document =
            std::make_shared<const pgn::PgnDocument>(std::get<pgn::PgnDocument>(std::move(parsed)));
        QSignalSpy inserted(&startup.gameRepository(), &persistence::Database::gameInserted);
        startup.gameRepository().insertGame(QUuid::createUuid(), document);
        if (!inserted.wait()) {
            return false;
        }
        const auto result = qvariant_cast<persistence::InsertGameResult>(inserted.front()[1]);
        const auto* game = std::get_if<persistence::Inserted>(&result);
        if (!game) {
            return false;
        }
        gameId = game->id;
        auto& supervisor = this->supervisor();
        engine::ValidatorTestAccess::configure(
            supervisor,
            engine_test::scenario(directory.filePath(QStringLiteral("validate.json")),
                                  engine_test::handshake()),
            1000);
        QSignalSpy validated(&supervisor, &engine::EngineSupervisor::completed);
        supervisor.validate(QUuid::createUuid(), QString::fromUtf8(FAKE_UCI_ENGINE));
        if (!validated.wait(5000)) {
            return false;
        }
        const auto engineResult = qvariant_cast<engine::ValidationResult>(validated.front()[1]);
        const auto* value = std::get_if<engine::ValidatedEngine>(&engineResult);
        if (!value) {
            return false;
        }
        QSignalSpy registered(&startup.gameRepository(), &persistence::Database::engineRegistered);
        startup.gameRepository().registerEngine(QUuid::createUuid(), *value);
        if (!registered.wait()) {
            return false;
        }
        startup.engines().refresh();
        if (!QTest::qWaitFor([this] {
                return !startup.engines().state().listing &&
                       !startup.engines().state().profiles.empty();
            })) {
            return false;
        }
        profileId = startup.engines().state().profiles.front().id;
        startup.reviewGame().selectGame(gameId);
        return QTest::qWaitFor([this] { return !startup.reviewGame().state().busy; });
    }
    engine::EngineSupervisor& supervisor() {
        return app::EngineControllerTestAccess::validator(startup.engines());
    }
    void script(int first = 0, int end = -1, int delay = 0, bool weak = false, bool crash = false,
                const QString& score = QStringLiteral("cp 25"),
                const QString& identity = QStringLiteral("Test engine")) {
        QJsonArray steps{
            engine_test::step(
                QStringLiteral("uci"),
                {engine_test::write(
                    QStringLiteral("id name %1\nid author Tests\nuciok\n").arg(identity))}),
            ready()};
        auto position = std::get<chess::Position>(document->tree.position(document->tree.rootId()));
        const auto moves = document->originalMainline();
        const int total = static_cast<int>(moves.size()) + 1;
        for (int index = 0; index < total; ++index) {
            if (index >= first && (end < 0 || index < end) && !position.legalMoves().empty()) {
                engine::AnalysisRequest request;
                request.id = QUuid::createUuid();
                request.position = position;
                request.wantWdl = true;
                request.limit = engine::NodeLimit{100000};
                const auto prepared =
                    std::get<engine::PreparedSearch>(engine::prepareSearch(request, {}));
                const auto best =
                    std::get<std::string>(position.uci(position.legalMoves().front()));
                steps.append(engine_test::step(QStringLiteral("ucinewgame"), {}));
                steps.append(ready());
                steps.append(engine_test::step(QString::fromUtf8(prepared.position).trimmed(), {}));
                if (crash) {
                    steps.append(engine_test::step(
                        QStringLiteral("go nodes 100000"),
                        {QJsonObject{{QStringLiteral("type"), QStringLiteral("crash")}}}));
                } else {
                    const auto output =
                        score.isEmpty()
                            ? QStringLiteral("bestmove %1\n").arg(QString::fromStdString(best))
                            : QStringLiteral(
                                  "info depth 7 nodes %1 score %4%2 pv %3\nbestmove %3\n")
                                  .arg(weak ? 1 : 100000)
                                  .arg(weak ? QStringLiteral(" lowerbound") : QString{})
                                  .arg(QString::fromStdString(best))
                                  .arg(score);
                    steps.append(engine_test::step(QStringLiteral("go nodes 100000"),
                                                   {engine_test::write(output, delay)}));
                    steps.append(ready());
                }
            }
            if (index < static_cast<int>(moves.size())) {
                (void)position.play(moves[static_cast<std::size_t>(index)]);
            }
        }
        steps.append(quit());
        engine::ValidatorTestAccess::configure(
            supervisor(),
            engine_test::scenario(directory.filePath(QStringLiteral("runtime.json")), steps), 1000);
        engine::ValidatorTestAccess::configureSearch(supervisor(), 10000, 1500);
    }
    void run(review::Quality quality = review::Quality::Standard) {
        startup.reviewGame().start(chess::Color::Black, profileId, quality);
    }
    bool done() {
        return QTest::qWaitFor(
            [this] {
                return !startup.reviewGame().state().active && !startup.reviewGame().state().busy;
            },
            12000);
    }
    bool sql(const QString& statement) {
        const auto connection = QUuid::createUuid().toString();
        bool result = false;
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(directory.filePath(QStringLiteral("review.sqlite3")));
            if (db.open()) {
                QSqlQuery query(db);
                result = query.exec(statement);
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        return result;
    }
    QTemporaryDir directory;
    app::StartupCoordinator startup;
    app::NavigationController navigation;
    ui::MainWindow window;
    std::shared_ptr<const pgn::PgnDocument> document;
    persistence::GameId gameId = 0;
    std::int64_t profileId = 0;
};
} // namespace
class QuickReviewTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        QTest::failOnWarning(QRegularExpression(
            QStringLiteral("^(?!This plugin does not support propagateSizeHints\\(\\)$).*")));
    }
    void wholeGame_data() {
        QTest::addColumn<QString>("pgn");
        QTest::addColumn<int>("positions");
        QTest::addColumn<bool>("weak");
        QTest::newRow("mainline-final") << QStringLiteral("1. e4 e5 *") << 3 << false;
        QTest::newRow("weak-bound-no-wdl") << QStringLiteral("1. e4 *") << 2 << true;
        QTest::newRow("zero-move") << QStringLiteral("*") << 1 << false;
        QTest::newRow("fen-terminal")
            << QStringLiteral("[SetUp \"1\"]\n[FEN \"7k/6Q1/6K1/8/8/8/8/8 b - - 0 1\"]\n*\n") << 1
            << false;
        QTest::newRow("fen-stalemate")
            << QStringLiteral("[SetUp \"1\"]\n[FEN \"7k/5Q2/6K1/8/8/8/8/8 b - - 0 1\"]\n*\n") << 1
            << false;
        QTest::newRow("fen-nonterminal")
            << QStringLiteral("[SetUp \"1\"]\n[FEN \"7k/8/6K1/8/8/8/8/8 w - - 0 1\"]\n*") << 1
            << false;
    }
    void wholeGame() {
        QFETCH(QString, pgn);
        QFETCH(int, positions);
        QFETCH(bool, weak);
        Fixture f;
        QVERIFY(f.seed(pgn.toStdString()));
        f.script(0, -1, 0, weak);
        f.run();
        QVERIFY(f.done());
        const auto& state = f.startup.reviewGame().state();
        QVERIFY2(state.error.isEmpty(), qPrintable(state.error));
        QVERIFY(state.run);
        QCOMPARE(state.run->status, review::JobStatus::Complete);
        QCOMPARE(state.run->totalPositions, positions);
        QCOMPARE(state.run->completedPositions, positions);
        QCOMPARE(state.run->selectedSide, chess::Color::Black);
        for (int index = 0; index < positions; ++index) {
            QSignalSpy loaded(&f.startup.gameRepository(),
                              &persistence::Database::reviewEvidenceLoaded);
            f.startup.gameRepository().loadReviewEvidence(QUuid::createUuid(), state.run->id,
                                                          index);
            QVERIFY(loaded.wait());
            const auto result =
                qvariant_cast<persistence::LoadReviewEvidenceResult>(loaded.front()[1]);
            QVERIFY(std::holds_alternative<persistence::StoredReviewEvidence>(result));
        }
        // Unverified resource identities cannot authorize cache reuse.
        QSignalSpy stored(&f.startup.gameRepository(), &persistence::Database::reviewLoaded);
        f.startup.gameRepository().loadReview(QUuid::createUuid(), state.run->id);
        QVERIFY(stored.wait());
        auto run = std::get<persistence::StoredReview>(
            qvariant_cast<persistence::LoadReviewResult>(stored.front()[1]));
        QVERIFY(!engine::verifiedResources(run.settings.fingerprint));
        QCOMPARE(run.settings.configuration.values.quality, review::Quality::Standard);
    }
    void cancelResumeAndRerun() {
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, 1);
        const auto connection =
            connect(&f.startup.reviewGame(), &app::ReviewGameController::changed,
                    &f.startup.reviewGame(), [&] {
                        const auto& state = f.startup.reviewGame().state();
                        if (state.active && state.run && state.run->completedPositions == 1) {
                            f.startup.reviewGame().cancel();
                        }
                    });
        f.run();
        QVERIFY(f.done());
        disconnect(connection);
        auto first = *f.startup.reviewGame().state().run;
        QCOMPARE(first.status, review::JobStatus::Cancelled);
        QCOMPARE(first.completedPositions, 1);
        QVERIFY(f.sql(QStringLiteral("DELETE FROM analysis_results")));
        f.script(1);
        f.startup.reviewGame().resume(first.id);
        QVERIFY(f.done());
        const auto resumed = *f.startup.reviewGame().state().run;
        QVERIFY2(f.startup.reviewGame().state().error.isEmpty(),
                 qPrintable(f.startup.reviewGame().state().error));
        QCOMPARE(resumed.id, first.id);
        QCOMPARE(resumed.status, review::JobStatus::Complete);
        QCOMPARE(resumed.completedPositions, 3);
        // Finish/reconnect before a new independent run; fake-engine cache safely misses.
        f.startup.engines().disconnectEngine();
        QTRY_COMPARE(f.startup.engines().state().runtime.phase, engine::RuntimePhase::Disconnected);
        f.script();
        f.run();
        QVERIFY(f.done());
        QVERIFY(f.startup.reviewGame().state().run->id != first.id);
    }
    void cancelledPreparationAndSecondRunRejected() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        f.startup.reviewGame().start(chess::Color::White, f.profileId, review::Quality::Fast);
        f.startup.reviewGame().selectGame(99999);
        QCOMPARE(f.startup.reviewGame().state().gameId, f.gameId);
        f.startup.reviewGame().cancel();
        QVERIFY(f.done());
        if (f.startup.reviewGame().state().run) {
            QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Cancelled);
        }
    }
    void engineCrashAndStorageFailure() {
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, 1, 0, false, true);
        f.run();
        QVERIFY(f.done());
        QVERIFY(f.startup.reviewGame().state().run);
        QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Failed);
        QVERIFY(!f.startup.reviewGame().state().error.isEmpty());
    }
    void failedCheckpointDoesNotAdvertiseProgress() {
        Fixture f;
        QVERIFY(f.seed());
        QVERIFY(f.sql(QStringLiteral("CREATE TRIGGER reject_evidence BEFORE INSERT ON "
                                     "review_positions BEGIN SELECT RAISE(FAIL,'fixture'); END")));
        f.script(0, 1);
        f.run();
        QVERIFY(f.done());
        QVERIFY(f.startup.reviewGame().state().run);
        QCOMPARE(f.startup.reviewGame().state().run->completedPositions, 0);
        QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Failed);
    }
    void mateEvidenceRemainsInspectable() {
        Fixture f;
        QVERIFY(f.seed("*"));
        f.script(0, -1, 0, false, false, QStringLiteral("mate 3"));
        f.run();
        QVERIFY(f.done());
        QVERIFY2(f.startup.reviewGame().state().error.isEmpty(),
                 qPrintable(f.startup.reviewGame().state().error));
        QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Complete);
    }
    void missingScoreIsRetainedWithoutClaims() {
        Fixture f;
        QVERIFY(f.seed("*"));
        f.script(0, -1, 0, false, false, QString{});
        f.run();
        QVERIFY(f.done());
        QVERIFY2(f.startup.reviewGame().state().error.isEmpty(),
                 qPrintable(f.startup.reviewGame().state().error));
        QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Complete);
    }
    void changedEngineDoesNotContinueSavedRun() {
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, 1);
        const auto connection =
            connect(&f.startup.reviewGame(), &app::ReviewGameController::changed,
                    &f.startup.reviewGame(), [&] {
                        const auto& state = f.startup.reviewGame().state();
                        if (state.active && state.run && state.run->completedPositions == 1) {
                            f.startup.reviewGame().cancel();
                        }
                    });
        f.run();
        QVERIFY(f.done());
        disconnect(connection);
        const auto run = *f.startup.reviewGame().state().run;
        f.script(3, -1, 0, false, false, QStringLiteral("cp 25"),
                 QStringLiteral("Different engine"));
        f.startup.reviewGame().resume(run.id);
        QVERIFY(f.done());
        QVERIFY(!f.startup.reviewGame().state().error.isEmpty());
        QCOMPARE(f.startup.reviewGame().state().run->revision, run.revision);
        QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Cancelled);
    }
    void mainlineScanKeepsEventLoopResponsive() {
        std::string text;
        for (int i = 0; i < 20; ++i) {
            text += "Nf3 Nf6 Ng1 Ng8 ";
        }
        text += "*";
        Fixture f;
        QVERIFY(f.seed(text));
        f.script();
        int heartbeats = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++heartbeats; });
        heartbeat.start(1);
        f.run();
        QVERIFY(f.done());
        heartbeat.stop();
        QVERIFY2(f.startup.reviewGame().state().error.isEmpty(),
                 qPrintable(f.startup.reviewGame().state().error));
        QCOMPARE(f.startup.reviewGame().state().run->completedPositions, 81);
        QVERIFY(heartbeats > 10);
    }
    void restartUsesOriginalConfiguration() {
        QTemporaryDir directory;
        const auto database = directory.filePath(QStringLiteral("restart.sqlite3"));
        review::ReviewId runId = 0;
        persistence::GameId gameId = 0;
        std::shared_ptr<const pgn::PgnDocument> document;
        {
            Fixture f(database);
            QVERIFY(f.seed());
            f.script(0, 1);
            connect(&f.startup.reviewGame(), &app::ReviewGameController::changed,
                    &f.startup.reviewGame(), [&] {
                        const auto& state = f.startup.reviewGame().state();
                        if (state.active && state.run && state.run->completedPositions == 1) {
                            f.startup.reviewGame().cancel();
                        }
                    });
            f.run();
            QVERIFY(f.done());
            runId = f.startup.reviewGame().state().run->id;
            gameId = f.gameId;
            document = f.document;
        }
        const auto configPath = directory.filePath(QStringLiteral("config.toml"));
        QFile config(configPath);
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write("version = 1\n[review]\nquality = \"fast\"\n");
        config.close();
        Fixture f(database, configPath);
        QTRY_COMPARE(f.startup.state(), app::Lifecycle::Ready);
        QTRY_COMPARE(f.startup.reviewConfiguration().state(), app::ConfigurationState::Valid);
        QTRY_VERIFY(!f.startup.engines().state().listing &&
                    !f.startup.engines().state().profiles.empty());
        f.document = document;
        f.gameId = gameId;
        f.profileId = f.startup.engines().state().profiles.front().id;
        f.startup.reviewGame().selectGame(gameId);
        QTRY_VERIFY(!f.startup.reviewGame().state().busy);
        QCOMPARE(f.startup.reviewGame().defaultQuality(), review::Quality::Fast);
        f.script(1);
        f.startup.reviewGame().resume(runId);
        QVERIFY(f.done());
        QVERIFY2(f.startup.reviewGame().state().error.isEmpty(),
                 qPrintable(f.startup.reviewGame().state().error));
        QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Complete);
        QCOMPARE(f.startup.reviewGame().state().run->quality, review::Quality::Standard);
    }
    void conflictingRevisionReloadsWithoutOverwriting() {
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, 1, 200);
        bool submitted = false;
        connect(&f.startup.reviewGame(), &app::ReviewGameController::changed,
                &f.startup.reviewGame(), [&] {
                    const auto& state = f.startup.reviewGame().state();
                    if (!submitted && state.run && state.phase == app::ReviewPhase::Scanning) {
                        submitted = true;
                        persistence::ReviewCheckpoint checkpoint;
                        checkpoint.reviewId = state.run->id;
                        checkpoint.expectedRevision = state.run->revision;
                        checkpoint.status = review::JobStatus::Partial;
                        f.startup.gameRepository().checkpointReview(QUuid::createUuid(),
                                                                    checkpoint);
                    }
                });
        f.run();
        QVERIFY(f.done());
        QVERIFY(submitted);
        QTRY_COMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Partial);
        QCOMPARE(f.startup.reviewGame().state().run->completedPositions, 0);
        QVERIFY(f.startup.reviewGame().state().error.contains(QStringLiteral("changed")));
        f.shutdown();
    }
    void damagedEvidenceIsNotReplacedOnResume() {
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, 1);
        const auto connection =
            connect(&f.startup.reviewGame(), &app::ReviewGameController::changed,
                    &f.startup.reviewGame(), [&] {
                        const auto& state = f.startup.reviewGame().state();
                        if (state.active && state.run && state.run->completedPositions == 1) {
                            f.startup.reviewGame().cancel();
                        }
                    });
        f.run();
        QVERIFY(f.done());
        disconnect(connection);
        const auto run = *f.startup.reviewGame().state().run;
        QVERIFY(f.sql(QStringLiteral("UPDATE review_evidence SET adapted=x'00'")));
        f.script(3);
        f.startup.reviewGame().resume(run.id);
        QVERIFY(f.done());
        QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Failed);
        QCOMPARE(f.startup.reviewGame().state().run->completedPositions, 1);
        QVERIFY(!f.startup.reviewGame().state().error.isEmpty());
    }
    void shutdownDuringLoadingAndSearch() {
        {
            Fixture f;
            QVERIFY(f.seed());
            f.startup.reviewGame().selectGame(f.gameId);
            f.shutdown();
            QCOMPARE(f.startup.state(), app::Lifecycle::Stopped);
        }
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, 1, 500);
        f.run();
        QTRY_COMPARE(f.startup.reviewGame().state().phase, app::ReviewPhase::Scanning);
        f.shutdown();
        QCOMPARE(f.startup.state(), app::Lifecycle::Stopped);
    }
    void cooperativeCancellationRetainsNoStoppedAttempt() {
        Fixture f;
        QVERIFY(f.seed());
        const auto position = chess::Position::initial();
        const auto best = QString::fromStdString(
            std::get<std::string>(position.uci(position.legalMoves().front())));
        QJsonArray steps{
            engine_test::step(QStringLiteral("uci"),
                              {engine_test::write(QStringLiteral(
                                  "id name Test engine\nid author Tests\nuciok\n"))}),
            ready(),
            engine_test::step(QStringLiteral("ucinewgame"), {}),
            ready(),
            engine_test::step(QStringLiteral("position startpos"), {}),
            engine_test::step(
                QStringLiteral("go nodes 100000"),
                {engine_test::write(
                    QStringLiteral("info depth 3 nodes 10 score cp 0 pv %1\n").arg(best))}),
            engine_test::step(QStringLiteral("stop"),
                              {engine_test::write(QStringLiteral("bestmove %1\n").arg(best))}),
            ready(),
            quit()};
        engine::ValidatorTestAccess::configure(
            f.supervisor(),
            engine_test::scenario(f.directory.filePath(QStringLiteral("stop.json")), steps), 1000);
        QSignalSpy evidence(&f.supervisor(), &engine::EngineSupervisor::scheduledUpdated);
        f.run();
        QTRY_VERIFY(!evidence.empty());
        f.startup.reviewGame().cancel();
        QVERIFY(f.done());
        QVERIFY2(f.startup.reviewGame().state().error.isEmpty(),
                 qPrintable(f.startup.reviewGame().state().error));
        QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Cancelled);
        QCOMPARE(f.startup.reviewGame().state().run->completedPositions, 0);
    }
    void busyAdmissionRetriesWithoutFlooding() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        const auto connectionName = QUuid::createUuid().toString();
        auto lock = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        lock.setDatabaseName(f.directory.filePath(QStringLiteral("review.sqlite3")));
        QVERIFY(lock.open());
        bool saturated = false;
        bool busy = false;
        connect(&f.startup.reviewGame(), &app::ReviewGameController::changed,
                &f.startup.reviewGame(), [&] {
                    if (saturated ||
                        f.startup.reviewGame().state().phase != app::ReviewPhase::Preparing) {
                        return;
                    }
                    saturated = true;
                    QSqlQuery transaction(lock);
                    QVERIFY(transaction.exec(QStringLiteral("BEGIN IMMEDIATE")));
                    persistence::CreateReview creation;
                    creation.gameId = f.gameId;
                    creation.engineProfileId = f.profileId;
                    creation.configuration = *f.startup.reviewConfiguration().snapshot();
                    creation.fingerprint =
                        std::get<engine::EngineFingerprint>(engine::quickReviewFingerprint(
                            *f.startup.engines().state().runtime.discovery));
                    for (int i = 0; i < 16; ++i) {
                        f.startup.gameRepository().createReview(QUuid::createUuid(), creation);
                    }
                });
        connect(&f.startup.gameRepository(), &persistence::Database::reviewCreated,
                &f.startup.reviewGame(), [&](QUuid, const persistence::CreateReviewResult& result) {
                    if (const auto* error = std::get_if<persistence::ReviewError>(&result);
                        error && error->code == persistence::ReviewErrorCode::Busy && !busy) {
                        busy = true;
                        QSqlQuery transaction(lock);
                        QVERIFY(transaction.exec(QStringLiteral("ROLLBACK")));
                    }
                });
        f.run();
        QVERIFY(f.done());
        QVERIFY(saturated);
        QVERIFY(busy);
        QVERIFY2(f.startup.reviewGame().state().error.isEmpty(),
                 qPrintable(f.startup.reviewGame().state().error));
        QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Complete);
        f.shutdown();
        lock.close();
        lock = {};
        QSqlDatabase::removeDatabase(connectionName);
    }
    void pinnedEngineCacheReuse() {
        const auto executable = qEnvironmentVariable("CHESS_REVIEW_STOCKFISH");
        if (executable.isEmpty()) {
            QSKIP("Set CHESS_REVIEW_STOCKFISH to select a local engine for the cache probe.");
        }
        Fixture f;
        QTRY_VERIFY(f.startup.engines().state().ready && !f.startup.engines().state().listing);
        QSignalSpy validated(&f.supervisor(), &engine::EngineSupervisor::completed);
        f.supervisor().validate(QUuid::createUuid(), executable);
        QTRY_COMPARE_WITH_TIMEOUT(validated.size(), 1, 15000);
        const auto validation = qvariant_cast<engine::ValidationResult>(validated.front()[1]);
        QVERIFY(std::holds_alternative<engine::ValidatedEngine>(validation));
        QSignalSpy registered(&f.startup.gameRepository(),
                              &persistence::Database::engineRegistered);
        f.startup.gameRepository().registerEngine(QUuid::createUuid(),
                                                  std::get<engine::ValidatedEngine>(validation));
        QVERIFY(registered.wait());
        f.startup.engines().refresh();
        QTRY_VERIFY(!f.startup.engines().state().listing &&
                    !f.startup.engines().state().profiles.empty());
        f.profileId = f.startup.engines().state().profiles.front().id;
        const std::string text = "1. e4 e5 *";
        pgn::PgnReader reader(text);
        f.document =
            std::make_shared<const pgn::PgnDocument>(std::get<pgn::PgnDocument>(reader.next()));
        QSignalSpy inserted(&f.startup.gameRepository(), &persistence::Database::gameInserted);
        f.startup.gameRepository().insertGame(QUuid::createUuid(), f.document);
        QVERIFY(inserted.wait());
        f.gameId = std::get<persistence::Inserted>(
                       qvariant_cast<persistence::InsertGameResult>(inserted.front()[1]))
                       .id;
        f.startup.reviewGame().selectGame(f.gameId);
        QTRY_VERIFY(!f.startup.reviewGame().state().busy);
        f.run();
        QVERIFY(f.done());
        QVERIFY2(f.startup.reviewGame().state().error.isEmpty(),
                 qPrintable(f.startup.reviewGame().state().error));
        const auto first = *f.startup.reviewGame().state().run;
        QCOMPARE(first.status, review::JobStatus::Complete);
        int compatibleHits = 0;
        auto position =
            std::get<chess::Position>(f.document->tree.position(f.document->tree.rootId()));
        const auto fingerprint = std::get<engine::EngineFingerprint>(
            engine::quickReviewFingerprint(*f.startup.engines().state().runtime.discovery));
        QVERIFY(engine::verifiedResources(fingerprint));
        for (int index = 0; index < first.totalPositions; ++index) {
            QSignalSpy found(&f.startup.gameRepository(), &persistence::Database::analysisFound);
            f.startup.gameRepository().findAnalysis(
                QUuid::createUuid(),
                engine::CacheQuery{
                    position.context(), fingerprint, engine::NodeLimit{100000}, 1, true, {}});
            QVERIFY(found.wait());
            const auto lookup = qvariant_cast<persistence::FindAnalysisResult>(found.front()[1]);
            QVERIFY(std::holds_alternative<persistence::CacheLookup>(lookup));
            if (std::get<persistence::CacheLookup>(lookup).hit) {
                ++compatibleHits;
            }
            if (index < static_cast<int>(f.document->originalMainline().size())) {
                QVERIFY(std::holds_alternative<std::monostate>(position.play(
                    f.document->originalMainline()[static_cast<std::size_t>(index)])));
            }
        }
        QVERIFY(compatibleHits > 0);
        QSignalSpy finished(&f.supervisor(), &engine::EngineSupervisor::scheduledFinished);
        f.run();
        QVERIFY(f.done());
        QVERIFY2(f.startup.reviewGame().state().error.isEmpty(),
                 qPrintable(f.startup.reviewGame().state().error));
        QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Complete);
        QCOMPARE(finished.size(), first.totalPositions - compatibleHits);
        QVERIFY(f.sql(QStringLiteral("DELETE FROM analysis_results")));
        QSignalSpy loaded(&f.startup.gameRepository(),
                          &persistence::Database::reviewEvidenceLoaded);
        f.startup.gameRepository().loadReviewEvidence(QUuid::createUuid(), first.id, 2);
        QVERIFY(loaded.wait());
        QVERIFY(std::holds_alternative<persistence::StoredReviewEvidence>(
            qvariant_cast<persistence::LoadReviewEvidenceResult>(loaded.front()[1])));
        f.shutdown();
        Fixture reopened(f.directory.filePath(QStringLiteral("review.sqlite3")));
        QTRY_COMPARE(reopened.startup.state(), app::Lifecycle::Ready);
        reopened.startup.reviewGame().selectGame(f.gameId);
        QTRY_VERIFY(!reopened.startup.reviewGame().state().busy);
        reopened.startup.reviewGame().inspect(first.id);
        QTRY_VERIFY(!reopened.startup.reviewGame().state().busy);
        QCOMPARE(reopened.startup.reviewGame().state().run->status, review::JobStatus::Complete);
        QCOMPARE(reopened.startup.engines().state().runtime.phase,
                 engine::RuntimePhase::Disconnected);
    }
    void renderWorkspace() {
        const auto directory = qEnvironmentVariable("CHESS_REVIEW_QUICK_RENDER_DIR");
        if (directory.isEmpty()) {
            QSKIP("Set CHESS_REVIEW_QUICK_RENDER_DIR to inspect workspace renders.");
        }
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        f.window.findChild<QPushButton*>(QStringLiteral("reviewTaskStatus"))->click();
        f.window.findChild<QComboBox*>(QStringLiteral("reviewSide"))->setCurrentIndex(2);
        f.window.resize(1100, 720);
        f.window.show();
        QTest::qWait(30);
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-light.png")));
        auto palette = qApp->palette();
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#202732")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#e5eaf0")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#171e27")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#e5eaf0")));
        palette.setColor(QPalette::Button, QColor(QStringLiteral("#303b49")));
        palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#e5eaf0")));
        f.window.setPalette(palette);
        f.window.resize(1360, 850);
        QTest::qWait(30);
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-dark.png")));
        auto font = f.window.font();
        font.setPointSize(font.pointSize() + 5);
        f.window.setFont(font);
        f.window.resize(1100, 720);
        QTest::qWait(30);
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-enlarged.png")));
    }
    void uiRequiresSideAndBackgroundChoice() {
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, -1, 200);
        auto* side = f.window.findChild<QComboBox*>(QStringLiteral("reviewSide"));
        auto* start = f.window.findChild<QPushButton*>(QStringLiteral("runReview"));
        QVERIFY(side);
        QVERIFY(start);
        QVERIFY(!start->isEnabled());
        side->setCurrentIndex(2);
        QVERIFY(start->isEnabled());
        start->click();
        QTRY_VERIFY(f.startup.reviewGame().state().run.has_value());
        // Show the contextual page through the persistent task affordance.
        f.window.findChild<QPushButton*>(QStringLiteral("reviewTaskStatus"))->click();
        f.window.findChild<QAction*>(QStringLiteral("gamesAction"))->trigger();
        auto* stayDialog = f.window.findChild<QMessageBox*>(QStringLiteral("leaveReviewDialog"));
        QVERIFY(stayDialog);
        for (auto* button : stayDialog->buttons()) {
            if (button->text() == QStringLiteral("Stay")) {
                button->click();
                break;
            }
        }
        QTRY_VERIFY(!f.window.findChild<QMessageBox*>(QStringLiteral("leaveReviewDialog")));
        f.navigation.navigate(app::Destination::Home);
        auto* dialog = f.window.findChild<QMessageBox*>(QStringLiteral("leaveReviewDialog"));
        QVERIFY(dialog);
        for (auto* button : dialog->buttons()) {
            if (button->text() == QStringLiteral("Continue in background")) {
                button->click();
                break;
            }
        }
        QTRY_VERIFY(!f.window.findChild<QMessageBox*>(QStringLiteral("leaveReviewDialog")));
        QCOMPARE(f.navigation.current(), app::Destination::Home);
        QVERIFY(f.done());
        QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Complete);
        const auto* progress = f.window.findChild<QProgressBar*>(QStringLiteral("reviewProgress"));
        QVERIFY(progress);
        QCOMPARE(progress->value(), 3);
    }
};
QTEST_MAIN(QuickReviewTest)
#include "QuickReviewTest.moc"
