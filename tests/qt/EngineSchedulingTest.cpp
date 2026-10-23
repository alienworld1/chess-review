#include "EngineTestSupport.h"
#include "TestEnvironment.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <algorithm>

using namespace chess_review::engine;
namespace {
AnalysisRequest request(SearchLimit limit = NodeLimit{200}) {
    AnalysisRequest value;
    value.id = QUuid::createUuid();
    value.limit = limit;
    return value;
}
QJsonObject ready(int delay = 0) {
    return engine_test::step(QStringLiteral("isready"),
                             {engine_test::write(QStringLiteral("readyok\n"), delay)});
}
void setup(QJsonArray& steps, int delay = 0) {
    steps.append(engine_test::step(QStringLiteral("ucinewgame"), {}));
    steps.append(ready(delay));
}
void search(QJsonArray& steps, const QString& go, const QJsonArray& output) {
    setup(steps);
    steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
    steps.append(engine_test::step(go, output));
}
void completedSearch(QJsonArray& steps, std::uint64_t nodes, int delay = 0) {
    search(steps, QStringLiteral("go nodes %1").arg(nodes),
           {engine_test::write(QStringLiteral("info depth 2 nodes %1 score cp 20 pv e2e4\n"
                                              "bestmove e2e4\n")
                                   .arg(nodes),
                               delay)});
    steps.append(ready());
}
void stoppedSearch(QJsonArray& steps, const QString& go = QStringLiteral("go infinite")) {
    search(steps, go,
           {engine_test::write(QStringLiteral("info depth 1 nodes 10 score cp 5 pv d2d4\n"))});
    steps.append(engine_test::step(QStringLiteral("stop"),
                                   {engine_test::write(QStringLiteral("bestmove d2d4\n"))}));
    steps.append(ready());
}
ScheduledResult result(const QSignalSpy& spy, qsizetype index = -1) {
    return qvariant_cast<ScheduledResult>(spy[index < 0 ? spy.size() - 1 : index][0]);
}
bool phase(const QSignalSpy& states, QUuid id, ScheduledPhase value) {
    return std::any_of(states.begin(), states.end(), [id, value](const auto& args) {
        const auto state = qvariant_cast<ScheduledState>(args[0]);
        return state.request == id && state.phase == value;
    });
}
class Conversation {
  public:
    Conversation() {
        ValidatorTestAccess::configure(
            fixture.service,
            engine_test::scenario(directory.filePath(QStringLiteral("register.json")),
                                  engine_test::handshake()),
            1000);
        QSignalSpy done(&fixture.service, &EngineSupervisor::completed);
        fixture.service.validate(QUuid::createUuid(),
                                 QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath());
        if (!done.wait(5000)) {
            qFatal("Registration timed out");
        }
        const auto registered = qvariant_cast<ValidationResult>(done.front()[1]);
        if (!std::holds_alternative<ValidatedEngine>(registered)) {
            qFatal("Registration failed");
        }
        profile.id = 1;
        profile.engine = std::get<ValidatedEngine>(registered);
        ValidatorTestAccess::configureSearch(fixture.service, 10000, 500);
    }
    void connect(QJsonArray steps, int quitDelay = 0) {
        QJsonArray all{engine_test::step(QStringLiteral("uci"),
                                         {engine_test::write(QStringLiteral(
                                             "id name Test engine\nid author Tests\nuciok\n"))}),
                       ready()};
        for (const auto& step : steps) {
            all.append(step);
        }
        all.append(engine_test::step(QStringLiteral("quit"),
                                     {QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")},
                                                  {QStringLiteral("afterMs"), quitDelay}}}));
        ValidatorTestAccess::configure(
            fixture.service,
            engine_test::scenario(directory.filePath(QStringLiteral("runtime.json")), all), 1000);
        QSignalSpy states(&fixture.service, &EngineSupervisor::runtimeChanged);
        fixture.service.connectEngine(session, profile);
        QElapsedTimer timer;
        timer.start();
        while (states.empty() ||
               qvariant_cast<RuntimeState>(states.back()[0]).phase != RuntimePhase::Ready) {
            if (timer.elapsed() > 5000) {
                qFatal("Connection timed out");
            }
            QTest::qWait(5);
        }
    }
    EngineSupervisor& service() { return fixture.service; }
    QTemporaryDir directory;
    engine_test::Validator fixture;
    EngineProfile profile;
    QUuid session = QUuid::createUuid();
};
} // namespace

class EngineSchedulingTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { QTest::failOnWarning(); }
    void priorityFifoAndLegacyAdmission() {
        Conversation c;
        QJsonArray steps;
        stoppedSearch(steps);
        for (const auto nodes : {2, 3, 4, 5, 6, 7, 8}) {
            completedSearch(steps, static_cast<std::uint64_t>(nodes));
        }
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy legacy(&service, &EngineSupervisor::analysisFinished);
        QSignalSpy legacyStates(&service, &EngineSupervisor::searchChanged);
        const auto first = request(InfiniteLimit{});
        service.scheduleAnalysis(c.session, first, AnalysisPriority::InteractiveAnalysis);
        QTRY_VERIFY(phase(states, first.id, ScheduledPhase::Analyzing));
        // Reverse priority submission verifies sorting independently of arrival order.
        std::vector<QUuid> expected(7);
        for (int index = 6; index >= 0; --index) {
            auto next = request(NodeLimit{static_cast<std::uint64_t>(index + 2)});
            expected[static_cast<std::size_t>(index)] = next.id;
            service.scheduleAnalysis(c.session, next, static_cast<AnalysisPriority>(index));
        }
        service.analyze(c.session, request());
        QTRY_COMPARE(legacy.size(), 1);
        QCOMPARE(qvariant_cast<AnalysisResult>(legacy.front()[0]).outcome,
                 AnalysisOutcome::Rejected);
        service.stopAnalysis(c.session, first.id); // Cannot bypass scheduled cancellation.
        service.cancelScheduledAnalysis(QUuid::createUuid(), first.id);
        service.cancelScheduledAnalysis(c.session, QUuid::createUuid());
        service.cancelScheduledAnalysis(c.session, first.id);
        service.cancelScheduledAnalysis(c.session, first.id);
        QTRY_COMPARE(done.size(), 8);
        QCOMPARE(result(done, 0).outcome, ScheduledOutcome::Cancelled);
        for (qsizetype index = 0; index < 7; ++index) {
            QCOMPARE(result(done, index + 1).identity.request,
                     expected[static_cast<std::size_t>(index)]);
            QCOMPARE(result(done, index + 1).outcome, ScheduledOutcome::Completed);
        }
        QVERIFY(legacyStates.empty());
    }
    void repeatedPreemptionRestartsBudgetAndPreservesFifo_data() {
        QTest::addColumn<int>("priority");
        QTest::newRow("background") << 4;
        QTest::newRow("puzzle") << 5;
        QTest::newRow("maintenance") << 6;
    }
    void repeatedPreemptionRestartsBudgetAndPreservesFifo() {
        QFETCH(int, priority);
        Conversation c;
        QJsonArray steps;
        stoppedSearch(steps, QStringLiteral("go nodes 900"));
        completedSearch(steps, 1, 50);
        stoppedSearch(steps, QStringLiteral("go nodes 900"));
        completedSearch(steps, 2, 50);
        completedSearch(steps, 900);
        completedSearch(steps, 901);
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy interrupted(&service, &EngineSupervisor::scheduledInterrupted);
        QSignalSpy legacy(&service, &EngineSupervisor::analysisFinished);
        auto background = request(NodeLimit{900});
        auto peer = request(NodeLimit{901});
        service.scheduleAnalysis(c.session, background,
                                 AnalysisPriority::ReviewBackgroundDeepening);
        QTRY_VERIFY(phase(states, background.id, ScheduledPhase::Analyzing));
        service.scheduleAnalysis(c.session, peer, static_cast<AnalysisPriority>(priority));
        service.scheduleAnalysis(c.session, request(NodeLimit{1}),
                                 AnalysisPriority::InteractiveAnalysis);
        QTRY_COMPARE(interrupted.size(), 1);
        QTRY_COMPARE(done.size(), 1);
        QTRY_VERIFY(std::count_if(states.begin(), states.end(), [&](const auto& args) {
                        auto state = qvariant_cast<ScheduledState>(args[0]);
                        return state.request == background.id &&
                               state.phase == ScheduledPhase::Analyzing;
                    }) == 2);
        service.scheduleAnalysis(c.session, request(NodeLimit{2}), AnalysisPriority::RetryMove);
        QTRY_COMPARE(done.size(), 4);
        QCOMPARE(interrupted.size(), 2);
        const auto first = qvariant_cast<ScheduledEvidence>(interrupted[0][0]);
        const auto second = qvariant_cast<ScheduledEvidence>(interrupted[1][0]);
        QCOMPARE(first.identity.request, background.id);
        QCOMPARE(first.evidence.statistics.nodes, 10);
        QCOMPARE(first.evidence.lines.size(), 1U);
        QVERIFY(first.identity.attempt != second.identity.attempt);
        QCOMPARE(result(done, 2).identity.request, background.id);
        QCOMPARE(result(done, 3).identity.request, peer.id);
        const auto final = result(done, 2);
        QCOMPARE(final.outcome, ScheduledOutcome::Completed);
        QVERIFY(final.identity.attempt != first.identity.attempt);
        QVERIFY(final.identity.attempt != second.identity.attempt);
        QCOMPARE(final.evidence->evidence.statistics.nodes, 900);
        QCOMPARE(final.evidence->evidence.lines.front().score->value, 20);
        QVERIFY(legacy.empty());
    }
    void nonPreemptible_data() {
        QTest::addColumn<int>("activePriority");
        QTest::addColumn<int>("incomingPriority");
        QTest::newRow("interactive") << 0 << 0;
        QTest::newRow("retry") << 1 << 0;
        QTest::newRow("visible") << 2 << 0;
        QTest::newRow("scan") << 3 << 0;
        QTest::newRow("background-not-interactive") << 6 << 2;
    }
    void nonPreemptible() {
        QFETCH(int, activePriority);
        QFETCH(int, incomingPriority);
        Conversation c;
        QJsonArray steps;
        completedSearch(steps, 1, 150);
        completedSearch(steps, 2);
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        QSignalSpy interrupted(&service, &EngineSupervisor::scheduledInterrupted);
        auto active = request(NodeLimit{1});
        service.scheduleAnalysis(c.session, active, static_cast<AnalysisPriority>(activePriority));
        QTRY_VERIFY(phase(states, active.id, ScheduledPhase::Analyzing));
        service.scheduleAnalysis(c.session, request(NodeLimit{2}),
                                 static_cast<AnalysisPriority>(incomingPriority));
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(result(done, 0).identity.request, active.id);
        QCOMPARE(result(done, 0).outcome, ScheduledOutcome::Completed);
        QVERIFY(interrupted.empty());
    }
    void jobCancellationAndQueueBound() {
        Conversation c;
        QJsonArray steps;
        stoppedSearch(steps);
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        auto active = request(InfiniteLimit{});
        const auto job = QUuid::createUuid();
        service.scheduleAnalysis(c.session, active, AnalysisPriority::InteractiveAnalysis, job);
        QTRY_VERIFY(phase(states, active.id, ScheduledPhase::Analyzing));
        for (int index = 0; index < 256; ++index) {
            service.scheduleAnalysis(c.session, request(), AnalysisPriority::Maintenance, job);
        }
        auto overflow = request();
        service.scheduleAnalysis(c.session, overflow, AnalysisPriority::Maintenance);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done).identity.request, overflow.id);
        QCOMPARE(result(done).error->code, ErrorCode::Busy);
        service.cancelAnalysisJob(QUuid::createUuid(), job);
        service.cancelAnalysisJob(c.session, job);
        service.cancelAnalysisJob(c.session, job);
        QTRY_COMPARE(done.size(), 258);
        for (qsizetype index = 1; index < done.size(); ++index) {
            QCOMPARE(result(done, index).outcome, ScheduledOutcome::Cancelled);
        }
        service.scheduleAnalysis(c.session, request(), AnalysisPriority::InteractiveAnalysis, job);
        QTRY_COMPARE(done.size(), 259);
        QCOMPARE(result(done).error->code, ErrorCode::Cancelled);
        service.scheduleAnalysis(c.session, active, AnalysisPriority::InteractiveAnalysis);
        QTRY_COMPARE(done.size(), 260);
        QCOMPARE(result(done).error->code, ErrorCode::InvalidRequest);
    }
    void configurationCancellation() {
        Conversation c;
        QJsonArray steps;
        setup(steps, 100); // No position/go/stop is permitted.
        completedSearch(steps, 2);
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        auto active = request();
        service.scheduleAnalysis(c.session, active, AnalysisPriority::InteractiveAnalysis);
        QTRY_VERIFY(phase(states, active.id, ScheduledPhase::Configuring));
        service.cancelScheduledAnalysis(c.session, active.id);
        service.scheduleAnalysis(c.session, request(NodeLimit{2}),
                                 AnalysisPriority::InteractiveAnalysis);
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(result(done, 0).outcome, ScheduledOutcome::Cancelled);
        QCOMPARE(result(done, 1).outcome, ScheduledOutcome::Completed);
    }
    void preemptionDuringConfiguration() {
        Conversation c;
        QJsonArray steps;
        setup(steps, 150); // First attempt must send neither go nor stop.
        completedSearch(steps, 2);
        completedSearch(steps, 200);
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy interrupted(&service, &EngineSupervisor::scheduledInterrupted);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        const auto background = request();
        service.scheduleAnalysis(c.session, background, AnalysisPriority::Maintenance);
        QTRY_VERIFY(phase(states, background.id, ScheduledPhase::Configuring));
        service.scheduleAnalysis(c.session, request(NodeLimit{2}),
                                 AnalysisPriority::InteractiveAnalysis);
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(interrupted.size(), 1);
        QVERIFY(qvariant_cast<ScheduledEvidence>(interrupted.front()[0]).evidence.lines.empty());
        QCOMPARE(result(done).identity.request, background.id);
        QCOMPARE(result(done).outcome, ScheduledOutcome::Completed);
    }
    void bestmoveWinsCancellation_data() {
        QTest::addColumn<QString>("operation");
        QTest::newRow("cancel") << QStringLiteral("cancel");
        QTest::newRow("disconnect") << QStringLiteral("disconnect");
        QTest::newRow("close") << QStringLiteral("close");
    }
    void bestmoveWinsCancellation() {
        QFETCH(QString, operation);
        Conversation c;
        QJsonArray steps;
        search(steps, QStringLiteral("go nodes 200"),
               {engine_test::write(QStringLiteral("bestmove e2e4\n"))});
        steps.append(ready(150));
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        auto active = request();
        const auto job = QUuid::createUuid();
        service.scheduleAnalysis(c.session, active, AnalysisPriority::Maintenance, job);
        QTRY_VERIFY(phase(states, active.id, ScheduledPhase::Synchronizing));
        if (operation == QStringLiteral("cancel")) {
            service.cancelScheduledAnalysis(c.session, active.id);
            service.cancelAnalysisJob(c.session, job);
        } else if (operation == QStringLiteral("disconnect")) {
            service.disconnectEngine(c.session);
        } else {
            service.close();
        }
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done).outcome, ScheduledOutcome::Completed);
        QVERIFY(result(done).evidence->evidence.bestMove);
    }
    void cancellationDuringPreemption() {
        Conversation c;
        QJsonArray steps;
        search(steps, QStringLiteral("go infinite"), {});
        steps.append(engine_test::step(QStringLiteral("stop"),
                                       {engine_test::write(QStringLiteral("bestmove e2e4\n"))}));
        steps.append(ready(150));
        completedSearch(steps, 2);
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        QSignalSpy interrupted(&service, &EngineSupervisor::scheduledInterrupted);
        auto active = request(InfiniteLimit{});
        service.scheduleAnalysis(c.session, active, AnalysisPriority::Maintenance);
        QTRY_VERIFY(phase(states, active.id, ScheduledPhase::Analyzing));
        service.scheduleAnalysis(c.session, request(NodeLimit{2}),
                                 AnalysisPriority::InteractiveAnalysis);
        QTRY_VERIFY(phase(states, active.id, ScheduledPhase::Synchronizing));
        service.cancelScheduledAnalysis(c.session, active.id);
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(result(done, 0).outcome, ScheduledOutcome::Cancelled);
        QVERIFY(interrupted.empty());
    }
    void scheduledWaitsForDirectAndInvalidDoesNotPreempt() {
        Conversation c;
        QJsonArray steps;
        stoppedSearch(steps);
        stoppedSearch(steps);
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy legacyStates(&service, &EngineSupervisor::searchChanged);
        QSignalSpy legacy(&service, &EngineSupervisor::analysisFinished);
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        auto direct = request(InfiniteLimit{});
        service.analyze(c.session, direct);
        QTRY_VERIFY(!legacyStates.empty() &&
                    qvariant_cast<SearchState>(legacyStates.back()[0]).phase ==
                        SearchPhase::Analyzing);
        auto scheduled = request(InfiniteLimit{});
        service.scheduleAnalysis(c.session, scheduled, AnalysisPriority::Maintenance);
        service.scheduleAnalysis(c.session, scheduled, AnalysisPriority::InteractiveAnalysis);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done).outcome, ScheduledOutcome::Rejected);
        service.stopAnalysis(c.session, direct.id);
        QTRY_COMPARE(legacy.size(), 1);
        QTRY_VERIFY(phase(states, scheduled.id, ScheduledPhase::Analyzing));
        auto invalid = request(NodeLimit{0});
        service.scheduleAnalysis(c.session, invalid, AnalysisPriority::InteractiveAnalysis);
        service.scheduleAnalysis(QUuid::createUuid(), request(),
                                 AnalysisPriority::InteractiveAnalysis);
        service.scheduleAnalysis(c.session, request(), AnalysisPriority::InteractiveAnalysis,
                                 QUuid{});
        QTRY_COMPARE(done.size(), 4);
        service.cancelScheduledAnalysis(c.session, scheduled.id);
        QTRY_COMPARE(done.size(), 5);
        QCOMPARE(result(done).outcome, ScheduledOutcome::Cancelled);
    }
    void failureDrainsQueue_data() {
        QTest::addColumn<QString>("failure");
        QTest::addColumn<bool>("closeDuringCleanup");
        QTest::newRow("crash") << QStringLiteral("crash") << false;
        QTest::newRow("stop-timeout") << QStringLiteral("stop-timeout") << false;
        QTest::newRow("sync-timeout") << QStringLiteral("sync-timeout") << false;
        QTest::newRow("close-after-failure") << QStringLiteral("stop-timeout") << true;
    }
    void failureDrainsQueue() {
        QFETCH(QString, failure);
        QFETCH(bool, closeDuringCleanup);
        Conversation c;
        QJsonArray steps;
        search(steps, QStringLiteral("go infinite"),
               {engine_test::write(QStringLiteral("info depth 1 score cp 5 pv e2e4\n"))});
        if (failure == QStringLiteral("crash")) {
            steps.append(engine_test::step(
                QStringLiteral("stop"),
                {QJsonObject{{QStringLiteral("type"), QStringLiteral("crash")}}}));
        } else if (failure == QStringLiteral("stop-timeout")) {
            steps.append(engine_test::step(QStringLiteral("stop"), {}));
        } else {
            steps.append(engine_test::step(
                QStringLiteral("stop"), {engine_test::write(QStringLiteral("bestmove e2e4\n"))}));
            steps.append(engine_test::step(QStringLiteral("isready"), {}));
        }
        c.connect(steps, closeDuringCleanup ? 200 : 0);
        auto& service = c.service();
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        QSignalSpy runtime(&service, &EngineSupervisor::runtimeChanged);
        auto active = request(InfiniteLimit{});
        service.scheduleAnalysis(c.session, active, AnalysisPriority::InteractiveAnalysis);
        QTRY_VERIFY(phase(states, active.id, ScheduledPhase::Analyzing));
        service.scheduleAnalysis(c.session, request(), AnalysisPriority::Maintenance);
        service.cancelScheduledAnalysis(c.session, active.id);
        if (closeDuringCleanup) {
            QTRY_VERIFY(!runtime.empty() && qvariant_cast<RuntimeState>(runtime.back()[0]).phase ==
                                                RuntimePhase::Disconnecting);
            service.close();
        }
        QTRY_COMPARE(done.size(), 2);
        for (qsizetype index = 0; index < 2; ++index) {
            QCOMPARE(result(done, index).outcome, ScheduledOutcome::Failed);
            QCOMPARE(result(done, index).error->code,
                     failure == QStringLiteral("crash") ? ErrorCode::Crash : ErrorCode::Timeout);
        }
        QTRY_VERIFY(!runtime.empty() &&
                    qvariant_cast<RuntimeState>(runtime.back()[0]).phase == RuntimePhase::Failed);
        QCOMPARE(result(done, 0).evidence->evidence.lines.size(), 1U);
        if (closeDuringCleanup) {
            return;
        }
        // An explicit reconnect starts an empty queue and clears job/request registries.
        QJsonArray reconnect;
        completedSearch(reconnect, 200);
        c.session = QUuid::createUuid();
        c.connect(reconnect);
        service.scheduleAnalysis(c.session, active = request(),
                                 AnalysisPriority::InteractiveAnalysis);
        QTRY_COMPARE(done.size(), 3);
        QCOMPARE(result(done).outcome, ScheduledOutcome::Completed);
    }
    void cancelledRestartRetainsInterruptedEvidence() {
        Conversation c;
        QJsonArray steps;
        stoppedSearch(steps, QStringLiteral("go nodes 900"));
        completedSearch(steps, 2, 50);
        setup(steps, 150); // Restart cancelled before go; old candidate remains useful.
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        QSignalSpy interrupted(&service, &EngineSupervisor::scheduledInterrupted);
        const auto background = request(NodeLimit{900});
        service.scheduleAnalysis(c.session, background, AnalysisPriority::Maintenance);
        QTRY_VERIFY(phase(states, background.id, ScheduledPhase::Analyzing));
        service.scheduleAnalysis(c.session, request(NodeLimit{2}),
                                 AnalysisPriority::InteractiveAnalysis);
        QTRY_COMPARE(done.size(), 1);
        QTRY_VERIFY(std::count_if(states.begin(), states.end(), [&](const auto& args) {
                        const auto state = qvariant_cast<ScheduledState>(args[0]);
                        return state.request == background.id &&
                               state.phase == ScheduledPhase::Configuring;
                    }) == 2);
        service.cancelScheduledAnalysis(c.session, background.id);
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(result(done).outcome, ScheduledOutcome::Cancelled);
        QCOMPARE(interrupted.size(), 1);
        const auto partial = qvariant_cast<ScheduledEvidence>(interrupted.front()[0]);
        QCOMPARE(result(done).evidence->identity.attempt, partial.identity.attempt);
        QVERIFY(result(done).identity.attempt != partial.identity.attempt);
        QCOMPARE(result(done).evidence->evidence.statistics.nodes, 10);
    }
    void boundedSessionRegistries() {
        Conversation c;
        QJsonArray steps;
        QJsonArray keepAlive;
        for (int index = 0; index < 120; ++index) {
            keepAlive.append(
                engine_test::write(QStringLiteral("info nodes %1\n").arg(index), index * 500));
        }
        search(steps, QStringLiteral("go infinite"), keepAlive);
        steps.append(engine_test::step(
            QStringLiteral("stop"),
            {QJsonObject{{QStringLiteral("type"), QStringLiteral("cancelPending")}},
             engine_test::write(QStringLiteral("bestmove e2e4\n"))}));
        steps.append(ready());
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy legacyStates(&service, &EngineSupervisor::searchChanged);
        QSignalSpy legacy(&service, &EngineSupervisor::analysisFinished);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        const auto direct = request(InfiniteLimit{});
        service.analyze(c.session, direct);
        QTRY_VERIFY(!legacyStates.empty() &&
                    qvariant_cast<SearchState>(legacyStates.back()[0]).phase ==
                        SearchPhase::Analyzing);
        // Busy rejection of the same direct ID must not release its occupied capacity.
        service.analyze(c.session, direct);
        QTRY_COMPARE(legacy.size(), 1);
        QCOMPARE(qvariant_cast<AnalysisResult>(legacy.front()[0]).outcome,
                 AnalysisOutcome::Rejected);
        AnalysisRequest first;
        for (int batch = 0; batch < 256; ++batch) {
            for (int index = 0; index < 256; ++index) {
                const auto next = request();
                if (batch == 0 && index == 0) {
                    first = next;
                }
                service.scheduleAnalysis(c.session, next, AnalysisPriority::Maintenance);
                service.cancelScheduledAnalysis(c.session, next.id);
            }
            QTRY_COMPARE(done.size(), 256);
            for (qsizetype index = 0; index < done.size(); ++index) {
                QCOMPARE(result(done, index).outcome, ScheduledOutcome::Cancelled);
            }
            done.clear();
        }
        service.scheduleAnalysis(c.session, request(), AnalysisPriority::Maintenance);
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(result(done).error->code, ErrorCode::InvalidRequest);
        service.stopAnalysis(c.session, direct.id);
        QTRY_COMPARE(legacy.size(), 2);
        QSignalSpy runtime(&service, &EngineSupervisor::runtimeChanged);
        service.disconnectEngine(c.session);
        QTRY_VERIFY(!runtime.empty() && qvariant_cast<RuntimeState>(runtime.back()[0]).phase ==
                                            RuntimePhase::Disconnected);
        QJsonArray reconnect;
        completedSearch(reconnect, 200);
        c.session = QUuid::createUuid();
        c.connect(reconnect);
        service.scheduleAnalysis(c.session, first, AnalysisPriority::InteractiveAnalysis);
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(result(done).outcome, ScheduledOutcome::Completed);
        for (int index = 0; index < 65536; ++index) {
            service.cancelAnalysisJob(c.session, QUuid::createUuid());
        }
        service.scheduleAnalysis(c.session, request(), AnalysisPriority::Maintenance);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 3, 10000);
        QCOMPARE(result(done).error->code, ErrorCode::InvalidRequest);
    }
    void stalledProgressAndEventAffinity() {
        Conversation c;
        QJsonArray steps;
        QJsonArray updates;
        for (int index = 0; index < 30; ++index) {
            updates.append(engine_test::write(
                QStringLiteral("info depth 2 nodes %1 score cp 20 pv e2e4\n").arg(index),
                index * 20));
        }
        search(steps, QStringLiteral("go infinite"), updates);
        steps.append(engine_test::step(QStringLiteral("stop"),
                                       {engine_test::write(QStringLiteral("bestmove e2e4\n"))}));
        steps.append(ready());
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy progress(&service, &EngineSupervisor::scheduledUpdated);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        bool correctThread = true;
        bool afterTerminal = false;
        QObject observer;
        QObject::connect(&service, &EngineSupervisor::scheduledUpdated, &observer,
                         [&](const ScheduledEvidence&) {
                             correctThread =
                                 correctThread && QThread::currentThread() == service.thread();
                             afterTerminal = afterTerminal || !done.empty();
                         });
        QObject::connect(
            &service, &EngineSupervisor::scheduledFinished, &observer, [&](const ScheduledResult&) {
                correctThread = correctThread && QThread::currentThread() == service.thread();
            });
        const auto active = request(InfiniteLimit{});
        service.scheduleAnalysis(c.session, active, AnalysisPriority::InteractiveAnalysis);
        QTRY_VERIFY(phase(states, active.id, ScheduledPhase::Analyzing));
        progress.clear();
        QTest::qSleep(900); // Intentionally stall the caller while the engine keeps publishing.
        QCoreApplication::sendPostedEvents(&service, QEvent::MetaCall);
        QCOMPARE(progress.size(), 1);
        QCOMPARE(qvariant_cast<ScheduledEvidence>(progress.front()[0]).evidence.statistics.nodes,
                 29);
        service.cancelScheduledAnalysis(c.session, active.id);
        QTRY_COMPARE(done.size(), 1);
        QCoreApplication::sendPostedEvents(&service, QEvent::MetaCall);
        QVERIFY(!afterTerminal);
        QVERIFY(correctThread);
        QCOMPARE(result(done).evidence->evidence.statistics.nodes, 29);
    }
    void stalledProgressAcrossQueueEvents_data() {
        QTest::addColumn<QString>("boundary");
        QTest::newRow("unrelated-queued") << QStringLiteral("queued");
        QTest::newRow("duplicate-rejected") << QStringLiteral("rejected");
        QTest::newRow("attempt-replaced") << QStringLiteral("preempted");
    }
    void stalledProgressAcrossQueueEvents() {
        QFETCH(QString, boundary);
        const bool preempted = boundary == QStringLiteral("preempted");
        Conversation c;
        QJsonArray steps;
        QJsonArray updates{engine_test::write(QStringLiteral("info nodes 1 score cp 5 pv e2e4\n"))};
        if (!preempted) {
            updates.append(
                engine_test::write(QStringLiteral("info nodes 2 score cp 6 pv e2e4\n"), 400));
        }
        search(steps, QStringLiteral("go infinite"), updates);
        steps.append(engine_test::step(QStringLiteral("stop"),
                                       {engine_test::write(QStringLiteral("bestmove e2e4\n"))}));
        steps.append(ready());
        if (preempted) {
            search(steps, QStringLiteral("go infinite"),
                   {engine_test::write(QStringLiteral("info nodes 22 score cp 7 pv d2d4\n"), 100)});
            steps.append(engine_test::step(
                QStringLiteral("stop"), {engine_test::write(QStringLiteral("bestmove d2d4\n"))}));
            steps.append(ready());
        }
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy progress(&service, &EngineSupervisor::scheduledUpdated);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        bool progressBeforeState = false;
        QObject observer;
        QObject::connect(&service, &EngineSupervisor::scheduledUpdated, &observer,
                         [&](const ScheduledEvidence& value) {
                             progressBeforeState =
                                 progressBeforeState ||
                                 !phase(states, value.identity.request, ScheduledPhase::Analyzing);
                         });
        const auto first = request(InfiniteLimit{});
        const auto second = request(InfiniteLimit{});
        service.scheduleAnalysis(c.session, first, AnalysisPriority::Maintenance);
        QTRY_VERIFY(phase(states, first.id, ScheduledPhase::Analyzing));
        progress.clear();
        QTest::qSleep(250); // One old notification is pending; keep the receiver stalled.
        if (boundary == QStringLiteral("rejected")) {
            service.scheduleAnalysis(c.session, first, AnalysisPriority::InteractiveAnalysis);
        } else {
            service.scheduleAnalysis(c.session, second,
                                     preempted ? AnalysisPriority::InteractiveAnalysis
                                               : AnalysisPriority::Maintenance);
        }
        QTest::qSleep(450); // The latest active-attempt update arrives after the boundary.
        QCoreApplication::sendPostedEvents(&service, QEvent::MetaCall);
        QCoreApplication::sendPostedEvents(&service, QEvent::MetaCall);
        QCOMPARE(progress.size(), 1);
        const auto latest = qvariant_cast<ScheduledEvidence>(progress.front()[0]);
        QCOMPARE(latest.identity.request, preempted ? second.id : first.id);
        QCOMPARE(latest.evidence.statistics.nodes, preempted ? 22 : 2);
        QVERIFY(!progressBeforeState);
        service.cancelScheduledAnalysis(c.session, preempted ? first.id : second.id);
        service.cancelScheduledAnalysis(c.session, preempted ? second.id : first.id);
        QTRY_COMPARE(done.size(), 2);
    }
    void terminalRestartCompletionUsesCurrentEvidence_data() {
        QTest::addColumn<bool>("close");
        QTest::newRow("disconnect") << false;
        QTest::newRow("close") << true;
    }
    void terminalRestartCompletionUsesCurrentEvidence() {
        QFETCH(bool, close);
        Conversation c;
        QJsonArray steps;
        const auto fen = QStringLiteral("7k/5K2/6Q1/8/8/8/8/8 b - - 0 1");
        const auto position = QStringLiteral("position fen ") + fen;
        setup(steps);
        steps.append(engine_test::step(position, {}));
        steps.append(engine_test::step(QStringLiteral("go nodes 900"),
                                       {engine_test::write(QStringLiteral("info nodes 10\n"))}));
        steps.append(engine_test::step(QStringLiteral("stop"),
                                       {engine_test::write(QStringLiteral("bestmove 0000\n"))}));
        steps.append(ready());
        completedSearch(steps, 2, 50);
        setup(steps);
        steps.append(engine_test::step(position, {}));
        steps.append(engine_test::step(
            QStringLiteral("go nodes 900"),
            {engine_test::write(QStringLiteral("info nodes 900\nbestmove 0000\n"))}));
        steps.append(ready(200));
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        QSignalSpy interrupted(&service, &EngineSupervisor::scheduledInterrupted);
        auto background = request(NodeLimit{900});
        background.position = std::get<chess_review::chess::Position>(
            chess_review::chess::Position::fromFen(fen.toStdString()));
        service.scheduleAnalysis(c.session, background, AnalysisPriority::Maintenance);
        QTRY_VERIFY(phase(states, background.id, ScheduledPhase::Analyzing));
        service.scheduleAnalysis(c.session, request(NodeLimit{2}),
                                 AnalysisPriority::InteractiveAnalysis);
        QTRY_VERIFY(std::count_if(states.begin(), states.end(), [&](const auto& args) {
                        const auto state = qvariant_cast<ScheduledState>(args[0]);
                        return state.request == background.id &&
                               state.phase == ScheduledPhase::Synchronizing;
                    }) == 2);
        if (close) {
            service.close();
        } else {
            service.disconnectEngine(c.session);
        }
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(interrupted.size(), 1);
        const auto final = result(done);
        QCOMPARE(final.identity.request, background.id);
        QCOMPARE(final.outcome, ScheduledOutcome::Completed);
        QVERIFY(final.evidence);
        QCOMPARE(final.evidence->identity.attempt, final.identity.attempt);
        QCOMPARE(final.evidence->evidence.statistics.nodes, 900);
        QVERIFY(final.evidence->evidence.lines.empty());
        QVERIFY(!final.evidence->evidence.bestMove);
    }
    void longHistoryQueueCancellation() {
        Conversation c;
        QJsonArray steps;
        stoppedSearch(steps);
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        const auto active = request(InfiniteLimit{});
        service.scheduleAnalysis(c.session, active, AnalysisPriority::InteractiveAnalysis);
        QTRY_VERIFY(phase(states, active.id, ScheduledPhase::Analyzing));
        auto queued = request();
        for (int cycle = 0; cycle < 3000; ++cycle) {
            for (const auto* uci : {"g1f3", "g8f6", "f3g1", "f6g8"}) {
                const auto move =
                    std::get<chess_review::chess::Move>(queued.position.parseUci(uci));
                QVERIFY(std::holds_alternative<std::monostate>(queued.position.play(move)));
            }
        }
        QElapsedTimer elapsed;
        elapsed.start();
        int ticks = 0;
        QTimer heartbeat;
        heartbeat.setInterval(5);
        QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++ticks; });
        heartbeat.start();
        const auto job = QUuid::createUuid();
        for (int index = 0; index < 16; ++index) {
            queued.id = QUuid::createUuid();
            service.scheduleAnalysis(c.session, queued, AnalysisPriority::Maintenance, job);
        }
        service.cancelAnalysisJob(c.session, job);
        service.cancelScheduledAnalysis(c.session, active.id);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 17, 10000);
        for (qsizetype index = 0; index < done.size(); ++index) {
            QCOMPARE(result(done, index).outcome, ScheduledOutcome::Cancelled);
        }
        QVERIFY(ticks > 0);
        qInfo("Queue/cancel sixteen 12,000-ply requests: %lld ms, %d caller heartbeats",
              static_cast<long long>(elapsed.elapsed()), ticks);
    }
    void shutdownDrainsQueue_data() {
        QTest::addColumn<bool>("close");
        QTest::newRow("disconnect") << false;
        QTest::newRow("close") << true;
    }
    void shutdownDrainsQueue() {
        QFETCH(bool, close);
        Conversation c;
        QJsonArray steps;
        stoppedSearch(steps);
        c.connect(steps);
        auto& service = c.service();
        QSignalSpy states(&service, &EngineSupervisor::scheduledChanged);
        QSignalSpy done(&service, &EngineSupervisor::scheduledFinished);
        QSignalSpy closed(&service, &EngineSupervisor::closed);
        QSignalSpy runtime(&service, &EngineSupervisor::runtimeChanged);
        auto active = request(InfiniteLimit{});
        service.scheduleAnalysis(c.session, active, AnalysisPriority::InteractiveAnalysis);
        QTRY_VERIFY(phase(states, active.id, ScheduledPhase::Analyzing));
        service.scheduleAnalysis(c.session, request(), AnalysisPriority::Maintenance);
        bool ordered = false;
        QObject observer;
        QObject::connect(&service, &EngineSupervisor::closed, &observer,
                         [&] { ordered = done.size() == 2; });
        if (close) {
            service.close();
            service.close();
        } else {
            service.disconnectEngine(c.session);
        }
        QTRY_COMPARE(done.size(), 2);
        QCOMPARE(result(done, 0).outcome, ScheduledOutcome::Cancelled);
        QCOMPARE(result(done, 1).outcome, ScheduledOutcome::Cancelled);
        if (close) {
            QTRY_COMPARE(closed.size(), 1);
            QVERIFY(ordered);
        } else {
            QTRY_VERIFY(!runtime.empty() && qvariant_cast<RuntimeState>(runtime.back()[0]).phase ==
                                                RuntimePhase::Disconnected);
        }
    }
};
int main(int argc, char** argv) {
    TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    EngineSchedulingTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "EngineSchedulingTest.moc"
