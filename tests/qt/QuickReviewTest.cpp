#include "EngineControllerTestAccess.h"
#include "EngineTestSupport.h"
#include "TestEnvironment.h"
#include "app/ReviewMovePresentation.h"
#include "app/ReviewWorkspaceController.h"
#include "app/StartupCoordinator.h"
#include "engine/EffectiveSearch.h"
#include "engine/private/SearchProtocol.h"
#include "ui/chess/ChessBoardWidget.h"
#include "ui/chess/MoveTimeline.h"
#include "ui/shell/MainWindow.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <utility>
#include <vector>
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
enum class RetryScript { None, Unknown, Played, Crash };
class Fixture {
  public:
    explicit Fixture(const QString& path = {},
                     const std::optional<QString>& configuration = std::nullopt,
                     const std::optional<QString>& uiState = std::nullopt)
        : window(navigation, startup.analysis()) {
        window.installEngines(startup.engines());
        window.installEngineAnalysis(startup.engineAnalysis());
        window.installImports(startup.localImports(), startup.library());
        window.installReview(startup.reviewGame());
        QObject::connect(&startup, &app::StartupCoordinator::ready, &window,
                         &ui::MainWindow::showReady);
        if (uiState) {
            QObject::connect(&startup, &app::StartupCoordinator::initialUiState, &window,
                             &ui::MainWindow::restoreUiState);
        }
        startup.start(path.isEmpty() ? directory.filePath(QStringLiteral("review.sqlite3")) : path,
                      uiState, configuration);
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
    bool seed(const std::string& text = "1. e4 e5 *", bool multiPv = false) {
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
            engine_test::scenario(
                directory.filePath(QStringLiteral("validate.json")),
                engine_test::handshake(
                    multiPv ? QStringLiteral("id name Test engine\nid author Tests\noption name "
                                             "MultiPV type spin default 1 min 1 max 3\nuciok\n")
                            : QStringLiteral("id name Test engine\nid author Tests\nuciok\n"))),
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
                const QString& identity = QStringLiteral("Test engine"), int quitDelay = 0,
                bool multiPv = false, const QString& refinedScore = {}, int deepFirst = 0,
                int verifyFirst = 0, bool boundedFinal = false,
                RetryScript retry = RetryScript::None, const std::vector<int>& refinementOrder = {},
                const std::vector<int>& whiteScores = {},
                const std::vector<int>& verificationOrder = {}, int extraRounds = -1,
                const std::vector<QString>& stageScores = {},
                const std::array<std::vector<int>, 2>& escalationOrders = {}) {
        const bool retrySearches = retry != RetryScript::None;
        QJsonArray steps{
            engine_test::step(
                QStringLiteral("uci"),
                {engine_test::write(
                    QStringLiteral("id name %1\nid author Tests\n%2uciok\n")
                        .arg(identity,
                             multiPv ? QStringLiteral(
                                           "option name MultiPV type spin default 1 min 1 max 3\n")
                                     : QString{}))}),
            ready()};
        engine::Capabilities capabilities;
        if (multiPv) {
            capabilities.supportsMultiPv = true;
            capabilities.options.push_back({QStringLiteral("MultiPV"),
                                            engine::OptionType::Spin,
                                            QStringLiteral("1"),
                                            1,
                                            3,
                                            {}});
        }
        auto position = std::get<chess::Position>(document->tree.position(document->tree.rootId()));
        const auto moves = document->originalMainline();
        const int total = static_cast<int>(moves.size()) + 1;
        const int additional = extraRounds >= 0
                                   ? extraRounds
                                   : ((weak || score.isEmpty()) && refinedScore.isEmpty() ? 2 : 0);
        const int stages = end < 0 && total > 1 ? 3 + additional : 1;
        for (int stage = 0; stage < stages; ++stage) {
            const std::array<std::uint64_t, 5> budgets{100000, 1000000, 5000000, 10000000,
                                                       20000000};
            const auto budget = budgets[static_cast<std::size_t>(stage)];
            position = std::get<chess::Position>(document->tree.position(document->tree.rootId()));
            const auto& stageOrder =
                stage >= 3 && !escalationOrders[static_cast<std::size_t>(stage - 3)].empty()
                    ? escalationOrders[static_cast<std::size_t>(stage - 3)]
                : stage == 2 && !verificationOrder.empty() ? verificationOrder
                                                           : refinementOrder;
            const int count =
                stage > 0 && !stageOrder.empty() ? static_cast<int>(stageOrder.size()) : total;
            for (int ordinal = 0; ordinal < count; ++ordinal) {
                const int index = stage > 0 && !stageOrder.empty()
                                      ? stageOrder[static_cast<std::size_t>(ordinal)]
                                      : ordinal;
                if (stage > 0 && !stageOrder.empty()) {
                    position =
                        std::get<chess::Position>(document->tree.position(document->tree.rootId()));
                    for (int ply = 0; ply < index; ++ply) {
                        (void)position.play(moves[static_cast<std::size_t>(ply)]);
                    }
                }
                if ((stage == 0 ? (index >= first && (end < 0 || index < end))
                                : index >= (stage == 1 ? deepFirst : verifyFirst)) &&
                    !position.legalMoves().empty()) {
                    engine::AnalysisRequest request;
                    request.id = QUuid::createUuid();
                    request.position = position;
                    request.wantWdl = true;
                    request.limit = engine::NodeLimit{budget};
                    request.multiPv = multiPv && stage > 0 ? 3 : 1;
                    const auto prepared = std::get<engine::PreparedSearch>(
                        engine::prepareSearch(request, capabilities));
                    const auto best =
                        std::get<std::string>(position.uci(position.legalMoves().front()));
                    for (const auto& command : prepared.options.split('\n')) {
                        if (!command.isEmpty()) {
                            steps.append(engine_test::step(QString::fromUtf8(command), {}));
                        }
                    }
                    steps.append(engine_test::step(QStringLiteral("ucinewgame"), {}));
                    steps.append(ready());
                    steps.append(
                        engine_test::step(QString::fromUtf8(prepared.position).trimmed(), {}));
                    if (crash) {
                        steps.append(engine_test::step(
                            QStringLiteral("go nodes %1").arg(budget),
                            {QJsonObject{{QStringLiteral("type"), QStringLiteral("crash")}}}));
                    } else {
                        QString output;
                        const bool weakStage = weak && (stage == 0 || refinedScore.isEmpty());
                        auto evaluation =
                            stage > 0 && !refinedScore.isEmpty() ? refinedScore : score;
                        if (static_cast<std::size_t>(stage) < stageScores.size()) {
                            evaluation = stageScores[static_cast<std::size_t>(stage)];
                        }
                        if (!whiteScores.empty()) {
                            const int white = whiteScores[static_cast<std::size_t>(index)];
                            const int wins =
                                position.sideToMove() == chess::Color::White ? white : 1000 - white;
                            evaluation =
                                QStringLiteral("cp 0 wdl %1 0 %2").arg(wins).arg(1000 - wins);
                        }
                        const auto legal = position.legalMoves();
                        if (!evaluation.isEmpty()) {
                            for (int rank = 1; rank <= prepared.effectiveMultiPv; ++rank) {
                                const auto root = std::get<std::string>(
                                    position.uci(legal[static_cast<std::size_t>(rank - 1)]));
                                auto pv = QString::fromStdString(root);
                                if (retrySearches) {
                                    auto continuation = position;
                                    (void)continuation.play(
                                        legal[static_cast<std::size_t>(rank - 1)]);
                                    for (int step = 0;
                                         step < 2 && !continuation.legalMoves().empty(); ++step) {
                                        const auto next = continuation.legalMoves().front();
                                        pv += QLatin1Char(' ') +
                                              QString::fromStdString(
                                                  std::get<std::string>(continuation.uci(next)));
                                        (void)continuation.play(next);
                                    }
                                }
                                output +=
                                    QStringLiteral(
                                        "info depth 7 nodes %1 multipv %2 score %3%4 pv %5\n")
                                        .arg(weakStage      ? 1
                                             : boundedFinal ? budget / 2
                                                            : budget)
                                        .arg(rank)
                                        .arg(evaluation)
                                        .arg(weakStage ? QStringLiteral(" lowerbound") : QString{})
                                        .arg(pv);
                            }
                        }
                        auto finalBest = best;
                        if (boundedFinal && !legal.empty()) {
                            finalBest = std::get<std::string>(
                                position.uci(legal[legal.size() > 1 ? 1 : 0]));
                            output += QStringLiteral("info depth 8 nodes %1 multipv 1 score cp 90 "
                                                     "lowerbound pv %2\n")
                                          .arg(budget)
                                          .arg(QString::fromStdString(finalBest));
                        }
                        output +=
                            QStringLiteral("bestmove %1\n").arg(QString::fromStdString(finalBest));
                        steps.append(engine_test::step(QStringLiteral("go nodes %1").arg(budget),
                                                       {engine_test::write(output, delay)}));
                        steps.append(ready());
                    }
                }
                if (index < static_cast<int>(moves.size())) {
                    (void)position.play(moves[static_cast<std::size_t>(index)]);
                }
            }
        }
        if (retrySearches) {
            auto root = std::get<chess::Position>(document->tree.position(document->tree.rootId()));
            (void)root.play(moves.front());
            const auto candidate =
                retry == RetryScript::Played ? moves[1] : root.legalMoves().at(3);
            (void)root.play(candidate);
            for (const auto budget : {1000000U, 5000000U}) {
                engine::AnalysisRequest request;
                request.id = QUuid::createUuid();
                request.position = root;
                request.wantWdl = true;
                request.limit = engine::NodeLimit{budget};
                const auto prepared =
                    std::get<engine::PreparedSearch>(engine::prepareSearch(request, capabilities));
                for (const auto& command : prepared.options.split('\n')) {
                    if (!command.isEmpty()) {
                        steps.append(engine_test::step(QString::fromUtf8(command), {}));
                    }
                }
                steps.append(engine_test::step(QStringLiteral("ucinewgame"), {}));
                steps.append(ready());
                steps.append(engine_test::step(QString::fromUtf8(prepared.position).trimmed(), {}));
                if (retry == RetryScript::Crash) {
                    steps.append(engine_test::step(
                        QStringLiteral("go nodes %1").arg(budget),
                        {QJsonObject{{QStringLiteral("type"), QStringLiteral("crash")}}}));
                } else {
                    const auto best = std::get<std::string>(root.uci(root.legalMoves().front()));
                    steps.append(engine_test::step(
                        QStringLiteral("go nodes %1").arg(budget),
                        {engine_test::write(
                            QStringLiteral(
                                "info depth 7 nodes %1 score cp -200 pv %2\nbestmove %2\n")
                                .arg(budget)
                                .arg(QString::fromStdString(best)))}));
                }
                steps.append(ready());
            }
        }
        steps.append(engine_test::step(
            QStringLiteral("quit"), {QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")},
                                                 {QStringLiteral("afterMs"), quitDelay}}}));
        engine::ValidatorTestAccess::configure(
            supervisor(),
            engine_test::scenario(directory.filePath(QStringLiteral("runtime.json")), steps), 1000);
        engine::ValidatorTestAccess::configureSearch(supervisor(), 10000, 1500);
    }
    void run(review::Quality quality = review::Quality::Standard) {
        startup.reviewGame().start(chess::Color::Black, profileId, quality);
    }
    bool done(int timeout = 12000) {
        return QTest::qWaitFor(
            [this] {
                return !startup.reviewGame().state().active && !startup.reviewGame().state().busy;
            },
            timeout);
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
    void explicitStartupNavigationSuppressesLegacyAndReviewRestore() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        f.startup.reviewGame().workspace().setVisible(true);
        f.shutdown();
        const auto stateFile = f.directory.filePath(QStringLiteral("ui-state.ini"));
        {
            QSettings settings(stateFile, QSettings::IniFormat);
            settings.setValue(QStringLiteral("workspace/v1/gameId"), static_cast<qint64>(f.gameId));
            settings.setValue(QStringLiteral("workspace/v1/node"),
                              QString::number(f.document->tree.rootId().value));
            settings.sync();
            QCOMPARE(settings.status(), QSettings::NoError);
        }
        Fixture reopened(f.directory.filePath(QStringLiteral("review.sqlite3")), std::nullopt,
                         stateFile);
        QSignalSpy recovered(&reopened.startup.reviewGame().workspace(),
                             &app::ReviewWorkspaceController::recoveryFinished);
        reopened.window.findChild<QAction*>(QStringLiteral("homeAction"))->trigger();
        QTRY_VERIFY(!recovered.empty());
        QVERIFY(!reopened.startup.library().opening());
        QVERIFY(!reopened.startup.analysis().busy());
        QVERIFY(!reopened.startup.analysis().gameId());
        QVERIFY(!reopened.startup.reviewGame().state().gameId);
        QCOMPARE(reopened.navigation.current(), app::Destination::Home);
    }
    void historyRefreshCannotReplacePendingInspection() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        auto& controller = f.startup.reviewGame();
        const auto id = controller.state().run->id;
        controller.inspect(id);
        controller.refreshRuns();
        QTRY_VERIFY(!controller.state().busy);
        QVERIFY(controller.state().run);
        QCOMPARE(controller.state().run->id, id);
    }
    void resumeDifferentRunRebindsTheWorkspace() {
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, 1);
        auto& controller = f.startup.reviewGame();
        const auto cancel =
            connect(&controller, &app::ReviewGameController::changed, &controller, [&] {
                if (controller.state().active && controller.state().run &&
                    controller.state().run->completedPositions == 1) {
                    controller.cancel();
                }
            });
        f.run();
        QVERIFY(f.done());
        disconnect(cancel);
        const auto first = *controller.state().run;
        QCOMPARE(first.status, review::JobStatus::Cancelled);
        f.script();
        f.run();
        QVERIFY(f.done());
        QVERIFY(controller.state().run->id != first.id);
        f.script(1);
        bool nested = false;
        const auto nestedRead =
            connect(&controller, &app::ReviewGameController::changed, &f.window, [&] {
                if (nested || !controller.state().inspecting) {
                    return;
                }
                nested = true;
                QEventLoop loop;
                const auto done =
                    connect(&controller, &app::ReviewGameController::changed, &loop, [&] {
                        if (controller.state().run && controller.state().run->id == first.id &&
                            !controller.state().inspecting) {
                            loop.quit();
                        }
                    });
                QTimer::singleShot(5000, &loop, &QEventLoop::quit);
                loop.exec();
                disconnect(done);
            });
        controller.resume(first.id);
        disconnect(nestedRead);
        QVERIFY(nested);
        QTRY_VERIFY_WITH_TIMEOUT(controller.taskState().run &&
                                     controller.taskState().run->id == first.id &&
                                     !controller.taskState().busy,
                                 15000);
        QCOMPARE(controller.taskState().run->id, first.id);
        QVERIFY(controller.state().run);
        QCOMPARE(controller.state().run->id, first.id);
        QCOMPARE(controller.state().run->status, review::JobStatus::Complete);
    }
    void invalidStartPreservesTheInspectedRun() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        auto& controller = f.startup.reviewGame();
        const auto id = controller.state().run->id;
        // Deliberately pass an invalid enum to verify boundary rejection.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        controller.start(static_cast<chess::Color>(99), f.profileId, review::Quality::Standard);
        QVERIFY(controller.state().run);
        QCOMPARE(controller.state().run->id, id);
        // Deliberately pass an invalid enum to verify boundary rejection.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        controller.start(chess::Color::Black, f.profileId, static_cast<review::Quality>(99));
        QVERIFY(controller.state().run);
        QCOMPARE(controller.state().run->id, id);
    }
    void reentrantGameSelectionKeepsNewestIntent() {
        Fixture f;
        QVERIFY(f.seed());
        auto& db = f.startup.gameRepository();
        pgn::PgnReader reader("1. d4 *");
        auto document =
            std::make_shared<const pgn::PgnDocument>(std::get<pgn::PgnDocument>(reader.next()));
        QSignalSpy inserted(&db, &persistence::Database::gameInserted);
        db.insertGame(QUuid::createUuid(), document);
        QTRY_VERIFY(!inserted.empty());
        const auto other = std::get<persistence::Inserted>(
                               qvariant_cast<persistence::InsertGameResult>(inserted.front()[1]))
                               .id;
        auto& controller = f.startup.reviewGame();
        bool armed = true;
        const auto selectionObserver = connect(
            &controller.navigation(), &app::ReviewNavigationController::changed, &controller, [&] {
                if (std::exchange(armed, false)) {
                    controller.selectGame(other);
                }
            });
        const auto disconnectSelection = qScopeGuard([&] { disconnect(selectionObserver); });
        controller.selectGame(f.gameId);
        QTRY_VERIFY(!controller.state().busy);
        QCOMPARE(controller.state().gameId, other);
        QVERIFY(controller.state().game.contains(QString::number(other)));
    }
    void hiddenLibraryDoesNotDeriveReviewSummaries() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        f.navigation.navigate(app::Destination::Games);
        auto& library = f.startup.library();
        QTRY_VERIFY(library.reviewAccuracy(f.gameId).contains(QLatin1Char('%')));
        f.navigation.navigate(app::Destination::Home);
        QSignalSpy metadata(&f.startup.gameRepository(),
                            &persistence::Database::libraryReviewsListed);
        QSignalSpy summaries(&f.startup.gameRepository(),
                             &persistence::Database::reviewOverviewLoaded);
        library.importedGame();
        QTest::qWait(400);
        QVERIFY(metadata.empty());
        QVERIFY(summaries.empty());
        f.navigation.navigate(app::Destination::Games);
        QTRY_VERIFY(!metadata.empty());
        QTRY_VERIFY(!summaries.empty());
    }
    void missingContextShowsAnErrorWithoutSelectingAnotherRun() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        auto& controller = f.startup.reviewGame();
        controller.inspect(999999);
        QTRY_VERIFY(!controller.state().busy && !controller.state().error.isEmpty());
        QCOMPARE(controller.state().gameId, f.gameId);
        QVERIFY(!controller.state().run);
        QVERIFY(controller.navigation().ready());
        QVERIFY(!controller.state().runs.empty());
        controller.selectGame(999999);
        QTRY_VERIFY(!controller.state().busy && !controller.state().error.isEmpty());
        QCOMPARE(f.navigation.current(), app::Destination::Games);
        auto* notice = f.window.findChild<QLabel*>(QStringLiteral("uiStateNotice"));
        QVERIFY(notice);
        QVERIFY(!notice->text().isEmpty());
        QVERIFY(!notice->isHidden());
    }
    void interruptedReviewsAreDiscoverableAndPaged() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        auto& db = f.startup.gameRepository();
        QSignalSpy loaded(&db, &persistence::Database::reviewLoaded);
        db.loadReview(QUuid::createUuid(), f.startup.reviewGame().state().run->id);
        QTRY_VERIFY(!loaded.empty());
        const auto settings = std::get<persistence::StoredReview>(
                                  qvariant_cast<persistence::LoadReviewResult>(loaded.front()[1]))
                                  .settings;
        QSignalSpy created(&db, &persistence::Database::reviewCreated);
        for (int i = 0; i < 51; ++i) {
            created.clear();
            db.createReview(QUuid::createUuid(), settings);
            QTRY_VERIFY(!created.empty());
            QVERIFY(std::holds_alternative<persistence::ReviewSummary>(
                qvariant_cast<persistence::CreateReviewResult>(created.front()[1])));
        }
        f.shutdown();
        Fixture reopened(f.directory.filePath(QStringLiteral("review.sqlite3")));
        auto& workspace = reopened.startup.reviewGame().workspace();
        QTRY_COMPARE(workspace.interrupted().size(), std::size_t{50});
        QVERIFY(workspace.recoveryHasNext());
        QCOMPARE(reopened.startup.engines().state().runtime.phase,
                 engine::RuntimePhase::Disconnected);
        workspace.refreshRecovery(1);
        QTRY_VERIFY(!workspace.recoveryBusy());
        QCOMPARE(workspace.interrupted().size(), std::size_t{1});
        QVERIFY(!workspace.recoveryHasNext());
        const auto last = workspace.interrupted().front();
        QSignalSpy continued(&reopened.startup.gameRepository(),
                             &persistence::Database::reviewContinued);
        reopened.startup.gameRepository().continueReview(
            QUuid::createUuid(),
            {last.id, last.revision, settings.fingerprint, settings.configuration});
        QTRY_VERIFY(!continued.empty());
        QVERIFY(std::holds_alternative<persistence::ReviewSummary>(
            qvariant_cast<persistence::ContinueReviewResult>(continued.front()[1])));
        workspace.refreshRecovery(1);
        QTRY_VERIFY(!workspace.recoveryBusy());
        QCOMPARE(workspace.recoveryPage(), std::int64_t{0});
        QCOMPARE(workspace.interrupted().size(), std::size_t{50});

        workspace.refreshRecovery();
        QTRY_COMPARE(workspace.interrupted().size(), std::size_t{50});
        reopened.navigation.navigate(app::Destination::Home);
        reopened.window.resize(1100, 720);
        reopened.window.show();
        auto* recovery = reopened.window.findChild<QWidget*>(QStringLiteral("reviewRecovery"));
        QVERIFY(recovery);
        QTRY_VERIFY(recovery->isVisible());
        auto* list = reopened.window.findChild<QWidget*>(QStringLiteral("interruptedReviewList"));
        auto* show =
            reopened.window.findChild<QPushButton*>(QStringLiteral("showInterruptedReviews"));
        QVERIFY(list);
        QVERIFY(show);
        QVERIFY(list->isHidden());
        show->setFocus();
        QTest::keyClick(show, Qt::Key_Space);
        QVERIFY(!list->isHidden());
        show->click();
        QVERIFY(list->isHidden());
        auto& library = reopened.startup.library();
        library.activate();
        QTRY_VERIFY(library.reviewStatus(f.gameId).contains(
            QStringLiteral("interrupted review available")));
        QTRY_VERIFY(library.reviewAccuracy(f.gameId).contains(QLatin1Char('%')));
        const auto output = qEnvironmentVariable("CHESS_REVIEW_QUICK_RENDER_DIR");
        if (!output.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(reopened.window.grab().save(output + QStringLiteral("/recovery-home.png")));
            show->click();
            QCoreApplication::processEvents();
            QVERIFY(reopened.window.grab().save(output + QStringLiteral("/recovery-list.png")));
            show->click();
            reopened.navigation.navigate(app::Destination::Games);
            QCoreApplication::processEvents();
            auto* table = reopened.window.findChild<QTableView*>(QStringLiteral("gamesTable"));
            QVERIFY(table);
            QVERIFY(table->columnViewportPosition(7) + table->columnWidth(7) <=
                    table->viewport()->width());
            QVERIFY(reopened.window.grab().save(output + QStringLiteral("/recovery-library.png")));
            auto palette = reopened.window.palette();
            palette.setColor(QPalette::Window, QColor("#202732"));
            palette.setColor(QPalette::WindowText, QColor("#e5eaf0"));
            palette.setColor(QPalette::Base, QColor("#171e27"));
            palette.setColor(QPalette::Text, QColor("#e5eaf0"));
            palette.setColor(QPalette::Button, QColor("#303b49"));
            palette.setColor(QPalette::ButtonText, QColor("#e5eaf0"));
            reopened.window.setPalette(palette);
            auto font = reopened.window.font();
            font.setPointSize(font.pointSize() + 5);
            reopened.window.setFont(font);
            reopened.navigation.navigate(app::Destination::Home);
            if (!show->isChecked()) {
                show->click();
            }
            QCoreApplication::processEvents();
            QVERIFY(reopened.window.grab().save(
                output + QStringLiteral("/recovery-home-dark-enlarged.png")));
        }
        reopened.navigation.navigate(app::Destination::Home);
        if (!show->isChecked()) {
            show->click();
        }
        auto* rows =
            reopened.window.findChild<QListWidget*>(QStringLiteral("interruptedReviewRows"));
        QVERIFY(rows);
        QCOMPARE(rows->count(), 50);
        rows->setCurrentRow(1);
        const auto chosen = rows->currentItem()->data(Qt::UserRole).toLongLong();
        rows->setFocus();
        QTRY_VERIFY(rows->hasFocus());
        workspace.refreshRecovery();
        QTRY_VERIFY(!workspace.recoveryBusy());
        QVERIFY(rows->hasFocus());
        QCOMPARE(rows->currentItem()->data(Qt::UserRole).toLongLong(), chosen);
        rows->setFocus();
        QTest::keyClick(rows, Qt::Key_Return);
        QTRY_VERIFY(reopened.startup.reviewGame().state().run &&
                    reopened.startup.reviewGame().state().run->id == chosen);
        QCOMPARE(reopened.startup.engines().state().runtime.phase,
                 engine::RuntimePhase::Disconnected);
    }
    void newerSelectionWinsOverPendingRestoreAndHoverDoesNotSave() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        auto& controller = f.startup.reviewGame();
        const auto run = controller.state().run->id;
        QSignalSpy saved(&controller.workspace(),
                         &app::ReviewWorkspaceController::contextCommitted);
        controller.selectMoment(1);
        QTRY_COMPARE(controller.navigation().displayed(), 1);
        QTRY_VERIFY(!saved.empty());
        controller.inspect(run);
        controller.navigation().select(controller.navigation().target(2));
        QTRY_VERIFY(!controller.state().busy);
        QTRY_COMPARE(controller.navigation().displayed(), 2);
        saved.clear();
        controller.selectMoment(1);
        QTRY_VERIFY(!saved.empty());
        controller.navigation().preview(controller.navigation().target(2));
        QTRY_COMPARE(controller.navigation().displayed(), 2);
        QSignalSpy contexts(&f.startup.gameRepository(),
                            &persistence::Database::reviewContextLoaded);
        const auto request = QUuid::createUuid();
        f.startup.gameRepository().loadReviewContext(request, f.gameId, run);
        QTRY_VERIFY(!contexts.empty());
        const auto context = std::get<persistence::ReviewContext>(
            qvariant_cast<persistence::LoadReviewContextResult>(contexts.back()[1]));
        QCOMPARE(context.position, std::optional<int>{1});
        controller.navigation().clearPreview();
        QTRY_COMPARE(controller.navigation().displayed(), 1);
    }
    void productionRestartAndCloseFlushLatestMoment() {
#ifndef Q_OS_LINUX
        QSKIP("Production profile isolation uses Linux XDG locations.");
#else
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        auto& review = f.startup.reviewGame();
        review.workspace().setVisible(true);
        review.selectMoment(1);
        f.shutdown();
        QTemporaryDir root;
        const auto data = root.filePath(QStringLiteral("data/chess-review"));
        QVERIFY(QDir().mkpath(data));
        QVERIFY(QFile::copy(f.directory.filePath(QStringLiteral("review.sqlite3")),
                            QDir(data).filePath(QStringLiteral("chess-review.sqlite3"))));
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_DATA_HOME"), root.filePath(QStringLiteral("data")));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"),
                           root.filePath(QStringLiteral("config")));
        environment.insert(QStringLiteral("XDG_CACHE_HOME"),
                           root.filePath(QStringLiteral("cache")));
        for (const auto& mode :
             {QStringLiteral("review-change"), QStringLiteral("review-reopen")}) {
            environment.insert(QStringLiteral("CHESS_REVIEW_EXIT_MODE"), mode);
            QProcess process;
            process.setProcessEnvironment(environment);
            process.start(QString::fromUtf8(CHESS_REVIEW_EXIT_PROBE), {});
            const auto cleanup = qScopeGuard([&] {
                if (process.state() != QProcess::NotRunning) {
                    process.kill();
                    if (!process.waitForFinished(3000)) {
                        qFatal("Review exit probe could not be reaped");
                    }
                }
            });
            QVERIFY(process.waitForStarted(5000));
            const bool exited = process.waitForFinished(10000);
            QVERIFY2(exited, process.readAllStandardError().constData());
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QCOMPARE(process.exitCode(), 0);
            const auto output = process.readAllStandardOutput();
            QVERIFY(output.contains("review-restored:"));
            QVERIFY(output.contains("event-loop-quitting"));
        }
        QFile events(root.filePath(QStringLiteral("cache/chess-review/logs/events-0.jsonl")));
        QVERIFY(events.open(QIODevice::ReadOnly));
        const auto records = events.readAll();
        QVERIFY(records.contains("session.stopped"));
        QVERIFY(!records.contains("engine.process.started"));
