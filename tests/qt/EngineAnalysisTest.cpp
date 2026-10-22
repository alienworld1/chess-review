#include "EngineTestSupport.h"
#include "TestEnvironment.h"
#include "engine/private/SearchProtocol.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <limits>

using namespace chess_review;
using namespace chess_review::engine;
namespace {
const QString identity =
    QStringLiteral("id name Analysis test\n"
                   "option name Threads type spin default 4 min 1 max 8\n"
                   "option name MultiPV type spin default 1 min 1 max 3\n"
                   "option name UCI_ShowWDL type check default false\nuciok\n");
QJsonObject quit() {
    return engine_test::step(QStringLiteral("quit"),
                             {QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}});
}
QJsonObject ready(const QString& output = QStringLiteral("readyok\n"), int delay = 0) {
    return engine_test::step(QStringLiteral("isready"), {engine_test::write(output, delay)});
}
AnalysisRequest request(SearchLimit limit = NodeLimit{200}, int width = 1, bool wdl = false) {
    AnalysisRequest value;
    value.id = QUuid::createUuid();
    value.limit = limit;
    value.multiPv = width;
    value.wantWdl = wdl;
    return value;
}
chess::Move move(const chess::Position& position, const char* uci) {
    return std::get<chess::Move>(position.parseUci(uci));
}
class Conversation {
  public:
    Conversation(const QString& declarations = identity, int idleMs = 10000, int responseMs = 300)
        : declarations_(declarations) {
        const auto registration =
            engine_test::scenario(directory.filePath(QStringLiteral("register.json")),
                                  engine_test::handshake(declarations));
        ValidatorTestAccess::configure(fixture.service, registration, 1000);
        QSignalSpy done(&fixture.service, &EngineSupervisor::completed);
        fixture.service.validate(QUuid::createUuid(),
                                 QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath());
        if (!done.wait(5000)) {
            qFatal("Registration timed out");
        }
        const auto result = qvariant_cast<ValidationResult>(done.front()[1]);
        if (!std::holds_alternative<ValidatedEngine>(result)) {
            qFatal("Registration failed");
        }
        profile.id = 1;
        profile.engine = std::get<ValidatedEngine>(result);
        ValidatorTestAccess::configureSearch(fixture.service, idleMs, responseMs);
    }
    void connect(QJsonArray searchSteps) {
        QJsonArray steps{
            engine_test::step(QStringLiteral("uci"), {engine_test::write(declarations_)}), ready()};
        for (const auto& step : searchSteps) {
            steps.append(step);
        }
        steps.append(quit());
        ValidatorTestAccess::configure(
            fixture.service,
            engine_test::scenario(directory.filePath(QStringLiteral("search.json")), steps), 1000);
        QSignalSpy events(&fixture.service, &EngineSupervisor::runtimeChanged);
        fixture.service.connectEngine(session, profile);
        QElapsedTimer timer;
        timer.start();
        while (events.empty() ||
               qvariant_cast<RuntimeState>(events.back()[0]).phase != RuntimePhase::Ready) {
            if (timer.elapsed() > 5000) {
                qFatal("Connection did not reach Ready");
            }
            QTest::qWait(5);
        }
    }
    QJsonArray setup(int width = 1, bool wdl = false) const {
        QJsonArray steps;
        if (profile.engine.capabilities.supportsMultiPv) {
            steps.append(engine_test::step(
                QStringLiteral("setoption name MultiPV value %1").arg(width), {}));
        }
        if (profile.engine.capabilities.supportsThreads) {
            steps.append(engine_test::step(QStringLiteral("setoption name Threads value 1"), {}));
        }
        if (profile.engine.capabilities.supportsWdl) {
            steps.append(
                engine_test::step(QStringLiteral("setoption name UCI_ShowWDL value %1")
                                      .arg(wdl ? QStringLiteral("true") : QStringLiteral("false")),
                                  {}));
        }
        steps.append(engine_test::step(QStringLiteral("ucinewgame"), {}));
        steps.append(ready());
        return steps;
    }
    static void append(QJsonArray& target, const QJsonArray& source) {
        for (const auto& value : source) {
            target.append(value);
        }
    }
    EngineSupervisor& service() { return fixture.service; }
    QTemporaryDir directory;
    engine_test::Validator fixture;
    EngineProfile profile;
    QUuid session = QUuid::createUuid();

