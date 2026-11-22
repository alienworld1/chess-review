#include "EngineControllerTestAccess.h"
#include "EngineTestSupport.h"
#include "PuzzleTestSupport.h"
#include "TestEnvironment.h"
#include "app/ReviewExplorationController.h"
#include "app/StartupCoordinator.h"
#include "engine/AnalysisCache.h"
#include "engine/private/SearchProtocol.h"
#include "ui/chess/ChessBoardWidget.h"
#include "ui/shell/MainWindow.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <algorithm>
#include <array>

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
QString identity() {
    return QStringLiteral(
        "id name Puzzle test engine\noption name MultiPV type spin default 1 min 1 max 4\n"
        "option name UCI_ShowWDL type check default false\nuciok\n");
}
class Fixture {
  public:
    QTemporaryDir directory;
    app::StartupCoordinator startup;
    app::NavigationController navigation;
    ui::MainWindow window{navigation, startup.analysis()};
    explicit Fixture(std::uint64_t verificationNodes = 10) {
        const auto config = directory.filePath(QStringLiteral("settings.toml"));
        QFile file(config);
        const auto text =
            QByteArray(
                "[analysis.standard]\nquickScanNodes=1\ndeepeningNodes=5\nverificationNodes=") +
            QByteArray::number(static_cast<qulonglong>(verificationNodes)) + '\n';
        if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size()) {
            qFatal("Cannot write puzzle configuration");
        }
        file.close();
        window.installEngines(startup.engines());
        window.installEngineAnalysis(startup.engineAnalysis());
        window.installImports(startup.localImports(), startup.library());
        window.installReview(startup.reviewGame());
        window.installPuzzles(startup.puzzles(), startup.puzzleGeneration());
        window.installSettings(startup.reviewConfiguration());
        QObject::connect(&startup, &app::StartupCoordinator::ready, &window,
                         &ui::MainWindow::showReady);
        startup.start(directory.filePath(QStringLiteral("library.sqlite3")), {}, config);
    }
    ~Fixture() {
        startup.shutdown();
        if (!QTest::qWaitFor([this] { return startup.state() == app::Lifecycle::Stopped; },
                             10000)) {
            qFatal("Puzzle fixture did not drain");
        }
        window.completeShutdown();
    }
    bool readyForWork() {
        return QTest::qWaitFor([this] {
            return startup.state() == app::Lifecycle::Ready &&
                   startup.reviewConfiguration().snapshot() && startup.engines().state().ready &&
                   !startup.engines().state().listing;
        });
    }
    persistence::PuzzleId
    saveOffline(persistence::PuzzleDocument document = puzzle_test::complete()) {
        auto& db = startup.gameRepository();
        persistence::PuzzleJob job;
        auto seed = document;
        seed.passes.clear();
        seed.solution = {};
        job.candidates = {seed};
        QSignalSpy checkpoint(&db, &persistence::Database::puzzleJobCheckpointed);
        db.checkpointPuzzleJob(QUuid::createUuid(), {job, {}});
        if (!checkpoint.wait()) {
            return 0;
        }
        const auto first = qvariant_cast<persistence::SavePuzzleJobResult>(checkpoint.back()[1]);
        if (!std::holds_alternative<persistence::PuzzleJob>(first)) {
            return 0;
        }
        job = std::get<persistence::PuzzleJob>(first);
        job.candidates[0] = std::move(document);
        job.current = 1;
        checkpoint.clear();
        db.checkpointPuzzleJob(QUuid::createUuid(), {job, job.candidates[0]});
        if (!checkpoint.wait()) {
            return 0;
        }
        const auto saved = qvariant_cast<persistence::SavePuzzleJobResult>(checkpoint.back()[1]);
        if (!std::holds_alternative<persistence::PuzzleJob>(saved)) {
            return 0;
        }
        return std::get<persistence::PuzzleJob>(saved).accepted.front();
    }
    engine::EngineSupervisor& supervisor() {
        return app::EngineControllerTestAccess::validator(startup.engines());
    }
    QString engineIdentity = identity();
    bool validate(int maximumWidth = 4) {
        engineIdentity = identity();
        engineIdentity.replace(QStringLiteral("max 4"), QStringLiteral("max %1").arg(maximumWidth));
        engine::ValidatorTestAccess::configure(
            supervisor(),
            engine_test::scenario(directory.filePath(QStringLiteral("validation.json")),
                                  engine_test::handshake(engineIdentity)),
            1000);
        QSignalSpy done(&supervisor(), &engine::EngineSupervisor::completed);
        supervisor().validate(QUuid::createUuid(), QString::fromUtf8(FAKE_UCI_ENGINE));
        if (!done.wait(5000)) {
            return false;
        }
        const auto result = qvariant_cast<engine::ValidationResult>(done.front()[1]);
        const auto* e = std::get_if<engine::ValidatedEngine>(&result);
        if (!e) {
            return false;
        }
        QSignalSpy registered(&startup.gameRepository(), &persistence::Database::engineRegistered);
        startup.gameRepository().registerEngine(QUuid::createUuid(), *e);
        if (!registered.wait()) {
            return false;
        }
        const auto saved = qvariant_cast<persistence::RegisterEngineResult>(registered.front()[1]);
        const auto* record = std::get_if<persistence::RegisteredEngine>(&saved);
        if (!record) {
            return false;
        }
        const auto& profile = record->profile;
        app::EngineControllerTestAccess::replaceProfiles(startup.engines(), {profile});
        return true;
    }
    void finite(QJsonArray& steps, std::uint64_t nodes, int delay = 0, bool cancel = false,
                bool malformed = false) {
        const auto root = puzzle_test::root();
        engine::AnalysisRequest request;
        request.id = QUuid::createUuid();
        request.position = root;
        request.limit = engine::NodeLimit{nodes};
        request.multiPv = 4;
        request.wantWdl = true;
        const auto prepared = std::get<engine::PreparedSearch>(engine::prepareSearch(
            request, startup.engines().state().profiles.front().engine.capabilities));
        for (const auto& command : prepared.options.split('\n')) {
            if (!command.isEmpty()) {
                steps.append(engine_test::step(QString::fromUtf8(command), {}));
            }
        }
        steps.append(engine_test::step(QStringLiteral("ucinewgame"), {}));
        steps.append(ready());
        steps.append(engine_test::step(QString::fromUtf8(prepared.position).trimmed(), {}));
        const auto sample = puzzle_test::pass(puzzle_test::seed(), nodes);
        QString output;
        for (const auto& line : sample.evidence.lines) {
            if (line.rank > prepared.effectiveMultiPv) {
                break;
            }
            output +=
                QStringLiteral("info depth 12 nodes %1 multipv %2 score %3 wdl %4 0 %5 pv %6\n")
                    .arg(malformed ? 1 : nodes)
                    .arg(line.rank)
                    .arg(line.rank == 1 ? QStringLiteral("mate 1") : QStringLiteral("cp 0"))
                    .arg(line.rank == 1 ? 1000 : 500)
                    .arg(line.rank == 1 ? 0 : 500)
                    .arg(QString::fromStdString(std::get<std::string>(root.uci(line.pv.front()))));
        }
        output += QStringLiteral("bestmove d1d8\n");
        steps.append(engine_test::step(QString::fromUtf8(prepared.go).trimmed(),
                                       cancel ? QJsonArray{}
                                              : QJsonArray{engine_test::write(output, delay)}));
        if (cancel) {
            steps.append(engine_test::step(QStringLiteral("stop"), {engine_test::write(output)}));
        }
        steps.append(ready());
    }
    void normalSearch(QJsonArray& steps, const engine::AnalysisResult& raw) {
        const auto& s = raw.evidence;
        auto position = std::get<chess::Position>(engine::restorePosition(s.provenance->position));
        engine::AnalysisRequest request;
        request.id = QUuid::createUuid();
        request.position = position;
        request.limit = s.limit;
        request.multiPv = s.requestedMultiPv;
        request.wantWdl = true;
        const auto prepared = std::get<engine::PreparedSearch>(engine::prepareSearch(
            request, startup.engines().state().profiles.front().engine.capabilities));
        for (const auto& option : prepared.options.split('\n')) {
            if (!option.isEmpty()) {
                steps.append(engine_test::step(QString::fromUtf8(option), {}));
            }
        }
        steps.append(engine_test::step(QStringLiteral("ucinewgame"), {}));
        steps.append(ready());
        steps.append(engine_test::step(QString::fromUtf8(prepared.position).trimmed(), {}));
        QString output;
        for (const auto& line : s.lines) {
            output +=
                QStringLiteral("info depth 12 nodes %1 multipv %2 score %3 wdl %4 0 %5 pv %6\n")
                    .arg(*s.statistics.nodes)
                    .arg(line.rank)
                    .arg(QStringLiteral("%1 %2")
                             .arg(line.score->type == engine::ScoreType::Mate
                                      ? QStringLiteral("mate")
                                      : QStringLiteral("cp"))
                             .arg(line.score->value))
                    .arg(line.wdl->wins)
                    .arg(line.wdl->losses)
                    .arg(QString::fromStdString(
                        std::get<std::string>(position.uci(line.pv.front()))));
        }
        output +=
            QStringLiteral("bestmove %1\n")
                .arg(QString::fromStdString(std::get<std::string>(position.uci(*s.bestMove))));
        steps.append(engine_test::step(QString::fromUtf8(prepared.go).trimmed(),
                                       {engine_test::write(output)}));
        steps.append(ready());
    }
    bool connectRuntime(const QJsonArray& searches) {
        QJsonArray steps{
            engine_test::step(QStringLiteral("uci"), {engine_test::write(engineIdentity)}),
            ready()};
        for (const auto& step : searches) {
            steps.append(step);
        }
        steps.append(quit());
        engine::ValidatorTestAccess::configure(
            supervisor(),
            engine_test::scenario(directory.filePath(QStringLiteral("runtime.json")), steps), 1000);
        engine::ValidatorTestAccess::configureSearch(supervisor(), 10000, 1000);
        startup.engines().connectEngine(1);
        return QTest::qWaitFor(
            [this] {
                return startup.engines().state().runtime.phase == engine::RuntimePhase::Ready;
            },
            5000);
    }
};
} // namespace
class PuzzleTest final : public QObject {
    Q_OBJECT
  private slots:
    void homeShowsOfflineLibraryAndLiveEngineStatus() {
        Fixture f;
        QVERIFY(f.readyForWork());
        f.window.show();
        auto* count = f.window.findChild<QLabel*>(QStringLiteral("homePuzzleSummary"));
        auto* engine = f.window.findChild<QLabel*>(QStringLiteral("homeEngineStatus"));
        auto* recent = f.window.findChild<QListWidget*>(QStringLiteral("homeRecentGames"));
        auto* import = f.window.findChild<QToolButton*>(QStringLiteral("homeImportButton"));
        auto* open = f.window.findChild<QPushButton*>(QStringLiteral("homeOpenPuzzles"));
        auto* settings = f.window.findChild<QPushButton*>(QStringLiteral("homeEngineSettings"));
        auto* shell = f.window.findChild<QScrollArea*>(QStringLiteral("shellScrollArea"));
        QVERIFY(count && engine && recent && import && open && settings && shell);
        QTRY_VERIFY(count->text().startsWith(QStringLiteral("No saved puzzles")));
        QTRY_VERIFY(engine->text().startsWith(QStringLiteral("Add an engine")));
        QVERIFY(import->isEnabled());
        QCOMPARE(f.startup.engines().state().runtime.phase, engine::RuntimePhase::Disconnected);

        const auto renderDirectory = qEnvironmentVariable("CHESS_REVIEW_HOME_RENDER_DIR");
        const auto originalPalette = qApp->palette();
        const auto restorePalette = qScopeGuard([&] { qApp->setPalette(originalPalette); });
        const auto render = [&](const QString& state) {
            if (renderDirectory.isEmpty()) {
                return;
            }
            for (const bool dark : {false, true}) {
                auto palette = originalPalette;
                palette.setColor(QPalette::Window, QColor(dark ? "#191f2a" : "#f4f6f8"));
                qApp->setPalette(palette);
                for (const auto size : {QSize(1100, 720), QSize(1360, 850), QSize(1800, 1000)}) {
                    f.window.resize(size);
                    QTRY_COMPARE(shell->verticalScrollBar()->maximum(), 0);
                    QTRY_COMPARE(shell->horizontalScrollBar()->maximum(), 0);
                    const auto filename =
                        QStringLiteral("%1-%2-%3.png")
                            .arg(state, dark ? QStringLiteral("dark") : QStringLiteral("light"))
                            .arg(size.width());
                    QVERIFY2(f.window.grab().save(QDir(renderDirectory).filePath(filename)),
                             qPrintable(filename));
                }
            }
        };
        render(QStringLiteral("empty"));

        auto& puzzles = f.startup.puzzles();
        const auto id = f.saveOffline();
        QVERIFY(id > 0);
        puzzles.invalidateSummary();
        QTRY_COMPARE(count->text(), QStringLiteral("1 saved puzzle"));
        puzzles.refresh(0, 999);
        puzzles.open(id);
        QTRY_VERIFY(puzzles.state().solving && !puzzles.state().loading);
        open->setFocus();
        // Separate events avoid a clang analyzer false positive in Qt's keyClick guard.
        QTest::keyPress(open, Qt::Key_Space);
        QTest::keyRelease(open, Qt::Key_Space);
        QCOMPARE(f.navigation.current(), app::Destination::Puzzles);
        QVERIFY(!puzzles.state().solving);
        QTRY_COMPARE(puzzles.state().library.puzzles.size(), std::size_t{1});
        QCOMPARE(puzzles.state().library.puzzles.front().id, id);
        puzzles.remove(id);
        QTRY_VERIFY(puzzles.state().library.puzzles.empty());
        f.navigation.navigate(app::Destination::Home);
        QTRY_VERIFY(count->text().startsWith(QStringLiteral("No saved puzzles")));

        settings->click();
        QCOMPARE(f.navigation.current(), app::Destination::Settings);
        QVERIFY(f.validate());
        f.startup.engines().refresh();
        QTRY_VERIFY(!f.startup.engines().state().listing);
        f.navigation.navigate(app::Destination::Home);
        QVERIFY(engine->text().contains(QStringLiteral("Default: Puzzle test engine")));
        QVERIFY(engine->text().contains(QStringLiteral("Disconnected")));
        QVERIFY(f.connectRuntime({}));
        QTRY_VERIFY(engine->text().contains(QStringLiteral("Connected and ready")));

        const std::array games{
            QStringLiteral("[White \"Ada\"]\n[Black \"Bela\"]\n[Date \"2026.11.19\"]\n\n1. e4 *"),
            QStringLiteral(
                "[White \"White player with a very long name used to test text elision "
                "across the width of the recent games list\"]\n[Black \"Owen\"]\n\n1. c4 *"),
            QStringLiteral("[White \"Sofia\"]\n[Black \"Mina\"]\n\n1. d4 *"),
            QStringLiteral("[White \"Sam\"]\n[Black \"Eli\"]\n\n1. Nf3 *"),
            QStringLiteral("[White \"Tara\"]\n[Black \"Zoe\"]\n\n1. g3 *")};
        for (const auto& game : games) {
            QVERIFY(f.startup.localImports().importText(game));
            QTRY_VERIFY(f.startup.reviewGame().state().game.startsWith(game.section(u'"', 1, 1)) &&
                        !f.startup.reviewGame().state().busy);
            f.navigation.navigate(app::Destination::Home);
        }
        QTRY_COMPARE(recent->count(), 5);
        render(QStringLiteral("populated"));
        f.window.resize(1100, 720);
        QTRY_COMPARE(shell->horizontalScrollBar()->maximum(), 0);
        QTRY_COMPARE(shell->verticalScrollBar()->maximum(), 0);
        auto& engines = f.startup.engines();
        engines.disconnectEngine();
        QTRY_COMPARE(engines.state().runtime.phase, engine::RuntimePhase::Disconnected);
        const QJsonArray failingHandshake{
            engine_test::step(QStringLiteral("uci"),
                              {QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}})};
        engine::ValidatorTestAccess::configure(
            f.supervisor(),
            engine_test::scenario(f.directory.filePath(QStringLiteral("failure.json")),
                                  failingHandshake),
            1000);
        engines.connectEngine(engines.state().profiles.front().id);
        QTRY_COMPARE(engines.state().runtime.phase, engine::RuntimePhase::Failed);
        QVERIFY(engine->text().contains(QStringLiteral("Connection failed")));
        QVERIFY(import->isEnabled());
    }
    void realEngineCreatesAnOfflinePuzzle() {
        const auto path = qEnvironmentVariable("CHESS_REVIEW_REAL_PUZZLE_ENGINE");
        if (path.isEmpty()) {
            QSKIP("Set CHESS_REVIEW_REAL_PUZZLE_ENGINE to a trusted local engine for the optional "
                  "functional probe.");
        }
        Fixture f(50000);
        QVERIFY(f.readyForWork());
        auto& settings = f.startup.engines();
        settings.selectExecutable(path);
        QTRY_COMPARE_WITH_TIMEOUT(settings.state().registration,
                                  app::EngineRegistrationState::Selected, 10000);
        settings.registerSelected();
        QTRY_VERIFY_WITH_TIMEOUT(settings.state().registration ==
                                         app::EngineRegistrationState::Idle &&
                                     !settings.state().profiles.empty(),
                                 10000);
        settings.connectEngine(settings.state().profiles.front().id);
        QTRY_COMPARE_WITH_TIMEOUT(settings.state().runtime.phase, engine::RuntimePhase::Ready,
                                  10000);
        const auto root = std::get<chess::Position>(
            chess::Position::fromFen("6k1/5ppp/q7/8/8/8/5PPP/3R2K1 w - - 0 1"));
        auto& generation = f.startup.puzzleGeneration();
        generation.savePosition(std::make_shared<const chess::Position>(root));
        QTRY_VERIFY_WITH_TIMEOUT(!generation.state().active, 45000);
        QVERIFY2(generation.state().lastSaved.has_value(),
                 qPrintable(generation.state().message + generation.state().error));
        settings.disconnectEngine();
        auto& puzzles = f.startup.puzzles();
        puzzles.open(*generation.state().lastSaved);
        QTRY_VERIFY(puzzles.board().interactive);
        puzzles.attemptMove(std::get<chess::Move>(root.parseUci("d1d8")), puzzles.board().revision);
        QTRY_VERIFY(puzzles.state().complete);
    }
    void completedReviewAutomaticallyCreatesAnOfflinePuzzle() {
        Fixture f;
        QVERIFY(f.readyForWork());
        QVERIFY(f.validate());
        const auto fingerprint = std::get<engine::EngineFingerprint>(
            engine::quickReviewFingerprint(f.startup.engines().state().profiles.front().engine));
        auto document = puzzle_test::seed();
        document.fingerprint = fingerprint;
        auto after = puzzle_test::root();
        QVERIFY(std::holds_alternative<std::monostate>(
            after.play(std::get<chess::Move>(after.parseUci("h2h3")))));
        QJsonArray searches;
        for (const auto nodes : {1ULL, 5ULL, 10ULL}) {
            const int width = nodes == 1 ? 1 : 3;
            auto beforeRaw = puzzle_test::pass(document, nodes);
            auto afterRaw = puzzle_test::normalPass(document, after, nodes, "g8f8", 500);
            for (auto* raw : {&beforeRaw, &afterRaw}) {
                auto& s = raw->evidence;
                s.lines.resize(static_cast<std::size_t>(width));
                s.requestedMultiPv = s.effectiveMultiPv = s.achievedWidth = width;
                auto p = *s.provenance;
                p.fingerprint = engine::reviewWidthFingerprint(fingerprint, width);
                s.provenance = std::make_shared<const engine::AnalysisProvenance>(std::move(p));
                f.normalSearch(searches, *raw);
            }
        }
        f.finite(searches, 20);
        f.finite(searches, 40);
        QVERIFY(f.connectRuntime(searches));
        pgn::PgnReader reader(
            "[SetUp \"1\"]\n[FEN \"6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1\"]\n\n1. h3 *");
        auto game =
            std::make_shared<const pgn::PgnDocument>(std::get<pgn::PgnDocument>(reader.next()));
        QSignalSpy inserted(&f.startup.gameRepository(), &persistence::Database::gameInserted);
        f.startup.gameRepository().insertGame(QUuid::createUuid(), game);
        QVERIFY(inserted.wait());
        const auto added = qvariant_cast<persistence::InsertGameResult>(inserted.front()[1]);
        QVERIFY(std::holds_alternative<persistence::Inserted>(added));
        const auto gameId = std::get<persistence::Inserted>(added).id;
        f.startup.reviewGame().selectGame(gameId);
        QTRY_VERIFY(!f.startup.reviewGame().state().busy);
        f.startup.reviewGame().start(chess::Color::White, 1, review::Quality::Standard, false);
        QTRY_VERIFY_WITH_TIMEOUT(!f.startup.reviewGame().taskState().active, 10000);
        QVERIFY2(f.startup.reviewGame().taskState().run.has_value(),
                 qPrintable(f.startup.reviewGame().taskState().error));
        QCOMPARE(f.startup.reviewGame().taskState().run->status, review::JobStatus::Complete);
        QTRY_VERIFY_WITH_TIMEOUT(f.startup.puzzleGeneration().state().lastSaved.has_value(), 10000);
        QTRY_VERIFY(!f.startup.puzzleGeneration().state().active);
        QTRY_COMPARE(f.startup.puzzles().savedSummary().count, std::optional{1});
        const auto id = *f.startup.puzzleGeneration().state().lastSaved;
        f.startup.engines().disconnectEngine();
        auto& puzzles = f.startup.puzzles();
        puzzles.open(id);
        QTRY_VERIFY(puzzles.board().interactive);
        puzzles.attemptMove(std::get<chess::Move>(puzzle_test::root().parseUci("d1d8")),
                            puzzles.board().revision);
        QTRY_VERIFY(puzzles.state().complete);
    }
    void reviewBlunderRetryAndSavedPuzzleWorkOffline_data() {
        QTest::addColumn<bool>("playedMoveNowBest");
        QTest::newRow("offline journey") << false;
        QTest::newRow("fresh searches contradict the lesson") << true;
    }
    void reviewBlunderRetryAndSavedPuzzleWorkOffline() {
        QFETCH(bool, playedMoveNowBest);
        Fixture f;
        QVERIFY(f.readyForWork());
        QVERIFY(f.validate());
        auto& settings = f.startup.engines();
        const auto fingerprint = std::get<engine::EngineFingerprint>(
            engine::quickReviewFingerprint(settings.state().profiles.front().engine));
        const auto document = puzzle_test::normalPuzzle(fingerprint);
        const auto root = chess::Position::initial();
        auto after = root;
        QVERIFY(std::holds_alternative<std::monostate>(
            after.play(std::get<chess::Move>(root.parseUci("d2d4")))));
        QJsonArray searches;
        for (const auto nodes : {1ULL, 5ULL, 10ULL}) {
            const int width = nodes == 1 ? 1 : 3;
            auto before = puzzle_test::normalPass(document, root, nodes, "e2e4", 600);
            auto played = puzzle_test::normalPass(document, after, nodes, "e7e5", 850);
            for (auto* raw : {&before, &played}) {
                auto& snapshot = raw->evidence;
                snapshot.lines.resize(static_cast<std::size_t>(width));
                snapshot.requestedMultiPv = snapshot.effectiveMultiPv = snapshot.achievedWidth =
                    width;
                auto provenance = *snapshot.provenance;
                provenance.fingerprint = engine::reviewWidthFingerprint(fingerprint, width);
                snapshot.provenance =
                    std::make_shared<const engine::AnalysisProvenance>(std::move(provenance));
                f.normalSearch(searches, *raw);
            }
        }
        if (playedMoveNowBest) {
            auto position = root;
            for (const auto* uci : {"d2d4", "e7e5", "g1f3"}) {
                for (const auto nodes : {20ULL, 40ULL}) {
                    f.normalSearch(searches,
                                   puzzle_test::normalPass(
                                       document, position, nodes, uci,
                                       position.sideToMove() == chess::Color::White ? 600 : 400));
                }
                QVERIFY(std::holds_alternative<std::monostate>(
                    position.play(std::get<chess::Move>(position.parseUci(uci)))));
            }
        } else {
            for (const auto& pass : document.passes) {
                f.normalSearch(searches, pass);
            }
        }
        QVERIFY(f.connectRuntime(searches));
        pgn::PgnReader reader("1. d4 *");
        const auto game =
            std::make_shared<const pgn::PgnDocument>(std::get<pgn::PgnDocument>(reader.next()));
        auto& database = f.startup.gameRepository();
        QSignalSpy inserted(&database, &persistence::Database::gameInserted);
        database.insertGame(QUuid::createUuid(), game);
        QVERIFY(inserted.wait());
        const auto added = qvariant_cast<persistence::InsertGameResult>(inserted.front()[1]);
        QVERIFY(std::holds_alternative<persistence::Inserted>(added));
        auto& review = f.startup.reviewGame();
        review.selectGame(std::get<persistence::Inserted>(added).id);
        QTRY_VERIFY(!review.state().busy);
        review.start(chess::Color::White, 1, review::Quality::Standard, false);
        QTRY_VERIFY_WITH_TIMEOUT(!review.taskState().active, 10000);
        const auto& generation = f.startup.puzzleGeneration().state();
        QTRY_VERIFY_WITH_TIMEOUT(!generation.active && generation.job &&
                                     generation.job->status ==
                                         persistence::PuzzleJobStatus::Complete,
                                 10000);
        QVERIFY2(generation.error.isEmpty(), qPrintable(generation.error));
        QCOMPARE(generation.lastSaved.has_value(), !playedMoveNowBest);
        if (playedMoveNowBest) {
            QVERIFY(f.startup.puzzles().state().library.puzzles.empty());
            return;
        }
        settings.disconnectEngine();
        QTRY_COMPARE(settings.state().runtime.phase, engine::RuntimePhase::Disconnected);

        auto& exploration = review.exploration();
        review.selectMoment(1);
        QTRY_VERIFY(exploration.state().canRetry);
        const auto* blunder = review.navigation().move(1);
        QVERIFY(blunder && blunder->classification);
        QCOMPARE(blunder->classification->label, review::Classification::Blunder);
        const auto best = std::get<chess::Move>(root.parseUci("e2e4"));
        exploration.tryAgain();
        QTRY_VERIFY(exploration.board().interactive);
        exploration.attemptMove(best, exploration.board().revision);
        QTRY_COMPARE(exploration.state().mode, app::ExplorationMode::Success);

        f.navigation.navigate(app::Destination::Puzzles);
        auto& puzzles = f.startup.puzzles();
        QTRY_COMPARE(puzzles.state().library.puzzles.size(), std::size_t{1});
        auto* solve = f.window.findChild<QPushButton*>(QStringLiteral("openPuzzle"));
        QVERIFY(solve && solve->isEnabled());
        solve->click();
        QTRY_VERIFY(puzzles.board().interactive);
        QCOMPARE(puzzles.state().message, QStringLiteral("Find the move that holds the position."));
        puzzles.attemptMove(best, puzzles.board().revision);
        QTRY_VERIFY(puzzles.state().complete);
        QCOMPARE(settings.state().runtime.phase, engine::RuntimePhase::Disconnected);
    }
    void init() {
        QTest::failOnWarning(QRegularExpression(
            QStringLiteral("^(?!This plugin does not support propagateSizeHints\\(\\)$).*")));
    }
    void offlineSolveHintsRevealAnalyzeAndDelete() {
        Fixture f;
        QVERIFY(f.readyForWork());
        const auto id = f.saveOffline();
        QVERIFY(id > 0);
        auto& puzzles = f.startup.puzzles();
        f.navigation.navigate(app::Destination::Puzzles);
        puzzles.open(id);
        QTRY_VERIFY(puzzles.board().interactive);
        const auto render = qEnvironmentVariable("CHESS_REVIEW_PUZZLE_RENDER_DIR");
        if (!render.isEmpty()) {
            QVERIFY(QDir().mkpath(render));
            f.window.resize(1100, 760);
            f.window.show();
        }
        const auto renderState = [&f, &render](const QString& filename) {
            if (!render.isEmpty()) {
                QCoreApplication::processEvents();
                QVERIFY(f.window.grab().save(QDir(render).filePath(filename)));
            }
        };
        renderState(QStringLiteral("puzzle-first-move.png"));
        // A failed background list request must neither disable the active board nor
        // leave a stale failure after the library can be read again.
        puzzles.refresh(0, -1);
        QTRY_VERIFY(!puzzles.state().libraryError.isEmpty());
        QVERIFY(puzzles.board().interactive);
        puzzles.refresh();
        QTRY_VERIFY(puzzles.state().libraryError.isEmpty());
        QTRY_COMPARE(puzzles.state().library.puzzles.size(), std::size_t{1});
        puzzles.hint();
        puzzles.hint();
        puzzles.hint();
        QCOMPARE(puzzles.state().hints.size(), 3);
        QVERIFY(!f.window.findChild<QPushButton*>(QStringLiteral("hintPuzzle"))->isEnabled());
        renderState(QStringLiteral("puzzle-hints.png"));
        const auto stored = puzzle_test::complete();
        const auto knownWeak = stored.passes.front().evidence.lines[1].pv.front();
        puzzles.attemptMove(knownWeak, puzzles.board().revision);
        QTRY_VERIFY(!puzzles.state().grading);
        QCOMPARE(puzzles.state().message, QStringLiteral("Try again."));
        QCOMPARE(puzzles.state().hints.size(), 3);
        renderState(QStringLiteral("puzzle-incorrect.png"));
        const auto legal = puzzle_test::root().legalMoves();
        const auto unknown = std::find_if(legal.begin(), legal.end(), [&](const auto& move) {
            return std::none_of(stored.passes.front().evidence.lines.begin(),
                                stored.passes.front().evidence.lines.end(),
                                [&](const auto& line) { return line.pv.front() == move; });
        });
        QVERIFY(unknown != legal.end());
        puzzles.attemptMove(*unknown, puzzles.board().revision);
        QTRY_VERIFY(!puzzles.state().grading);
        QVERIFY(puzzles.state().message.contains(QStringLiteral("Grading unavailable")));
        QCOMPARE(puzzles.state().hints.size(), 3);
        const auto mate = std::get<chess::Move>(puzzle_test::root().parseUci("d1d8"));
        puzzles.attemptMove(mate, puzzles.board().revision);
        QTRY_VERIFY(puzzles.state().complete);
        QCOMPARE(puzzles.board().status, app::BoardStatus::Checkmate);
        renderState(QStringLiteral("puzzle-complete.png"));
        puzzles.retry();
        QTRY_VERIFY(puzzles.board().interactive);
        QVERIFY(puzzles.state().hints.empty());
        puzzles.reveal();
        QTRY_VERIFY(puzzles.state().revealed && !puzzles.state().loading &&
                    puzzles.board().status != app::BoardStatus::Checkmate);
        auto* previous = f.window.findChild<QPushButton*>(QStringLiteral("previousPuzzleMove"));
        auto* next = f.window.findChild<QPushButton*>(QStringLiteral("nextPuzzleMove"));
        QVERIFY(previous && next && !previous->isEnabled() && next->isEnabled());
        next->click();
        QTRY_COMPARE(puzzles.board().status, app::BoardStatus::Checkmate);
        QVERIFY(!puzzles.state().complete);
        QVERIFY(previous->isEnabled() && !next->isEnabled());
        previous->click();
        QTRY_VERIFY(puzzles.board().status != app::BoardStatus::Checkmate);
        puzzles.openAnalysis();
        QTRY_VERIFY(!puzzles.linkedAnalysis().busy() && puzzles.linkedAnalysis().position());
        auto* back = f.window.findChild<QPushButton*>(QStringLiteral("backToPuzzle"));
        QVERIFY(back);
        back->click();
        QCOMPARE(f.navigation.current(), app::Destination::Puzzles);
        QVERIFY(puzzles.state().revealed);
        f.window.resize(1100, 760);
        f.window.show();
        if (f.window.width() >= 760 && f.window.height() >= 710) {
            auto* analyze = f.window.findChild<QPushButton*>(QStringLiteral("analyzePuzzle"));
            QVERIFY(analyze);
            QTRY_VERIFY(f.window.rect().contains(
                QRect(analyze->mapTo(&f.window, QPoint{}), analyze->size())));
        }
        renderState(QStringLiteral("puzzle-revealed.png"));
        puzzles.remove(id);
        QTRY_VERIFY(!puzzles.state().selected);
        QTRY_VERIFY(puzzles.state().library.puzzles.empty());
    }
    void reopeningAfterLeavingDuringReconstruction() {
        Fixture f;
        QVERIFY(f.readyForWork());
        const auto id = f.saveOffline();
        QVERIFY(id > 0);
        auto& puzzles = f.startup.puzzles();
        bool left = false;
        connect(&puzzles, &app::PuzzleController::changed, this, [&] {
            if (!left && puzzles.state().loading && puzzles.state().selected == id) {
                left = true;
                puzzles.showLibrary();
            }
        });
        puzzles.open(id);
        QTRY_VERIFY(left);
        QVERIFY(!puzzles.state().solving);
        puzzles.open(id);
        QTRY_VERIFY(puzzles.board().interactive);
        QCOMPARE(puzzles.state().selected, id);
    }
    void locallyProvenMateOutsideSavedSolutionsWorksOffline() {
        Fixture f;
        QVERIFY(f.readyForWork());
        const auto root = std::get<chess::Position>(
            chess::Position::fromFen("6k1/5ppp/8/8/8/8/5PPP/R2R2K1 w - - 0 1"));
        const auto alternate = std::get<chess::Move>(root.parseUci("a1a8"));
        auto d = puzzle_test::seed();
        d.definition.position = root.context();
        for (const auto nodes : {20ULL, 40ULL}) {
            auto pass = puzzle_test::normalPass(d, root, nodes, "d1d8", 900, alternate);
            pass.evidence.lines.front().score =
                engine::AnalysisScore{engine::ScoreType::Mate, 1, engine::ScoreBound::Exact};
            pass.evidence.lines.front().wdl = engine::Wdl{1000, 0, 0};
            d.passes.push_back(std::move(pass));
        }
        const auto verified = std::get<puzzles::Verification>(persistence::verifyPuzzle(d));
        QCOMPARE(verified.outcome, puzzles::Outcome::Accepted);
        d.solution = verified.solution;
        QCOMPARE(d.solution.nodes.front().moves.size(), std::size_t{1});
        const auto id = f.saveOffline(d);
        QVERIFY(id > 0);
        auto& puzzles = f.startup.puzzles();
        puzzles.open(id);
        QTRY_VERIFY(puzzles.board().interactive);
        puzzles.attemptMove(alternate, puzzles.board().revision);
        QTRY_VERIFY(puzzles.state().complete);
        QCOMPARE(puzzles.board().status, app::BoardStatus::Checkmate);
    }
    void manualGenerationVerifiesAndPersistsBeforeCompleting() {
        Fixture f;
        QVERIFY(f.readyForWork());
        QVERIFY(f.validate());
        QJsonArray searches;
        f.finite(searches, 20);
        f.finite(searches, 40);
        QVERIFY(f.connectRuntime(searches));
        auto& generation = f.startup.puzzleGeneration();
        generation.savePosition(std::make_shared<const chess::Position>(puzzle_test::root()));
        QTRY_VERIFY_WITH_TIMEOUT(!generation.state().active, 10000);
        QVERIFY2(generation.state().error.isEmpty(), qPrintable(generation.state().error));
        QVERIFY(generation.state().lastSaved);
        auto& puzzles = f.startup.puzzles();
        f.startup.engines().disconnectEngine();
        puzzles.open(*generation.state().lastSaved);
        QTRY_VERIFY(puzzles.board().interactive);
        puzzles.attemptMove(std::get<chess::Move>(puzzle_test::root().parseUci("d1d8")),
                            puzzles.board().revision);
        QTRY_VERIFY(puzzles.state().complete);
    }
    void cancelledVerificationKeepsCompletedSearchAndResumesExplicitly() {
        Fixture f;
        QVERIFY(f.readyForWork());
        QVERIFY(f.validate());
        QJsonArray searches;
        f.finite(searches, 20);
        f.finite(searches, 40, 0, true);
        QVERIFY(f.connectRuntime(searches));
        QSignalSpy scheduled(&f.supervisor(), &engine::EngineSupervisor::scheduledChanged);
        auto& generation = f.startup.puzzleGeneration();
        generation.savePosition(std::make_shared<const chess::Position>(puzzle_test::root()));
        QTRY_VERIFY_WITH_TIMEOUT(
            std::count_if(scheduled.begin(), scheduled.end(),
                          [](const auto& entry) {
                              return qvariant_cast<engine::ScheduledState>(entry[0]).phase ==
                                     engine::ScheduledPhase::Analyzing;
                          }) >= 2,
            10000);
        generation.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!generation.state().active, 10000);
        QVERIFY(generation.state().job);
        const auto jobId = generation.state().job->id;
        QSignalSpy loaded(&f.startup.gameRepository(), &persistence::Database::puzzleJobLoaded);
        f.startup.gameRepository().loadPuzzleJob(QUuid::createUuid(), jobId);
        QVERIFY(loaded.wait());
        const auto result = qvariant_cast<persistence::SavePuzzleJobResult>(loaded.back()[1]);
        QVERIFY(std::holds_alternative<persistence::PuzzleJob>(result));
        const auto job = std::get<persistence::PuzzleJob>(result);
        QCOMPARE(job.status, persistence::PuzzleJobStatus::Paused);
        QCOMPARE(job.candidates.front().passes.size(), std::size_t{1});
        f.startup.engines().disconnectEngine();
        QTRY_COMPARE(f.startup.engines().state().runtime.phase, engine::RuntimePhase::Disconnected);
        QJsonArray remaining;
        f.finite(remaining, 40);
        QVERIFY(f.connectRuntime(remaining));
        generation.resume(jobId);
        QTRY_VERIFY_WITH_TIMEOUT(!generation.state().active, 10000);
        QVERIFY2(generation.state().error.isEmpty(), qPrintable(generation.state().error));
        QVERIFY(generation.state().lastSaved);
    }
    void strongUnlistedAlternativeGetsACompleteVerifiedContinuation() {
        Fixture f;
        QVERIFY(f.readyForWork());
        QVERIFY(f.validate());
        const auto fingerprint = std::get<engine::EngineFingerprint>(
            engine::quickReviewFingerprint(f.startup.engines().state().profiles.front().engine));
        const auto document = puzzle_test::normalPuzzle(fingerprint);
        const auto id = f.saveOffline(document);
        QVERIFY(id > 0);
        auto position = chess::Position::initial();
        const auto alternative = std::get<chess::Move>(position.parseUci("d2d4"));
        QVERIFY(std::holds_alternative<std::monostate>(position.play(alternative)));
        QJsonArray searches;
        for (const auto nodes : {20ULL, 40ULL}) {
            f.normalSearch(searches,
                           puzzle_test::normalPass(document, position, nodes, "d7d5", 400));
        }
        QVERIFY(std::holds_alternative<std::monostate>(
            position.play(std::get<chess::Move>(position.parseUci("d7d5")))));
        for (const auto nodes : {20ULL, 40ULL}) {
            f.normalSearch(searches,
                           puzzle_test::normalPass(document, position, nodes, "g1f3", 600));
        }
        QVERIFY(f.connectRuntime(searches));
        auto& puzzles = f.startup.puzzles();
        puzzles.open(id);
        QTRY_VERIFY(puzzles.board().interactive);
        puzzles.attemptMove(alternative, puzzles.board().revision);
        QTRY_VERIFY_WITH_TIMEOUT(puzzles.state().complete, 10000);
        QVERIFY(puzzles.state().error.isEmpty());
        QCOMPARE(puzzles.board().sideToMove, chess::Color::White);
        QVERIFY(puzzles.board().lastMove);
        QCOMPARE(puzzles.board().lastMove->from.name(), std::string("d7"));
        QCOMPARE(puzzles.board().lastMove->to.name(), std::string("d5"));
        puzzles.retry();
        QTRY_VERIFY(puzzles.board().interactive);
        f.startup.engines().disconnectEngine();
        puzzles.attemptMove(alternative, puzzles.board().revision);
        QTRY_VERIFY(puzzles.state().complete);
    }
    void weakEngineOutputCannotCreatePuzzle() {
        Fixture f;
        QVERIFY(f.readyForWork());
        QVERIFY(f.validate());
        QJsonArray searches;
        f.finite(searches, 20, 0, false, true);
        QVERIFY(f.connectRuntime(searches));
        auto& generation = f.startup.puzzleGeneration();
        generation.savePosition(std::make_shared<const chess::Position>(puzzle_test::root()));
        QTRY_VERIFY_WITH_TIMEOUT(!generation.state().active, 10000);
        QVERIFY(!generation.state().lastSaved);
        QVERIFY(generation.state().job);
        QCOMPARE(generation.state().job->status, persistence::PuzzleJobStatus::Complete);
        f.startup.puzzles().refresh();
        QTRY_VERIFY(f.startup.puzzles().state().library.puzzles.empty());
    }
    void insufficientEngineWidthSkipsTheCandidate() {
        Fixture f;
        QVERIFY(f.readyForWork());
        QVERIFY(f.validate(3));
        QJsonArray searches;
        f.finite(searches, 20);
        QVERIFY(f.connectRuntime(searches));
        auto& generation = f.startup.puzzleGeneration();
        generation.savePosition(std::make_shared<const chess::Position>(puzzle_test::root()));
        QTRY_VERIFY_WITH_TIMEOUT(!generation.state().active, 10000);
        QVERIFY2(generation.state().error.isEmpty(), qPrintable(generation.state().error));
        QVERIFY(!generation.state().lastSaved);
        QVERIFY(generation.state().job);
        QCOMPARE(generation.state().job->status, persistence::PuzzleJobStatus::Complete);
    }
    void failedOpenDoesNotExposeThePreviousPuzzle() {
        Fixture f;
        QVERIFY(f.readyForWork());
        const auto id = f.saveOffline();
        QVERIFY(id > 0);
        auto& puzzles = f.startup.puzzles();
        puzzles.open(id);
        QTRY_VERIFY(puzzles.board().interactive);
        puzzles.open(id + 100);
        QTRY_VERIFY(!puzzles.state().error.isEmpty());
        QVERIFY(!puzzles.state().selected);
        QVERIFY(!puzzles.state().solving);
        QVERIFY(!puzzles.board().interactive);
        puzzles.open(id);
        QTRY_VERIFY(puzzles.board().interactive);
    }
    void staleAnalysisApprovalHonorsNewerNavigation_data() {
        QTest::addColumn<bool>("differentDestination");
        QTest::newRow("library") << false;
        QTest::newRow("another-destination") << true;
    }
    void staleAnalysisApprovalHonorsNewerNavigation() {
        QFETCH(bool, differentDestination);
        Fixture f;
        QVERIFY(f.readyForWork());
        const auto id = f.saveOffline();
        QVERIFY(id > 0);
        auto& puzzles = f.startup.puzzles();
        f.navigation.navigate(app::Destination::Puzzles);
        puzzles.open(id);
        QTRY_VERIFY(puzzles.board().interactive);
        puzzles.openAnalysis();
        auto& linked = puzzles.linkedAnalysis();
        QTRY_VERIFY(!linked.busy() && linked.position());
        linked.attemptMove(std::get<chess::Move>(puzzle_test::root().parseUci("h2h3")),
                           linked.board().revision);
        QTRY_VERIFY(!linked.busy() && linked.hasContent());
        const auto edited = linked.currentNode();
        f.window.findChild<QPushButton*>(QStringLiteral("backToPuzzle"))->click();
        f.window.show();
        puzzles.openAnalysis();
        auto* dialog = f.window.findChild<QMessageBox*>(QStringLiteral("discardAnalysisDialog"));
        QTRY_VERIFY(dialog && dialog->isVisible());
        const auto expected =
            differentDestination ? app::Destination::Games : app::Destination::Puzzles;
        if (differentDestination) {
            f.navigation.navigate(expected);
        } else {
            puzzles.showLibrary();
        }
        dialog->button(QMessageBox::Discard)->click();
        QCoreApplication::processEvents();
        QCOMPARE(f.navigation.current(), expected);
        QCOMPARE(linked.currentNode(), edited);
        if (!differentDestination) {
            QVERIFY(!puzzles.state().solving);
        }
    }
};
int main(int argc, char** argv) {
    TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QApplication application(argc, argv);
    PuzzleTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "PuzzleTest.moc"
