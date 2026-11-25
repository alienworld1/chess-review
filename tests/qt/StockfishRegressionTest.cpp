#include "EngineControllerTestAccess.h"
#include "EngineTestSupport.h"
#include "TestEnvironment.h"
#include "app/AnalysisController.h"
#include "app/EngineAnalysisController.h"
#include "app/NavigationController.h"
#include "app/PuzzleController.h"
#include "app/PuzzleGenerationController.h"
#include "app/ReviewGameController.h"
#include "app/StartupCoordinator.h"
#include "chess/Position.h"
#include "engine/EffectiveSearch.h"
#include "engine/private/SearchProtocol.h"
#include "persistence/Database.h"
#include "pgn/Pgn.h"
#include "review/Classification.h"
#include "ui/shell/MainWindow.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <memory>
#include <variant>

using namespace chess_review;

namespace {
QJsonObject readyStep() {
    return engine_test::step(QStringLiteral("isready"),
                             {engine_test::write(QStringLiteral("readyok\n"))});
}

QJsonObject quitStep() {
    return engine_test::step(QStringLiteral("quit"),
                             {QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}});
}

QJsonArray buildReviewScript(const pgn::PgnDocument& document, int first, int end, int delayMs = 0,
                             const QString& score = QStringLiteral("cp 40"),
                             const review::NodeBudgets& limits = {100000, 1000000, 5000000}) {
    QJsonArray steps{engine_test::step(QStringLiteral("uci"),
                                       {engine_test::write(QStringLiteral(
                                           "id name Test engine\nid author Tests\nuciok\n"))}),
                     readyStep()};
    const engine::Capabilities capabilities;
    const auto moves = document.originalMainline();
    const int total = static_cast<int>(moves.size()) + 1;
    const int stages = end < 0 && total > 1 ? 3 : 1;

    for (int stage = 0; stage < stages; ++stage) {
        const auto budget = *review::stageBudget(limits, static_cast<review::EvidenceStage>(stage));
        auto position = std::get<chess::Position>(document.tree.position(document.tree.rootId()));
        for (int index = 0; index < total; ++index) {
            if ((stage == 0 ? (index >= first && (end < 0 || index < end)) : index >= 0) &&
                !position.legalMoves().empty()) {
                engine::AnalysisRequest request;
                request.id = QUuid::createUuid();
                request.position = position;
                request.wantWdl = true;
                request.limit = engine::NodeLimit{budget};
                request.multiPv = 1;
                const auto prepared =
                    std::get<engine::PreparedSearch>(engine::prepareSearch(request, capabilities));
                const auto legal = position.legalMoves();
                const auto best = std::get<std::string>(position.uci(legal.front()));
                for (const auto& command : prepared.options.split('\n')) {
                    if (!command.isEmpty()) {
                        steps.append(engine_test::step(QString::fromUtf8(command), {}));
                    }
                }
                steps.append(engine_test::step(QStringLiteral("ucinewgame"), {}));
                steps.append(readyStep());
                steps.append(engine_test::step(QString::fromUtf8(prepared.position).trimmed(), {}));
                const QString output =
                    QStringLiteral("info depth 7 nodes %1 multipv 1 score %2 pv %3\nbestmove %3\n")
                        .arg(budget)
                        .arg(score, QString::fromStdString(best));
                steps.append(engine_test::step(QStringLiteral("go nodes %1").arg(budget),
                                               {engine_test::write(output, delayMs)}));
                steps.append(readyStep());
            }
            if (index < static_cast<int>(moves.size())) {
                (void)position.play(moves[static_cast<std::size_t>(index)]);
            }
        }
    }
    steps.append(quitStep());
    return steps;
}

class RegressionFixture final {
  public:
    explicit RegressionFixture(const QByteArray& configToml)
        : window(navigation, startup.analysis()) {
        window.installEngines(startup.engines());
        window.installEngineAnalysis(startup.engineAnalysis());
        window.installImports(startup.localImports(), startup.library());
        window.installReview(startup.reviewGame());
        window.installPuzzles(startup.puzzles(), startup.puzzleGeneration());
        window.installSettings(startup.reviewConfiguration());
        QObject::connect(&startup, &app::StartupCoordinator::ready, &window,
                         &ui::MainWindow::showReady);

        const auto configPath = directory.filePath(QStringLiteral("config.toml"));
        QFile configFile(configPath);
        if (!configFile.open(QIODevice::WriteOnly) ||
            configFile.write(configToml) != configToml.size()) {
            qFatal("Cannot write regression configuration");
        }
        configFile.close();
        startup.start(directory.filePath(QStringLiteral("regression.sqlite3")), {}, configPath);
    }