  private:
    QString declarations_;
};
AnalysisResult result(const QSignalSpy& spy, qsizetype index = -1) {
    return qvariant_cast<AnalysisResult>(spy[index < 0 ? spy.size() - 1 : index][0]);
}
QStringList tokens(const QString& text) { return text.simplified().split(u' '); }
} // namespace
class EngineAnalysisTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { QTest::failOnWarning(); }
    void limitsAndReuse() {
        Conversation conversation;
        auto steps = conversation.setup(3, true);
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        steps.append(engine_test::step(
            QStringLiteral("go nodes 200"),
            {engine_test::write(QStringLiteral(
                "info depth 2 multipv 1 score cp 25 wdl 200 700 100 nodes 200 pv e2e4 e7e5\n"
                "info depth 2 multipv 2 score cp 15 nodes 200 pv d2d4 d7d5\n"
                "info depth 2 multipv 3 score cp 10 nodes 200 pv c2c4 e7e5\n"
                "info depth 3 multipv 1 score cp 30 lowerbound wdl 210 690 100 pv e2e4 e7e5\n"
                "bestmove e2e4 ponder e7e5\n"))}));
        steps.append(ready());
        Conversation::append(steps, conversation.setup());
        steps.append(
            engine_test::step(QStringLiteral("position startpos moves f2f3 e7e5 g2g4"), {}));
        steps.append(
            engine_test::step(QStringLiteral("go depth 2"),
                              {engine_test::write(QStringLiteral(
                                  "info depth 2 score mate 1 pv d8h4\nbestmove d8h4\n"))}));
        steps.append(ready());
        Conversation::append(steps, conversation.setup());
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        steps.append(
            engine_test::step(QStringLiteral("go movetime 20 searchmoves d2d4"),
                              {engine_test::write(QStringLiteral("bestmove d2d4\n"), 20)}));
        steps.append(ready());
        Conversation::append(steps, conversation.setup());
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        steps.append(engine_test::step(
            QStringLiteral("go infinite"),
            {engine_test::write(QStringLiteral("info depth 1 score cp 10 pv e2e4\n"))}));
        steps.append(engine_test::step(QStringLiteral("stop"),
                                       {engine_test::write(QStringLiteral("bestmove e2e4\n"))}));
        steps.append(ready());
        conversation.connect(steps);
        auto& service = conversation.service();
        QSignalSpy done(&service, &EngineSupervisor::analysisFinished);
        QSignalSpy states(&service, &EngineSupervisor::searchChanged);
        auto first = request(NodeLimit{200}, 8, true);
        service.analyze(conversation.session, first);
        QTRY_COMPARE(done.size(), 1);
        auto evidence = result(done).evidence;
        QCOMPARE(result(done).outcome, AnalysisOutcome::Completed);
        QCOMPARE(evidence.requestedMultiPv, 8);
        QCOMPARE(evidence.effectiveMultiPv, 3);
        QCOMPARE(evidence.achievedWidth, 3);
        QVERIFY(!evidence.commonDepth);
        QCOMPARE(evidence.lines[0].statistics.depth, 3);
        QCOMPARE(evidence.lines[1].statistics.depth, 2);
        QCOMPARE(evidence.lines[0].score->bound, ScoreBound::Lower);
        QCOMPARE(evidence.lines[0].wdl->wins, 210);
        QVERIFY(evidence.ponder);
        QCOMPARE(evidence.statistics.nodes, 200);
        auto second = request(DepthLimit{2});
        for (const auto* uci : {"f2f3", "e7e5", "g2g4"}) {
            QVERIFY(std::holds_alternative<std::monostate>(
                second.position.play(move(second.position, uci))));
        }
        service.analyze(conversation.session, second);
        QTRY_COMPARE(done.size(), 2);
        evidence = result(done).evidence;
        QCOMPARE(result(done).outcome, AnalysisOutcome::Completed);
        QCOMPARE(evidence.lines[0].score->type, ScoreType::Mate);
        QCOMPARE(evidence.lines[0].score->value, 1);
        QVERIFY(!evidence.lines[0].wdl);
        QCOMPARE(evidence.commonDepth, 2);
        auto timed = request(TimeLimit{20});
        timed.searchMoves = std::vector{move(timed.position, "d2d4")};
        service.analyze(conversation.session, timed);
        QTRY_COMPARE(done.size(), 3);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Completed);
        const auto infinite = request(InfiniteLimit{});
        service.analyze(conversation.session, infinite);
        QTRY_COMPARE(qvariant_cast<SearchState>(states.back()[0]).phase, SearchPhase::Analyzing);
        service.stopAnalysis(conversation.session, infinite.id);
        service.stopAnalysis(conversation.session, infinite.id);
        QTRY_COMPARE(done.size(), 4);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Stopped);
        QVERIFY(result(done).evidence.bestMove);
        // Old terminal IDs cannot accidentally stop or be reused for another go.
        service.analyze(conversation.session, first);
        QTRY_COMPARE(done.size(), 5);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Rejected);
    }
    void rejectionAndStopBeforeGo() {
        Conversation conversation;
        auto steps = conversation.setup();
        steps[steps.size() - 1] = ready(QStringLiteral("readyok\n"), 80);
        conversation.connect(steps);
        auto& service = conversation.service();
        QSignalSpy done(&service, &EngineSupervisor::analysisFinished);
        auto invalid = request(NodeLimit{0});
        service.analyze(conversation.session, invalid);
        service.analyze(QUuid::createUuid(), request());
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(result(done, 0).error->code, ErrorCode::InvalidRequest);
        QCOMPARE(result(done, 1).error->code, ErrorCode::Busy);
        const auto active = request();
        service.analyze(conversation.session, active);
        service.analyze(conversation.session, request());
        service.stopAnalysis(QUuid::createUuid(), active.id);
        service.stopAnalysis(conversation.session, QUuid::createUuid());
        service.stopAnalysis(conversation.session, active.id);
        QTRY_COMPARE(done.size(), 4);
        QCOMPARE(result(done, 2).error->code, ErrorCode::Busy);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Stopped);
        QVERIFY(!result(done).evidence.bestMove);
        QTest::qWait(400); // The consumed configuration deadline must not kill Ready.
        QSignalSpy runtime(&service, &EngineSupervisor::runtimeChanged);
        service.disconnectEngine(conversation.session);
        QTRY_VERIFY(!runtime.empty());
        QTRY_COMPARE(qvariant_cast<RuntimeState>(runtime.back()[0]).phase,
                     RuntimePhase::Disconnected);
    }
    void auxiliaryEvidenceAndFragmentedWhitespace() {
        Conversation conversation;
        auto steps = conversation.setup();
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        steps.append(engine_test::step(
            QStringLiteral("go nodes 200"),
            {engine_test::write(QStringLiteral("\tinfo  currline 1 e2e4 e7e5 refut")),
             engine_test::write(
                 QStringLiteral(
                     "ation d2d4 d7d5 currmove e2e4 currmovenumber 1 cpuload 1000 sbhits 2 "
                     "depth 3 score cp 23 pv e2e4 e7e5 vendor value nodes 200\r\n"
                     " bestmove\te2e4\tponder "),
                 10),
             engine_test::write(QStringLiteral("e7e5 \n"), 20)}));
        steps.append(ready());
        conversation.connect(steps);
        QSignalSpy done(&conversation.service(), &EngineSupervisor::analysisFinished);
        QSignalSpy progress(&conversation.service(), &EngineSupervisor::analysisUpdated);
        conversation.service().analyze(conversation.session, request());
        QTRY_COMPARE(done.size(), 1);
        const auto completed = result(done);
        QCOMPARE(completed.outcome, AnalysisOutcome::Completed);
        QCOMPARE(completed.evidence.lines.size(), std::size_t(1));
        QCOMPARE(completed.evidence.lines.front().score->value, 23);
        QCOMPARE(completed.evidence.lines.front().pv.size(), std::size_t(2));
        QCOMPARE(completed.evidence.statistics.nodes, 200);
        QCOMPARE(completed.evidence.commonDepth, 3);
        QVERIFY(completed.evidence.bestMove);
        QVERIFY(completed.evidence.ponder);
        QVERIFY(!progress.empty());
        const auto final = qvariant_cast<AnalysisSnapshot>(progress.back()[0]);
        QCOMPARE(final.bestMove, completed.evidence.bestMove);
        QCOMPARE(final.ponder, completed.evidence.ponder);
    }
    void malformedEvidence_data() {
        QTest::addColumn<QString>("output");
        for (const auto& row :
             {std::pair{"invalid-current-move", "info score cp 20 pv e2e4 currmove e2e5\n"},
              {"invalid-refutation", "info refutation e2e4 e7e\n"},
              {"invalid-current-line", "info currline 1 e2e4 e7e5qq\n"},
              {"invalid-cpu-number", "info currline 0 e2e4\n"},
              {"invalid-cpu-load", "info cpuload 1001\n"},
              {"invalid-move-number", "info currmovenumber 0\n"},
              {"duplicate-auxiliary", "info sbhits 1 sbhits 2\n"},
              {"trailing-bestmove", "bestmove e2e4 ponder e7e5 extra\n"},
              {"illegal-pv", "info depth 1 score cp 10 pv e2e5\n"},
              {"bad-rank", "info multipv 2 score cp 10 pv e2e4\n"},
              {"bad-score", "info score cp broken pv e2e4\n"},
              {"bad-wdl", "info wdl 1000 1 0 pv e2e4\n"},
              {"overflow", "info nodes 18446744073709551616\n"},
              {"negative-depth", "info depth -1\n"},
              {"bad-bestmove", "bestmove e2e5\n"},
              {"null-live-position", "bestmove 0000\n"},
              {"illegal-ponder", "bestmove e2e4 ponder e2e3\n"},
              {"duplicate-field", "info depth 1 depth 2 score cp 1 pv e2e4\n"},
              {"unsolicited-ready", "readyok\n"},
              {"bundled-barrier", "bestmove e2e4\nreadyok\n"}}) {
            QTest::newRow(row.first) << QString::fromUtf8(row.second);
        }
    }
    void malformedEvidence() {
        QFETCH(QString, output);
        Conversation conversation;
        auto steps = conversation.setup();
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        steps.append(
            engine_test::step(QStringLiteral("go nodes 200"), {engine_test::write(output)}));
        if (output.startsWith(QStringLiteral("bestmove e2e4\nreadyok"))) {
            steps.append(engine_test::step(QStringLiteral("isready"), {}));
        }
        conversation.connect(steps);
        QSignalSpy done(&conversation.service(), &EngineSupervisor::analysisFinished);
        QSignalSpy runtime(&conversation.service(), &EngineSupervisor::runtimeChanged);
        conversation.service().analyze(conversation.session, request());
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Failed);
        QCOMPARE(result(done).error->code, ErrorCode::Protocol);
        QTRY_COMPARE(qvariant_cast<RuntimeState>(runtime.back()[0]).phase, RuntimePhase::Failed);
        QVERIFY(result(done).evidence.session == conversation.session);
        QVERIFY(result(done).evidence.lines.empty());
        if (!output.startsWith(QStringLiteral("bestmove e2e4\nreadyok"))) {
            QVERIFY(!result(done).evidence.bestMove);
        }
    }
    void probesAndStop() {
        Conversation conversation(identity, 70, 300);
        auto steps = conversation.setup();
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        steps.append(engine_test::step(QStringLiteral("go infinite"), {}));
        steps.append(ready());
        steps.append(engine_test::step(
            QStringLiteral("stop"),
            {engine_test::write(
                QStringLiteral("info depth 1 score cp -10 upperbound pv e2e4\nbestmove e2e4\n"))}));
        steps.append(ready());
        conversation.connect(steps);
        auto& service = conversation.service();
        QSignalSpy done(&service, &EngineSupervisor::analysisFinished);
        const auto search = request(InfiniteLimit{});
        service.analyze(conversation.session, search);
        QTest::qWait(170);
        QVERIFY(done.empty());
        service.stopAnalysis(conversation.session, search.id);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Stopped);
        QCOMPARE(result(done).evidence.lines[0].score->bound, ScoreBound::Upper);
    }
    void searchFailures_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("config-timeout") << QStringLiteral("config");
        QTest::newRow("probe-timeout") << QStringLiteral("probe");
        QTest::newRow("stop-timeout") << QStringLiteral("stop");
        QTest::newRow("movetime-timeout") << QStringLiteral("time");
        QTest::newRow("barrier-timeout") << QStringLiteral("barrier");
        QTest::newRow("crash") << QStringLiteral("crash");
    }
    void searchFailures() {
        QFETCH(QString, kind);
        Conversation conversation(identity, kind == QStringLiteral("probe") ? 50 : 10000, 150);
        auto steps = conversation.setup();
        auto search = request(kind == QStringLiteral("time") ? SearchLimit{TimeLimit{10}}
                                                             : SearchLimit{InfiniteLimit{}});
        if (kind == QStringLiteral("config")) {
            steps[steps.size() - 1] = engine_test::step(QStringLiteral("isready"), {});
        } else {
            steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
            QJsonArray actions;
            if (kind == QStringLiteral("crash")) {
                actions.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("crash")}});
            }
            if (kind == QStringLiteral("barrier")) {
                search.limit = NodeLimit{200};
                actions.append(engine_test::write(QStringLiteral("bestmove e2e4\n")));
            }
            steps.append(engine_test::step(
                kind == QStringLiteral("time")      ? QStringLiteral("go movetime 10")
                : kind == QStringLiteral("barrier") ? QStringLiteral("go nodes 200")
                                                    : QStringLiteral("go infinite"),
                actions));
            if (kind == QStringLiteral("probe") || kind == QStringLiteral("barrier")) {
                steps.append(engine_test::step(QStringLiteral("isready"), {}));
            }
            if (kind == QStringLiteral("stop")) {
                steps.append(engine_test::step(QStringLiteral("stop"), {}));
            }
        }
        conversation.connect(steps);
        QSignalSpy done(&conversation.service(), &EngineSupervisor::analysisFinished);
        QSignalSpy states(&conversation.service(), &EngineSupervisor::searchChanged);
        conversation.service().analyze(conversation.session, search);
        if (kind == QStringLiteral("stop")) {
            QTRY_VERIFY(!states.empty() && qvariant_cast<SearchState>(states.back()[0]).phase ==
                                               SearchPhase::Analyzing);
            conversation.service().stopAnalysis(conversation.session, search.id);
        }
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Failed);
        QCOMPARE(result(done).error->code,
                 kind == QStringLiteral("crash") ? ErrorCode::Crash : ErrorCode::Timeout);
    }
    void shutdownDuringSearch() {
        Conversation conversation;
        auto steps = conversation.setup();
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        steps.append(engine_test::step(QStringLiteral("go infinite"), {}));
        steps.append(engine_test::step(QStringLiteral("stop"),
                                       {engine_test::write(QStringLiteral("bestmove e2e4\n"))}));
        steps.append(ready());
        conversation.connect(steps);
        auto& service = conversation.service();
        QSignalSpy states(&service, &EngineSupervisor::searchChanged);
        QSignalSpy done(&service, &EngineSupervisor::analysisFinished);
        QSignalSpy closed(&service, &EngineSupervisor::closed);
        service.analyze(conversation.session, request(InfiniteLimit{}));
        QTRY_VERIFY(!states.empty() &&
                    qvariant_cast<SearchState>(states.back()[0]).phase == SearchPhase::Analyzing);
        service.close();
        QTRY_COMPARE(closed.size(), 1);
        QCOMPARE(done.size(), 1);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Stopped);
        service.analyze(conversation.session, request());
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Rejected);
        QCOMPARE(result(done).error->code, ErrorCode::Closing);
    }
    void probeCompletionRaceAndFragmentation() {
        Conversation conversation(identity, 50, 300);
        auto steps = conversation.setup();
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        steps.append(engine_test::step(QStringLiteral("go nodes 200"), {}));
        steps.append(ready(
            QStringLiteral("info depth 2 score cp 10 pv e2e4 e7e5\nbestmove e2e4\nreadyok\n")));
        // The probe reply cannot also serve as the post-bestmove synchronization reply.
        steps.append(engine_test::step(QStringLiteral("isready"),
                                       {engine_test::write(QStringLiteral("read")),
                                        engine_test::write(QStringLiteral("yok\r\n"), 20)}));
        conversation.connect(steps);
        QSignalSpy done(&conversation.service(), &EngineSupervisor::analysisFinished);
        conversation.service().analyze(conversation.session, request());
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Completed);
        QCOMPARE(result(done).evidence.commonDepth, 2);
    }
    void disconnectDuringSearchAndReconnect() {
        Conversation conversation;
        auto steps = conversation.setup();
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        steps.append(engine_test::step(QStringLiteral("go infinite"), {}));
        steps.append(engine_test::step(
            QStringLiteral("stop"), {engine_test::write(QStringLiteral("bestmove e2e4\n"), 20)}));
        steps.append(ready());
        conversation.connect(steps);
        auto& service = conversation.service();
        QSignalSpy states(&service, &EngineSupervisor::searchChanged);
        QSignalSpy done(&service, &EngineSupervisor::analysisFinished);
        QSignalSpy runtime(&service, &EngineSupervisor::runtimeChanged);
        service.analyze(conversation.session, request(InfiniteLimit{}));
        QTRY_VERIFY(!states.empty() &&
                    qvariant_cast<SearchState>(states.back()[0]).phase == SearchPhase::Analyzing);
        service.disconnectEngine(conversation.session);
        QTRY_VERIFY(!runtime.empty() && qvariant_cast<RuntimeState>(runtime.back()[0]).phase ==
                                            RuntimePhase::Disconnected);
        QCOMPARE(done.size(), 1);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Stopped);
        ValidatorTestAccess::configure(
            service,
            engine_test::scenario(conversation.directory.filePath(QStringLiteral("reconnect.json")),
                                  engine_test::handshake(identity)),
            1000);
        service.connectEngine(QUuid::createUuid(), conversation.profile);
        QTRY_COMPARE(qvariant_cast<RuntimeState>(runtime.back()[0]).phase, RuntimePhase::Ready);
    }
    void minimalEngineAndTerminalPosition() {
        Conversation conversation(QStringLiteral("id name Minimal\nuciok\n"));
        auto search = request(NodeLimit{200}, 10, true);
        search.position =
            std::get<chess::Position>(chess::Position::fromFen("7k/6Q1/5K2/8/8/8/8/8 b - - 0 1"));
        auto steps = conversation.setup();
        steps.append(
            engine_test::step(QStringLiteral("position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1"), {}));
        steps.append(engine_test::step(
            QStringLiteral("go nodes 200"),
            {engine_test::write(QStringLiteral("info depth 0 score mate 0\nbestmove 0000\n"))}));
        steps.append(ready());
        conversation.connect(steps);
        QSignalSpy done(&conversation.service(), &EngineSupervisor::analysisFinished);
        conversation.service().analyze(conversation.session, search);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Completed);
        QVERIFY(!result(done).evidence.bestMove);
        QCOMPARE(result(done).evidence.effectiveMultiPv, 1);
        QCOMPARE(result(done).evidence.achievedWidth, 0);
        QVERIFY(!result(done).evidence.wdlSupported);
        QVERIFY(!result(done).evidence.lines[0].wdl);
    }
    void streamingBoundsAndProgress() {
        // Scale the configurable byte budget down; the same code enforces 1 MiB in production.
        Conversation conversation(identity, 10000, 300);
        auto steps = conversation.setup();
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        QJsonArray actions;
        for (int i = 0; i < 30; ++i) {
            actions.append(engine_test::write(
                QStringLiteral("info depth 1 score cp 10 nodes %1 pv e2e4\n").arg(i), i * 80));
        }
        actions.append(engine_test::write(QStringLiteral("bestmove e2e4\n"), 2500));
        steps.append(engine_test::step(QStringLiteral("go nodes 200"), actions));
        steps.append(ready());
        QJsonArray all{engine_test::step(QStringLiteral("uci"), {engine_test::write(identity)}),
                       ready()};
        Conversation::append(all, steps);
        all.append(quit());
        ValidatorTestAccess::configure(
            conversation.service(),
            engine_test::scenario(conversation.directory.filePath(QStringLiteral("stream.json")),
                                  all),
            1000, 1024);
        QSignalSpy runtime(&conversation.service(), &EngineSupervisor::runtimeChanged);
        conversation.service().connectEngine(conversation.session, conversation.profile);
        QTRY_VERIFY(!runtime.empty() &&
                    qvariant_cast<RuntimeState>(runtime.back()[0]).phase == RuntimePhase::Ready);
        QSignalSpy done(&conversation.service(), &EngineSupervisor::analysisFinished);
        QSignalSpy progress(&conversation.service(), &EngineSupervisor::analysisUpdated);
        QTimer heartbeat;
        heartbeat.setInterval(5);
        int ticks = 0;
        QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&ticks] { ++ticks; });
        heartbeat.start();
        QElapsedTimer elapsed;
        elapsed.start();
        conversation.service().analyze(conversation.session, request());
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Completed);
        QVERIFY(elapsed.elapsed() >= 2500);
        QVERIFY(ticks > 20);
        QVERIFY(progress.size() >= 10); // Worker-side progress timer remained serviceable.
        QVERIFY(progress.size() <= 27);
        QCOMPARE(result(done).evidence.statistics.nodes, 29);
    }
    void searchOutputFailures_data() {
        QTest::addColumn<QString>("data");
        QTest::addColumn<bool>("stderrStream");
        QTest::newRow("oversized-line") << QString(65537, u'x') << false;
        QTest::newRow("stdout-rate")
            << QStringLiteral("info string ") + QString(32700, u'x') + QStringLiteral("\n")
            << false;
        QTest::newRow("stderr-rate") << QString(32700, u'x') << true;
    }
    void searchOutputFailures() {
        QFETCH(QString, data);
        QFETCH(bool, stderrStream);
        Conversation conversation;
        auto steps = conversation.setup();
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        auto action = engine_test::write(data);
        if (stderrStream) {
            action[QStringLiteral("stream")] = QStringLiteral("stderr");
        }
        steps.append(engine_test::step(QStringLiteral("go nodes 200"), {action}));
        QJsonArray all{engine_test::step(QStringLiteral("uci"), {engine_test::write(identity)}),
                       ready()};
        Conversation::append(all, steps);
        all.append(quit());
        const auto budget = data.size() > 65536 ? 1024 * 1024 : 16000;
        ValidatorTestAccess::configure(
            conversation.service(),
            engine_test::scenario(conversation.directory.filePath(QStringLiteral("flood.json")),
                                  all),
            1000, budget);
        QSignalSpy runtime(&conversation.service(), &EngineSupervisor::runtimeChanged);
        conversation.service().connectEngine(conversation.session, conversation.profile);
        QTRY_VERIFY(!runtime.empty() &&
                    qvariant_cast<RuntimeState>(runtime.back()[0]).phase == RuntimePhase::Ready);
        QSignalSpy done(&conversation.service(), &EngineSupervisor::analysisFinished);
        conversation.service().analyze(conversation.session, request());
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Failed);
        QCOMPARE(result(done).error->code, ErrorCode::OutputLimit);
    }
    void distinctCandidatesRequiredForCoverage() {
        const auto search = request(NodeLimit{200}, 2);
        AnalysisSnapshot snapshot;
        snapshot.effectiveMultiPv = 2;
        for (const auto* text : {"info depth 3 multipv 1 score cp 20 pv e2e4",
                                 "info depth 3 multipv 2 score cp 10 pv e2e4"}) {
            updateEvidence(snapshot, std::get<InfoUpdate>(parseSearchInfo(
                                         tokens(QString::fromUtf8(text)), search, 2)));
        }
        QCOMPARE(snapshot.achievedWidth, 1);
        QVERIFY(!snapshot.commonDepth);
        // An intermediate rank overlap is not itself a protocol failure: rankings can change.
        updateEvidence(
            snapshot,
            std::get<InfoUpdate>(parseSearchInfo(
                tokens(QStringLiteral("info depth 3 multipv 2 score cp 10 pv d2d4")), search, 2)));
        QCOMPARE(snapshot.achievedWidth, 2);
        QCOMPARE(snapshot.commonDepth, 3);
    }
    void malformedMoveTails_data() {
        QTest::addColumn<QString>("tail");
        QTest::newRow("truncated") << QStringLiteral("e7e");
        QTest::newRow("overlong") << QStringLiteral("e7e5garbage");
        QTest::newRow("invalid-promotion-length") << QStringLiteral("e7e5qq");
    }
    void malformedMoveTails() {
        QFETCH(QString, tail);
        const auto parsed = parseSearchInfo(
            tokens(QStringLiteral("info depth 3 score cp 20 pv e2e4 ") + tail), request(), 1);
        QVERIFY(std::holds_alternative<Error>(parsed));
    }
    void disconnectDiscardsSameReadTail() {
        Conversation conversation;
        auto steps = conversation.setup();
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        steps.append(engine_test::step(QStringLiteral("go infinite"), {}));
        steps.append(engine_test::step(QStringLiteral("stop"),
                                       {engine_test::write(QStringLiteral("bestmove e2e4\n"))}));
        // Teardown begins within this very read, immediately after the readiness reply.
        steps.append(ready(QStringLiteral("readyok\nbestmove e2e5\n")));
        conversation.connect(steps);
        auto& service = conversation.service();
        QSignalSpy states(&service, &EngineSupervisor::searchChanged);
        QSignalSpy runtime(&service, &EngineSupervisor::runtimeChanged);
        QSignalSpy done(&service, &EngineSupervisor::analysisFinished);
        service.analyze(conversation.session, request(InfiniteLimit{}));
        QTRY_VERIFY(!states.empty() &&
                    qvariant_cast<SearchState>(states.back()[0]).phase == SearchPhase::Analyzing);
        service.disconnectEngine(conversation.session);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Stopped);
        QTRY_COMPARE(qvariant_cast<RuntimeState>(runtime.back()[0]).phase,
                     RuntimePhase::Disconnected);
    }
    void stalledCallerCoalescesProgress() {
        Conversation conversation;
        auto steps = conversation.setup();
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        QJsonArray updates;
        for (int i = 0; i < 30; ++i) {
            updates.append(engine_test::write(
                QStringLiteral("info depth 2 nodes %1 score cp 20 pv e2e4\n").arg(i), i * 20));
        }
        steps.append(engine_test::step(QStringLiteral("go infinite"), updates));
        steps.append(engine_test::step(QStringLiteral("stop"),
                                       {engine_test::write(QStringLiteral("bestmove e2e4\n"))}));
        steps.append(ready());
        conversation.connect(steps);
        auto& service = conversation.service();
        QSignalSpy states(&service, &EngineSupervisor::searchChanged);
        QSignalSpy progress(&service, &EngineSupervisor::analysisUpdated);
        QSignalSpy done(&service, &EngineSupervisor::analysisFinished);
        const auto search = request(InfiniteLimit{});
        service.analyze(conversation.session, search);
        QTRY_VERIFY(!states.empty() &&
                    qvariant_cast<SearchState>(states.back()[0]).phase == SearchPhase::Analyzing);
        progress.clear();
        QTest::qSleep(1200); // Deliberately stall only the caller; the engine thread stays live.
        QCoreApplication::sendPostedEvents(&service, QEvent::MetaCall);
        QCOMPARE(progress.size(), 1);
        QCOMPARE(qvariant_cast<AnalysisSnapshot>(progress.back()[0]).statistics.nodes, 29);
        service.stopAnalysis(conversation.session, search.id);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Stopped);
    }
    void integerEvidenceUsesRepresentableRange() {
        const auto parsed = parseSearchInfo(
            tokens(QStringLiteral(
                "info nodes 18446744073709551615 time 0 score cp -9223372036854775808 pv e2e4")),
            request(), 1);
        QVERIFY(std::holds_alternative<InfoUpdate>(parsed));
        const auto& line = std::get<InfoUpdate>(parsed).line;
        QCOMPARE(line.statistics.nodes, std::numeric_limits<std::uint64_t>::max());
        QCOMPARE(line.score->value, std::numeric_limits<std::int64_t>::min());
        QVERIFY(std::holds_alternative<Error>(parseSearchInfo(
            tokens(QStringLiteral("info nodes 18446744073709551616")), request(), 1)));
        QVERIFY(std::holds_alternative<Error>(parseSearchInfo(
            tokens(QStringLiteral("info score cp -9223372036854775809")), request(), 1)));
        QVERIFY(std::holds_alternative<Error>(prepareSearch(
            request(NodeLimit{std::numeric_limits<std::uint64_t>::max()}), Capabilities{})));
    }
    void stalledCallerReceivesFinalBeforeCompletion() {
        Conversation conversation;
        auto steps = conversation.setup();
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        steps.append(engine_test::step(
            QStringLiteral("go nodes 200"),
            {engine_test::write(QStringLiteral("info depth 2 score cp 20 nodes 100 pv e2e4\n")),
             engine_test::write(
                 QStringLiteral("info depth 3 score cp 30 nodes 200 pv e2e4 e7e5\nbestmove e2e4\n"),
                 250)}));
        steps.append(ready());
        conversation.connect(steps);
        auto& service = conversation.service();
        QSignalSpy states(&service, &EngineSupervisor::searchChanged);
        QSignalSpy progress(&service, &EngineSupervisor::analysisUpdated);
        QSignalSpy done(&service, &EngineSupervisor::analysisFinished);
        bool terminal = false;
        bool lateProgress = false;
        std::atomic<bool> correctThread{true};
        QObject observer; // Disconnect before captured locals die, including assertion failures.
        QObject::connect(&service, &EngineSupervisor::analysisUpdated, &observer,
                         [&](const AnalysisSnapshot&) { lateProgress = lateProgress || terminal; });
        QObject::connect(
            &service, &EngineSupervisor::analysisUpdated, &observer,
            [&](const AnalysisSnapshot&) {
                if (QThread::currentThread() != service.thread()) {
                    correctThread.store(false);
                }
            },
            Qt::DirectConnection);
        QObject::connect(&service, &EngineSupervisor::analysisFinished, &observer,
                         [&](const AnalysisResult&) { terminal = true; });
        service.analyze(conversation.session, request());
        QTRY_VERIFY(!states.empty() &&
                    qvariant_cast<SearchState>(states.back()[0]).phase == SearchPhase::Analyzing);
        QTest::qSleep(700);
        QTRY_COMPARE(done.size(), 1);
        QTest::qWait(150);
        QVERIFY(!lateProgress);
        QVERIFY(correctThread.load());
        QVERIFY(!progress.empty());
        const auto last = qvariant_cast<AnalysisSnapshot>(progress.back()[0]);
        QVERIFY(last.bestMove);
        QCOMPARE(last.statistics.nodes, 200);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Completed);
    }
    void analysisStressKeepsCallerResponsive() {
        auto search = request(NodeLimit{200}, 20);
        QString positionCommand = QStringLiteral("position startpos moves");
        for (int i = 0; i < 1000; ++i) {
            for (const auto* uci : {"g1f3", "g8f6", "f3g1", "f6g8"}) {
                QVERIFY(std::holds_alternative<std::monostate>(
                    search.position.play(move(search.position, uci))));
                positionCommand += u' ' + QString::fromUtf8(uci);
            }
        }
        Conversation conversation(
            QString(identity).replace(QStringLiteral("max 3"), QStringLiteral("max 20")));
        auto steps = conversation.setup(20);
        steps.append(engine_test::step(positionCommand, {}));
        QString batch;
        const auto legal = search.position.legalMoves();
        QCOMPARE(legal.size(), std::size_t(20));
        for (std::size_t i = 0; i < legal.size(); ++i) {
            batch += QStringLiteral("info depth 8 multipv %1 score cp 20 nodes 200 pv %2\n")
                         .arg(i + 1)
                         .arg(QString::fromStdString(
                             std::get<std::string>(search.position.uci(legal[i]))));
        }
        QJsonArray actions;
        for (int i = 0; i < 50; ++i) {
            actions.append(engine_test::write(batch, i * 20));
        }
        actions.append(engine_test::write(
            QStringLiteral("bestmove ") +
                QString::fromStdString(std::get<std::string>(search.position.uci(legal[0]))) +
                u'\n',
            1100));
        steps.append(engine_test::step(QStringLiteral("go nodes 200"), actions));
        steps.append(ready());
        conversation.connect(steps);
        auto& service = conversation.service();
        QSignalSpy done(&service, &EngineSupervisor::analysisFinished);
        QSignalSpy progress(&service, &EngineSupervisor::analysisUpdated);
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 lastTick = 0;
        qint64 maxGap = 0;
        int ticks = 0;
        QTimer heartbeat;
        heartbeat.setInterval(5);
        QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
            const auto now = elapsed.elapsed();
            maxGap = std::max(maxGap, now - lastTick);
            lastTick = now;
            ++ticks;
        });
        heartbeat.start();
        service.analyze(conversation.session, search);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000);
        QCOMPARE(result(done).outcome, AnalysisOutcome::Completed);
        QCOMPARE(result(done).evidence.achievedWidth, 20);
        QCOMPARE(result(done).evidence.commonDepth, 8);
        QVERIFY(ticks > 20);
        QVERIFY(progress.size() > 2);
        qInfo("Search stress: %lld ms, %d caller ticks, %lld ms max gap, %lld progress frames",
              static_cast<long long>(elapsed.elapsed()), ticks, static_cast<long long>(maxGap),
              static_cast<long long>(progress.size()));
    }
    void evidenceAndInputBounds() {
        auto search = request();
        const auto first = move(search.position, "e2e4");
        search.searchMoves = std::vector{first};
        QVERIFY(std::holds_alternative<Error>(
            parseSearchInfo(tokens(QStringLiteral("info score cp 1 pv d2d4")), search, 1)));
        QVERIFY(std::holds_alternative<Error>(
            parseBestMove(tokens(QStringLiteral("bestmove d2d4")), search)));
        const auto parsed = parseSearchInfo(
            tokens(QStringLiteral(
                "info pv e2e4 e7e5 depth 2 score cp 12 nodes 200 string ignored score malformed")),
            search, 1);
        QVERIFY(std::holds_alternative<InfoUpdate>(parsed));
        QCOMPARE(std::get<InfoUpdate>(parsed).line.statistics.depth, 2);
        QCOMPARE(std::get<InfoUpdate>(parsed).line.statistics.nodes, 200);
        const auto extension =
            parseSearchInfo(tokens(QStringLiteral(
                                "info score cp 10 pv e2e4 custom_extension opaque_value nodes 12")),
                            search, 1);
        QVERIFY(std::holds_alternative<InfoUpdate>(extension));
        QCOMPARE(std::get<InfoUpdate>(extension).line.pv.size(), std::size_t(1));
        QCOMPARE(std::get<InfoUpdate>(extension).line.statistics.nodes, 12);
        QVERIFY(std::holds_alternative<Error>(
            parseSearchInfo(tokens(QStringLiteral("info score cp 10 pv e2e4 e7e9")), search, 1)));
        search.searchMoves.reset();
        QString pv = QStringLiteral("info score cp 1 pv");
        for (int i = 0; i < 257; ++i) {
            pv += QStringLiteral(" g1f3 g8f6 f3g1 f6g8");
        }
        QVERIFY(std::holds_alternative<Error>(parseSearchInfo(tokens(pv), search, 1)));
        for (int i = 0; i < 3300; ++i) {
            for (const auto* uci : {"g1f3", "g8f6", "f3g1", "f6g8"}) {
                const auto played = search.position.play(move(search.position, uci));
                QVERIFY(std::holds_alternative<std::monostate>(played));
            }
        }
        const auto context = search.position.context();
        QVERIFY(std::holds_alternative<Error>(prepareSearch(search, Capabilities{})));
        QCOMPARE(search.position.context(),
                 context); // Oversized history is never truncated/mutated.
    }
    void parserAndPositionBoundaries() {
        auto search = request();
        auto capabilities = Capabilities{};
        auto prepared = std::get<PreparedSearch>(prepareSearch(search, capabilities));
        QCOMPARE(prepared.position, QByteArray("position startpos\n"));
        search.multiPv = 10;
        QCOMPARE(std::get<PreparedSearch>(prepareSearch(search, capabilities)).effectiveMultiPv, 1);
        search.searchMoves = std::vector<chess::Move>{};
        QVERIFY(std::holds_alternative<Error>(prepareSearch(search, capabilities)));
        search.searchMoves =
            std::vector{move(search.position, "e2e4"), move(search.position, "e2e4")};
        QVERIFY(std::holds_alternative<Error>(prepareSearch(search, capabilities)));
        search.searchMoves.reset();
        auto update =
            parseSearchInfo(tokens(QStringLiteral("info string score broken pv e2e5")), search, 1);
        QVERIFY(std::holds_alternative<InfoUpdate>(update));
        QVERIFY(!std::get<InfoUpdate>(update).hasEvidence);
        update = parseSearchInfo(
            tokens(QStringLiteral(
                "info currmove e2e4 depth 0 nodes 0 hashfull 0 score cp -12 pv e2e4")),
            search, 1);
        QVERIFY(std::holds_alternative<InfoUpdate>(update));
        AnalysisSnapshot snapshot;
        updateEvidence(snapshot, std::get<InfoUpdate>(update));
        QCOMPARE(snapshot.commonDepth, 0);
        updateEvidence(
            snapshot, std::get<InfoUpdate>(parseSearchInfo(
                          tokens(QStringLiteral("info depth 5 seldepth 7 nodes 100")), search, 1)));
        QCOMPARE(snapshot.lines[0].score->value, -12);
        QCOMPARE(snapshot.statistics.nodes, 100);
        QCOMPARE(snapshot.statistics.depth, 5);
        QCOMPARE(snapshot.statistics.selectiveDepth, 7);
        QCOMPARE(snapshot.lines[0].statistics.depth, 0);
        for (const auto& example : {std::pair{"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1g1"},
                                    {"4k3/P7/8/8/8/8/8/4K3 w - - 0 1", "a7a8q"},
                                    {"4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2", "e5d6"}}) {
            search.position = std::get<chess::Position>(chess::Position::fromFen(example.first));
            const auto line = parseSearchInfo(
                tokens(QStringLiteral("info score cp 1 pv ") + QString::fromUtf8(example.second)),
                search, 1);
            QVERIFY(std::holds_alternative<InfoUpdate>(line));
            QCOMPARE(std::get<InfoUpdate>(line).line.pv.size(), std::size_t(1));
            QVERIFY(std::get<PreparedSearch>(prepareSearch(search, capabilities))
                        .position.startsWith("position fen "));
        }
        search.position =
            std::get<chess::Position>(chess::Position::fromFen("7k/6Q1/5K2/8/8/8/8/8 b - - 0 1"));
        QVERIFY(std::holds_alternative<BestMove>(
            parseBestMove(tokens(QStringLiteral("bestmove 0000")), search)));
        QVERIFY(
            !std::get<BestMove>(parseBestMove(tokens(QStringLiteral("bestmove (none)")), search))
                 .move);
        search = request(NodeLimit{std::numeric_limits<std::uint64_t>::max()});
        QVERIFY(std::holds_alternative<Error>(prepareSearch(search, capabilities)));
        search = request(DepthLimit{0});
        QVERIFY(std::holds_alternative<Error>(prepareSearch(search, capabilities)));
        search = request(TimeLimit{0});
        QVERIFY(std::holds_alternative<Error>(prepareSearch(search, capabilities)));
    }
};
QTEST_GUILESS_MAIN(EngineAnalysisTest)
#include "EngineAnalysisTest.moc"
