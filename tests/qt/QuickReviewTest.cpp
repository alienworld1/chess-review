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
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWheelEvent>
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
                int verifyFirst = 0) {
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
        const int stages = end < 0 && !weak && !score.isEmpty() && total > 1 ? 3 : 1;
        for (int stage = 0; stage < stages; ++stage) {
            const std::uint64_t budget = stage == 0 ? 100000 : stage == 1 ? 1000000 : 5000000;
            position = std::get<chess::Position>(document->tree.position(document->tree.rootId()));
            for (int index = 0; index < total; ++index) {
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
                        const auto evaluation =
                            stage > 0 && !refinedScore.isEmpty() ? refinedScore : score;
                        const auto legal = position.legalMoves();
                        if (!evaluation.isEmpty()) {
                            for (int rank = 1; rank <= prepared.effectiveMultiPv; ++rank) {
                                const auto root = std::get<std::string>(
                                    position.uci(legal[static_cast<std::size_t>(rank - 1)]));
                                output += QStringLiteral(
                                              "info depth 7 nodes %1 multipv %2 score %3%4 pv %5\n")
                                              .arg(weak ? 1 : budget)
                                              .arg(rank)
                                              .arg(evaluation)
                                              .arg(weak ? QStringLiteral(" lowerbound") : QString{})
                                              .arg(QString::fromStdString(root));
                            }
                        }
                        output += QStringLiteral("bestmove %1\n").arg(QString::fromStdString(best));
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
        QVERIFY(f.sql(QStringLiteral(
            "CREATE TABLE saved_classification_fixture AS SELECT * FROM review_classifications")));
        QVERIFY(f.sql(QStringLiteral("UPDATE review_classifications SET document=zeroblob(128)")));
        moves.retry();
        QTRY_VERIFY(!moves.state().error.isEmpty());
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
        QVERIFY(retry->isHidden());
        QVERIFY(moves.state().page->moves.front().classification->label);
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
    void refinementCancellationResumesOnlyMissingPositions() {
        Fixture f;
        QVERIFY(f.seed());
        f.script();
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
        f.script(3, -1, 0, false, false, QStringLiteral("cp 25"), QStringLiteral("Test engine"), 0,
                 false, {}, 1);
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
        f.script(0, -1, 0, false, false, QStringLiteral("cp 0"));
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
        QVERIFY(details->toPlainText().contains(QStringLiteral("Rule:")));
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
        QVERIFY(QTest::qWaitFor(
            [&] { return f.startup.reviewGame().moves().state().page.has_value(); }));
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-light.png")));
        auto* tabs = f.window.findChild<QTabWidget*>(QStringLiteral("reviewResultsTabs"));
        QVERIFY(tabs);
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
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-dark.png")));
        tabs->setCurrentIndex(0);
        QCoreApplication::processEvents();
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-summary-dark.png")));
        tabs->setCurrentIndex(1);
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
        shell->ensureWidgetVisible(retry);
        QCoreApplication::processEvents();
        QVERIFY(shell->viewport()->rect().contains(
            retry->mapTo(shell->viewport(), retry->rect().center())));
        QVERIFY(f.window.grab().save(directory + QStringLiteral("/review-error-retry.png")));
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