    ~RegressionFixture() {
        if (startup.state() != app::Lifecycle::Stopped) {
            startup.shutdown();
            if (!QTest::qWaitFor([this] { return startup.state() == app::Lifecycle::Stopped; },
                                 15000)) {
                qFatal("Regression fixture did not stop cleanly");
            }
        }
        window.completeShutdown();
    }

    bool waitUntilReady() {
        return QTest::qWaitFor(
            [this] {
                return startup.state() == app::Lifecycle::Ready &&
                       startup.reviewConfiguration().snapshot() &&
                       startup.engines().state().ready && !startup.engines().state().listing;
            },
            10000);
    }

    std::optional<persistence::GameId> importPgn(const std::string& pgnText,
                                                 std::shared_ptr<const pgn::PgnDocument>& outDoc) {
        pgn::PgnReader reader(pgnText);
        auto parsed = reader.next();
        if (!std::holds_alternative<pgn::PgnDocument>(parsed)) {
            return std::nullopt;
        }
        outDoc =
            std::make_shared<const pgn::PgnDocument>(std::get<pgn::PgnDocument>(std::move(parsed)));
        QSignalSpy inserted(&startup.gameRepository(), &persistence::Database::gameInserted);
        startup.gameRepository().insertGame(QUuid::createUuid(), outDoc);
        if (!inserted.wait(5000)) {
            return std::nullopt;
        }
        const auto result = qvariant_cast<persistence::InsertGameResult>(inserted.front()[1]);
        const auto* added = std::get_if<persistence::Inserted>(&result);
        return added ? std::optional{added->id} : std::nullopt;
    }

    engine::EngineSupervisor& supervisor() {
        return app::EngineControllerTestAccess::validator(startup.engines());
    }

    QTemporaryDir directory;
    app::StartupCoordinator startup;
    app::NavigationController navigation;
    ui::MainWindow window;
};
} // namespace

class StockfishRegressionTest final : public QObject {
    Q_OBJECT

  private slots:
    void init();
    void interruptedReviewWithMovedEngineRecoversAndResumes();
    void stockfishEndToEndRegression();
};

void StockfishRegressionTest::init() {
    QTest::failOnWarning(QRegularExpression(
        QStringLiteral("^(?!This plugin does not support propagateSizeHints\\(\\)$).*")));
}

