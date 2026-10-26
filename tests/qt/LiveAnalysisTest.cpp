#include "EngineControllerTestAccess.h"
#include "EngineTestSupport.h"
#include "TestEnvironment.h"
#include "app/StartupCoordinator.h"
#include "ui/analysis/EngineAnalysisPanel.h"
#include "ui/chess/ChessBoardWidget.h"
#include "ui/shell/MainWindow.h"
#include <QAccessible>
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QThread>
#include <limits>

using namespace chess_review;
namespace {
QJsonObject ready(int delay = 0) {
    return engine_test::step(QStringLiteral("isready"),
                             {engine_test::write(QStringLiteral("readyok\n"), delay)});
}
QJsonObject quit() {
    return engine_test::step(QStringLiteral("quit"),
                             {QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}});
}
void search(QJsonArray& steps, const QString& position, const QString& pv, int score = 42) {
    steps.append(engine_test::step(QStringLiteral("ucinewgame"), {}));
    steps.append(ready());
    steps.append(engine_test::step(position, {}));
    steps.append(engine_test::step(
        QStringLiteral("go infinite"),
        {engine_test::write(
            QStringLiteral("info depth 7 score cp %1 pv %2\n").arg(score).arg(pv))}));
}
void stop(QJsonArray& steps, const QString& bestMove, int delay = 0) {
    steps.append(engine_test::step(
        QStringLiteral("stop"),
        {engine_test::write(QStringLiteral("bestmove %1\n").arg(bestMove), delay)}));
    steps.append(ready());
}
class Application final {
  public:
    explicit Application(bool installLive = true) : window(navigation, startup.analysis()) {
        window.installEngines(startup.engines());
        if (installLive) {
            window.installEngineAnalysis(startup.engineAnalysis());
        }
        QObject::connect(&startup, &app::StartupCoordinator::ready, &window,
                         &ui::MainWindow::showReady);
        startup.start(directory.filePath(QStringLiteral("library.sqlite3")));
        navigation.navigate(app::Destination::Analyze);
    }
    ~Application() {
        shutdown();
        window.completeShutdown();
    }
    void shutdown() {
        if (startup.state() == app::Lifecycle::Stopped) {
            return;
        }
        startup.shutdown();
        if (!QTest::qWaitFor([this] { return startup.state() == app::Lifecycle::Stopped; }, 8000)) {
            qFatal("Live analysis application did not close");
        }
    }
    engine::EngineSupervisor& supervisor() {
        return app::EngineControllerTestAccess::validator(startup.engines());
    }
    bool prepare(const QJsonArray& searches,
                 const QString& displayName = QStringLiteral("Test engine")) {
        if (!QTest::qWaitFor([this] {
                return startup.engines().state().ready && !startup.engines().state().listing &&
                       !startup.analysis().busy() && startup.analysis().position();
            })) {
            return false;
        }
        const auto registration = engine_test::scenario(
            directory.filePath(QStringLiteral("registration.json")), engine_test::handshake());
        engine::ValidatorTestAccess::configure(supervisor(), registration, 1000);
        QSignalSpy done(&supervisor(), &engine::EngineSupervisor::completed);
        supervisor().validate(QUuid::createUuid(), QString::fromUtf8(FAKE_UCI_ENGINE));
        if (!done.wait(5000)) {
            return false;
        }
        const auto result = qvariant_cast<engine::ValidationResult>(done.front()[1]);
        if (!std::holds_alternative<engine::ValidatedEngine>(result)) {
            return false;
        }
        engine::EngineProfile profile;
        profile.id = 1;
        profile.displayName = displayName;
        profile.engine = std::get<engine::ValidatedEngine>(result);
        app::EngineControllerTestAccess::replaceProfiles(startup.engines(), {profile});
        return reconnect(searches);
    }
    bool reconnect(const QJsonArray& searches) {
        QJsonArray steps{engine_test::step(QStringLiteral("uci"),
                                           {engine_test::write(QStringLiteral(
                                               "id name Test engine\nid author Tests\nuciok\n"))}),
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
    QTemporaryDir directory;
    app::StartupCoordinator startup;
    app::NavigationController navigation;
    ui::MainWindow window;
};
} // namespace
class LiveAnalysisTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        QTest::failOnWarning(QRegularExpression(
            QStringLiteral("^(?!This plugin does not support propagateSizeHints\\(\\)$).*")));
    }
    void scorePresentation() {
        const auto white = chess::Position::initial();
        auto black = white;
        QVERIFY(std::holds_alternative<std::monostate>(
            black.play(std::get<chess::Move>(black.parseUci("e2e4")))));
        engine::AnalysisLine line;
        line.score =
            engine::AnalysisScore{engine::ScoreType::Centipawns, 42, engine::ScoreBound::Exact};
        auto shown = app::presentEngineLine(white, line);
        QCOMPARE(shown.evaluation, QStringLiteral("+0.42"));
        QCOMPARE(shown.whiteShare, std::optional<double>{0.521});
        QCOMPARE(app::presentEngineLine(black, line).evaluation, QStringLiteral("−0.42"));
        line.score->bound = engine::ScoreBound::Lower;
        QCOMPARE(app::presentEngineLine(black, line).evaluation, QStringLiteral("≤ −0.42"));
        QVERIFY(!app::presentEngineLine(black, line).whiteShare);
        line.score->bound = engine::ScoreBound::Upper;
        QCOMPARE(app::presentEngineLine(black, line).evaluation, QStringLiteral("≥ −0.42"));
        line.score->value = std::numeric_limits<std::int64_t>::min();
        QCOMPARE(app::presentEngineLine(white, line).evaluation,
                 QStringLiteral("≤ −92233720368547758.08"));
        QCOMPARE(app::presentEngineLine(black, line).evaluation,
                 QStringLiteral("≥ +92233720368547758.08"));
        line.score->bound = engine::ScoreBound::Exact;
        line.score->type = engine::ScoreType::Mate;
        line.score->value = 3;
        QCOMPARE(app::presentEngineLine(black, line).evaluation,
                 QStringLiteral("Black mates in 3"));
        line.score->value = -3;
        QCOMPARE(app::presentEngineLine(black, line).evaluation,
                 QStringLiteral("White mates in 3"));
        line.score->value = 0;
        QCOMPARE(app::presentEngineLine(white, line).evaluation,
                 QStringLiteral("Black mates in 0"));
        line.score.reset();
        shown = app::presentEngineLine(white, line);
        QVERIFY(shown.evaluation.isEmpty());
        QVERIFY(!shown.whiteShare);
    }
    void sanPresentation_data() {
        QTest::addColumn<QString>("fen");
        QTest::addColumn<QStringList>("moves");
        QTest::addColumn<QString>("san");
        QTest::newRow("initial") << QString{} << QStringList{"e2e4", "e7e5", "g1f3"}
                                 << QStringLiteral("1. e4 e5 2. Nf3");
        QTest::newRow("black-root")
            << QStringLiteral("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 12")
            << QStringList{"e7e5", "g1f3"} << QStringLiteral("12... e5 13. Nf3");
        QTest::newRow("promotion") << QStringLiteral("7k/P7/8/8/8/8/8/7K w - - 0 1")
                                   << QStringList{"a7a8q"} << QStringLiteral("1. a8=Q+");
        QTest::newRow("castle") << QStringLiteral("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1")
                                << QStringList{"e1g1"} << QStringLiteral("1. O-O");
        QTest::newRow("en-passant") << QStringLiteral("7k/8/8/3pP3/8/8/8/7K w - d6 0 2")
                                    << QStringList{"e5d6"} << QStringLiteral("2. exd6");
    }
    void sanPresentation() {
        QFETCH(QString, fen);
        QFETCH(QStringList, moves);
        QFETCH(QString, san);
        auto root = fen.isEmpty()
                        ? chess::Position::initial()
                        : std::get<chess::Position>(chess::Position::fromFen(fen.toStdString()));
        auto cursor = root;
        engine::AnalysisLine line;
        line.statistics.depth = 7;
        for (const auto& text : moves) {
            const auto move = std::get<chess::Move>(cursor.parseUci(text.toStdString()));
            line.pv.push_back(move);
            QVERIFY(std::holds_alternative<std::monostate>(cursor.play(move)));
        }
        const auto shown = app::presentEngineLine(root, line);
        QCOMPARE(shown.pv, san);
        QCOMPARE(shown.depth, std::optional<int>{7});
        line.pv.push_back({});
        QVERIFY(app::presentEngineLine(root, line).pv.isEmpty());
    }
    void noEngineAndManage() {
        Application a;
        QTRY_VERIFY(!a.startup.analysis().busy());
        auto* start = a.window.findChild<QPushButton*>(QStringLiteral("startEngineAnalysis"));
        QVERIFY(start);
        QVERIFY(!start->isEnabled());
        QVERIFY(a.startup.engineAnalysis().state().line.evaluation.isEmpty());
        a.window.findChild<QPushButton*>(QStringLiteral("manageAnalysisEngines"))->click();
        QCOMPARE(a.navigation.current(), app::Destination::Settings);
    }
    void stopBeforePhaseDeliveryRetainsTerminalEvidence() {
        Application a;
        QJsonArray steps;
        search(steps, QStringLiteral("position startpos"), QStringLiteral("e2e4 e7e5"));
        stop(steps, QStringLiteral("e2e4"));
        QVERIFY(a.prepare(steps));
        auto& live = a.startup.engineAnalysis();
        QSignalSpy finished(&a.supervisor(), &engine::EngineSupervisor::scheduledFinished);
        live.start();
        // Simulate a busy GUI while the independent engine thread starts its search.
        // No queued phase/progress event is delivered to the presentation controller yet.
        QThread::msleep(250);
        live.stop();
        QTRY_VERIFY(live.state().canStart);
        QCOMPARE(finished.size(), 1);
        const auto terminal = qvariant_cast<engine::ScheduledResult>(finished.front()[0]);
        QVERIFY(terminal.evidence && terminal.evidence->evidence.searchStarted);
        QVERIFY(!terminal.evidence->evidence.lines.empty());
        QVERIFY(QTest::qWaitFor([&] { return live.state().line.pv == QStringLiteral("1. e4 e5"); },
                                1500));
        QCOMPARE(live.state().status, QStringLiteral("Stopped"));
    }
    void closedBoardCannotStartAnalysis() {
        Application a;
        QVERIFY(a.prepare({}));
        auto& board = a.startup.analysis();
        QSignalSpy closed(&board, &app::AnalysisController::closed);
        board.close();
        QTRY_COMPARE(closed.size(), 1);
        QVERIFY(!a.startup.engineAnalysis().state().canStart);
    }
    void engineTooltipIsLiteralText() {
        Application a;
        const auto name = QStringLiteral("<b>Forged complete status</b>");
        QVERIFY(a.prepare({}, name));
        auto* label = a.window.findChild<QLabel*>(QStringLiteral("analysisEngineName"));
        QVERIFY(label);
        QCOMPARE(label->textFormat(), Qt::PlainText);
        QCOMPARE(label->text(), name);
        QTextDocument tooltip;
        tooltip.setHtml(label->toolTip());
        QCOMPARE(tooltip.toPlainText(), name);
    }
    void redirectedNavigationCannotEnableHiddenAnalysis() {
        Application a(false);
        a.navigation.navigate(app::Destination::Home);
        connect(&a.navigation, &app::NavigationController::destinationChanged, &a.window,
                [&](app::Destination destination) {
                    if (destination == app::Destination::Analyze) {
                        a.navigation.navigate(app::Destination::Home);
                    }
                });
        a.window.installEngineAnalysis(a.startup.engineAnalysis());
        QVERIFY(a.prepare({}));
        a.navigation.navigate(app::Destination::Analyze);
        QCOMPARE(a.navigation.current(), app::Destination::Home);
        QVERIFY(!a.startup.engineAnalysis().state().canStart);
    }
    void reentrantCloseAllowsDeletionAfterClosed() {
        Application a;
        QVERIFY(a.prepare({}));
        auto* consumer = new app::EngineAnalysisController(
            a.startup.analysis(), a.startup.engines(), a.supervisor(), &a.window);
        QSignalSpy retired(consumer, &QObject::destroyed);
        QObject observer;
        bool requested = false;
        connect(consumer, &app::EngineAnalysisController::closed, &a.window,
                [consumer] { delete consumer; });
        connect(consumer, &app::EngineAnalysisController::changed, &observer, [&] {
            if (!requested) {
                requested = true;
                consumer->close();
            }
        });
        // A reset makes the position unavailable; observers may retire their consumer.
        a.startup.analysis().reset();
        QTRY_COMPARE(retired.size(), 1);
    }
    void nestedRetirement_data() {
        QTest::addColumn<QString>("during");
        QTest::newRow("formatted-progress") << QStringLiteral("format");
        QTest::newRow("terminal-notification") << QStringLiteral("terminal");
        QTest::newRow("visibility-change") << QStringLiteral("hide");
    }
    void nestedRetirement() {
        QFETCH(QString, during);
        Application a;
        QJsonArray steps;
        search(steps, QStringLiteral("position startpos"), QStringLiteral("e2e4"));
        stop(steps, QStringLiteral("e2e4"));
        QVERIFY(a.prepare(steps));
        auto* consumer = new app::EngineAnalysisController(
            a.startup.analysis(), a.startup.engines(), a.supervisor(), &a.window);
        QSignalSpy retired(consumer, &QObject::destroyed);
        QObject observer;
        bool requested = false;
        bool retiredInsideNotification = false;
        connect(consumer, &app::EngineAnalysisController::closed, &a.window,
                [consumer] { delete consumer; });
        connect(consumer, &app::EngineAnalysisController::changed, &observer, [&] {
            const bool retire = during == QStringLiteral("format")
                                    ? !consumer->state().line.pv.isEmpty()
                                : during == QStringLiteral("terminal")
                                    ? consumer->state().status == QStringLiteral("Stopped")
                                    : consumer->state().status == QStringLiteral("Stopping");
            if (!requested && retire) {
                requested = true;
                consumer->close();
                // A modal observer can run an event loop until the consumer closes.
                retiredInsideNotification =
                    QTest::qWaitFor([&] { return !retired.isEmpty(); }, 2500);
            }
        });
        consumer->setVisible(true);
        consumer->start();
        if (during != QStringLiteral("format")) {
            QTRY_VERIFY(!consumer->state().line.pv.isEmpty());
            if (during == QStringLiteral("hide")) {
                consumer->setVisible(false);
            } else {
                consumer->stop();
            }
        }
        QTRY_COMPARE(retired.size(), 1);
        QVERIFY(retiredInsideNotification);
    }
    void boundedMateDoesNotClaimACertainWinner() {
        engine::AnalysisLine line;
        line.score = engine::AnalysisScore{engine::ScoreType::Mate, 3, engine::ScoreBound::Upper};
        const auto shown = app::presentEngineLine(chess::Position::initial(), line);
        QCOMPARE(shown.evaluation, QStringLiteral("≤ +M3 (White mating score)"));
        QVERIFY(!shown.whiteShare);
    }
    void unrelatedRejectionCannotFailAnIdlePanel() {
        Application a;
        QVERIFY(a.prepare({}));
        QSignalSpy finished(&a.supervisor(), &engine::EngineSupervisor::scheduledFinished);
        engine::AnalysisRequest invalid;
        a.supervisor().scheduleAnalysis(a.startup.engines().state().runtime.session, invalid,
                                        engine::AnalysisPriority::InteractiveAnalysis);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(qvariant_cast<engine::ScheduledResult>(finished.front()[0]).outcome,
                 engine::ScheduledOutcome::Rejected);
        QCOMPARE(a.startup.engineAnalysis().state().status, QStringLiteral("Ready"));
        QVERIFY(a.startup.engineAnalysis().state().error.isEmpty());
    }
    void duplicateRejectionCannotRetireOwnedRequest_data() {
        QTest::addColumn<bool>("queued");
        QTest::newRow("running") << false;
        QTest::newRow("queued-behind-direct") << true;
    }
    void duplicateRejectionCannotRetireOwnedRequest() {
        QFETCH(bool, queued);
        Application a;
        QJsonArray steps;
        search(steps, QStringLiteral("position startpos"), QStringLiteral("e2e4"));
        stop(steps, QStringLiteral("e2e4"));
        QVERIFY(a.prepare(steps));
        const auto session = a.startup.engines().state().runtime.session;
        engine::AnalysisRequest direct;
        direct.id = QUuid::createUuid();
        direct.limit = engine::InfiniteLimit{};
        QSignalSpy directStates(&a.supervisor(), &engine::EngineSupervisor::searchChanged);
        QSignalSpy directDone(&a.supervisor(), &engine::EngineSupervisor::analysisFinished);
        if (queued) {
            a.supervisor().analyze(session, direct);
            QTRY_VERIFY(!directStates.empty() &&
                        qvariant_cast<engine::SearchState>(directStates.last()[0]).phase ==
                            engine::SearchPhase::Analyzing);
        }
        QSignalSpy states(&a.supervisor(), &engine::EngineSupervisor::scheduledChanged);
        QSignalSpy finished(&a.supervisor(), &engine::EngineSupervisor::scheduledFinished);
        auto& live = a.startup.engineAnalysis();
        live.start();
        QTRY_VERIFY(!states.empty());
        if (!queued) {
            QTRY_COMPARE(live.state().line.pv, QStringLiteral("1. e4"));
        }
        engine::AnalysisRequest duplicate;
        duplicate.id = qvariant_cast<engine::ScheduledState>(states.front()[0]).request;
        duplicate.limit = engine::InfiniteLimit{};
        a.supervisor().scheduleAnalysis(session, duplicate,
                                        engine::AnalysisPriority::InteractiveAnalysis);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(qvariant_cast<engine::ScheduledResult>(finished.front()[0]).outcome,
                 engine::ScheduledOutcome::Rejected);
        QVERIFY(live.state().canStop);
        QCOMPARE(live.state().status,
                 queued ? QStringLiteral("Queued") : QStringLiteral("Analyzing"));
        live.stop();
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(live.state().status, QStringLiteral("Stopped"));
        if (queued) {
            a.supervisor().stopAnalysis(session, direct.id);
            QTRY_COMPARE(directDone.size(), 1);
        }
    }
    void engineStatusIsExposedToAccessibility() {
        Application a;
        QVERIFY(a.prepare({}));
        auto* status = a.window.findChild<QLabel*>(QStringLiteral("engineAnalysisStatus"));
        QVERIFY(status);
        auto* accessible = QAccessible::queryAccessibleInterface(status);
        QVERIFY(accessible);
        QCOMPARE(accessible->text(QAccessible::Name), a.startup.engineAnalysis().state().status);
    }
    void followsPositionAndRetainsStoppedEvidence() {
        Application a;
        QJsonArray steps;
        search(steps, QStringLiteral("position startpos"), QStringLiteral("e2e4 e7e5"));
        stop(steps, QStringLiteral("e2e4"), 100);
        search(steps, QStringLiteral("position startpos moves e2e4"), QStringLiteral("e7e5"), 21);
        stop(steps, QStringLiteral("e7e5"), 100);
        search(steps, QStringLiteral("position startpos"), QStringLiteral("d2d4"), 15);
        stop(steps, QStringLiteral("d2d4"));
        QVERIFY(a.prepare(steps));
        auto& live = a.startup.engineAnalysis();
        QSignalSpy updates(&a.supervisor(), &engine::EngineSupervisor::scheduledChanged);
        QSignalSpy evidence(&a.supervisor(), &engine::EngineSupervisor::scheduledUpdated);
        QVERIFY(live.state().canStart);
        QVERIFY(updates.isEmpty());
        auto* start = a.window.findChild<QPushButton*>(QStringLiteral("startEngineAnalysis"));
        start->setFocus();
        QTest::keyPress(start, Qt::Key_Space);
        QTest::keyRelease(start, Qt::Key_Space);
        QTRY_COMPARE(live.state().line.pv, QStringLiteral("1. e4 e5"));
        QVERIFY(!evidence.empty());
        const auto obsolete = qvariant_cast<engine::ScheduledEvidence>(evidence.last()[0]);
        QCOMPARE(live.state().line.depth, std::optional<int>{7});
        auto& board = a.startup.analysis();
        const auto states = updates.size();
        board.flip();
        board.selectSquare(*chess::Square::fromName("e2"));
        board.toggleAnnotation({*chess::Square::fromName("a1"), *chess::Square::fromName("a1"), 0});
        QTest::qWait(130);
        QCOMPARE(updates.size(), states);
        const auto move = std::get<chess::Move>(board.position()->parseUci("e2e4"));
        board.attemptMove(move, board.board().revision);
        QVERIFY(live.state().line.pv.isEmpty());
        QTRY_COMPARE(live.state().line.pv, QStringLiteral("1... e5"));
        a.supervisor().scheduledUpdated(obsolete);
        a.supervisor().scheduledFinished(
            {obsolete.identity, engine::ScheduledOutcome::Failed, obsolete, {}});
        QTest::qWait(50);
        QCOMPARE(live.state().line.pv, QStringLiteral("1... e5"));
        QCOMPARE(live.state().line.evaluation, QStringLiteral("−0.21"));
        QCOMPARE(board.position()->context().moves.size(), std::size_t{1});
        board.previous();
        board.next();
        board.previous();
        QVERIFY(live.state().line.pv.isEmpty());
        QTRY_COMPARE(live.state().line.pv, QStringLiteral("1. d4"));
        auto* stopButton = a.window.findChild<QPushButton*>(QStringLiteral("stopEngineAnalysis"));
        stopButton->setFocus();
        QTest::keyPress(stopButton, Qt::Key_Space);
        QTest::keyRelease(stopButton, Qt::Key_Space);
        QTRY_COMPARE(live.state().status, QStringLiteral("Stopped"));
        QTRY_VERIFY(live.state().canStart);
        QCOMPARE(live.state().line.pv, QStringLiteral("1. d4"));
        a.navigation.navigate(app::Destination::Home);
        a.navigation.navigate(app::Destination::Analyze);
        QCOMPARE(live.state().line.pv, QStringLiteral("1. d4"));
        QVERIFY(!live.state().canStop);
    }
    void stopBoundaries_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("leave") << QStringLiteral("leave");
        QTest::newRow("reset") << QStringLiteral("reset");
        QTest::newRow("shutdown") << QStringLiteral("shutdown");
    }
    void stopBoundaries() {
        QFETCH(QString, action);
        Application a;
        QJsonArray steps;
        search(steps, QStringLiteral("position startpos"), QStringLiteral("e2e4"));
        stop(steps, QStringLiteral("e2e4"));
        QVERIFY(a.prepare(steps));
        auto& live = a.startup.engineAnalysis();
        live.start();
        QTRY_VERIFY(!live.state().line.pv.isEmpty());
        if (action == QStringLiteral("leave")) {
            a.navigation.navigate(app::Destination::Home);
            QTRY_COMPARE(live.state().status, QStringLiteral("Stopped"));
            a.navigation.navigate(app::Destination::Analyze);
            QVERIFY(live.state().canStart);
            QVERIFY(!live.state().canStop);
        } else if (action == QStringLiteral("reset")) {
            a.startup.analysis().reset();
            QTRY_VERIFY(live.state().canStart);
            QVERIFY(live.state().line.pv.isEmpty());
        } else {
            a.shutdown();
            QVERIFY(!live.state().canStart);
        }
    }
    void terminalPositionCompletes_data() {
        QTest::addColumn<QString>("fen");
        QTest::addColumn<QString>("score");
        QTest::addColumn<QString>("evaluation");
        QTest::newRow("checkmate")
            << QStringLiteral("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1") << QStringLiteral("mate 0")
            << QStringLiteral("White mates in 0");
        QTest::newRow("stalemate") << QStringLiteral("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1")
                                   << QStringLiteral("cp 0") << QStringLiteral("+0.00");
    }
    void terminalPositionCompletes() {
        QFETCH(QString, fen);
        QFETCH(QString, score);
        QFETCH(QString, evaluation);
        Application a;
        QJsonArray steps{
            engine_test::step(QStringLiteral("ucinewgame"), {}), ready(),
            engine_test::step(QStringLiteral("position fen ") + fen, {}),
            engine_test::step(QStringLiteral("go depth 1"),
                              {engine_test::write(QStringLiteral("info depth 0 score %1\n"
                                                                 "bestmove (none)\n")
                                                      .arg(score))}),
            ready()};
        search(steps, QStringLiteral("position startpos"), QStringLiteral("e2e4"));
        stop(steps, QStringLiteral("e2e4"));
        QVERIFY(a.prepare(steps));
        auto& board = a.startup.analysis();
        board.reset(fen.toStdString());
        QTRY_VERIFY(!board.busy() && board.board().interactive);
        QVERIFY(board.board().legalMoves.empty());
        auto& live = a.startup.engineAnalysis();
        QTRY_VERIFY(live.state().canStart);
        live.start();
        QTRY_COMPARE(live.state().status, QStringLiteral("Completed"));
        QTRY_COMPARE(live.state().line.evaluation, evaluation);
        QVERIFY(live.state().line.pv.isEmpty());
        QVERIFY(live.state().error.isEmpty());
        QCOMPARE(a.startup.engines().state().runtime.phase, engine::RuntimePhase::Ready);
        QVERIFY(!live.state().canStop);
        board.reset();
        QTRY_VERIFY(live.state().canStart);
        live.start();
        QTRY_VERIFY(!live.state().line.pv.isEmpty());
        live.stop();
        QTRY_COMPARE(live.state().status, QStringLiteral("Stopped"));
    }
    void unsolicitedInfiniteCompletionFails() {
        Application a;
        QJsonArray steps;
        search(steps, QStringLiteral("position startpos"), QStringLiteral("e2e4"));
        // Infinite UCI searches must wait for Stop; unsolicited bestmove is a protocol failure.
        auto go = steps.last().toObject();
        auto actions = go.value(QStringLiteral("actions")).toArray();
        actions.append(engine_test::write(QStringLiteral("bestmove e2e4\n"), 200));
        go[QStringLiteral("actions")] = actions;
        steps[steps.size() - 1] = go;
        steps.append(ready());
        QVERIFY(a.prepare(steps));
        auto& live = a.startup.engineAnalysis();
        live.start();
        QTRY_COMPARE(live.state().status, QStringLiteral("Failed"));
        QTRY_COMPARE(live.state().line.pv, QStringLiteral("1. e4"));
        QVERIFY(!live.state().canStart);
        QVERIFY(!live.state().canStop);
    }
    void shutdownDuringConfiguration() {
        Application a;
        const QJsonArray steps{engine_test::step(QStringLiteral("ucinewgame"), {}), ready(250)};
        QVERIFY(a.prepare(steps));
        a.startup.engineAnalysis().start();
        QTRY_COMPARE(a.startup.engineAnalysis().state().status, QStringLiteral("Configuring"));
        a.shutdown();
        QCOMPARE(a.startup.engines().state().runtime.phase, engine::RuntimePhase::Disconnected);
    }
    void failureAndExplicitReconnect() {
        Application a;
        QJsonArray steps;
        search(steps, QStringLiteral("position startpos"), QStringLiteral("e2e4"));
        auto go = steps.last().toObject();
        auto actions = go.value(QStringLiteral("actions")).toArray();
        actions.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")},
                                   {QStringLiteral("afterMs"), 400}});
        go[QStringLiteral("actions")] = actions;
        steps[steps.size() - 1] = go;
        QVERIFY(a.prepare(steps));
        auto& live = a.startup.engineAnalysis();
        live.start();
        QTRY_COMPARE(live.state().line.pv, QStringLiteral("1. e4"));
        QTRY_COMPARE(live.state().status, QStringLiteral("Failed"));
        QVERIFY(!live.state().error.isEmpty());
        QCOMPARE(live.state().line.pv, QStringLiteral("1. e4"));
        QVERIFY(!live.state().canStart);
        QVERIFY(a.reconnect({}));
        QVERIFY(live.state().canStart);
        QVERIFY(!live.state().canStop);
        QCOMPARE(live.state().status, QStringLiteral("Ready"));
        QVERIFY(live.state().line.pv.isEmpty());
    }
    void layouts_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("largeFont");
        QTest::newRow("desktop-light") << QSize(1360, 850) << false << false;
        QTest::newRow("laptop-dark") << QSize(1100, 720) << true << false;
        QTest::newRow("wide-dark") << QSize(1800, 1000) << true << false;
        QTest::newRow("large-font") << QSize(1100, 720) << false << true;
    }
    void layouts() {
        QFETCH(QSize, size);
        QFETCH(bool, dark);
        QFETCH(bool, largeFont);
        Application a;
        QJsonArray steps;
        search(steps, QStringLiteral("position startpos"), QStringLiteral("e2e4 e7e5 g1f3"));
        stop(steps, QStringLiteral("e2e4"));
        QVERIFY(a.prepare(steps));
        if (dark) {
            auto palette = a.window.palette();
            palette.setColor(QPalette::Window, QColor("#202831"));
            palette.setColor(QPalette::WindowText, QColor("#eef0f2"));
            palette.setColor(QPalette::Base, QColor("#161e27"));
            palette.setColor(QPalette::Text, QColor("#eef0f2"));
            palette.setColor(QPalette::Button, QColor("#303c48"));
            palette.setColor(QPalette::ButtonText, QColor("#eef0f2"));
            a.window.setPalette(palette);
        }
        if (largeFont) {
            auto font = a.window.font();
            font.setPointSizeF(font.pointSizeF() * 1.5);
            a.window.setFont(font);
        }
        a.window.resize(size);
        a.window.show();
        a.startup.engineAnalysis().start();
        QTRY_COMPARE(a.startup.engineAnalysis().state().line.pv, QStringLiteral("1. e4 e5 2. Nf3"));
        QTest::qWait(100);
        auto* board = a.window.findChild<ui::ChessBoardWidget*>();
        auto* panel = a.window.findChild<ui::EngineAnalysisPanel*>();
        auto* bar = a.window.findChild<QWidget*>(QStringLiteral("evaluationBar"));
        QVERIFY(bar);
        QVERIFY(board && panel);
        const auto squareTop = board->mapTo(&a.window, board->boardRect().topLeft().toPoint());
        const auto barTop = bar->mapTo(&a.window, QPoint{});
        QCOMPARE(barTop.y(), squareTop.y());
        QCOMPARE(barTop.x() + bar->width() + 6, squareTop.x());
        QCOMPARE(bar->height(), static_cast<int>(board->boardRect().height()));
        QCOMPARE(board->boardRect().width(), board->boardRect().height());
        QVERIFY(board->boardRect().width() >= 240);
        for (const auto* name :
             {"startEngineAnalysis", "stopEngineAnalysis", "engineEvaluation", "enginePv"}) {
            const auto* control = a.window.findChild<QWidget*>(QString::fromLatin1(name));
            QVERIFY(control);
            QVERIFY2(a.window.rect().contains(
                         QRect(control->mapTo(&a.window, QPoint{}), control->size())),
                     name);
        }
        if (qEnvironmentVariableIsSet("CHESS_REVIEW_SCREENSHOT_DIR")) {
            const auto path = qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR") +
                              QLatin1Char('/') + QString::fromLatin1(QTest::currentDataTag()) +
                              QStringLiteral(".png");
            QVERIFY(a.window.grab().save(path));
        }
    }
    void stockfishPanel() {
        const auto path = qEnvironmentVariable("CHESS_REVIEW_STOCKFISH");
        if (path.isEmpty()) {
            QSKIP("Set CHESS_REVIEW_STOCKFISH to explicitly select a local engine for this probe.");
        }
        Application a;
        QTRY_VERIFY(a.startup.engines().state().ready && !a.startup.engines().state().listing);
        QSignalSpy validation(&a.supervisor(), &engine::EngineSupervisor::completed);
        a.supervisor().validate(QUuid::createUuid(), path);
        QTRY_COMPARE_WITH_TIMEOUT(validation.size(), 1, 15000);
        const auto result = qvariant_cast<engine::ValidationResult>(validation.front()[1]);
        QVERIFY(std::holds_alternative<engine::ValidatedEngine>(result));
        engine::EngineProfile profile;
        profile.id = 1;
        profile.engine = std::get<engine::ValidatedEngine>(result);
        profile.displayName = profile.engine.reportedName;
        app::EngineControllerTestAccess::replaceProfiles(a.startup.engines(), {profile});
        a.startup.engines().connectEngine(1);
        auto& live = a.startup.engineAnalysis();
        QTRY_VERIFY_WITH_TIMEOUT(live.state().canStart, 15000);
        a.window.resize(1360, 850);
        a.window.show();
        live.start();
        QTRY_VERIFY_WITH_TIMEOUT(live.state().line.depth && *live.state().line.depth >= 8, 15000);
        QVERIFY(!live.state().line.pv.isEmpty());
        QVERIFY(!live.state().line.evaluation.isEmpty());
        auto& board = a.startup.analysis();
        const auto revision = board.board().revision;
        board.attemptMove(std::get<chess::Move>(board.position()->parseUci("e2e4")), revision);
        QTRY_VERIFY(board.board().revision != revision && !board.busy());
        QTRY_VERIFY_WITH_TIMEOUT(live.state().line.pv.startsWith(QStringLiteral("1...")), 15000);
        live.stop();
        QTRY_VERIFY_WITH_TIMEOUT(live.state().canStart, 15000);
        QCOMPARE(live.state().status, QStringLiteral("Stopped"));
        if (qEnvironmentVariableIsSet("CHESS_REVIEW_SCREENSHOT_DIR")) {
            QVERIFY(a.window.grab().save(qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR") +
                                         QStringLiteral("/stockfish-stopped.png")));
        }
        for (const auto& fen :
             {"7k/6Q1/6K1/8/8/8/8/8 b - - 0 1", "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"}) {
            board.reset(std::string(fen));
            QTRY_VERIFY(live.state().canStart);
            live.start();
            QTRY_COMPARE_WITH_TIMEOUT(live.state().status, QStringLiteral("Completed"), 15000);
            QTRY_VERIFY(!live.state().line.evaluation.isEmpty());
            QVERIFY(live.state().line.pv.isEmpty());
            QVERIFY(live.state().error.isEmpty());
            QCOMPARE(a.startup.engines().state().runtime.phase, engine::RuntimePhase::Ready);
        }
    }
};
int main(int argc, char* argv[]) {
    const TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    LiveAnalysisTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "LiveAnalysisTest.moc"