#endif
    }
    void libraryShowsChosenSideAccuracyWithoutEngineRestart() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        QSignalSpy saved(&f.startup.reviewGame().workspace(),
                         &app::ReviewWorkspaceController::contextCommitted);
        f.startup.reviewGame().selectMoment(2);
        QTRY_VERIFY(!saved.empty());
        auto& library = f.startup.library();
        library.activate();
        QTRY_VERIFY(library.reviewStatus(f.gameId).contains(QStringLiteral("Black")));
        QTRY_VERIFY(library.reviewAccuracy(f.gameId).contains(QLatin1Char('%')));
        QVERIFY(library.reviewAccuracy(f.gameId).contains(QStringLiteral("evaluated")));
        QSignalSpy summaries(&f.startup.gameRepository(),
                             &persistence::Database::reviewOverviewLoaded);
        QSignalSpy metadata(&f.startup.gameRepository(),
                            &persistence::Database::libraryReviewsListed);
        library.importedGame();
        QTRY_VERIFY(!metadata.empty());
        QVERIFY(summaries.empty());
        metadata.clear();
        library.refresh();
        QTRY_VERIFY(!metadata.empty());
        QTRY_VERIFY(!summaries.empty());
    }
    void restartRestoresReviewWithoutEngineOrValidToml() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        const auto id = f.startup.reviewGame().state().run->id;
        auto& controller = f.startup.reviewGame();
        QSignalSpy saved(&controller.workspace(),
                         &app::ReviewWorkspaceController::contextCommitted);
        controller.workspace().setVisible(true);
        controller.selectMoment(2);
        QTRY_COMPARE(controller.navigation().displayed(), 2);
        QTRY_VERIFY(!saved.empty());
        f.shutdown();
        const auto invalid = f.directory.filePath(QStringLiteral("bad.toml"));
        QFile config(invalid);
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write("invalid = [");
        config.close();
        Fixture reopened(f.directory.filePath(QStringLiteral("review.sqlite3")), invalid);
        auto& restored = reopened.startup.reviewGame();
        QTRY_VERIFY(restored.state().run && restored.state().run->id == id &&
                    !restored.state().busy);
        QTRY_COMPARE(restored.navigation().selected(), 2);
        QTRY_COMPARE(restored.navigation().displayed(), 2);
        QCOMPARE(restored.state().run->selectedSide, chess::Color::Black);
        QCOMPARE(reopened.startup.engines().state().runtime.phase,
                 engine::RuntimePhase::Disconnected);
        QCOMPARE(reopened.startup.reviewConfiguration().state(), app::ConfigurationState::Invalid);
        QVERIFY(restored.state().overview);
        QVERIFY(restored.navigation().move(2));
    }
    void backgroundReviewDoesNotReplaceAnotherGame() {
        Fixture f;
        QVERIFY(f.seed());
        pgn::PgnReader reader("1. d4 d5 *");
        auto doc =
            std::make_shared<const pgn::PgnDocument>(std::get<pgn::PgnDocument>(reader.next()));
        auto& db = f.startup.gameRepository();
        QSignalSpy inserted(&db, &persistence::Database::gameInserted);
        db.insertGame(QUuid::createUuid(), doc);
        QVERIFY(inserted.wait());
        const auto other = std::get<persistence::Inserted>(
                               qvariant_cast<persistence::InsertGameResult>(inserted.front()[1]))
                               .id;
        f.script(0, -1, 100);
        f.run();
        auto& controller = f.startup.reviewGame();
        QTRY_VERIFY(controller.taskState().run);
        controller.selectGame(other);
        QTRY_VERIFY(controller.state().gameId == other && !controller.state().busy);
        QVERIFY(!controller.startBlocker(f.profileId).isEmpty());
        QCOMPARE(controller.taskState().gameId, f.gameId);
        QTRY_VERIFY(!controller.taskState().busy);
        QCOMPARE(controller.state().gameId, other);
        QVERIFY(!controller.state().run);
        QCOMPARE(controller.taskState().run->status, review::JobStatus::Complete);
        controller.workspace().returnToTask();
        QTRY_VERIFY(controller.state().run &&
                    controller.state().run->status == review::JobStatus::Complete &&
                    !controller.state().busy);
        QCOMPARE(controller.state().gameId, f.gameId);
    }
    void switchingRunsRestoresCommittedMoments() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        auto& controller = f.startup.reviewGame();
        const auto first = controller.state().run->id;
        controller.selectMoment(2);
        QTRY_COMPARE(controller.navigation().displayed(), 2);
        f.script();
        f.run();
        QVERIFY(f.done());
        const auto second = controller.state().run->id;
        QVERIFY(first != second);
        controller.selectMoment(1);
        QTRY_COMPARE(controller.navigation().displayed(), 1);
        controller.inspect(first);
        QTRY_VERIFY(controller.state().run && controller.state().run->id == first &&
                    !controller.state().busy);
        QTRY_COMPARE(controller.navigation().selected(), 2);
        controller.inspect(second);
        QTRY_VERIFY(controller.state().run && controller.state().run->id == second &&
                    !controller.state().busy);
        QTRY_COMPARE(controller.navigation().selected(), 1);
    }
    void init() {
        QTest::failOnWarning(QRegularExpression(
            QStringLiteral("^(?!This plugin does not support propagateSizeHints\\(\\)$).*")));
    }
    void staleMoveErrorCannotReappearAfterDeselection() {
        persistence::Database database;
        std::unique_ptr<app::ReviewMovesController> moves;
        bool delivered = false;
        connect(&database, &persistence::Database::reviewMovesLoaded, &database,
                [&](QUuid, const persistence::LoadReviewMovesResult&) {
                    delivered = true;
                    moves->bind(std::nullopt);
                });
        moves = std::make_unique<app::ReviewMovesController>(database);
        persistence::ReviewSummary run;
        run.id = 1;
        run.totalPositions = 2;
        moves->bind(run);
        QTRY_VERIFY(delivered);
        QCOMPARE(moves->state().error, QString{});
        QVERIFY(!moves->state().page);
        QVERIFY(!moves->state().loading);
        moves->close();
        QVERIFY(moves->drained());
    }
    void busyBackfillRetriesItsOwnPage() {
        std::string text;
        for (int i = 0; i < 13; ++i) {
            text += "Nf3 Nf6 Ng1 Ng8 ";
        }
        Fixture f;
        QVERIFY(f.seed(text + "*"));
        f.script(0, -1, 0, false, false, QStringLiteral("cp 0"));
        f.run();
        QVERIFY(f.done(40000));
        auto& owner = f.startup.reviewGame();
        owner.moves().close();
        QTRY_VERIFY(owner.moves().drained());
        std::vector<int> offsets;
        bool rejected = false;
        const auto intercept =
            connect(&f.startup.gameRepository(), &persistence::Database::reviewMovesLoaded,
                    &f.startup.gameRepository(),
                    [&](QUuid id, const persistence::LoadReviewMovesResult& result) {
                        if (const auto* page = std::get_if<persistence::ReviewMovePage>(&result)) {
                            offsets.push_back(page->offset);
                            if (page->offset == 50 && !rejected) {
                                rejected = true;
                                emit f.startup.gameRepository().reviewMovesLoaded(
                                    id, persistence::ReviewError{persistence::ReviewErrorCode::Busy,
                                                                 QStringLiteral("Fixture busy")});
                            }
                        }
                    });
        app::ReviewMovesController moves(f.startup.gameRepository());
        moves.bind(owner.state().run);
        const bool completed = QTest::qWaitFor([&] { return offsets.size() >= 3; });
        moves.close();
        QTRY_VERIFY(moves.drained());
        disconnect(intercept);
        QVERIFY(completed);
        QCOMPARE(offsets[0], 0);
        QCOMPARE(offsets[1], 50);
        QCOMPARE(offsets[2], 50);
    }
    void moveEvidenceFailureHasAnExplicitRetry() {
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, -1, 0, false, false, QStringLiteral("cp 0"));
        f.run();
        QVERIFY(f.done());
        auto& moves = f.startup.reviewGame().moves();
        QTRY_VERIFY(moves.state().page && !moves.state().loading);
        auto& nav = f.startup.reviewGame().navigation();
        QTRY_VERIFY(nav.ready());
        nav.select(nav.target(2));
        QTRY_COMPARE(nav.displayed(), 2);
        QTRY_VERIFY(nav.evaluation().whiteExpected);
        QTRY_VERIFY(!f.startup.reviewGame().exploration().state().candidates.empty());
        QVERIFY(f.sql(QStringLiteral(
            "CREATE TABLE saved_classification_fixture AS SELECT * FROM review_classifications")));
        QVERIFY(f.sql(QStringLiteral("UPDATE review_classifications SET document=zeroblob(128)")));
        moves.retry();
        QTRY_VERIFY(!moves.state().error.isEmpty());
        QVERIFY(!nav.move(1));
        QVERIFY(!nav.evaluation().whiteExpected);
        QVERIFY(f.startup.reviewGame().exploration().state().candidates.empty());
        QVERIFY(!f.startup.reviewGame().exploration().state().canRetry);
        auto* retry = f.window.findChild<QPushButton*>(QStringLiteral("retryReviewMoves"));
        QVERIFY(retry);
        QVERIFY(!retry->isHidden());
        QVERIFY(retry->isEnabled());
        QVERIFY(f.sql(QStringLiteral(
            "UPDATE review_classifications SET document=(SELECT document FROM "
            "saved_classification_fixture s WHERE s.review_id=review_classifications.review_id AND "
            "s.position_index=review_classifications.position_index)")));
        retry->click();
        QTRY_VERIFY(moves.state().page && !moves.state().loading && moves.state().error.isEmpty());
        QCOMPARE(moves.state().selectedPosition, 2);
        QVERIFY(retry->isHidden());
        QTRY_VERIFY(nav.evaluation().whiteExpected);
        QTRY_VERIFY(!f.startup.reviewGame().exploration().state().candidates.empty());
        QVERIFY(moves.state().page->moves.front().classification->label);
    }
    void longPlayerNamesDoNotWidenReviewWorkspace() {
        Fixture f;
        const auto name = std::string("<b>&") + std::string(252, 'W');
        QVERIFY(f.seed("[White \"" + name + "\"]\n[Black \"" + name + "\"]\n1. e4 e5 *"));
        f.window.findChild<QPushButton*>(QStringLiteral("reviewTaskStatus"))->click();
        f.window.resize(1100, 720);
        f.window.show();
        auto& nav = f.startup.reviewGame().navigation();
        QTRY_VERIFY(nav.ready());
        auto* splitter = f.window.findChild<QSplitter*>(QStringLiteral("reviewSplitter"));
        auto* shell = f.window.findChild<QScrollArea*>(QStringLiteral("shellScrollArea"));
        QVERIFY(splitter);
        QVERIFY(shell);
        QCoreApplication::processEvents();
        QVERIFY(splitter->width() <= shell->viewport()->width());
        for (const auto& objectName :
             {QStringLiteral("reviewTopPlayer"), QStringLiteral("reviewBottomPlayer")}) {
            auto* label = f.window.findChild<QLabel*>(objectName);
            QVERIFY(label);
            QCOMPARE(label->textFormat(), Qt::PlainText);
            QCOMPARE(label->text(), QString::fromStdString(name));
            QCOMPARE(label->toolTip(),
                     QStringLiteral("<qt>%1</qt>").arg(label->text().toHtmlEscaped()));
        }
    }
    void scrollingEvidenceKeepsCommittedPageAndBoundsReads() {
        std::string text;
        for (int i = 0; i < 75; ++i) {
            text += "Nf3 Nf6 Ng1 Ng8 ";
        }
        Fixture f;
        QVERIFY(f.seed(text + "*"));
        f.script(0, 1, 100);
        f.run();
        auto& owner = f.startup.reviewGame();
        QTRY_VERIFY2(owner.state().run, qPrintable(owner.state().error));
        owner.cancel();
        QVERIFY(f.done());
        auto& nav = owner.navigation();
        QTRY_VERIFY(nav.ready());
        nav.select(nav.target(2));
        QTRY_VERIFY(nav.move(2));
        owner.moves().close();
        QTRY_VERIFY(owner.moves().drained());
        QSignalSpy reads(&f.startup.gameRepository(), &persistence::Database::reviewMovesLoaded);
        for (int position : {51, 101, 151, 201, 251}) {
            nav.visible(nav.target(position), nav.target(position + 1));
            QTRY_VERIFY(nav.move(position));
            QCOMPARE(nav.selected(), 2);
            QVERIFY(nav.move(2));
        }
        // One bounded page per new visible range; repeated paint/visibility intents
        // must not keep issuing storage reads.
        const auto count = reads.count();
        for (int i = 0; i < 100; ++i) {
            nav.visible(nav.target(251), nav.target(252));
        }
        QTest::qWait(100);
        QCOMPARE(reads.count(), count);
        QVERIFY(count <= 6);
        QVERIFY(!nav.move(51));
    }
    void boundedFinalIterationDoesNotEraseUsableReview() {
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 2. Nf3 Nc6 *"));
        f.script(0, -1, 0, false, false, QStringLiteral("cp 0"), QStringLiteral("Test engine"), 0,
                 false, {}, 0, 0, true);
        QSignalSpy attempts(&f.supervisor(), &engine::EngineSupervisor::scheduledFinished);
        f.run();
        QVERIFY(f.done());
        auto& controller = f.startup.reviewGame();
        QVERIFY2(controller.state().error.isEmpty(), qPrintable(controller.state().error));
        QTRY_VERIFY(controller.state().overview);
        QCOMPARE(controller.state().overview->sides[0].evaluated, 2);
        QCOMPARE(controller.state().overview->sides[1].evaluated, 2);
        QVERIFY(controller.state().overview->sides[1].accuracy);
        QTRY_VERIFY(controller.moves().state().page &&
                    controller.moves().state().page->revision == controller.state().run->revision &&
                    std::all_of(controller.moves().state().page->moves.begin(),
                                controller.moves().state().page->moves.end(), [](const auto& row) {
                                    return row.classification && row.classification->label;
                                }));
        for (const auto& row : controller.moves().state().page->moves) {
            QVERIFY(row.classification);
            QVERIFY(row.classification->label);
            QCOMPARE(row.quickScan.state, review::ScanState::Usable);
        }
        bool sawBoundWithDifferentExactMove = false;
        for (const auto& event : attempts) {
            const auto result = qvariant_cast<engine::ScheduledResult>(event.front());
            if (!result.evidence) {
                continue;
            }
            const auto& raw = result.evidence->evidence;
            if (!raw.completedIteration.empty() && !raw.lines.empty() &&
                raw.lines.front().score->bound == engine::ScoreBound::Lower) {
                sawBoundWithDifferentExactMove =
                    sawBoundWithDifferentExactMove ||
                    raw.bestMove != raw.completedIteration.front().pv.front();
                QCOMPARE(raw.completedIteration.front().score->bound, engine::ScoreBound::Exact);
                QVERIFY(raw.completedIteration.front().statistics.nodes < raw.statistics.nodes);
            }
        }
        QVERIFY(sawBoundWithDifferentExactMove);
        const auto id = controller.state().run->id;
        controller.inspect(id);
        QTRY_VERIFY(!controller.state().busy);
        QTRY_VERIFY(controller.moves().state().page);
        QCOMPARE(controller.state().overview->sides[1].evaluated, 2);
    }
    void fingerprintRejectsInvalidEngineValues() {
        QVERIFY(std::holds_alternative<engine::Error>(
            engine::quickReviewFingerprint(engine::ValidatedEngine{})));
    }
    void reviewHeaderBoundsUntrustedNames() {
        Fixture f;
        const auto name = std::string("<b>") + std::string(8192, 'x') + "</b>";
        QVERIFY(f.seed("[White \"" + name + "\"]\n[Black \"" + name + "\"]\n*"));
        QVERIFY(f.startup.reviewGame().state().game.size() <= 600);
        QVERIFY(f.startup.reviewGame().state().game.contains(QStringLiteral("<b>")));
    }
    void selectingGameResetsHistoryPage() {
        Fixture f;
        QVERIFY(f.seed());
        QTRY_VERIFY(!f.startup.reviewGame().state().listing);
        f.startup.reviewGame().refreshRuns(1);
        QTRY_VERIFY(!f.startup.reviewGame().state().listing);
        QCOMPARE(f.startup.reviewGame().state().page, 1);
        f.startup.reviewGame().selectGame(f.gameId);
        QCOMPARE(f.startup.reviewGame().state().page, 0);
        QVERIFY(!f.startup.reviewGame().state().hasNext);
    }
    void resumeDuringDisconnectCompletes() {
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, 1, 0, false, false, QStringLiteral("cp 25"), QStringLiteral("Test engine"),
                 300);
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
        f.script(1);
        f.startup.engines().disconnectEngine();
        QCOMPARE(f.startup.engines().state().runtime.phase, engine::RuntimePhase::Disconnecting);
        f.startup.reviewGame().resume(run.id);
        QTRY_VERIFY_WITH_TIMEOUT(!f.startup.reviewGame().state().active, 4000);
        QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Complete);
    }
    void closingFromNotificationAllowsDestruction() {
        Fixture f;
        QVERIFY(f.seed());
        auto controller = std::make_unique<app::ReviewGameController>(
            f.startup.gameRepository(), f.startup.engines(), f.supervisor(),
            f.startup.reviewConfiguration());
        controller->setReady(true);
        bool closing = false;
        bool survivedNestedNotification = false;
        connect(controller.get(), &app::ReviewGameController::changed, &f.window, [&] {
            if (controller && controller->state().phase == app::ReviewPhase::Idle && !closing) {
                closing = true;
                controller->close();
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                survivedNestedNotification = static_cast<bool>(controller);
            }
        });
        connect(controller.get(), &app::ReviewGameController::closed, &f.window,
                [&] { controller.reset(); });
        controller->selectGame(f.gameId);
        QTRY_VERIFY(!controller);
        QVERIFY(survivedNestedNotification);
    }
    void closingDuringWorkspaceNotificationDefersDestruction_data() {
        QTest::addColumn<bool>("navigation");
        QTest::newRow("navigation") << true;
        QTest::newRow("exploration") << false;
    }
    void closingDuringWorkspaceNotificationDefersDestruction() {
        QFETCH(bool, navigation);
        Fixture f;
        QVERIFY(f.seed());
        auto controller = std::make_unique<app::ReviewGameController>(
            f.startup.gameRepository(), f.startup.engines(), f.supervisor(),
            f.startup.reviewConfiguration());
        controller->setReady(true);
        controller->selectGame(f.gameId);
        QTRY_VERIFY(!controller->state().busy && controller->navigation().ready());
        bool closing = false;
        bool survived = false;
        const auto observer = [&] {
            if (!closing && controller &&
                (!navigation || controller->navigation().selected() == 1)) {
                closing = true;
                controller->close();
                QEventLoop loop;
                QTimer::singleShot(100, &loop, &QEventLoop::quit);
                loop.exec();
                survived = static_cast<bool>(controller);
            }
        };
        if (navigation) {
            connect(&controller->navigation(), &app::ReviewNavigationController::changed, &f.window,
                    observer);
        } else {
            connect(&controller->exploration(), &app::ReviewExplorationController::changed,
                    &f.window, observer);
        }
        connect(controller.get(), &app::ReviewGameController::closed, &f.window,
                [&] { controller.reset(); });
        if (navigation) {
            controller->selectMoment(1);
        } else {
            controller->exploration().returnToGame();
        }
        QTRY_VERIFY(!controller);
        QVERIFY(survived);
    }
    void closingDuringEngineNotificationAllowsDestruction() {
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, 0);
        auto controller = std::make_unique<app::ReviewGameController>(
            f.startup.gameRepository(), f.startup.engines(), f.supervisor(),
            f.startup.reviewConfiguration());
        controller->setReady(true);
        controller->selectGame(f.gameId);
        QTRY_VERIFY(!controller->state().busy);
        bool closing = false;
        bool inspected = false;
        bool survivedNestedNotification = false;
        connect(controller.get(), &app::ReviewGameController::changed, &f.window, [&] {
            if (controller && controller->state().phase == app::ReviewPhase::Connecting &&
                !closing) {
                closing = true;
                controller->close();
            }
        });
        connect(controller.get(), &app::ReviewGameController::closed, &f.window,
                [&] { controller.reset(); });
        const auto observer =
            connect(&f.startup.engines(), &app::EngineSettingsController::changed, &f.window, [&] {
                if (closing && !inspected) {
                    inspected = true;
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    survivedNestedNotification = static_cast<bool>(controller);
                }
            });
        controller->start(chess::Color::White, f.profileId, review::Quality::Standard);
        QTRY_VERIFY(!controller);
        disconnect(observer);
        QVERIFY(inspected);
        QVERIFY(survivedNestedNotification);
    }
    void rapidInspectionShowsLatestSelection() {
        Fixture f;
        QVERIFY(f.seed("[SetUp \"1\"]\n[FEN \"7k/6Q1/6K1/8/8/8/8/8 b - - 0 1\"]\n*"));
        f.script();
        f.run();
        QVERIFY(f.done());
        const auto first = f.startup.reviewGame().state().run->id;
        f.run();
        QVERIFY(f.done());
        const auto second = f.startup.reviewGame().state().run->id;
        QVERIFY(first != second);
        f.startup.reviewGame().inspect(first);
        f.startup.reviewGame().inspect(second);
        QTRY_VERIFY(!f.startup.reviewGame().state().busy);
        QCOMPARE(f.startup.reviewGame().state().run->id, second);
    }
    void engineChooserBoundsAndEscapesNames() {
        Fixture f;
        QVERIFY(f.seed());
        auto profiles = f.startup.engines().state().profiles;
        profiles.front().displayName = QString(QChar(0x202e)) + QString(4095, u'x');
        app::EngineControllerTestAccess::replaceProfiles(f.startup.engines(), std::move(profiles));
        const auto* engines = f.window.findChild<QComboBox*>(QStringLiteral("reviewEngine"));
        QVERIFY(engines->currentText().startsWith(QStringLiteral("\\u202e")));
        QVERIFY(engines->currentText().size() <= 257);
        QVERIFY(engines->sizeHint().width() < 600);
    }
    void wheelDoesNotChangeReviewSetup() {
        Fixture f;
        QVERIFY(f.seed());
        auto* quality = f.window.findChild<QComboBox*>(QStringLiteral("reviewQuality"));
        const auto selected = quality->currentIndex();
        QWheelEvent wheel(QPointF(5, 5), quality->mapToGlobal(QPoint(5, 5)), {}, QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(quality, &wheel);
        QCOMPARE(quality->currentIndex(), selected);
    }
    void summaryRefreshHonorsBusyBackoff() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        auto& controller = f.startup.reviewGame();
        auto& database = f.startup.gameRepository();
        bool attempted = false;
        bool replacedBackoff = false;
        QSignalSpy publications(&controller, &app::ReviewGameController::changed);
        connect(&database, &persistence::Database::reviewOverviewLoaded, &database,
                [&](QUuid, const persistence::LoadReviewOverviewResult& result) {
                    const auto* error = std::get_if<persistence::ReviewError>(&result);
                    if (error && error->code == persistence::ReviewErrorCode::Busy && !attempted) {
                        attempted = true;
                        // Saturate admission again, before the retry timer fires. A refresh
                        // during backoff must not replace that operation or submit another read.
                        for (int n = 0; n < 16; ++n) {
                            database.loadGame(QUuid::createUuid(), f.gameId);
                        }
                        const auto before = publications.size();
                        controller.refreshOverview();
                        replacedBackoff = publications.size() != before;
                    }
                });
        for (int n = 0; n < 16; ++n) {
            database.loadGame(QUuid::createUuid(), f.gameId);
        }
        controller.refreshOverview();
        QTRY_VERIFY(attempted);
        QTRY_VERIFY(!controller.state().busy);
        QVERIFY(!replacedBackoff);
        QVERIFY(controller.state().overviewError.isEmpty());
    }
    void failedSummaryReadDoesNotPresentCurrentNumbers() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        auto& controller = f.startup.reviewGame();
        QVERIFY(controller.state().overview);
        QVERIFY(f.sql(QStringLiteral("UPDATE review_evidence SET adapted=x'01'")));
        controller.refreshOverview();
        QTRY_VERIFY(!controller.state().busy);
        QVERIFY(!controller.state().overviewError.isEmpty());
        auto* summary = f.window.findChild<QPlainTextEdit*>(QStringLiteral("reviewSummary"));
        QVERIFY(summary);
        QVERIFY(
            summary->toPlainText().contains(QStringLiteral("Last successfully loaded summary")));
    }

    void retryEngineCrashRemainsUngradedAndDrainsShutdown() {
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 *", true));
        f.script(0, -1, 0, false, false, QStringLiteral("cp 200"), QStringLiteral("Test engine"), 0,
                 true, {}, 0, 0, false, RetryScript::Crash);
        f.run();
        QVERIFY(f.done());
        auto& review = f.startup.reviewGame();
        auto& exploration = review.exploration();
        review.selectMoment(2);
        QTRY_VERIFY(exploration.state().canRetry);
        exploration.tryAgain();
        QTRY_VERIFY(exploration.board().interactive);
        auto root = *review.navigation().position();
        QVERIFY(root.undo());
        exploration.attemptMove(root.legalMoves()[3], exploration.board().revision);
        QTRY_VERIFY(exploration.state().mode == app::ExplorationMode::Retry &&
                    exploration.state().message.contains(QStringLiteral("Unable to grade")));
        f.shutdown();
        QCOMPARE(f.startup.state(), app::Lifecycle::Stopped);
    }
    void strongerCachedRetryRestartsWithinTheSavedBudget() {
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 *", true));
        f.script(0, -1, 0, false, false, QStringLiteral("cp 200"), QStringLiteral("Test engine"), 0,
                 true, {}, 0, 0, false, RetryScript::Played);
        f.run();
        QVERIFY(f.done());
        auto& review = f.startup.reviewGame();
        auto& database = f.startup.gameRepository();
        review.selectMoment(2);
        QTRY_VERIFY(review.exploration().state().canRetry);
        QSignalSpy evidence(&database, &persistence::Database::reviewEvidenceLoaded);
        database.loadReviewEvidence(QUuid::createUuid(), review.state().run->id, 2,
                                    review::EvidenceStage::Verification);
        QVERIFY(evidence.wait());
        auto record = qvariant_cast<persistence::LoadReviewEvidenceResult>(evidence.front()[1]);
        QVERIFY(std::holds_alternative<persistence::StoredReviewEvidence>(record));
        auto cached = *std::get<persistence::StoredReviewEvidence>(record).analysis;
        QSignalSpy runLoaded(&database, &persistence::Database::reviewLoaded);
        database.loadReview(QUuid::createUuid(), review.state().run->id);
        QVERIFY(runLoaded.wait());
        auto runResult = qvariant_cast<persistence::LoadReviewResult>(runLoaded.front()[1]);
        QVERIFY(std::holds_alternative<persistence::StoredReview>(runResult));
        auto provenance = std::make_shared<engine::AnalysisProvenance>(*cached.evidence.provenance);
        provenance->fingerprint =
            std::get<persistence::StoredReview>(runResult).settings.fingerprint;
        cached.evidence.provenance = provenance;
        cached.evidence.lines.resize(1);
        if (!cached.evidence.completedIteration.empty()) {
            cached.evidence.completedIteration.resize(1);
        }
        cached.evidence.requestedMultiPv = cached.evidence.effectiveMultiPv =
            cached.evidence.achievedWidth = 1;
        // Inject a typed repository reply, not a cache authorization exception for the fake binary.
        QObject observer;
        bool injecting = false, injected = false;
        connect(&database, &persistence::Database::analysisFound, &observer,
                [&](QUuid id, const persistence::FindAnalysisResult&) {
                    if (injecting || injected) {
                        return;
                    }
                    injecting = injected = true;
                    emit database.analysisFound(id, persistence::CacheLookup{cached, 0});
                    injecting = false;
                });
        app::ReviewExplorationController probe(database, review.navigation(), f.startup.engines(),
                                               f.supervisor());
        const auto close = qScopeGuard([&] {
            QSignalSpy closed(&probe, &app::ReviewExplorationController::closed);
            probe.close();
            if (closed.empty() && !closed.wait(10000)) {
                qFatal("Cache retry probe did not drain");
            }
        });
        probe.bind(review.state().run);
        QTRY_VERIFY(probe.state().canRetry);
        QSignalSpy searches(&f.supervisor(), &engine::EngineSupervisor::scheduledChanged);
        probe.tryAgain();
        QTRY_VERIFY(probe.board().interactive);
        probe.attemptMove(f.document->originalMainline()[1], probe.board().revision);
        QTRY_VERIFY(injected);
        QTRY_COMPARE_WITH_TIMEOUT(probe.state().mode, app::ExplorationMode::Success, 10000);
        QVERIFY(!searches.empty());
    }

    void feedbackCancellationCannotResurrectPractice() {
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 *", true));
        f.script(0, -1, 0, false, false, QStringLiteral("cp 200"), QStringLiteral("Test engine"), 0,
                 true);
        f.run();
        QVERIFY(f.done());
        auto& review = f.startup.reviewGame();
        auto& exploration = review.exploration();
        review.selectMoment(2);
        QTRY_VERIFY(exploration.state().canRetry);
        bool cancelled = false;
        QObject observer;
        connect(&exploration, &app::ReviewExplorationController::moveFeedback, &observer,
                [&](chess::Move, bool) {
                    cancelled = true;
                    exploration.returnToGame();
                });
        exploration.tryAgain();
        QTRY_VERIFY(exploration.board().interactive);
        auto root = *review.navigation().position();
        QVERIFY(root.undo());
        exploration.attemptMove(root.legalMoves()[1], exploration.board().revision);
        QTRY_VERIFY(cancelled);
        QCOMPARE(exploration.state().mode, app::ExplorationMode::Game);
    }
    void failedLinkedInitializationDoesNotBecomeAResumeOrigin() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        auto& review = f.startup.reviewGame();
        auto& exploration = review.exploration();
        review.selectMoment(2);
        QTRY_VERIFY(exploration.state().canAnalyze);
        exploration.openAnalysis();
        auto& linked = exploration.linkedAnalysis();
        QTRY_VERIFY(!linked.busy() && linked.position());
        QVERIFY(exploration.sameAnalysisOrigin());
        review.selectMoment(1);
        QTRY_VERIFY(exploration.state().canAnalyze && !review.navigation().pending());
        exploration.openAnalysis();
        QVERIFY(!exploration.sameAnalysisOrigin());
        linked.reset(std::string("invalid FEN"));
        QTRY_VERIFY(!linked.busy() && !linked.error().isEmpty());
        QVERIFY(!exploration.sameAnalysisOrigin());
        exploration.openAnalysis();
        QTRY_VERIFY(!linked.busy() && linked.error().isEmpty());
        QVERIFY(exploration.sameAnalysisOrigin());
        QCOMPARE(linked.position()->context(), review.navigation().position()->context());
    }
    void navigationDuringPreviewClearCancelsExploration_data() {
        QTest::addColumn<bool>("practice");
        QTest::newRow("candidate-line") << false;
        QTest::newRow("try-again") << true;
    }
    void navigationDuringPreviewClearCancelsExploration() {
        QFETCH(bool, practice);
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 *", true));
        f.script(0, -1, 0, false, false, QStringLiteral("cp 200"), QStringLiteral("Test engine"), 0,
                 true);
        f.run();
        QVERIFY(f.done());
        auto& review = f.startup.reviewGame();
        auto& navigation = review.navigation();
        auto& exploration = review.exploration();
        review.selectMoment(2);
        QTRY_VERIFY(exploration.state().canRetry);
        navigation.preview(navigation.target(1));
        QTRY_COMPARE(navigation.displayed(), 1);
        bool redirected = false;
        QObject observer;
        connect(&navigation, &app::ReviewNavigationController::changed, &observer, [&] {
            if (!redirected && !navigation.previewing()) {
                redirected = true;
                review.selectMoment(0);
            }
        });
        if (practice) {
            exploration.tryAgain();
        } else {
            exploration.showLine();
        }
        QVERIFY(redirected);
        QTRY_COMPARE(navigation.displayed(), 0);
        QCOMPARE(navigation.selected(), 0);
        QCOMPARE(exploration.state().mode, app::ExplorationMode::Game);
        QVERIFY(!exploration.active());
        QVERIFY(!exploration.hidesSolution());
    }
    void navigationDuringReviewReturnKeepsNewerMoment() {
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 *", true));
        f.script(0, -1, 0, false, false, QStringLiteral("cp 200"), QStringLiteral("Test engine"), 0,
                 true);
        f.run();
        QVERIFY(f.done());
        auto& review = f.startup.reviewGame();
        auto& exploration = review.exploration();
        review.selectMoment(2);
        QTRY_VERIFY(exploration.state().canAnalyze && exploration.state().canRetry);
        exploration.showLine();
        QTRY_VERIFY(exploration.state().canAnalyze);
        exploration.openAnalysis();
        QTRY_VERIFY(!exploration.linkedAnalysis().busy() && exploration.sameAnalysisOrigin());
        bool redirected = false;
        QObject observer;
        connect(&exploration, &app::ReviewExplorationController::changed, &observer, [&] {
            if (!redirected && exploration.state().mode == app::ExplorationMode::Game) {
                redirected = true;
                review.selectMoment(0);
            }
        });
        exploration.restoreReviewOrigin();
        QVERIFY(redirected);
        QTRY_COMPARE(review.navigation().displayed(), 0);
        QCOMPARE(review.navigation().selected(), 0);
        QCOMPARE(exploration.state().mode, app::ExplorationMode::Game);
    }
    void comparisonRowsReplayPlayedChildAndHideDuringRetry() {
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 *", true));
        f.script(0, -1, 0, false, false, QStringLiteral("cp 200"), QStringLiteral("Test engine"), 0,
                 true);
        f.run();
        QVERIFY(f.done());
        auto& review = f.startup.reviewGame();
        auto& exploration = review.exploration();
        review.selectMoment(2);
        QTRY_VERIFY(!exploration.state().loading && exploration.state().canRetry);
        QVERIFY(!exploration.state().comparisonRows.empty());
        QVERIFY(exploration.state().comparisonRows.front().startsWith(QStringLiteral("Played:")));
        QVERIFY(exploration.state().comparisonRows.front().contains(QStringLiteral("after-move")));
        const auto original = review.navigation().board().pieces;
        const int playedLine = exploration.state().comparisonLines.front();
        QVERIFY(playedLine >= 0);
        f.window.findChild<QPushButton*>(QStringLiteral("reviewTaskStatus"))->click();
        f.window.show();
        auto* rows = f.window.findChild<QTableView*>(QStringLiteral("reviewComparisonRows"));
        QVERIFY(rows && rows->model()->rowCount() > 0);
        rows->setCurrentIndex(rows->model()->index(0, 1));
        QTest::keyClick(rows, Qt::Key_Return);
        QTRY_COMPARE(exploration.state().mode, app::ExplorationMode::Line);
        QCOMPARE(exploration.state().candidate, playedLine);
        QTRY_VERIFY(exploration.state().canAnalyze);
        exploration.next();
        QTRY_VERIFY(exploration.state().canAnalyze);
        QCOMPARE(exploration.board().pieces, original);
        exploration.openAnalysis();
        QTRY_VERIFY(exploration.sameAnalysisOrigin());
        exploration.restoreReviewOrigin();
        QTRY_COMPARE(exploration.state().candidate, playedLine);
        QCOMPARE(exploration.state().step, 1);
        exploration.returnToGame();
        QTRY_VERIFY(exploration.state().canRetry);
        exploration.tryAgain();
        QTRY_VERIFY(exploration.hidesSolution());
        QVERIFY(!f.window.findChild<QWidget*>(QStringLiteral("reviewComparison"))->isVisible());
        QCOMPARE(rows->model()->rowCount(), 0);
        QVERIFY(f.window.findChild<QLabel*>(QStringLiteral("reviewComparisonStatus"))
                    ->text()
                    .isEmpty());
        exploration.returnToGame();
        QTRY_VERIFY(!exploration.state().comparisonRows.empty());
    }
    void comparisonUncertaintyUsesBoundedRounds_data() {
        QTest::addColumn<bool>("unresolved");
        QTest::addColumn<bool>("resume");
        QTest::addColumn<bool>("steady");
        QTest::newRow("already-confirmed") << false << false << true;
        QTest::newRow("confirmed") << false << false << false;
        QTest::newRow("exhausted") << true << false << false;
        QTest::newRow("resume-comparison-only") << false << true << false;
    }
    void comparisonUncertaintyUsesBoundedRounds() {
        QFETCH(bool, unresolved);
        QFETCH(bool, resume);
        QFETCH(bool, steady);
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 *", true));
        const int rounds = steady ? 0 : unresolved ? 2 : 1;
        f.script(0, -1, 0, false, false, QStringLiteral("cp 200"), QStringLiteral("Test engine"), 0,
                 true, QStringLiteral("cp 0"), 0, 0, false, RetryScript::None, {}, {}, {}, rounds);
        QFile runtime(f.directory.filePath(QStringLiteral("runtime.json")));
        QVERIFY(runtime.open(QIODevice::ReadOnly));
        auto steps = QJsonDocument::fromJson(runtime.readAll()).object()["steps"].toArray();
        runtime.close();
        for (qsizetype i = 0; i < steps.size(); ++i) {
            auto step = steps[i].toObject();
            const auto command = step["expect"].toString();
            if (command == QStringLiteral("go nodes 5000000") ||
                command == QStringLiteral("go nodes 10000000") ||
                command == QStringLiteral("go nodes 20000000")) {
                auto actions = step["actions"].toArray();
                auto write = actions[0].toObject();
                auto output = write["data"].toString();
                const auto score =
                    steady || (unresolved && command == QStringLiteral("go nodes 10000000"))
                        ? QStringLiteral("cp 0")
                        : QStringLiteral("cp -30");
                output.replace(QStringLiteral("multipv 2 score cp 0"),
                               QStringLiteral("multipv 2 score ") + score);
                output.replace(QStringLiteral("multipv 3 score cp 0"),
                               QStringLiteral("multipv 3 score ") + score);
                write["data"] = output;
                actions[0] = write;
                step["actions"] = actions;
                steps[i] = step;
            }
        }
        const auto fullSteps = steps;
        int retainedPrefix = 0;
        if (resume) {
            int count = 0;
            for (qsizetype i = 0; i < steps.size(); ++i) {
                if (steps[i].toObject()["expect"].toString().startsWith(
                        QStringLiteral("go nodes")) &&
                    ++count == 10) {
                    retainedPrefix = static_cast<int>(i) + 2;
                    break;
                }
            }
            QVERIFY(retainedPrefix > 2);
            while (steps.size() > retainedPrefix) {
                steps.removeLast();
            }
            steps.append(quit());
        }
        engine_test::scenario(runtime.fileName(), steps);
        std::array<int, 5> searches{};
        int heartbeats = 0;
        QTimer heartbeat;
        heartbeat.setInterval(5);
        connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++heartbeats; });
        connect(&f.supervisor(), &engine::EngineSupervisor::scheduledFinished, &heartbeat,
                [&](const engine::ScheduledResult& result) {
                    if (result.evidence) {
                        const auto* nodes =
                            std::get_if<engine::NodeLimit>(&result.evidence->evidence.limit);
                        const std::array<std::uint64_t, 5> budgets{100000, 1000000, 5000000,
                                                                   10000000, 20000000};
                        if (nodes) {
                            const auto found =
                                std::find(budgets.begin(), budgets.end(), nodes->nodes);
                            if (found != budgets.end()) {
                                ++searches[static_cast<std::size_t>(found - budgets.begin())];
                            }
                        }
                    }
                });
        QElapsedTimer elapsed;
        elapsed.start();
        heartbeat.start();
        auto& controller = f.startup.reviewGame();
        QMetaObject::Connection cancellation;
        if (resume) {
            cancellation =
                connect(&controller, &app::ReviewGameController::changed, &heartbeat, [&] {
                    if (controller.state().active && controller.state().escalationRound == 1 &&
                        controller.state().refinementDone == 1) {
                        controller.cancel();
                    }
                });
        }
        f.run();
        QVERIFY(f.done());
        disconnect(cancellation);
        if (resume) {
            QCOMPARE(controller.state().run->status, review::JobStatus::Cancelled);
            const auto id = controller.state().run->id;
            const auto savedAttempt = [&]() {
                QSignalSpy loaded(&f.startup.gameRepository(),
                                  &persistence::Database::reviewEvidenceLoaded);
                f.startup.gameRepository().loadReviewEvidence(QUuid::createUuid(), id, 0,
                                                              review::EvidenceStage::EscalationOne);
                if (!loaded.wait()) {
                    return std::string{};
                }
                const auto result =
                    qvariant_cast<persistence::LoadReviewEvidenceResult>(loaded.front()[1]);
                const auto* value = std::get_if<persistence::StoredReviewEvidence>(&result);
                return value ? value->evidence.attemptIdentity : std::string{};
            };
            const auto retained = savedAttempt();
            QVERIFY(!retained.empty());
            f.startup.engines().disconnectEngine();
            QTRY_COMPARE(f.startup.engines().state().runtime.phase,
                         engine::RuntimePhase::Disconnected);
            steps = fullSteps;
            for (int i = 2; i < retainedPrefix; ++i) {
                steps.removeAt(2);
            }
            engine_test::scenario(runtime.fileName(), steps);
            controller.resume(id);
            QVERIFY(f.done());
            QCOMPARE(savedAttempt(), retained);
        }
        heartbeat.stop();
        QVERIFY2(controller.state().error.isEmpty(), qPrintable(controller.state().error));
        QCOMPARE(controller.state().run->status, review::JobStatus::Complete);
        QCOMPARE(searches, (std::array<int, 5>{3, 3, 3, steady ? 0 : 3, unresolved ? 3 : 0}));
        controller.selectMoment(1);
        QTRY_VERIFY(!controller.exploration().state().loading &&
                    !controller.exploration().state().comparisonRows.empty());
        const auto* moment = controller.exploration().moment();
        QVERIFY(moment && moment->classification->stable);
        QCOMPARE(moment->alternatives.reason, unresolved ? review::ComparisonReason::Disagreement
                                                         : review::ComparisonReason::Confirmed);
        QVERIFY(heartbeats > 0);
        qInfo("alternative comparison: steady=%d resume=%d unresolved=%d total=%lldms "
              "searches=%d/%d/%d/%d/%d "
              "heartbeats=%d",
              steady, resume, unresolved, static_cast<long long>(elapsed.elapsed()), searches[0],
              searches[1], searches[2], searches[3], searches[4], heartbeats);
        const auto output = qEnvironmentVariable("CHESS_REVIEW_QUICK_RENDER_DIR");
        if (!output.isEmpty()) {
            f.window.findChild<QPushButton*>(QStringLiteral("reviewTaskStatus"))->click();
            f.window.resize(1100, 720);
            f.window.show();
            QTest::qWait(30);
            f.window.resize(1100, 720);
            QCoreApplication::processEvents();
            QVERIFY(f.window.grab().save(
                output + (unresolved ? QStringLiteral("/comparison-unresolved.png")
                                     : QStringLiteral("/comparison-confirmed.png"))));
        }
    }
    void candidateSelectorTracksDisplayedLine() {
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 *", true));
        f.script(0, -1, 0, false, false, QStringLiteral("cp 200"), QStringLiteral("Test engine"), 0,
                 true);
        f.run();
        QVERIFY(f.done());
        auto& review = f.startup.reviewGame();
        auto& exploration = review.exploration();
        review.selectMoment(2);
        QTRY_VERIFY(!exploration.state().loading && exploration.state().canRetry);
        exploration.showLine(1);
        QTRY_VERIFY(exploration.state().canAnalyze);
        auto* candidates = f.window.findChild<QComboBox*>(QStringLiteral("reviewCandidates"));
        QCOMPARE(candidates->currentIndex(), 1);
        QSignalSpy reads(&f.startup.gameRepository(),
                         &persistence::Database::reviewExplorationLoaded);
        QSignalSpy searches(&f.supervisor(), &engine::EngineSupervisor::scheduledChanged);
        for (int i = 0; i < 30; ++i) {
            exploration.end();
            exploration.beginning();
        }
        exploration.end();
        QTRY_VERIFY(exploration.state().canAnalyze);
        QCOMPARE(exploration.state().step, exploration.state().length);
        QCOMPARE(reads.count(), 0);
        QCOMPARE(searches.count(), 0);
    }
    void completedRefinementRefreshesUnpinnedCandidates() {
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 *", true));
        f.script(0, -1, 150, false, false, QStringLiteral("cp 200"), QStringLiteral("Test engine"),
                 0, true, QStringLiteral("cp 0"));
        f.run();
        auto& review = f.startup.reviewGame();
        auto& exploration = review.exploration();
        QTRY_VERIFY(review.state().run && review.state().run->completedPositions >= 3);
        review.selectMoment(2);
        QTRY_VERIFY(!exploration.state().candidates.empty());
        QVERIFY(exploration.state().candidates.front().contains(QStringLiteral("67.5%")));
        exploration.showLine(0);
        QCOMPARE(exploration.state().mode, app::ExplorationMode::Line);
        QTRY_VERIFY(exploration.state().canAnalyze);
        QVERIFY(f.done());
        QCOMPARE(exploration.state().mode, app::ExplorationMode::Line);
        QVERIFY(!exploration.state().notice.isEmpty());
        QVERIFY(exploration.moment() && !exploration.moment()->classification->stable);
        exploration.returnToGame();
        QTRY_VERIFY(!exploration.state().loading && !exploration.state().candidates.empty());
        QVERIFY(exploration.state().candidates.front().contains(QStringLiteral("50.0%")));
    }
    void sandboxPreparationNeverAdmitsThePreviousBoard() {
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 *", true));
        f.script(0, -1, 0, false, false, QStringLiteral("cp 200"), QStringLiteral("Test engine"), 0,
                 true);
        f.run();
        QVERIFY(f.done());
        auto& review = f.startup.reviewGame();
        auto& exploration = review.exploration();
        review.selectMoment(2);
        QTRY_VERIFY(!exploration.state().loading && exploration.state().canRetry);
        bool invalidReady = false;
        const auto observe =
            connect(&exploration, &app::ReviewExplorationController::changed, &exploration, [&] {
                if (exploration.state().mode == app::ExplorationMode::Line &&
                    exploration.state().canAnalyze && exploration.state().step < 0) {
                    invalidReady = true;
                }
            });
        exploration.showLine(1);
        QTRY_VERIFY(exploration.state().canAnalyze);
        const auto before = exploration.board().pieces;
        exploration.end();
        QTRY_VERIFY(exploration.state().canAnalyze);
        exploration.flip();
        bool wrongInput = false;
        const auto input =
            connect(&exploration, &app::ReviewExplorationController::changed, &exploration, [&] {
                if (exploration.state().mode == app::ExplorationMode::Retry &&
                    exploration.board().interactive && exploration.board().pieces != before) {
                    wrongInput = true;
                }
            });
        exploration.tryAgain();
        QTRY_VERIFY(exploration.board().interactive);
        disconnect(observe);
        disconnect(input);
        QVERIFY(!invalidReady);
        QVERIFY(!wrongInput);
    }
    void discardConfirmationCannotRetargetAnalysis() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        auto& review = f.startup.reviewGame();
        auto& exploration = review.exploration();
        review.selectMoment(2);
        QTRY_VERIFY(exploration.state().canAnalyze && !review.navigation().pending());
        f.window.findChild<QPushButton*>(QStringLiteral("reviewTaskStatus"))->click();
        f.window.show();
        auto* analyze = f.window.findChild<QPushButton*>(QStringLiteral("reviewAnalyzePosition"));
        analyze->click();
        auto& linked = exploration.linkedAnalysis();
        QTRY_VERIFY(!linked.busy() && linked.position());
        linked.attemptMove(linked.board().legalMoves.front(), linked.board().revision);
        QTRY_VERIFY(!linked.busy() && linked.hasContent());
        const auto edited = linked.board().pieces;
        f.window.findChild<QPushButton*>(QStringLiteral("backToReview"))->click();
        review.selectMoment(1);
        QTRY_VERIFY(exploration.state().canAnalyze && !review.navigation().pending());
        analyze->click();
        QTRY_VERIFY(f.window.findChild<QMessageBox*>(QStringLiteral("discardAnalysisDialog")));
        review.selectMoment(0);
        QTRY_VERIFY(!review.navigation().pending());
        auto* dialog = f.window.findChild<QMessageBox*>(QStringLiteral("discardAnalysisDialog"));
        dialog->button(QMessageBox::Discard)->click();
        QTRY_VERIFY(!linked.busy());
        QVERIFY(linked.hasContent());
        QCOMPARE(linked.board().pieces, edited);
    }
    void explorationLinesRetriesAndLinkedAnalysisAreIsolated() {
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 *", true));
        f.script(0, -1, 0, false, false, QStringLiteral("cp 200"), QStringLiteral("Test engine"), 0,
                 true, {}, 0, 0, false, RetryScript::Unknown);
        f.run();
        QVERIFY(f.done());
        auto& review = f.startup.reviewGame();
        auto& nav = review.navigation();
        auto& exploration = review.exploration();
        review.selectMoment(2);
        QTRY_COMPARE(nav.displayed(), 2);
        QTRY_VERIFY(!exploration.state().loading && exploration.state().canRetry);
        QCOMPARE(exploration.state().candidates.size(), 4);
        const auto original = nav.board().pieces;
        const auto revision = review.state().run->revision;
        const auto storedNode = nav.tree()->nodes.size();
        f.window.findChild<QPushButton*>(QStringLiteral("reviewTaskStatus"))->click();
        f.window.show();
        auto* board = f.window.findChild<ui::ChessBoardWidget*>(QStringLiteral("reviewBoard"));
        QVERIFY(board);
        exploration.showLine(1);
        QTRY_VERIFY(exploration.state().canAnalyze);
        QCOMPARE(exploration.state().step, 0);
        exploration.end();
        QTRY_COMPARE(exploration.state().step, 3);
        QTRY_VERIFY(exploration.state().canAnalyze);
        exploration.beginning();
        QTRY_COMPARE(exploration.state().step, 0);
        exploration.next();
        QTRY_COMPARE(exploration.state().step, 1);
        QTRY_VERIFY(exploration.state().canAnalyze);
        const auto candidateBoard = board->state().pieces;
        QVERIFY(candidateBoard != original);
        // Ordinary Analysis content survives opening and returning from linked Analysis.
        auto& ordinary = f.startup.analysis();
        ordinary.start();
        QTRY_VERIFY(!ordinary.busy() && ordinary.position());
        const auto e4 = std::get<chess::Move>(ordinary.position()->parseUci("e2e4"));
        ordinary.attemptMove(e4, ordinary.board().revision);
        QTRY_VERIFY(!ordinary.busy() && ordinary.hasContent());
        const auto ordinaryBoard = ordinary.board().pieces;
        const auto expectedContext = nav.position()->context();
        f.window.findChild<QPushButton*>(QStringLiteral("reviewAnalyzePosition"))->click();
        QTRY_VERIFY(!exploration.linkedAnalysis().busy() &&
                    exploration.linkedAnalysis().position());
        QCOMPARE(exploration.linkedAnalysis().board().pieces, candidateBoard);
        QCOMPARE(exploration.linkedAnalysis().position()->context().moves.size(),
                 expectedContext.moves.size());
        QVERIFY(exploration.linkedAnalysis().position()->context().moves != expectedContext.moves);
        QCOMPARE(ordinary.board().pieces, ordinaryBoard);
        QCOMPARE(exploration.linkedAnalysis().board().blackAtBottom, nav.board().blackAtBottom);
        auto& linked = exploration.linkedAnalysis();
        const bool ordinaryOrientation = ordinary.board().blackAtBottom;
        const bool linkedOrientation = linked.board().blackAtBottom;
        auto* flip = f.window.findChild<QAction*>(QStringLiteral("flipBoardAction"));
        QVERIFY(flip);
        flip->trigger();
        QCOMPARE(linked.board().blackAtBottom, !linkedOrientation);
        QCOMPARE(ordinary.board().blackAtBottom, ordinaryOrientation);
        flip->trigger();
        linked.attemptMove(linked.board().legalMoves.front(), linked.board().revision);
        QTRY_VERIFY(!linked.busy() && linked.hasContent());
        const auto linkedEdited = linked.board().pieces;
        review.selectMoment(1);
        QTRY_COMPARE(nav.displayed(), 1);
        f.window.findChild<QPushButton*>(QStringLiteral("backToReview"))->click();
        QTRY_VERIFY(exploration.state().canAnalyze);
        f.window.findChild<QPushButton*>(QStringLiteral("reviewAnalyzePosition"))->click();
        QCOMPARE(linked.board().pieces, linkedEdited);
        QVERIFY(!f.window.findChild<QMessageBox*>(QStringLiteral("discardAnalysisDialog")));
        f.window.findChild<QPushButton*>(QStringLiteral("backToReview"))->click();
        QTRY_VERIFY(!exploration.state().pendingBoard);
        QCOMPARE(exploration.state().mode, app::ExplorationMode::Line);
        QCOMPARE(exploration.state().step, 1);
        QCOMPARE(board->state().pieces, candidateBoard);
        exploration.returnToGame();
        QCOMPARE(board->state().pieces, original);
        // Practice hides the solution, including values reachable through accessibility.
        exploration.tryAgain();
        QTRY_VERIFY(board->state().interactive);
        QVERIFY(f.window.findChild<QComboBox*>(QStringLiteral("reviewCandidates"))->count() == 0);
        QVERIFY(f.window.findChild<QLabel*>(QStringLiteral("reviewMomentExplanation"))
                    ->text()
                    .isEmpty());
        QVERIFY(f.window.findChild<QPlainTextEdit*>(QStringLiteral("reviewMoveDetails"))
                    ->toPlainText()
                    .isEmpty());
        QVERIFY(f.window.findChild<QProgressBar*>(QStringLiteral("reviewEvaluationBar"))
                    ->accessibleDescription()
                    .isEmpty());
        exploration.hint();
        QVERIFY(exploration.state().message.contains(QStringLiteral("expected score")));
        exploration.hint();
        QVERIFY(exploration.state().message.contains(QStringLiteral("piece on")));
        exploration.hint();
        QVERIFY(exploration.state().message.contains(QStringLiteral("moving it")));
        auto root = *nav.position();
        QVERIFY(root.undo());
        // A demonstrated equivalent move succeeds without scheduling an engine request.
        const auto alternative = root.legalMoves().at(1);
        QSignalSpy searches(&f.supervisor(), &engine::EngineSupervisor::scheduledChanged);
        const auto from = board->squareRect(alternative.from).center().toPoint();
        const auto to = board->squareRect(alternative.to).center().toPoint();
        QTest::mousePress(board, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(board, to);
        QTest::mouseRelease(board, Qt::LeftButton, Qt::NoModifier, to);
        QTRY_COMPARE(exploration.state().mode, app::ExplorationMode::Success);
        QCOMPARE(searches.size(), 0);
        QTRY_COMPARE(exploration.state().mode, app::ExplorationMode::Game);
        QCOMPARE(board->state().pieces, original);
        // Cancelling during asynchronous preparation suppresses stale work and feedback.
        bool cancelled = false;
        const auto cancellation =
            connect(&exploration, &app::ReviewExplorationController::changed, &exploration, [&] {
                if (!cancelled && exploration.state().mode == app::ExplorationMode::Grading) {
                    cancelled = true;
                    exploration.returnToGame();
                }
            });
        exploration.tryAgain();
        QTRY_VERIFY(board->state().interactive);
        exploration.attemptMove(root.legalMoves().at(3), board->state().revision);
        QTRY_VERIFY(cancelled);
        QCOMPARE(exploration.state().mode, app::ExplorationMode::Game);
        disconnect(cancellation);
        QCOMPARE(searches.size(), 0);
        QTRY_VERIFY(([&] {
            exploration.tryAgain();
            return exploration.state().mode == app::ExplorationMode::Retry;
        })());
        // An unknown legal move receives both stronger child searches and Strong credit.
        QTRY_VERIFY(board->state().interactive);
        exploration.attemptMove(root.legalMoves().at(3), board->state().revision);
        QTRY_COMPARE_WITH_TIMEOUT(exploration.state().mode, app::ExplorationMode::Success, 10000);
        QVERIFY(!searches.empty());
        exploration.returnToGame();
        f.startup.engines().disconnectEngine();
        QTRY_COMPARE(f.startup.engines().state().runtime.phase, engine::RuntimePhase::Disconnected);
        exploration.tryAgain();
        QTRY_VERIFY(board->state().interactive);
        exploration.attemptMove(root.legalMoves().at(4), board->state().revision);
        QTRY_VERIFY(exploration.state().message.contains(QStringLiteral("Connect the original")));
        QCOMPARE(exploration.state().mode, app::ExplorationMode::Retry);
        exploration.returnToGame();
        QCOMPARE(review.state().run->revision, revision);
        QCOMPARE(nav.tree()->nodes.size(), storedNode);
        QCOMPARE(nav.selected(), 2);
        QCOMPARE(board->state().pieces, original);
        review.selectMoment(1);
        QTRY_VERIFY(exploration.state().canAnalyze && !nav.pending());
        auto* analyze = f.window.findChild<QPushButton*>(QStringLiteral("reviewAnalyzePosition"));
        analyze->click();
        QTRY_VERIFY(f.window.findChild<QMessageBox*>(QStringLiteral("discardAnalysisDialog")));
        auto* dialog = f.window.findChild<QMessageBox*>(QStringLiteral("discardAnalysisDialog"));
        dialog->button(QMessageBox::Cancel)->click();
        QTRY_VERIFY(!f.window.findChild<QMessageBox*>(QStringLiteral("discardAnalysisDialog")));
        QCOMPARE(linked.board().pieces, linkedEdited);
        QCOMPARE(ordinary.board().pieces, ordinaryBoard);
        analyze->click();
        QTRY_VERIFY(f.window.findChild<QMessageBox*>(QStringLiteral("discardAnalysisDialog")));
        dialog = f.window.findChild<QMessageBox*>(QStringLiteral("discardAnalysisDialog"));
        dialog->button(QMessageBox::Discard)->click();
        QTRY_VERIFY(!linked.busy() && !linked.hasContent());
        f.window.findChild<QPushButton*>(QStringLiteral("backToReview"))->click();
        QTRY_COMPARE(nav.displayed(), 1);
        QCOMPARE(ordinary.board().pieces, ordinaryBoard);
    }
    void explorationRejectsCorruptionAndLateReads() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        auto& review = f.startup.reviewGame();
        auto& exploration = review.exploration();
        review.selectMoment(1);
        review.selectMoment(2);
        QTRY_VERIFY(!exploration.state().loading && !exploration.state().candidates.empty());
        exploration.showLine();
        QTRY_VERIFY(exploration.state().canAnalyze);
        review.selectMoment(1);
        QTRY_COMPARE(exploration.state().mode, app::ExplorationMode::Game);
        QTRY_VERIFY(!exploration.state().loading && !exploration.state().candidates.empty());
        QVERIFY(f.sql(QStringLiteral("UPDATE review_evidence SET adapted=x'01'")));
        exploration.reload();
        QTRY_VERIFY(!exploration.state().loading && !exploration.state().message.isEmpty());
        QVERIFY(exploration.state().candidates.empty());
        QVERIFY(!exploration.state().canRetry);
        exploration.showLine();
        QCOMPARE(exploration.state().mode, app::ExplorationMode::Game);
    }
    void adaptiveQueueRunsRankedSeparatePasses_data() {
        QTest::addColumn<int>("policy");
        QTest::addColumn<bool>("routine");
        QTest::newRow("policy2-risk") << 2 << false;
        QTest::newRow("policy3-risk") << 3 << false;
        QTest::newRow("policy4-risk") << 4 << false;
        QTest::newRow("policy2-routine") << 2 << true;
        QTest::newRow("policy3-routine") << 3 << true;
        QTest::newRow("policy4-routine") << 4 << true;
    }
    void adaptiveQueueRunsRankedSeparatePasses() {
        QFETCH(int, policy);
        QFETCH(bool, routine);
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 2. Nf3 Nc6 *"));
        // Move 2 loses 6%; move 4 loses 20%. Move 3 is selected as an opportunity reply.
        auto& controller = f.startup.reviewGame();
        // Capture a real runtime fingerprint, then create an empty run with the desired
        // released policy through the repository API. No production policy override is needed.
        f.script(0, 1);
        const auto cancel =
            connect(&controller, &app::ReviewGameController::changed, &controller, [&] {
                if (controller.state().active && controller.state().run &&
                    controller.state().run->completedPositions == 1) {
                    controller.cancel();
                }
            });
        f.run();
        QVERIFY(f.done());
        disconnect(cancel);
        auto& db = f.startup.gameRepository();
        QSignalSpy loaded(&db, &persistence::Database::reviewLoaded);
        db.loadReview(QUuid::createUuid(), controller.state().run->id);
        QTRY_VERIFY(!loaded.empty());
        auto settings = std::get<persistence::StoredReview>(
                            qvariant_cast<persistence::LoadReviewResult>(loaded.front()[1]))
                            .settings;
        settings.pipelineVersion = static_cast<std::uint32_t>(policy);
        QSignalSpy created(&db, &persistence::Database::reviewCreated);
        db.createReview(QUuid::createUuid(), settings);
        QTRY_VERIFY(!created.empty());
        const auto run = std::get<persistence::ReviewSummary>(
            qvariant_cast<persistence::CreateReviewResult>(created.front()[1]));
        persistence::ReviewCheckpoint cancelled;
        cancelled.reviewId = run.id;
        cancelled.expectedRevision = run.revision;
        cancelled.status = review::JobStatus::Cancelled;
        QSignalSpy checkpointed(&db, &persistence::Database::reviewCheckpointed);
        db.checkpointReview(QUuid::createUuid(), cancelled);
        QTRY_VERIFY(!checkpointed.empty());
        QVERIFY(std::holds_alternative<persistence::ReviewSummary>(
            qvariant_cast<persistence::CheckpointReviewResult>(checkpointed.front()[1])));
        const std::vector<int> order = policy == 2 ? std::vector<int>{0, 1, 2, 3, 4}
                                       : routine   ? std::vector<int>{}
                                                   : std::vector<int>{3, 4, 1, 2};
        f.script(0, -1, 0, false, false, QStringLiteral("cp 0"), QStringLiteral("Test engine"), 0,
                 false, {}, 0, 0, false, RetryScript::None, order,
                 routine ? std::vector<int>{500, 500, 500, 500, 500}
                         : std::vector<int>{500, 500, 560, 560, 760});
        std::vector<std::pair<std::uint64_t, int>> searches;
        bool barrier = true;
        QElapsedTimer timer;
        timer.start();
        qint64 firstDeep = -1;
        qint64 leadingRiskDeep = -1;
        const auto observer =
            connect(&f.supervisor(), &engine::EngineSupervisor::scheduledFinished, &controller,
                    [&](const engine::ScheduledResult& result) {
                        if (!result.evidence) {
                            return;
                        }
                        const auto& evidence = result.evidence->evidence;
                        const auto* nodes = std::get_if<engine::NodeLimit>(&evidence.limit);
                        if (!nodes || !evidence.provenance) {
                            return;
                        }
                        const int index =
                            static_cast<int>(evidence.provenance->position.moves.size());
                        searches.emplace_back(nodes->nodes, index);
                        if (nodes->nodes > 100000) {
                            barrier = barrier && controller.taskState().run &&
                                      controller.taskState().run->completedPositions == 5;
                            if (nodes->nodes == 1000000 && index == 4) {
                                leadingRiskDeep = timer.elapsed();
                            }
                            if (firstDeep < 0) {
                                firstDeep = timer.elapsed();
                            }
                        }
                    });
        const auto disconnectObserver = qScopeGuard([&] { disconnect(observer); });
        controller.resume(run.id);
        QVERIFY(f.done());
        QVERIFY2(controller.state().error.isEmpty(), qPrintable(controller.state().error));
        QCOMPARE(controller.state().run->status, review::JobStatus::Complete);
        QTRY_VERIFY(controller.state().overview &&
                    controller.state().overview->revision == controller.state().run->revision);
        QCOMPARE(controller.state().overview->refinementPositions, order);
        std::vector<std::pair<std::uint64_t, int>> expected;
        for (int index = 0; index < 5; ++index) {
            expected.emplace_back(100000, index);
        }
        for (const std::uint64_t nodes : {1000000U, 5000000U}) {
            for (const int index : order) {
                expected.emplace_back(nodes, index);
            }
        }
        QCOMPARE(searches, expected);
        QVERIFY(barrier);
        QTRY_VERIFY(controller.moves().state().page && !controller.moves().state().loading);
        const auto selected = std::count_if(controller.moves().state().page->moves.begin(),
                                            controller.moves().state().page->moves.end(),
                                            [](const auto& move) { return move.refinementTarget; });
        qInfo("adaptive policy=%d routine=%d selected=%lld quick=5 targets=%zu first-deep=%lldms "
              "leading-risk-deep=%lldms total=%lldms",
              policy, routine, static_cast<long long>(selected), order.size(),
              static_cast<long long>(firstDeep), static_cast<long long>(leadingRiskDeep),
              static_cast<long long>(timer.elapsed()));
        if (routine || policy == 2) {
            return;
        }
        controller.selectMoment(3);
        QTRY_VERIFY(controller.moves().state().page && !controller.moves().state().loading);
        const auto page = controller.moves().state().page.value();
        const auto reply = std::find_if(page.moves.begin(), page.moves.end(),
                                        [](const auto& move) { return move.positionIndex == 3; });
        QVERIFY(reply != page.moves.end());
        const auto details = app::reviewMoveDetails(*reply);
        QVERIFY(details.contains(QStringLiteral("Selection policy: %1").arg(policy)));
        QVERIFY(details.contains(QStringLiteral("preceding move (ply 2)")));
        QVERIFY(details.contains(QStringLiteral("not a verified missed chance")));
        const auto output = qEnvironmentVariable("CHESS_REVIEW_QUICK_RENDER_DIR");
        if (!output.isEmpty()) {
            f.window.findChild<QPushButton*>(QStringLiteral("reviewTaskStatus"))->click();
            f.window.resize(1100, 720);
            f.window.show();
            QTest::qWait(30);
            f.window.resize(1100, 720);
            f.window.findChild<QPushButton*>(QStringLiteral("reviewDetailsToggle"))->click();
            f.window.findChild<QTabWidget*>(QStringLiteral("reviewResultsTabs"))
                ->setCurrentIndex(1);
            QCoreApplication::processEvents();
            auto* inspector =
                f.window.findChild<QPlainTextEdit*>(QStringLiteral("reviewMoveDetails"));
            for (auto* scroll : f.window.findChildren<QScrollArea*>()) {
                scroll->ensureWidgetVisible(inspector);
            }
            QCoreApplication::processEvents();
            QVERIFY(f.window.grab().save(output + QStringLiteral("/adaptive-opportunity.png")));
            QVERIFY(inspector->grab().save(output +
                                           QStringLiteral("/adaptive-opportunity-details.png")));
        }
    }
    void rankedResumeSkipsAcknowledgedHigherPosition() {
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 2. Nf3 Nc6 *"));
        const std::vector<int> order{3, 4, 1, 2};
        const std::vector<int> scores{500, 500, 560, 560, 760};
        f.script(0, -1, 0, false, false, QStringLiteral("cp 0"), QStringLiteral("Test engine"), 0,
                 false, {}, 0, 0, false, RetryScript::None, order, scores);
        QFile runtime(f.directory.filePath(QStringLiteral("runtime.json")));
        QVERIFY(runtime.open(QIODevice::ReadOnly));
        auto script = QJsonDocument::fromJson(runtime.readAll()).object()["steps"].toArray();
        runtime.close();
        int keep = 0;
        for (int index = 0; index < script.size(); ++index) {
            if (script[index].toObject()["expect"].toString() ==
                QStringLiteral("go nodes 1000000")) {
                keep = index + 2; // Retain the final readiness barrier before quit.
                break;
            }
        }
        QVERIFY(keep > 0);
        while (script.size() > keep) {
            script.removeLast();
        }
        script.append(quit());
        engine_test::scenario(runtime.fileName(), script);
        auto& controller = f.startup.reviewGame();
        std::vector<std::pair<std::uint64_t, int>> searches;
        const auto observed = connect(
            &f.supervisor(), &engine::EngineSupervisor::scheduledFinished, &controller,
            [&](const engine::ScheduledResult& result) {
                if (result.evidence && result.evidence->evidence.provenance) {
                    const auto& evidence = result.evidence->evidence;
                    if (const auto* nodes = std::get_if<engine::NodeLimit>(&evidence.limit)) {
                        searches.emplace_back(
                            nodes->nodes,
                            static_cast<int>(evidence.provenance->position.moves.size()));
                    }
                }
            });
        const auto disconnectObserved = qScopeGuard([&] { disconnect(observed); });
        const auto cancel = connect(&f.supervisor(), &engine::EngineSupervisor::scheduledFinished,
                                    &controller, [&](const engine::ScheduledResult& result) {
                                        if (result.evidence) {
                                            const auto* nodes = std::get_if<engine::NodeLimit>(
                                                &result.evidence->evidence.limit);
                                            if (nodes && nodes->nodes == 1000000) {
                                                controller.cancel();
                                            }
                                        }
                                    });
        const auto disconnectCancel = qScopeGuard([&] { disconnect(cancel); });
        f.run();
        QVERIFY(f.done());
        disconnect(cancel);
        const auto run = *controller.state().run;
        QCOMPARE(run.status, review::JobStatus::Cancelled);
        auto& db = f.startup.gameRepository();
        QSignalSpy loaded(&db, &persistence::Database::reviewEvidenceLoaded);
        db.loadReviewEvidence(QUuid::createUuid(), run.id, 3, review::EvidenceStage::Deepening);
        QTRY_VERIFY(!loaded.empty());
        const auto saved = qvariant_cast<persistence::LoadReviewEvidenceResult>(loaded.front()[1]);
        QVERIFY(std::holds_alternative<persistence::StoredReviewEvidence>(saved));
        const auto attempt =
            std::get<persistence::StoredReviewEvidence>(saved).evidence.attemptIdentity;
        f.script(5, -1, 0, false, false, QStringLiteral("cp 0"), QStringLiteral("Test engine"), 0,
                 false, {}, 0, 0, false, RetryScript::None, {4, 1, 2}, scores, order);
        controller.resume(run.id);
        QVERIFY(f.done());
        QVERIFY2(controller.state().error.isEmpty(), qPrintable(controller.state().error));
        QCOMPARE(controller.state().run->status, review::JobStatus::Complete);
        std::vector<std::pair<std::uint64_t, int>> expected;
        for (int index = 0; index < 5; ++index) {
            expected.emplace_back(100000, index);
        }
        for (const std::uint64_t nodes : {1000000U, 5000000U}) {
            for (const int index : order) {
                expected.emplace_back(nodes, index);
            }
        }
        QCOMPARE(searches, expected);
        loaded.clear();
        db.loadReviewEvidence(QUuid::createUuid(), run.id, 3, review::EvidenceStage::Deepening);
        QTRY_VERIFY(!loaded.empty());
        const auto retained =
            qvariant_cast<persistence::LoadReviewEvidenceResult>(loaded.front()[1]);
        QVERIFY(std::holds_alternative<persistence::StoredReviewEvidence>(retained));
        QCOMPARE(std::get<persistence::StoredReviewEvidence>(retained).evidence.attemptIdentity,
                 attempt);
    }
    void scanGapsFeedOneRefinementCycle_data() {
        QTest::addColumn<bool>("repair");
        QTest::addColumn<bool>("missing");
        QTest::newRow("weak-repaired") << true << false;
        QTest::newRow("weak-unresolved") << false << false;
        QTest::newRow("missing-repaired") << true << true;
        QTest::newRow("missing-unresolved") << false << true;
    }
    void scanGapsFeedOneRefinementCycle() {
        QFETCH(bool, repair);
        QFETCH(bool, missing);
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 *"));
        f.script(0, -1, 0, !missing, false, missing ? QString{} : QStringLiteral("cp 25"),
                 QStringLiteral("Test engine"), 0, false,
                 repair ? QStringLiteral("cp 0") : QString{});
        auto& controller = f.startup.reviewGame();
        std::array<int, 5> searches{};
        bool earlyReview = false;
        bool fullScanBarrier = true;
        QElapsedTimer elapsed;
        qint64 scanMilliseconds = -1;
        const auto scanChanged =
            connect(&controller, &app::ReviewGameController::changed, &controller, [&] {
                const auto& state = controller.state();
                if (state.run && state.run->completedPositions == state.run->totalPositions &&
                    scanMilliseconds < 0) {
                    scanMilliseconds = elapsed.elapsed();
                }
                if (state.active && state.overview && state.run &&
                    state.run->completedPositions < state.run->totalPositions &&
                    state.overview->quickScan[0].unusable > 0) {
                    earlyReview = true;
                }
            });
        const auto scanFinished =
            connect(&f.supervisor(), &engine::EngineSupervisor::scheduledFinished, &controller,
                    [&](const engine::ScheduledResult& result) {
                        if (!result.evidence) {
                            return;
                        }
                        const auto* nodes =
                            std::get_if<engine::NodeLimit>(&result.evidence->evidence.limit);
                        if (!nodes) {
                            return;
                        }
                        const int stage = nodes->nodes == 100000     ? 0
                                          : nodes->nodes == 1000000  ? 1
                                          : nodes->nodes == 5000000  ? 2
                                          : nodes->nodes == 10000000 ? 3
                                                                     : 4;
                        ++searches[static_cast<std::size_t>(stage)];
                        if (stage > 0 && (!controller.taskState().run ||
                                          controller.taskState().run->completedPositions != 3)) {
                            fullScanBarrier = false;
                        }
                    });
        const auto disconnectObservers = qScopeGuard([&] {
            disconnect(scanChanged);
            disconnect(scanFinished);
        });
        elapsed.start();
        f.run();
        QVERIFY(f.done());
        QVERIFY2(controller.state().error.isEmpty(), qPrintable(controller.state().error));
        QCOMPARE(controller.state().run->status, review::JobStatus::Complete);
        QTRY_VERIFY(controller.state().overview &&
                    controller.state().overview->revision == controller.state().run->revision);
        const auto& overview = *controller.state().overview;
        QCOMPARE(overview.quickScan[0].unusable, 1);
        QCOMPARE(overview.quickScan[1].unusable, 1);
        QCOMPARE(overview.quickScan[0].usable, 0);
        QCOMPARE(overview.quickScan[0].awaiting, 0);
        QCOMPARE(overview.refinementPositions, (std::vector<int>{0, 1, 2}));
        QCOMPARE(overview.deepenedPositions, 3);
        QCOMPARE(overview.verifiedPositions, 3);
        QCOMPARE(overview.sides[0].evaluated, repair ? 1 : 0);
        QCOMPARE(searches, (std::array<int, 5>{3, 3, 3, repair ? 0 : 3, repair ? 0 : 3}));
        QVERIFY(earlyReview);
        QVERIFY(fullScanBarrier);
        QTRY_VERIFY(controller.moves().state().page && !controller.moves().state().loading);
        const auto& move = controller.moves().state().page->moves.front();
        QCOMPARE(move.quickScan.state, review::ScanState::Unusable);
        QVERIFY(move.quickScan.needsRefinement());
        QVERIFY(
            app::reviewMoveDetails(move).contains(QStringLiteral("Quick-scan refinement reasons")));
        const auto details = app::reviewMoveDetails(move);
        QVERIFY(details.contains(QStringLiteral("Original quick scan:")));
        QVERIFY(!details.contains(QStringLiteral("Stronger evidence is pending")));
        QVERIFY(app::reviewOverviewText(overview, chess::Color::White)
                    .contains(QStringLiteral("original quick-scan coverage")));

        qInfo("scan map: scan=%lld ms total=%lld ms quick=%d deep=%d verification=%d usable=%d "
              "gaps=%d targets=%zu",
              static_cast<long long>(scanMilliseconds), static_cast<long long>(elapsed.elapsed()),
              searches[0], searches[1], searches[2],
              overview.quickScan[0].usable + overview.quickScan[1].usable,
              overview.quickScan[0].unusable + overview.quickScan[1].unusable,
              overview.refinementPositions.size());
    }
    void unavailableBestMovesAreNotReportedAsUnchanged() {
        persistence::ReviewMove move;
        move.refinementTarget = true;
        move.quickScan.pipelineVersion = 5;
        move.classification.emplace();
        move.stabilityEvidenceIds[2] = 1;
        QVERIFY(app::reviewMoveDetails(move).contains(QStringLiteral("best move not comparable")));
        move.comparisonBestMoveChanged = false;
        QVERIFY(app::reviewMoveDetails(move).contains(QStringLiteral("best move unchanged")));
        move.comparisonBestMoveChanged = true;
        QVERIFY(app::reviewMoveDetails(move).contains(QStringLiteral("best move changed")));
    }
    void adaptiveDeeperSearches_data() {
        QTest::addColumn<int>("policy");
        QTest::addColumn<int>("rounds");
        QTest::addColumn<bool>("unresolved");
        QTest::newRow("already-stable") << 5 << 0 << false;
        QTest::newRow("one-extra-round") << 5 << 1 << false;
        QTest::newRow("two-extra-rounds") << 5 << 2 << false;
        QTest::newRow("exhausted") << 5 << 2 << true;
        QTest::newRow("policy4-stable") << 4 << 0 << false;
        QTest::newRow("policy4-disagreement") << 4 << 1 << false;
        QTest::newRow("policy4-missing") << 4 << 2 << false;
        QTest::newRow("policy4-bounds") << 4 << 2 << true;
    }
    void adaptiveDeeperSearches() {
        QFETCH(int, policy);
        QFETCH(int, rounds);
        QFETCH(bool, unresolved);
        Fixture f;
        QVERIFY(f.seed());
        std::optional<review::ReviewId> baseline;
        {
            auto& controller = f.startup.reviewGame();
            f.script(0, 1);
            const auto cancel =
                connect(&controller, &app::ReviewGameController::changed, &controller, [&] {
                    if (controller.state().active && controller.state().run &&
                        controller.state().run->completedPositions == 1) {
                        controller.cancel();
                    }
                });
            f.run();
            QVERIFY(f.done());
            disconnect(cancel);
            auto& db = f.startup.gameRepository();
            QSignalSpy loaded(&db, &persistence::Database::reviewLoaded);
            db.loadReview(QUuid::createUuid(), controller.state().run->id);
            QTRY_VERIFY(!loaded.empty());
            auto settings = std::get<persistence::StoredReview>(
                                qvariant_cast<persistence::LoadReviewResult>(loaded.front()[1]))
                                .settings;
            settings.pipelineVersion = static_cast<std::uint32_t>(policy);
            QSignalSpy created(&db, &persistence::Database::reviewCreated);
            db.createReview(QUuid::createUuid(), settings);
            QTRY_VERIFY(!created.empty());
            const auto run = std::get<persistence::ReviewSummary>(
                qvariant_cast<persistence::CreateReviewResult>(created.front()[1]));
            persistence::ReviewCheckpoint cp;
            cp.reviewId = run.id;
            cp.expectedRevision = run.revision;
            cp.status = review::JobStatus::Cancelled;
            QSignalSpy checkpointed(&db, &persistence::Database::reviewCheckpointed);
            db.checkpointReview(QUuid::createUuid(), cp);
            QTRY_VERIFY(!checkpointed.empty());
            QVERIFY(std::holds_alternative<persistence::ReviewSummary>(
                qvariant_cast<persistence::CheckpointReviewResult>(checkpointed.front()[1])));
            baseline = run.id;
        }
        const std::vector<QString> scores =
            rounds == 1 ? std::vector<QString>{QString{}, QStringLiteral("cp 0"),
                                               QStringLiteral("cp 100"), QStringLiteral("cp 100")}
            : rounds == 2 && !unresolved
                ? std::vector<QString>{QString{}, QString{}, QString{}, QStringLiteral("cp 0"),
                                       QStringLiteral("cp 0")}
                : std::vector<QString>{};
        f.script(0, -1, 0, unresolved, false, QStringLiteral("cp 25"),
                 QStringLiteral("Test engine"), 0, false, {}, 0, 0, false, RetryScript::None, {},
                 {}, {}, policy == 5 ? rounds : 0, scores);
        std::array<int, 5> searches{};
        QElapsedTimer elapsed;
        qint64 quickMs = -1;
        qint64 confirmedMs = -1;
        int heartbeats = 0;
        QTimer heartbeat;
        heartbeat.setInterval(5);
        auto& controller = f.startup.reviewGame();
        const auto observer = connect(
            &f.supervisor(), &engine::EngineSupervisor::scheduledFinished, &controller,
            [&](const engine::ScheduledResult& result) {
                if (result.evidence) {
                    if (const auto* limit =
                            std::get_if<engine::NodeLimit>(&result.evidence->evidence.limit)) {
                        const std::array<std::uint64_t, 5> budgets{100000, 1000000, 5000000,
                                                                   10000000, 20000000};
                        const auto found = std::find(budgets.begin(), budgets.end(), limit->nodes);
                        if (found != budgets.end()) {
                            ++searches[static_cast<std::size_t>(found - budgets.begin())];
                        }
                    }
                }
            });
        connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++heartbeats; });
        const auto progress =
            connect(&controller, &app::ReviewGameController::changed, &controller, [&] {
                if (quickMs < 0 && controller.state().run &&
                    controller.state().run->completedPositions == 3) {
                    quickMs = elapsed.elapsed();
                }
                if (confirmedMs < 0 && controller.state().overview &&
                    controller.state().overview->sides[0].stable +
                            controller.state().overview->sides[1].stable ==
                        2) {
                    confirmedMs = elapsed.elapsed();
                }
            });
        elapsed.start();
        heartbeat.start();
        if (baseline) {
            controller.resume(*baseline);
        } else {
            f.run();
        }
        QVERIFY(f.done());
        heartbeat.stop();
        disconnect(observer);
        disconnect(progress);
        QVERIFY2(controller.state().error.isEmpty(), qPrintable(controller.state().error));
        QCOMPARE(controller.state().run->status, review::JobStatus::Complete);
        QTRY_VERIFY(controller.state().overview &&
                    controller.state().overview->revision == controller.state().run->revision);
        QCOMPARE(searches[0], 3);
        QCOMPARE(searches[1], 3);
        QCOMPARE(searches[2], 3);
        QCOMPARE(searches[3], policy == 5 && rounds >= 1 ? 3 : 0);
        QCOMPARE(searches[4], policy == 5 && rounds >= 2 ? 3 : 0);
        const auto stable = controller.state().overview->sides[0].stable +
                            controller.state().overview->sides[1].stable;
        QCOMPARE(stable, unresolved || (policy == 4 && rounds > 0) ? 0 : 2);
        if (stable == 2 && confirmedMs < 0) {
            confirmedMs = elapsed.elapsed();
        }
        QTRY_VERIFY(controller.moves().state().page && !controller.moves().state().loading);
        const auto& move = controller.moves().state().page->moves.front();
        QCOMPARE(move.escalationRound, policy == 5 ? rounds : 0);
        if (policy == 5) {
            QVERIFY(app::reviewMoveDetails(move).contains(
                unresolved ? QStringLiteral("two-round limit")
                           : QStringLiteral("confirm the result")));
        }
        QVERIFY(heartbeats > 0);
        const auto output = qEnvironmentVariable("CHESS_REVIEW_QUICK_RENDER_DIR");
        if (!output.isEmpty() && policy == 5 && rounds == 2) {
            f.window.findChild<QPushButton*>(QStringLiteral("reviewTaskStatus"))->click();
            f.window.resize(1100, 720);
            f.window.show();
            QTest::qWait(30);
            f.window.resize(1100, 720);
            f.window.findChild<QPushButton*>(QStringLiteral("reviewDetailsToggle"))->click();
            f.window.findChild<QTabWidget*>(QStringLiteral("reviewResultsTabs"))
                ->setCurrentIndex(1);
            controller.selectMoment(1);
            QTRY_VERIFY(!controller.moves().state().loading &&
                        !controller.exploration().state().loading);
            auto* inspector =
                f.window.findChild<QPlainTextEdit*>(QStringLiteral("reviewMoveDetails"));
            const auto adaptive =
                inspector->toPlainText().indexOf(QStringLiteral("Adaptive deeper searches"));
            QVERIFY(adaptive >= 0);
            auto cursor = inspector->textCursor();
            cursor.setPosition(static_cast<int>(adaptive));
            inspector->setTextCursor(cursor);
            inspector->verticalScrollBar()->setValue(cursor.blockNumber());
            for (auto* scroll : f.window.findChildren<QScrollArea*>()) {
                scroll->ensureWidgetVisible(inspector);
            }
            QCoreApplication::processEvents();
            const auto name = unresolved ? QStringLiteral("/escalation-unresolved")
                                         : QStringLiteral("/escalation-confirmed");
            QVERIFY(f.window.grab().save(output + name + QStringLiteral(".png")));
            QVERIFY(inspector->grab().save(output + name + QStringLiteral("-details.png")));
        }
        const std::array<std::uint64_t, 5> requested{100000, 1000000, 5000000, 10000000, 20000000};
        std::uint64_t requestedNodes = 0;
        for (std::size_t i = 0; i < searches.size(); ++i) {
            requestedNodes += requested[i] * static_cast<std::uint64_t>(searches[i]);
        }
        qInfo("adaptive escalation: policy=%d rounds=%d unresolved=%d quick=%lldms total=%lldms "
              "confirmed=%lldms searches=%d/%d/%d/%d/%d requestedNodes=%llu stable=%d/2 "
              "heartbeats=%d",
              policy, rounds, unresolved, static_cast<long long>(quickMs),
              static_cast<long long>(elapsed.elapsed()), static_cast<long long>(confirmedMs),
              searches[0], searches[1], searches[2], searches[3], searches[4],
              static_cast<unsigned long long>(requestedNodes), stable, heartbeats);
    }
    void escalationResumeRetainsAcknowledgedAttempt() {
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, -1, 0, true);
        QFile runtime(f.directory.filePath(QStringLiteral("runtime.json")));
        QVERIFY(runtime.open(QIODevice::ReadOnly));
        auto steps = QJsonDocument::fromJson(runtime.readAll()).object()["steps"].toArray();
        runtime.close();
        int searches = 0;
        int keep = 0;
        for (int i = 0; i < steps.size(); ++i) {
            if (steps[i].toObject()["expect"].toString().startsWith(QStringLiteral("go nodes")) &&
                ++searches == 10) {
                keep = i + 2;
                break;
            }
        }
        QVERIFY(keep > 0);
        while (steps.size() > keep) {
            steps.removeLast();
        }
        steps.append(quit());
        engine_test::scenario(runtime.fileName(), steps);
        auto& controller = f.startup.reviewGame();
        const auto cancel =
            connect(&controller, &app::ReviewGameController::changed, &controller, [&] {
                if (controller.state().active && controller.state().escalationRound == 1 &&
                    controller.state().refinementDone == 1) {
                    controller.cancel();
                }
            });
        f.run();
        QVERIFY(f.done());
        disconnect(cancel);
        QVERIFY2(controller.state().error.isEmpty(), qPrintable(controller.state().error));
        QCOMPARE(controller.state().run->status, review::JobStatus::Cancelled);
        const auto id = controller.state().run->id;
        const auto readSaved = [&]() {
            QSignalSpy loaded(&f.startup.gameRepository(),
                              &persistence::Database::reviewEvidenceLoaded);
            f.startup.gameRepository().loadReviewEvidence(QUuid::createUuid(), id, 0,
                                                          review::EvidenceStage::EscalationOne);
            if (!loaded.wait()) {
                return persistence::LoadReviewEvidenceResult{persistence::ReviewError{}};
            }
            return qvariant_cast<persistence::LoadReviewEvidenceResult>(loaded.front()[1]);
        };
        const auto saved = readSaved();
        QVERIFY(std::holds_alternative<persistence::StoredReviewEvidence>(saved));
        const auto attempt =
            std::get<persistence::StoredReviewEvidence>(saved).evidence.attemptIdentity;
        f.startup.engines().disconnectEngine();
        QTRY_COMPARE(f.startup.engines().state().runtime.phase, engine::RuntimePhase::Disconnected);
        f.script(0, -1, 0, true);
        QVERIFY(runtime.open(QIODevice::ReadOnly));
        steps = QJsonDocument::fromJson(runtime.readAll()).object()["steps"].toArray();
        runtime.close();
        searches = 0;
        int eraseEnd = 2;
        for (int i = 0; i < steps.size(); ++i) {
            if (steps[i].toObject()["expect"].toString().startsWith(QStringLiteral("go nodes")) &&
                ++searches == 10) {
                eraseEnd = i + 2;
                break;
            }
        }
        while (eraseEnd-- > 2) {
            steps.removeAt(2);
        }
        engine_test::scenario(runtime.fileName(), steps);
        int resumed = 0;
        const auto observer =
            connect(&f.supervisor(), &engine::EngineSupervisor::scheduledFinished, &controller,
                    [&](const engine::ScheduledResult& result) {
                        if (result.outcome == engine::ScheduledOutcome::Completed) {
                            ++resumed;
                        }
                    });
        controller.resume(id);
        QVERIFY(f.done());
        disconnect(observer);
        QVERIFY2(controller.state().error.isEmpty(), qPrintable(controller.state().error));
        QCOMPARE(controller.state().run->status, review::JobStatus::Complete);
        QCOMPARE(resumed, 5);
        const auto retained = readSaved();
        QVERIFY(std::holds_alternative<persistence::StoredReviewEvidence>(retained));
        QCOMPARE(std::get<persistence::StoredReviewEvidence>(retained).evidence.attemptIdentity,
                 attempt);
    }
    void refinementComparesAlternativesAndUpdatesSummary() {
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 *", true));
        f.script(0, -1, 0, false, false, QStringLiteral("cp 200"), QStringLiteral("Test engine"), 0,
                 true, QStringLiteral("cp 0"));
        f.run();
        QVERIFY(f.done());
        auto& controller = f.startup.reviewGame();
        QVERIFY2(controller.state().error.isEmpty(), qPrintable(controller.state().error));
        QCOMPARE(controller.state().run->status, review::JobStatus::Complete);
        QTRY_VERIFY(controller.state().overview &&
                    controller.state().overview->revision == controller.state().run->revision);
        QCOMPARE(controller.state().overview->sides[0].accuracy, std::optional<double>(100));
        QCOMPARE(controller.state().overview->sides[1].stable, 1);
        controller.selectMoment(2);
        QTRY_VERIFY(!controller.exploration().state().loading &&
                    !controller.exploration().state().candidates.empty());
        QVERIFY(
            controller.exploration().state().candidates.front().contains(QStringLiteral("50.0%")));
        QVERIFY(!controller.exploration().state().canRetry);
        QCOMPARE(controller.state().overview->totalKeyMoments, 0);
        QTRY_VERIFY(controller.moves().state().page && !controller.moves().state().loading);
        const auto& move = controller.moves().state().page->moves.front();
        QVERIFY(move.classification->stable);
        QCOMPARE(move.classification->alternatives.size(), std::size_t(2));
        QVERIFY(f.sql(QStringLiteral("DELETE FROM analysis_results")));
        f.startup.engines().disconnectEngine();
        QTRY_COMPARE(f.startup.engines().state().runtime.phase, engine::RuntimePhase::Disconnected);
        controller.refreshOverview();
        QTRY_VERIFY(!controller.state().busy);
        QVERIFY(controller.state().overviewError.isEmpty());
        QCOMPARE(controller.state().overview->sides[1].accuracy, std::optional<double>(100));
    }
    void refinementCancellationResumesOnlyMissingPositions_data() {
        QTest::addColumn<bool>("gap");
        QTest::newRow("ordinary-targets") << false;
        QTest::newRow("gap-targets") << true;
    }
    void refinementCancellationResumesOnlyMissingPositions() {
        QFETCH(bool, gap);
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, -1, 0, gap);
        QFile runtime(f.directory.filePath(QStringLiteral("runtime.json")));
        QVERIFY(runtime.open(QIODevice::ReadOnly));
        auto script = QJsonDocument::fromJson(runtime.readAll()).object()["steps"].toArray();
        runtime.close();
        int searches = 0;
        int keep = 0;
        for (int i = 0; i < script.size(); ++i) {
            if (script[i].toObject()["expect"].toString().startsWith(QStringLiteral("go nodes")) &&
                ++searches == 4) {
                keep = i + 2;
                break;
            }
        }
        QVERIFY(keep > 0);
        while (script.size() > keep) {
            script.removeLast();
        }
        script.append(quit());
        engine_test::scenario(runtime.fileName(), script);
        auto& controller = f.startup.reviewGame();
        const auto cancel =
            connect(&controller, &app::ReviewGameController::changed, &controller, [&] {
                if (controller.state().active && controller.state().refinementDone == 1) {
                    controller.cancel();
                }
            });
        f.run();
        QVERIFY(f.done());
        disconnect(cancel);
        QCOMPARE(controller.state().run->status, review::JobStatus::Cancelled);
        const auto runId = controller.state().run->id;
        f.startup.engines().disconnectEngine();
        QTRY_COMPARE(f.startup.engines().state().runtime.phase, engine::RuntimePhase::Disconnected);
        const std::array<std::vector<int>, 2> extraOrder{{{0, 1}, {1, 2}}};
        f.script(3, -1, 0, false, false, QStringLiteral("cp 25"), QStringLiteral("Test engine"), 0,
                 false, {}, 1, 0, false, RetryScript::None, {}, {}, {}, gap ? 2 : 0, {},
                 extraOrder);
        controller.resume(runId);
        QVERIFY(f.done());
        QVERIFY2(controller.state().error.isEmpty(), qPrintable(controller.state().error));
        QCOMPARE(controller.state().run->status, review::JobStatus::Complete);
        QTRY_VERIFY(controller.state().overview &&
                    controller.state().overview->revision == controller.state().run->revision);
        QCOMPARE(controller.state().overview->deepenedPositions, 3);
        QCOMPARE(controller.state().overview->verifiedPositions, 3);
    }
    void chronologicalMomentsAttachOpponentContext() {
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, -1, 0, false, false, QStringLiteral("cp 200"));
        f.run();
        QVERIFY(f.done());
        const auto& state = f.startup.reviewGame().state();
        QVERIFY2(state.error.isEmpty(), qPrintable(state.error));
        QTRY_VERIFY(state.overview && state.overview->revision == state.run->revision);
        QCOMPARE(state.overview->totalKeyMoments, 1);
        QCOMPARE(state.overview->keyMoments.front().positionIndex, 2);
        QCOMPARE(state.overview->keyMoments.front().contextPosition, std::optional<int>(1));
        f.startup.reviewGame().selectMoment(2);
        QTRY_VERIFY(!f.startup.reviewGame().moves().state().loading);
        QCOMPARE(f.startup.reviewGame().moves().state().selectedPosition, 2);
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
    void classificationsAppearBeforeTheScanFinishes() {
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 2. Nf3 Nc6 3. Bb5 a6 *"));
        f.script(0, -1, 100, false, false, QStringLiteral("cp 0"));
        f.run();
        auto& controller = f.startup.reviewGame();
        QVERIFY(QTest::qWaitFor(
            [&] {
                const auto& state = controller.moves().state();
                return controller.state().active && state.page && !state.page->moves.empty() &&
                       state.page->moves.front().classification.has_value();
            },
            10000));
        QVERIFY(controller.state().run->completedPositions >= 2);
        QVERIFY(controller.state().run->completedPositions <
                controller.state().run->totalPositions);
        const auto label = controller.moves().state().page->moves.front().classification->label;
        controller.cancel();
        QVERIFY(f.done());
        QVERIFY(QTest::qWaitFor([&] {
            return controller.moves().state().page && !controller.moves().state().loading;
        }));
        QCOMPARE(controller.moves().state().page->moves.front().classification->label, label);
        QVERIFY(!controller.moves().state().page->moves.back().classification);
    }
    void moveInspectionShowsVerifiedEvidenceAndKeyboardSelection() {
        Fixture f;
        QVERIFY(f.seed());
        f.script(0, -1, 0, false, false, QStringLiteral("cp 200"));
        f.run();
        QVERIFY(f.done());
        auto& controller = f.startup.reviewGame();
        QVERIFY(QTest::qWaitFor([&] {
            return controller.moves().state().page && !controller.moves().state().loading;
        }));
        QVERIFY2(controller.moves().state().error.isEmpty(),
                 qPrintable(controller.moves().state().error));
        const auto& page = *controller.moves().state().page;
        QCOMPARE(page.moves.size(), std::size_t(2));
        QVERIFY(page.moves[0].classification);
        QVERIFY(page.moves[0].classification->label);
        QVERIFY(page.moves[0].classification->stable);
        auto* table = f.window.findChild<QTableView*>(QStringLiteral("reviewMoves"));
        auto* details = f.window.findChild<QPlainTextEdit*>(QStringLiteral("reviewMoveDetails"));
        QVERIFY(table);
        QVERIFY(details);
        QCOMPARE(table->model()->rowCount(), 2);
        controller.selectMoment(1);
        QTRY_VERIFY(details->toPlainText().contains(QStringLiteral("Rule:")));
        QVERIFY(details->toPlainText().contains(QStringLiteral("Evidence IDs:")));
        QVERIFY(details->toPlainText().contains(QStringLiteral("not assessed")));
        table->setCurrentIndex(table->model()->index(0, 0));
        QTest::keyPress(table, Qt::Key_Down);
        QTest::keyRelease(table, Qt::Key_Down);
        QCOMPARE(controller.moves().state().selectedPosition, 2);
        QVERIFY(details->toPlainText().contains(QStringLiteral("Black")));
    }
    void classificationPagesCoalesceAndKeepUnavailableMoves() {
        std::string text;
        for (int i = 0; i < 30; ++i) {
            text += "Nf3 Nf6 Ng1 Ng8 ";
        }
        text += "*";
        Fixture f;
        QVERIFY(f.seed(text));
        f.script(0, 2);
        f.run();
        QVERIFY(QTest::qWaitFor(
            [&] {
                return f.startup.reviewGame().state().run &&
                       f.startup.reviewGame().state().run->completedPositions >= 2;
            },
            10000));
        f.startup.reviewGame().cancel();
        QVERIFY(f.done());
        auto& moves = f.startup.reviewGame().moves();
        moves.showPage(50);
        moves.showPage(100);
        moves.showPage(50);
        QVERIFY(QTest::qWaitFor([&] {
            return moves.state().page && moves.state().page->offset == 50 && !moves.state().loading;
        }));
        QCOMPARE(moves.state().page->moves.size(), std::size_t(50));
        QVERIFY(!moves.state().page->moves.front().classification);
        moves.select(100);
        QCOMPARE(moves.state().selectedPosition, 100);
        moves.showPage(100);
        QVERIFY(QTest::qWaitFor([&] {
            return moves.state().page && moves.state().page->offset == 100 &&
                   !moves.state().loading;
        }));
        QCOMPARE(moves.state().page->moves.size(), std::size_t(20));
        moves.showPage(0);
        QVERIFY(QTest::qWaitFor([&] {
            return moves.state().page && moves.state().page->offset == 0 && !moves.state().loading;
        }));
        QVERIFY(moves.state().page->moves.front().classification);
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
        auto& controller = f.startup.reviewGame();
        controller.refreshOverview();
        controller.inspect(first.id);
        QTRY_VERIFY(controller.state().run && controller.state().run->id == first.id &&
                    !controller.state().busy);
        QVERIFY(controller.state().overviewError.isEmpty());
    }
    void cancelledPreparationAndSecondRunRejected() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        f.startup.reviewGame().start(chess::Color::White, f.profileId, review::Quality::Fast);
        f.startup.reviewGame().selectGame(99999);
        QCOMPARE(f.startup.reviewGame().taskState().gameId, f.gameId);
        QCOMPARE(f.startup.reviewGame().state().gameId, persistence::GameId{99999});
        f.startup.reviewGame().cancel();
        QTRY_VERIFY(!f.startup.reviewGame().taskState().busy);
        if (f.startup.reviewGame().taskState().run) {
            QCOMPARE(f.startup.reviewGame().taskState().run->status, review::JobStatus::Cancelled);
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
        QSignalSpy lookups(&f.startup.gameRepository(), &persistence::Database::analysisFound);
        QSignalSpy pages(&f.startup.gameRepository(), &persistence::Database::reviewMovesLoaded);
        QElapsedTimer elapsed;
        elapsed.start();
        f.run();
        QVERIFY(f.done(90000));
        heartbeat.stop();
        qInfo("81-position adaptive review: %lld ms; %lld cache lookups; %lld move pages",
              static_cast<long long>(elapsed.elapsed()), static_cast<long long>(lookups.size()),
              static_cast<long long>(pages.size()));
        QVERIFY2(f.startup.reviewGame().state().error.isEmpty(),
                 qPrintable(f.startup.reviewGame().state().error));
        QCOMPARE(f.startup.reviewGame().state().run->completedPositions, 81);
        QVERIFY(heartbeats > 10);
        QCOMPARE(lookups.size(), 0); // This fake engine's resources never authorize reuse.
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
        const auto revision = f.startup.reviewGame().state().run->revision;
        bool regressed = false;
        const auto revisionObserver =
            connect(&f.startup.reviewGame(), &app::ReviewGameController::changed, &f.window, [&] {
                const auto& run = f.startup.reviewGame().state().run;
                if (run && run->revision < revision) {
                    regressed = true;
                }
            });
        const auto disconnectRevision = qScopeGuard([&] { disconnect(revisionObserver); });
        f.startup.engines().refresh();
        QTRY_VERIFY(!f.startup.engines().state().listing);
        QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Partial);
        QCOMPARE(f.startup.reviewGame().state().run->revision, revision);
        QVERIFY(!regressed);
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
    void stockfishProducesUsefulReview() {
        const auto executable = qEnvironmentVariable("CHESS_REVIEW_STOCKFISH");
        if (executable.isEmpty()) {
            QSKIP("Set CHESS_REVIEW_STOCKFISH to select the local engine for this regression.");
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
        pgn::PgnReader reader("1. e4 e5 2. Nf3 Nc6 3. Bc4 Qg5 4. Nxg5 *");
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
        QVERIFY(f.done(90000));
        auto& controller = f.startup.reviewGame();
        QVERIFY2(controller.state().error.isEmpty(), qPrintable(controller.state().error));
        QTRY_VERIFY(controller.state().overview);
        const auto& summary = *controller.state().overview;
        qInfo("Stockfish review: White %d/%d evaluated; Black %d/%d evaluated; key moments %d",
              summary.sides[0].evaluated, summary.sides[0].total, summary.sides[1].evaluated,
              summary.sides[1].total, summary.totalKeyMoments);
        QVERIFY(summary.sides[0].evaluated > 0);
        QVERIFY(summary.sides[1].evaluated >= 2);
        QVERIFY(summary.sides[1].accuracy);
        QVERIFY(summary.totalKeyMoments > 0);
        QVERIFY(
            summary.sides[1].counts[static_cast<std::size_t>(review::Classification::Blunder)] +
                summary.sides[1].counts[static_cast<std::size_t>(review::Classification::Mistake)] >
            0);
        const auto saved = controller.state().run->id;
        f.startup.engines().disconnectEngine();
        QTRY_COMPARE(f.startup.engines().state().runtime.phase, engine::RuntimePhase::Disconnected);
        controller.inspect(saved);
        QTRY_VERIFY(!controller.state().busy);
        QTRY_VERIFY(controller.state().overview);
        QVERIFY(controller.state().overview->sides[1].accuracy);
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
        QVERIFY(f.done(90000));
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
        QVERIFY(f.done(90000));
        QVERIFY2(f.startup.reviewGame().state().error.isEmpty(),
                 qPrintable(f.startup.reviewGame().state().error));
        QCOMPARE(f.startup.reviewGame().state().run->status, review::JobStatus::Complete);
        int quickSearches = 0;
        for (const auto& event : finished) {
            const auto result = qvariant_cast<engine::ScheduledResult>(event.front());
            if (result.evidence) {
                const auto* nodes =
                    std::get_if<engine::NodeLimit>(&result.evidence->evidence.limit);
                if (nodes && nodes->nodes == 100000) {
                    ++quickSearches;
                }
            }
        }
        QCOMPARE(quickSearches, first.totalPositions - compatibleHits);
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
        auto& moves = reopened.startup.reviewGame().moves();
        QTRY_VERIFY(moves.state().page && !moves.state().loading);
        QCOMPARE(moves.state().error, QString{});
        QCOMPARE(moves.state().page->moves.size(), std::size_t(2));
        for (const auto& move : moves.state().page->moves) {
            QVERIFY(move.classification);
            QCOMPARE(move.classification->modelVersion, review::classificationModelVersion);
        }
        QCOMPARE(reopened.startup.engines().state().runtime.phase,
                 engine::RuntimePhase::Disconnected);
    }
    void boardPreviewKeepsExplanationAndCommitIsExplicit() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
        f.run();
        QVERIFY(f.done());
        f.window.findChild<QPushButton*>(QStringLiteral("reviewTaskStatus"))->click();
        f.window.resize(1100, 720);
        f.window.show();
        auto& nav = f.startup.reviewGame().navigation();
        QTRY_VERIFY(nav.ready());
        QTRY_VERIFY(f.startup.reviewGame().moves().state().page);
        auto* inspector = f.window.findChild<QTableView*>(QStringLiteral("reviewMoves"));
        QCOMPARE(nav.selected(), 0);
        QVERIFY(!inspector->currentIndex().isValid());
        QVERIFY(inspector->selectionModel()->selectedIndexes().isEmpty());
        nav.select(nav.target(1));
        QTRY_COMPARE(nav.displayed(), 1);
        QTRY_VERIFY(nav.move(1));
        auto* headline = f.window.findChild<QLabel*>(QStringLiteral("reviewMomentHeadline"));
        auto* preview = f.window.findChild<QLabel*>(QStringLiteral("reviewPreview"));
        auto* board = f.window.findChild<ui::ChessBoardWidget*>(QStringLiteral("reviewBoard"));
        auto* timeline = f.window.findChild<ui::MoveTimeline*>(QStringLiteral("reviewTimeline"));
        QVERIFY(headline);
        QVERIFY(preview);
        QVERIFY(board);
        QVERIFY(timeline);
        const auto selectedHeadline = headline->text();
        const auto selectedBoard = board->state().pieces;
        bool sawLoadingPreview = false;
        QObject previewObserver;
        const auto previewChanged =
            connect(&nav, &app::ReviewNavigationController::changed, &previewObserver, [&] {
                if (nav.previewing() && nav.pending()) {
                    sawLoadingPreview = true;
                    QVERIFY(preview->text().startsWith(QStringLiteral("Loading preview")));
                }
            });
        const auto blackMove = timeline->model()->index(1, 2);
        QTest::mouseMove(timeline->viewport(), timeline->visualRect(blackMove).center());
        QTRY_VERIFY(nav.previewing());
        QTRY_COMPARE(nav.displayed(), 2);
        QVERIFY(sawLoadingPreview);
        disconnect(previewChanged);
        QCOMPARE(nav.selected(), 1);
        QCOMPARE(headline->text(), selectedHeadline);
        QVERIFY(!board->state().interactive);
        QVERIFY(nav.evaluation().whiteExpected);
        f.startup.reviewGame().moves().retry();
        QTRY_VERIFY(!f.startup.reviewGame().moves().state().loading);
        QCOMPARE(nav.selected(), 1);
        QCOMPARE(headline->text(), selectedHeadline);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(timeline, &leave);
        QTRY_COMPARE(nav.displayed(), 1);
        QCOMPARE(board->state().pieces, selectedBoard);
        QTest::mouseMove(timeline->viewport(),
                         timeline->visualRect(blackMove).center() + QPoint(1, 0));
        QTRY_VERIFY(nav.previewing());
        QTest::mouseClick(timeline->viewport(), Qt::LeftButton, Qt::NoModifier,
                          timeline->visualRect(blackMove).center());
        QTRY_COMPARE(nav.selected(), 2);
        QTRY_COMPARE(nav.displayed(), 2);
        QVERIFY(!nav.previewing());
        QVERIFY(headline->text().contains(QStringLiteral("e5")));
        nav.beginning();
        QTRY_COMPARE(nav.displayed(), 0);
        QVERIFY(!inspector->currentIndex().isValid());
        QVERIFY(inspector->selectionModel()->selectedIndexes().isEmpty());
        nav.select(nav.target(2));
        QTRY_COMPARE(nav.displayed(), 2);
        f.navigation.navigate(app::Destination::Home);
        f.window.findChild<QPushButton*>(QStringLiteral("reviewTaskStatus"))->click();
        QCOMPARE(nav.selected(), 2);
    }
    void renderWorkspace() {
        const auto directory = qEnvironmentVariable("CHESS_REVIEW_QUICK_RENDER_DIR");
        if (directory.isEmpty()) {
            QSKIP("Set CHESS_REVIEW_QUICK_RENDER_DIR to inspect workspace renders.");
        }
        Fixture f;
        QVERIFY(f.seed("1. e4 e5 *", true));
        f.script(0, -1, 0, false, false, QStringLiteral("cp 200"), QStringLiteral("Test engine"), 0,
                 true);
        f.run();
        QVERIFY(f.done());
        f.window.findChild<QPushButton*>(QStringLiteral("reviewTaskStatus"))->click();
        f.window.findChild<QComboBox*>(QStringLiteral("reviewSide"))->setCurrentIndex(2);
        f.window.resize(1100, 720);
        f.window.show();
        QTest::qWait(30);
        // Initial geometry restoration can clamp to the offscreen display. Apply
        // the requested render size after that first show has settled.
        f.window.resize(1100, 720);
        QCoreApplication::processEvents();
        QCOMPARE(f.window.size(), QSize(1100, 720));
        QVERIFY(QTest::qWaitFor(
            [&] { return f.startup.reviewGame().moves().state().page.has_value(); }));
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-light.png")));
        auto& exploration = f.startup.reviewGame().exploration();
        f.startup.reviewGame().selectMoment(2);
        QTRY_COMPARE(f.startup.reviewGame().navigation().displayed(), 2);
        QTRY_VERIFY(exploration.state().canRetry);
        exploration.showLine(1);
        QTRY_VERIFY(exploration.state().canAnalyze);
        exploration.end();
        QTRY_COMPARE(exploration.state().step, 1);
        QTRY_VERIFY(exploration.state().canAnalyze);
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-line-light.png")));
        exploration.returnToGame();
        exploration.tryAgain();
        QTRY_VERIFY(exploration.board().interactive);
        exploration.hint();
        exploration.hint();
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-practice-light.png")));
        exploration.returnToGame();
        auto* tabs = f.window.findChild<QTabWidget*>(QStringLiteral("reviewResultsTabs"));
        QVERIFY(tabs);
        auto* board = f.window.findChild<ui::ChessBoardWidget*>(QStringLiteral("reviewBoard"));
        QVERIFY(board);
        QVERIFY(board->boardRect().width() >= 236);
        QCOMPARE(board->boardRect().width(), board->boardRect().height());
        f.window.findChild<QPushButton*>(QStringLiteral("reviewDetailsToggle"))->click();
        tabs->setCurrentIndex(0);
        QCoreApplication::processEvents();
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-summary-light.png")));
        tabs->setCurrentIndex(1);
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
        auto* reviewNext = f.window.findChild<QPushButton*>(QStringLiteral("reviewNextMove"));
        auto* reviewShell = f.window.findChild<QScrollArea*>(QStringLiteral("shellScrollArea"));
        QVERIFY(reviewShell->viewport()->rect().contains(
            reviewNext->mapTo(reviewShell->viewport(), reviewNext->rect().center())));
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-dark.png")));
        exploration.tryAgain();
        QTRY_VERIFY(exploration.board().interactive);
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-practice-dark.png")));
        exploration.returnToGame();
        tabs->setCurrentIndex(0);
        QCoreApplication::processEvents();
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-summary-dark.png")));
        tabs->setCurrentIndex(1);
        f.window.resize(1800, 1000);
        QTest::qWait(30);
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-desktop.png")));
        auto font = f.window.font();
        font.setPointSize(font.pointSize() + 5);
        f.window.setFont(font);
        f.window.resize(1100, 720);
        QTest::qWait(30);
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-enlarged.png")));
        auto* shell = f.window.findChild<QScrollArea*>(QStringLiteral("shellScrollArea"));
        auto* next = f.window.findChild<QPushButton*>(QStringLiteral("nextReviewMoves"));
        QVERIFY(shell);
        QVERIFY(next);
        auto* explanationScroll =
            f.window.findChild<QScrollArea*>(QStringLiteral("reviewExplanationScroll"));
        QVERIFY(explanationScroll);
        explanationScroll->ensureWidgetVisible(next);
        shell->ensureWidgetVisible(next);
        QCoreApplication::processEvents();
        QVERIFY(shell->viewport()->rect().contains(
            next->mapTo(shell->viewport(), next->rect().center())));
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-enlarged-details.png")));
        QVERIFY(f.sql(QStringLiteral("UPDATE review_classifications SET document=zeroblob(128)")));
        f.startup.reviewGame().moves().retry();
        QTRY_VERIFY(!f.startup.reviewGame().moves().state().error.isEmpty());
        auto* retry = f.window.findChild<QPushButton*>(QStringLiteral("retryReviewMoves"));
        QVERIFY(retry);
        QVERIFY(!retry->isHidden());
        explanationScroll->ensureWidgetVisible(retry);
        shell->ensureWidgetVisible(retry);
        QCoreApplication::processEvents();
        QVERIFY(shell->viewport()->rect().contains(
            retry->mapTo(shell->viewport(), retry->rect().center())));
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-error-retry.png")));
        f.startup.reviewGame().refreshOverview();
        QTRY_VERIFY(!f.startup.reviewGame().state().busy);
        QVERIFY(!f.startup.reviewGame().state().overviewError.isEmpty());
        tabs->setCurrentIndex(0);
        auto* refreshSummary =
            f.window.findChild<QPushButton*>(QStringLiteral("refreshReviewSummary"));
        QVERIFY(refreshSummary);
        explanationScroll->ensureWidgetVisible(refreshSummary);
        shell->ensureWidgetVisible(refreshSummary);
        QCoreApplication::processEvents();
        QVERIFY(shell->viewport()->rect().contains(
            refreshSummary->mapTo(shell->viewport(), refreshSummary->rect().center())));
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-summary-error.png")));
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