void StockfishRegressionTest::interruptedReviewWithMovedEngineRecoversAndResumes() {
    const QByteArray configToml = "[analysis.thorough]\n"
                                  "quickScanNodes = 100000\n"
                                  "deepeningNodes = 1000000\n"
                                  "verificationNodes = 5000000\n\n"
                                  "[review]\n"
                                  "preferWdl = true\n";
    RegressionFixture fixture(configToml);
    QVERIFY(fixture.waitUntilReady());

    const QString dirA = fixture.directory.filePath(QStringLiteral("engine-a"));
    const QString dirB = fixture.directory.filePath(QStringLiteral("engine-b"));
    QVERIFY(QDir().mkpath(dirA));
    QVERIFY(QDir().mkpath(dirB));
    const QString pathA = QDir(dirA).filePath(QStringLiteral("uci-engine"));
    const QString pathB = QDir(dirB).filePath(QStringLiteral("uci-engine"));
    QVERIFY(QFile::copy(QString::fromUtf8(FAKE_UCI_ENGINE), pathA));
    QVERIFY(QFile::setPermissions(pathA, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                             QFileDevice::ExeOwner));

    auto& engines = fixture.startup.engines();
    auto& supervisor = fixture.supervisor();
    engine::ValidatorTestAccess::configure(
        supervisor,
        engine_test::scenario(fixture.directory.filePath(QStringLiteral("handshake-a.json")),
                              engine_test::handshake()),
        2000);
    engines.selectExecutable(pathA);
    QTRY_COMPARE(engines.state().registration, app::EngineRegistrationState::Selected);
    engines.registerSelected();
    QTRY_COMPARE(engines.state().registration, app::EngineRegistrationState::Idle);
    QCOMPARE(engines.state().profiles.size(), std::size_t(1));
    const auto profileId = engines.state().profiles.front().id;

    std::shared_ptr<const pgn::PgnDocument> document;
    const auto gameId = fixture.importPgn(
        "[Event \"Moved Engine Recovery\"]\n[Result \"*\"]\n\n1. e4 e5 *\n", document);
    QVERIFY(gameId.has_value());

    auto& review = fixture.startup.reviewGame();
    review.selectGame(*gameId);
    QTRY_VERIFY(!review.state().busy);

    engine::ValidatorTestAccess::configure(
        supervisor,
        engine_test::scenario(fixture.directory.filePath(QStringLiteral("run-part1.json")),
                              buildReviewScript(*document, 0, 1, 250)),
        5000);
    engine::ValidatorTestAccess::configureSearch(supervisor, 10000, 1000);

    const auto cancelConn = connect(&review, &app::ReviewGameController::changed, &review, [&] {
        const auto& task = review.taskState();
        if (task.active && task.run && task.run->completedPositions >= 1 && !task.cancelling) {
            review.cancel();
        }
    });
    review.start(chess::Color::White, profileId, review::Quality::Thorough);
    QTRY_VERIFY_WITH_TIMEOUT(!review.taskState().active && review.state().run.has_value(), 10000);
    disconnect(cancelConn);
    QVERIFY(review.state().run->completedPositions >= 1);
    QVERIFY(review.state().run->status != review::JobStatus::Complete);
    const auto runId = review.state().run->id;
    const int completedBeforeMove = review.state().run->completedPositions;

    engines.disconnectEngine();
    QTRY_COMPARE(engines.state().runtime.phase, engine::RuntimePhase::Disconnected);

    QVERIFY(QFile::rename(pathA, pathB));
    QVERIFY(!QFileInfo::exists(pathA));
    engines.refresh();
    QTRY_VERIFY(!engines.state().listing && engines.state().missingProfiles.contains(profileId));

    const QString blocker = review.startBlocker(profileId);
    QVERIFY(!blocker.isEmpty());
    QVERIFY(blocker.contains(QStringLiteral("missing"), Qt::CaseInsensitive));

    review.resume(runId);
    QTRY_VERIFY(!review.taskState().active);
    QVERIFY(!review.state().error.isEmpty());
    QCOMPARE(review.state().run->completedPositions, completedBeforeMove);

    engine::ValidatorTestAccess::configure(
        supervisor,
        engine_test::scenario(fixture.directory.filePath(QStringLiteral("handshake-b.json")),
                              engine_test::handshake()),
        2000);
    engines.relocateProfile(profileId, pathB);
    QTRY_COMPARE(engines.state().registration, app::EngineRegistrationState::Selected);
    QCOMPARE(engines.state().relocatingProfileId, std::optional{profileId});
    engines.registerSelected();
    QTRY_COMPARE(engines.state().registration, app::EngineRegistrationState::Idle);
    QVERIFY(!engines.state().missingProfiles.contains(profileId));
    QCOMPARE(engines.state().profiles.size(), std::size_t(1));
    QCOMPARE(engines.state().profiles.front().id, profileId);
    QCOMPARE(engines.state().profiles.front().engine.executablePath,
             QFileInfo(pathB).canonicalFilePath());
    QVERIFY(review.startBlocker(profileId).isEmpty());

    engine::ValidatorTestAccess::configure(
        supervisor,
        engine_test::scenario(fixture.directory.filePath(QStringLiteral("run-part2.json")),
                              buildReviewScript(*document, completedBeforeMove, -1)),
        5000);
    review.resume(runId);
    QTRY_VERIFY_WITH_TIMEOUT(!review.taskState().active && review.state().run &&
                                 review.state().run->status == review::JobStatus::Complete,
                             15000);
    QTRY_VERIFY(review.state().overview.has_value());
    QCOMPARE(review.state().run->id, runId);
    QCOMPARE(review.state().run->completedPositions, review.state().run->totalPositions);
}

void StockfishRegressionTest::stockfishEndToEndRegression() {
    const QString stockfish = qEnvironmentVariable("CHESS_REVIEW_STOCKFISH");
    if (stockfish.isEmpty()) {
        QSKIP("Set CHESS_REVIEW_STOCKFISH to run genuine Stockfish regression tests.");
    }
    QVERIFY2(QFileInfo::exists(stockfish), qPrintable(stockfish));

    const QByteArray configToml = "[analysis.standard]\n"
                                  "quickScanNodes = 500\n"
                                  "deepeningNodes = 1500\n"
                                  "verificationNodes = 3000\n\n"
                                  "[analysis.thorough]\n"
                                  "quickScanNodes = 500\n"
                                  "deepeningNodes = 1500\n"
                                  "verificationNodes = 3000\n";
    RegressionFixture fixture(configToml);
    QVERIFY(fixture.waitUntilReady());

    auto& engines = fixture.startup.engines();
    engines.selectExecutable(stockfish);
    QTRY_COMPARE(engines.state().registration, app::EngineRegistrationState::Selected);
    engines.registerSelected();
    QTRY_COMPARE_WITH_TIMEOUT(engines.state().registration, app::EngineRegistrationState::Idle,
                              15000);
    QCOMPARE(engines.state().profiles.size(), std::size_t(1));
    const auto& profile = engines.state().profiles.front();
    QVERIFY2(profile.engine.reportedName.contains(QStringLiteral("Stockfish"), Qt::CaseInsensitive),
             qPrintable(profile.engine.reportedName));
    QCOMPARE(profile.engine.executableSha256.size(), 32);

    // 4. Nc3 hangs the Queen cleanly while missing 4. Qxf7#.
    std::shared_ptr<const pgn::PgnDocument> document;
    const auto gameId =
        fixture.importPgn("[Event \"Stockfish Blunder Regression\"]\n"
                          "[White \"Test White\"]\n"
                          "[Black \"Test Black\"]\n"
                          "[Result \"0-1\"]\n\n"
                          "1. e4 e5 2. Qh5 Nc6 3. Bc4 Nf6 4. Nc3 Nxh5 5. Nf3 Nf4 0-1\n",
                          document);
    QVERIFY(gameId.has_value());

    auto& review = fixture.startup.reviewGame();
    review.selectGame(*gameId);
    QTRY_VERIFY(!review.state().busy);
    review.start(chess::Color::White, profile.id, review::Quality::Standard);
    QTRY_VERIFY_WITH_TIMEOUT(!review.taskState().active && review.state().run &&
                                 review.state().run->status == review::JobStatus::Complete,
                             60000);
    QTRY_VERIFY_WITH_TIMEOUT(review.state().overview.has_value(), 10000);
    const auto& overview = *review.state().overview;
    QVERIFY(overview.totalKeyMoments >= 1);

    const auto blunderIt =
        std::find_if(overview.keyMoments.begin(), overview.keyMoments.end(),
                     [](const persistence::ReviewMove& move) {
                         return move.classification &&
                                move.classification->label == review::Classification::Blunder;
                     });
    QVERIFY(blunderIt != overview.keyMoments.end());
    QVERIFY(blunderIt->beforeEvidenceId > 0);
    QVERIFY(blunderIt->afterEvidenceId > 0);
    QVERIFY(!blunderIt->bestSan.isEmpty());

    auto& generator = fixture.startup.puzzleGeneration();
    generator.generate(review.state().run->id);
    QTRY_VERIFY_WITH_TIMEOUT(!generator.state().active, 60000);
    QVERIFY2(generator.state().error.isEmpty(), qPrintable(generator.state().error));

    auto& puzzles = fixture.startup.puzzles();
    puzzles.refresh();
    QTRY_VERIFY(!puzzles.libraryLoading());
    QVERIFY(!puzzles.state().library.puzzles.empty());

    puzzles.open(puzzles.state().library.puzzles.front().id);
    QTRY_VERIFY(!puzzles.state().loading && puzzles.state().solving);
    puzzles.reveal();
    QTRY_VERIFY(puzzles.state().revealed && !puzzles.state().loading);
    QVERIFY(puzzles.canStepForward());
    puzzles.next();
    QTRY_VERIFY(puzzles.canStepBack());

    fixture.window.show();
    fixture.navigation.navigate(app::Destination::Analyze);
    auto& analysis = fixture.startup.analysis();
    analysis.start();
    analysis.reset();
    QTRY_VERIFY(!analysis.busy() && analysis.position() != nullptr);

    auto& live = fixture.startup.engineAnalysis();
    live.setVisible(true);
    live.setMultiPv(2);
    QTRY_VERIFY(live.state().canStart);
    live.start();
    QTRY_VERIFY_WITH_TIMEOUT(live.state().candidates.size() >= 2 &&
                                 !live.state().candidates.front().pv.isEmpty(),
                             15000);
    live.stop();
    QTRY_VERIFY(!live.state().canStop);

    engines.disconnectEngine();
    QTRY_COMPARE_WITH_TIMEOUT(engines.state().runtime.phase, engine::RuntimePhase::Disconnected,
                              10000);
}

int main(int argc, char* argv[]) {
    const TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("chess-review"));
    application.setQuitOnLastWindowClosed(false);
    StockfishRegressionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "StockfishRegressionTest.moc"
