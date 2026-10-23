#include "engine/private/EngineProcessWorker.h"
#include "EngineTestSupport.h"
#include "TestEnvironment.h"
#include "engine/private/SupervisorCoordinator.h"
#include <QElapsedTimer>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <array>
#include <utility>

using namespace chess_review::engine;
namespace {
using Stage = ProcessStage;

class Attempt final {
  public:
    Attempt(AttemptMode mode, const QString& scenario, int responseMs = 5000, int idleMs = 10000,
            qsizetype outputLimit = 1024 * 1024)
        : worker(mode, {{QStringLiteral("--scenario"), scenario},
                        1000,
                        1000,
                        outputLimit,
                        idleMs,
                        responseMs}) {
        QObject::connect(&worker, &EngineProcessWorker::finished, &worker,
                         [this] { finished = true; });
    }
    ~Attempt() {
        if (finished) {
            return;
        }
        QEventLoop loop;
        QObject::connect(&worker, &EngineProcessWorker::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(6000, &loop, &QEventLoop::quit);
        worker.close();
        if (!finished) {
            loop.exec();
        }
        if (!finished) {
            qFatal("Process worker did not finish");
        }
    }
    EngineProcessWorker worker;
    bool finished = false;
};

QList<Stage> stages(const QSignalSpy& events) {
    QList<Stage> result;
    for (const auto& event : events) {
        result.append(qvariant_cast<Stage>(event.front()));
    }
    return result;
}

EngineProfile profileFor(EngineSupervisor& supervisor, const QString& scenario) {
    ValidatorTestAccess::configure(supervisor, scenario);
    QSignalSpy completed(&supervisor, &EngineSupervisor::completed);
    supervisor.validate(QUuid::createUuid(),
                        QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath());
    if (!completed.wait(5000)) {
        qFatal("Profile fixture timed out");
    }
    const auto result = qvariant_cast<ValidationResult>(completed.front()[1]);
    if (!std::holds_alternative<ValidatedEngine>(result)) {
        qFatal("Profile fixture failed");
    }
    EngineProfile profile;
    profile.id = 1;
    profile.engine = std::get<ValidatedEngine>(result);
    return profile;
}
} // namespace

class EngineProcessWorkerTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { QTest::failOnWarning(); }

    void transitionRules() {
        // Independent edge list makes accidentally permitting a backwards/duplicate edge visible.
        constexpr std::array edges{std::pair{Stage::Created, Stage::CheckingExecutable},
                                   std::pair{Stage::CheckingExecutable, Stage::Starting},
                                   std::pair{Stage::Starting, Stage::AwaitingUciOk},
                                   std::pair{Stage::AwaitingUciOk, Stage::AwaitingReadyOk},
                                   std::pair{Stage::AwaitingReadyOk, Stage::CheckingIntegrity},
                                   std::pair{Stage::CheckingIntegrity, Stage::Ready},
                                   std::pair{Stage::Starting, Stage::Quitting},
                                   std::pair{Stage::AwaitingUciOk, Stage::Quitting},
                                   std::pair{Stage::AwaitingReadyOk, Stage::Quitting},
                                   std::pair{Stage::CheckingIntegrity, Stage::Quitting},
                                   std::pair{Stage::Ready, Stage::Quitting},
                                   std::pair{Stage::Ready, Stage::ConfiguringSearch},
                                   std::pair{Stage::ConfiguringSearch, Stage::Analyzing},
                                   std::pair{Stage::ConfiguringSearch, Stage::SynchronizingSearch},
                                   std::pair{Stage::Analyzing, Stage::Stopping},
                                   std::pair{Stage::Analyzing, Stage::FailureStopping},
                                   std::pair{Stage::FailureStopping, Stage::Quitting},
                                   std::pair{Stage::Analyzing, Stage::SynchronizingSearch},
                                   std::pair{Stage::Stopping, Stage::SynchronizingSearch},
                                   std::pair{Stage::SynchronizingSearch, Stage::Ready},
                                   std::pair{Stage::ConfiguringSearch, Stage::Quitting},
                                   std::pair{Stage::Analyzing, Stage::Quitting},
                                   std::pair{Stage::Stopping, Stage::Quitting},
                                   std::pair{Stage::SynchronizingSearch, Stage::Quitting},
                                   std::pair{Stage::Quitting, Stage::Terminating},
                                   std::pair{Stage::Quitting, Stage::CheckingIntegrity},
                                   std::pair{Stage::Terminating, Stage::Killing}};
        constexpr std::array all{
            Stage::Created,       Stage::CheckingExecutable,  Stage::Starting,
            Stage::AwaitingUciOk, Stage::AwaitingReadyOk,     Stage::CheckingIntegrity,
            Stage::Ready,         Stage::ConfiguringSearch,   Stage::Analyzing,
            Stage::Stopping,      Stage::SynchronizingSearch, Stage::FailureStopping,
            Stage::Quitting,      Stage::Terminating,         Stage::Killing,
            Stage::Finished};
        for (const auto current : all) {
            for (const auto next : all) {
                const bool expected =
                    (current != Stage::Finished && next == Stage::Finished) ||
                    std::ranges::find(edges, std::pair{current, next}) != edges.end();
                QCOMPARE(canTransition(current, next), expected);
            }
        }
    }

    void directSearchCancellation_data() {
        QTest::addColumn<bool>("configuring");
        QTest::newRow("before-go") << true;
        QTest::newRow("after-go") << false;
    }
    void directSearchCancellation() {
        QFETCH(bool, configuring);
        QTemporaryDir directory;
        engine_test::Validator registration;
        const auto profile =
            profileFor(registration.service,
                       engine_test::scenario(directory.filePath(QStringLiteral("register.json")),
                                             engine_test::handshake()));
        auto steps = engine_test::handshake();
        const auto quitStep = steps.takeAt(steps.size() - 1);
        steps.append(engine_test::step(QStringLiteral("ucinewgame"), {}));
        steps.append(engine_test::step(QStringLiteral("isready"),
                                       {engine_test::write(QStringLiteral("readyok\n"))}));
        if (!configuring) {
            steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
            steps.append(engine_test::step(QStringLiteral("go infinite"), {}));
            steps.append(engine_test::step(
                QStringLiteral("stop"), {engine_test::write(QStringLiteral("bestmove e2e4\n"))}));
            steps.append(engine_test::step(QStringLiteral("isready"),
                                           {engine_test::write(QStringLiteral("readyok\n"))}));
        }
        steps.append(quitStep);
        Attempt attempt(
            AttemptMode::Runtime,
            engine_test::scenario(directory.filePath(QStringLiteral("search.json")), steps));
        QSignalSpy runtime(&attempt.worker, &EngineProcessWorker::runtimeChanged);
        QSignalSpy done(&attempt.worker, &EngineProcessWorker::analysisFinished);
        const auto session = QUuid::createUuid();
        attempt.worker.connectEngine(session, profile);
        QTRY_VERIFY(!runtime.empty() &&
                    qvariant_cast<RuntimeState>(runtime.back()[0]).phase == RuntimePhase::Ready);
        connect(&attempt.worker, &EngineProcessWorker::searchChanged, &attempt.worker,
                [&attempt, configuring](const SearchState& state) {
                    if (state.phase ==
                        (configuring ? SearchPhase::Configuring : SearchPhase::Analyzing)) {
                        attempt.worker.close();
                    }
                });
        AnalysisRequest search;
        search.id = QUuid::createUuid();
        search.limit = InfiniteLimit{};
        attempt.worker.analyze(session, search);
        QTRY_VERIFY(attempt.finished);
        QCOMPARE(done.size(), 1);
        QCOMPARE(qvariant_cast<AnalysisResult>(done.front()[0]).outcome, AnalysisOutcome::Stopped);
        QCOMPARE(qvariant_cast<RuntimeState>(runtime.back()[0]).phase, RuntimePhase::Disconnected);
    }

    void timeoutRecovery_data() {
        QTest::addColumn<QString>("reply");
        QTest::addColumn<bool>("closeDuringRecovery");
        for (const auto* kind : {"valid", "silent", "bad-bestmove", "bad-control", "crash", "flood",
                                 "terminate", "kill"}) {
            QTest::newRow(kind) << QString::fromLatin1(kind) << false;
        }
        QTest::newRow("probe-late-ready") << QStringLiteral("probe") << false;
        QTest::newRow("stderr-budget") << QStringLiteral("stderr-budget") << false;
        QTest::newRow("nul") << QStringLiteral("nul") << false;
        QTest::newRow("close-during-grace") << QStringLiteral("valid") << true;
    }
    void timeoutRecovery() {
        QFETCH(QString, reply);
        QFETCH(bool, closeDuringRecovery);
        QTemporaryDir directory;
        engine_test::Validator registration;
        const auto profile =
            profileFor(registration.service,
                       engine_test::scenario(directory.filePath(QStringLiteral("register.json")),
                                             engine_test::handshake()));
        auto steps = engine_test::handshake();
        auto quitStep = steps.takeAt(steps.size() - 1);
        steps.append(engine_test::step(QStringLiteral("ucinewgame"), {}));
        steps.append(engine_test::step(QStringLiteral("isready"),
                                       {engine_test::write(QStringLiteral("readyok\n"))}));
        steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
        steps.append(engine_test::step(
            reply == QStringLiteral("probe") ? QStringLiteral("go infinite")
                                             : QStringLiteral("go movetime 1"),
            {engine_test::write(QStringLiteral("info depth 2 score cp 7 pv e2e4\n"))}));
        QJsonArray actions;
        if (reply == QStringLiteral("probe")) {
            steps.append(engine_test::step(QStringLiteral("isready"), {}));
            actions.append(engine_test::write(QStringLiteral("readyok\n")));
        }
        if (reply == QStringLiteral("valid") || reply == QStringLiteral("probe")) {
            actions.append(engine_test::write(
                QStringLiteral("info depth 99 score cp 999 pv d2d4\nbestmove d2d4\n"), 50));
        } else if (reply == QStringLiteral("bad-bestmove")) {
            actions.append(engine_test::write(QStringLiteral("bestmove e2e5\n")));
        } else if (reply == QStringLiteral("bad-control")) {
            actions.append(engine_test::write(QStringLiteral("readyok extra\n")));
        } else if (reply == QStringLiteral("crash")) {
            actions.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("crash")}});
        } else if (reply == QStringLiteral("nul")) {
            actions.append(
                engine_test::write(QStringLiteral("info string ") + QChar::Null + u'\n'));
        } else if (reply == QStringLiteral("stderr-budget")) {
            auto action = engine_test::write(QString(8192, u'x'));
            action[QStringLiteral("stream")] = QStringLiteral("stderr");
            actions.append(action);
        } else if (reply == QStringLiteral("flood")) {
            actions.append(engine_test::write(QString(65537, u'x')));
        }
        steps.append(engine_test::step(QStringLiteral("stop"), actions));
        if (reply == QStringLiteral("terminate") || reply == QStringLiteral("kill")) {
            quitStep = engine_test::step(
                QStringLiteral("quit"),
                {QJsonObject{{QStringLiteral("type"), reply == QStringLiteral("kill")
                                                          ? QStringLiteral("stubbornHang")
                                                          : QStringLiteral("hang")}}});
        }
        steps.append(quitStep);
        Attempt attempt(
            AttemptMode::Runtime,
            engine_test::scenario(directory.filePath(QStringLiteral("search.json")), steps), 200,
            reply == QStringLiteral("probe") ? 50 : 10000,
            reply == QStringLiteral("stderr-budget") ? 4096 : 1024 * 1024);
        QSignalSpy runtime(&attempt.worker, &EngineProcessWorker::runtimeChanged);
        QSignalSpy done(&attempt.worker, &EngineProcessWorker::analysisFinished);
        QSignalSpy lifecycle(&attempt.worker, &EngineProcessWorker::stateChanged);
        auto* process = attempt.worker.findChild<QProcess*>();
        QVERIFY(process);
        const auto session = QUuid::createUuid();
        attempt.worker.connectEngine(session, profile);
        QTRY_VERIFY(!runtime.empty() &&
                    qvariant_cast<RuntimeState>(runtime.back()[0]).phase == RuntimePhase::Ready);
        int ticks = 0;
        QTimer heartbeat;
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, &attempt.worker, [&] { ++ticks; });
        heartbeat.start();
        QElapsedTimer grace;
        qint64 graceMs = -1;
        connect(&attempt.worker, &EngineProcessWorker::stateChanged, &attempt.worker,
                [&](Stage stage) {
                    if (stage == Stage::FailureStopping) {
                        grace.start();
                        if (closeDuringRecovery) {
                            attempt.worker.close();
                            attempt.worker.close();
                        }
                    } else if (stage == Stage::Quitting && grace.isValid()) {
                        graceMs = grace.elapsed();
                    }
                });
        AnalysisRequest search;
        search.id = QUuid::createUuid();
        search.limit = reply == QStringLiteral("probe") ? SearchLimit{InfiniteLimit{}}
                                                        : SearchLimit{TimeLimit{1}};
        attempt.worker.analyze(session, search);
        QTRY_VERIFY_WITH_TIMEOUT(attempt.finished, 6000);
        QCOMPARE(done.size(), 1);
        const auto failed = qvariant_cast<AnalysisResult>(done.front()[0]);
        QCOMPARE(failed.outcome, AnalysisOutcome::Failed);
        QCOMPARE(failed.error->code, ErrorCode::Timeout);
        QCOMPARE(failed.error->stage, reply == QStringLiteral("probe")
                                          ? FailureStage::ReadinessProbe
                                          : FailureStage::TimedSearchCompletion);
        QCOMPARE(failed.evidence.lines.size(), std::size_t(1));
        QCOMPARE(failed.evidence.lines.front().score->value, 7);
        QCOMPARE(failed.evidence.statistics.depth, 2);
        QVERIFY(!failed.evidence.bestMove);
        QVERIFY(!failed.evidence.ponder);
        const auto terminal = qvariant_cast<RuntimeState>(runtime.back()[0]);
        QCOMPARE(terminal.phase, RuntimePhase::Failed);
        QCOMPARE(terminal.error->stage, failed.error->stage);
        QCOMPARE(terminal.error->message, failed.error->message);
        const auto observed = stages(lifecycle);
        QCOMPARE(observed.count(Stage::FailureStopping), 1);
        if (reply == QStringLiteral("crash")) {
            // Abort notification may arrive before or after grace expires.
            QVERIFY(observed.count(Stage::Quitting) <= 1);
        } else {
            QCOMPARE(observed.count(Stage::Quitting), 1);
        }
        QCOMPARE(observed.count(Stage::Finished), 1);
        QVERIFY(ticks >= 5);
        if (reply == QStringLiteral("silent") || reply == QStringLiteral("terminate") ||
            reply == QStringLiteral("kill")) {
            QVERIFY(graceMs >= 200);
        }
        if (reply == QStringLiteral("valid") || reply == QStringLiteral("probe")) {
            // A scripted reply must end grace before its deadline, after the scheduled delay.
            QVERIFY(graceMs >= 40);
            QVERIFY(graceMs < 200);
        }
        if (reply == QStringLiteral("bad-bestmove") || reply == QStringLiteral("bad-control") ||
            reply == QStringLiteral("flood") || reply == QStringLiteral("stderr-budget") ||
            reply == QStringLiteral("nul")) {
            QVERIFY(graceMs < 200);
        }
        if (reply == QStringLiteral("terminate") || reply == QStringLiteral("kill")) {
            QVERIFY(observed.contains(Stage::Terminating));
            QCOMPARE(observed.contains(Stage::Killing), reply == QStringLiteral("kill"));
        } else if (reply != QStringLiteral("crash")) {
            // Zero means every scripted command (stop then quit) matched, without forced cleanup.
            QCOMPARE(process->exitStatus(), QProcess::NormalExit);
            QCOMPARE(process->exitCode(), 0);
            QVERIFY(!observed.contains(Stage::Terminating));
        }
    }

    void registrationSequence() {
        QTemporaryDir directory;
        const auto script = engine_test::scenario(directory.filePath(QStringLiteral("worker.json")),
                                                  engine_test::handshake());
        Attempt attempt(AttemptMode::Registration, script);
        QSignalSpy states(&attempt.worker, &EngineProcessWorker::stateChanged);
        QSignalSpy completed(&attempt.worker, &EngineProcessWorker::completed);
        QSignalSpy finished(&attempt.worker, &EngineProcessWorker::finished);
        QVERIFY(states.isEmpty()); // Construction launches nothing.
        attempt.worker.validate(QUuid::createUuid(),
                                QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath());
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(completed.size(), 1);
        QVERIFY(std::holds_alternative<ValidatedEngine>(
            qvariant_cast<ValidationResult>(completed.front()[1])));
        QCOMPARE(stages(states),
                 (QList<Stage>{Stage::CheckingExecutable, Stage::Starting, Stage::AwaitingUciOk,
                               Stage::AwaitingReadyOk, Stage::Quitting, Stage::CheckingIntegrity,
                               Stage::Finished}));
        attempt.worker.cancel();
        attempt.worker.close();
        QCoreApplication::sendPostedEvents();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(completed.size(), 1);
    }

    void closeAtBoundary_data() {
        QTest::addColumn<Stage>("boundary");
        QTest::addColumn<bool>("runtime");
        for (const auto mode : {false, true}) {
            const auto prefix = mode ? QByteArray("runtime-") : QByteArray("registration-");
            for (const auto stage :
                 {Stage::CheckingExecutable, Stage::Starting, Stage::AwaitingUciOk,
                  Stage::AwaitingReadyOk, Stage::CheckingIntegrity}) {
                QTest::newRow((prefix + QByteArray::number(static_cast<int>(stage))).constData())
                    << stage << mode;
            }
        }
        QTest::newRow("runtime-ready") << Stage::Ready << true;
        QTest::newRow("runtime-quitting") << Stage::Quitting << true;
        QTest::newRow("registration-quitting") << Stage::Quitting << false;
        QTest::newRow("runtime-terminating") << Stage::Terminating << true;
        QTest::newRow("runtime-killing") << Stage::Killing << true;
    }

    void closeAtBoundary() {
        QFETCH(Stage, boundary);
        QFETCH(bool, runtime);
        QTemporaryDir directory;
        engine_test::Validator service;
        const auto registration = engine_test::scenario(
            directory.filePath(QStringLiteral("registration.json")), engine_test::handshake());
        const auto profile = profileFor(service.service, registration);
        auto steps = engine_test::handshake();
        const bool forced = boundary == Stage::Terminating || boundary == Stage::Killing;
        if (forced) {
            steps[2] = engine_test::step(
                QStringLiteral("quit"),
                {QJsonObject{{QStringLiteral("type"), QStringLiteral("stubbornHang")}}});
        }
        // State hooks run synchronously, before the next protocol command is sent.
        if (boundary == Stage::AwaitingUciOk) {
            steps.removeAt(0);
            steps.removeAt(0);
        } else if (boundary == Stage::AwaitingReadyOk) {
            steps.removeAt(1);
        }
        const auto script =
            engine_test::scenario(directory.filePath(QStringLiteral("worker.json")), steps);
        Attempt attempt(runtime ? AttemptMode::Runtime : AttemptMode::Registration, script);
        QSignalSpy completed(&attempt.worker, &EngineProcessWorker::completed);
        QSignalSpy events(&attempt.worker, &EngineProcessWorker::runtimeChanged);
        QSignalSpy finished(&attempt.worker, &EngineProcessWorker::finished);
        bool reached = false;
        connect(&attempt.worker, &EngineProcessWorker::stateChanged, &attempt.worker,
                [&](Stage state) {
                    if (state == boundary) {
                        reached = true;
                        attempt.worker.close();
                        attempt.worker.close();
                        attempt.worker
                            .cancel(); // Registration-only; must not affect a runtime attempt.
                    }
                });
        if (runtime) {
            attempt.worker.connectEngine(QUuid::createUuid(), profile);
            if (boundary == Stage::Quitting || forced) {
                QTRY_VERIFY(!events.isEmpty() &&
                            qvariant_cast<RuntimeState>(events.back()[0]).phase ==
                                RuntimePhase::Ready);
                attempt.worker.close();
            }
        } else {
            attempt.worker.validate(QUuid::createUuid(), profile.engine.executablePath);
        }
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(reached);
        if (runtime) {
            QCOMPARE(completed.size(), 0);
            const auto terminal = qvariant_cast<RuntimeState>(events.back()[0]);
            QCOMPARE(terminal.phase, forced ? RuntimePhase::Failed : RuntimePhase::Disconnected);
            if (forced) {
                QVERIFY(terminal.error);
                QCOMPARE(terminal.error->code, ErrorCode::Cleanup);
            } else {
                QVERIFY(!terminal.error);
            }
            QVERIFY(!terminal.discovery);
        } else {
            QCOMPARE(completed.size(), 1);
            QCOMPARE(std::get<Error>(qvariant_cast<ValidationResult>(completed.front()[1])).code,
                     ErrorCode::Cancelled);
        }
        attempt.worker.close();
        QCoreApplication::sendPostedEvents();
        QCOMPARE(finished.size(), 1);
    }
    void admissionUntilWorkerDestroyed() {
        QTemporaryDir directory;
        const auto script = engine_test::scenario(directory.filePath(QStringLiteral("worker.json")),
                                                  engine_test::handshake());
        SupervisorCoordinator coordinator;
        coordinator.configure({QStringLiteral("--scenario"), script}, 1000, 1000, 1024 * 1024);
        const auto path = QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath();
        const auto first = QUuid::createUuid();
        const auto competing = QUuid::createUuid();
        QSignalSpy completed(&coordinator, &SupervisorCoordinator::completed);
        coordinator.validate(first, path);
        QPointer<EngineProcessWorker> worker = coordinator.findChild<EngineProcessWorker*>();
        QVERIFY(worker);
        bool retiredBeforeCompletion = false;
        connect(&coordinator, &SupervisorCoordinator::completed, &coordinator,
                [&](QUuid request, const ValidationResult&) {
                    if (request == first) {
                        retiredBeforeCompletion = worker.isNull();
                    }
                });
        connect(worker, &EngineProcessWorker::finished, &coordinator, [&] {
            // Worker has completed but its deferred destruction has not run yet.
            coordinator.validate(competing, path);
        });
        QTRY_COMPARE(completed.size(), 2);
        QCOMPARE(completed[0][0].toUuid(), competing);
        QCOMPARE(std::get<Error>(qvariant_cast<ValidationResult>(completed[0][1])).code,
                 ErrorCode::Busy);
        QCOMPARE(completed[1][0].toUuid(), first);
        QVERIFY(std::holds_alternative<ValidatedEngine>(
            qvariant_cast<ValidationResult>(completed[1][1])));
        QVERIFY(retiredBeforeCompletion);
        QVERIFY(worker.isNull());

        // A failed attempt and a subsequent healthy one each own a fresh worker.
        coordinator.validate(QUuid::createUuid(), QString());
        QPointer<EngineProcessWorker> failed = coordinator.findChild<EngineProcessWorker*>();
        QVERIFY(failed);
        QTRY_COMPARE(completed.size(), 3);
        QVERIFY(failed.isNull());
        QCOMPARE(std::get<Error>(qvariant_cast<ValidationResult>(completed[2][1])).code,
                 ErrorCode::Path);
        coordinator.validate(QUuid::createUuid(), path);
        QPointer<EngineProcessWorker> next = coordinator.findChild<EngineProcessWorker*>();
        QVERIFY(next);
        QTRY_COMPARE(completed.size(), 4);
        QVERIFY(next.isNull());
        QVERIFY(std::holds_alternative<ValidatedEngine>(
            qvariant_cast<ValidationResult>(completed[3][1])));
    }

    void finalOutputDrainedBeforeCompletion() {
        QTemporaryDir directory;
        auto steps = engine_test::handshake();
        auto finalOutput = engine_test::write(QString(130000, u'x'));
        finalOutput[QStringLiteral("stream")] = QStringLiteral("stderr");
        steps[2] = engine_test::step(
            QStringLiteral("quit"),
            {finalOutput, QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}});
        const auto script =
            engine_test::scenario(directory.filePath(QStringLiteral("worker.json")), steps);
        Attempt attempt(AttemptMode::Registration, script);
        QSignalSpy completed(&attempt.worker, &EngineProcessWorker::completed);
        bool drained = false;
        connect(&attempt.worker, &EngineProcessWorker::finished, &attempt.worker, [&] {
            auto* process = attempt.worker.findChild<QProcess*>();
            if (!process || process->state() != QProcess::NotRunning) {
                return;
            }
            process->setReadChannel(QProcess::StandardOutput);
            const bool stdoutEmpty = process->bytesAvailable() == 0;
            process->setReadChannel(QProcess::StandardError);
            drained = stdoutEmpty && process->bytesAvailable() == 0;
        });
        attempt.worker.validate(QUuid::createUuid(),
                                QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath());
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(std::holds_alternative<ValidatedEngine>(
            qvariant_cast<ValidationResult>(completed.front()[1])));
        QVERIFY(drained);
    }
    void failedLaunchCompletesOnce() {
#ifndef Q_OS_UNIX
        QSKIP("The deterministic missing-interpreter fixture uses Unix executable semantics.");
#else
        QTemporaryDir directory;
        QFile file(directory.filePath(QStringLiteral("missing-interpreter-engine")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto contents =
            QByteArray("#!") + directory.filePath(QStringLiteral("absent")).toUtf8() + '\n';
        QCOMPARE(file.write(contents), contents.size());
        file.close();
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner));
        Attempt attempt(AttemptMode::Registration, QString());
        QSignalSpy completed(&attempt.worker, &EngineProcessWorker::completed);
        QSignalSpy finished(&attempt.worker, &EngineProcessWorker::finished);
        QSignalSpy states(&attempt.worker, &EngineProcessWorker::stateChanged);
        attempt.worker.validate(QUuid::createUuid(), QFileInfo(file).canonicalFilePath());
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(completed.size(), 1);
        QCOMPARE(std::get<Error>(qvariant_cast<ValidationResult>(completed.front()[1])).code,
                 ErrorCode::Start);
        QCOMPARE(stages(states),
                 (QList<Stage>{Stage::CheckingExecutable, Stage::Starting, Stage::Finished}));
        attempt.worker.close();
        QCOMPARE(finished.size(), 1);
#endif
    }

    void reconnectAfterTerminal_data() {
        QTest::addColumn<QString>("outcome");
        for (const auto* outcome : {"clean", "cancel", "protocol", "forced", "protocol-forced"}) {
            QTest::newRow(outcome) << QString::fromLatin1(outcome);
        }
    }

    void reconnectAfterTerminal() {
        QFETCH(QString, outcome);
        QTemporaryDir directory;
        engine_test::Validator service;
        const auto registration = engine_test::scenario(
            directory.filePath(QStringLiteral("registration.json")), engine_test::handshake());
        const auto profile = profileFor(service.service, registration);
        auto steps = engine_test::handshake();
        const bool protocol = outcome.startsWith(QStringLiteral("protocol"));
        const bool forced = outcome.endsWith(QStringLiteral("forced"));
        if (protocol || outcome == QStringLiteral("cancel")) {
            steps[0] = engine_test::step(
                QStringLiteral("uci"),
                protocol ? QJsonArray{engine_test::write(QStringLiteral("bestmove e2e4\n"))}
                         : QJsonArray{});
            steps.removeAt(1);
        }
        if (forced) {
            steps[steps.size() - 1] = engine_test::step(
                QStringLiteral("quit"),
                {QJsonObject{{QStringLiteral("type"), QStringLiteral("stubbornHang")}}});
        }
        const auto script =
            engine_test::scenario(directory.filePath(QStringLiteral("worker.json")), steps);
        ValidatorTestAccess::configure(service.service, script, 5000);
        QSignalSpy events(&service.service, &EngineSupervisor::runtimeChanged);
        const auto oldSession = QUuid::createUuid();
        service.service.connectEngine(oldSession, profile);
        auto latest = [&] {
            return events.isEmpty() ? RuntimeState{}
                                    : qvariant_cast<RuntimeState>(events.back()[0]);
        };
        if (!protocol) {
            QTRY_COMPARE(latest().phase, outcome == QStringLiteral("cancel")
                                             ? RuntimePhase::Handshaking
                                             : RuntimePhase::Ready);
            service.service.disconnectEngine(oldSession);
            service.service.disconnectEngine(oldSession);
        }
        QTRY_COMPARE_WITH_TIMEOUT(
            latest().phase, protocol || forced ? RuntimePhase::Failed : RuntimePhase::Disconnected,
            6000);
        if (protocol || forced) {
            QVERIFY(latest().error);
            QCOMPARE(latest().error->code, protocol ? ErrorCode::Protocol : ErrorCode::Cleanup);
        }
        const auto terminalIndex = events.size();
        ValidatorTestAccess::configure(service.service, registration, 5000);
        const auto nextSession = QUuid::createUuid();
        service.service.connectEngine(nextSession, profile);
        QTRY_COMPARE(latest().phase, RuntimePhase::Ready);
        service.service.disconnectEngine(
            oldSession);          // A retired session cannot stop the replacement.
        service.service.cancel(); // Registration cancellation cannot stop runtime work.
        service.service.disconnectEngine(nextSession);
        QTRY_COMPARE(latest().phase, RuntimePhase::Disconnected);
        QVERIFY(!latest().error);
        int oldTerminals = 0;
        int newTerminals = 0;
        for (qsizetype i = 0; i < events.size(); ++i) {
            const auto state = qvariant_cast<RuntimeState>(events[i][0]);
            if (i >= terminalIndex) {
                QCOMPARE(state.session, nextSession);
            }
            if (!runtimeActive(state.phase)) {
                if (state.session == oldSession) {
                    ++oldTerminals;
                } else if (state.session == nextSession) {
                    ++newTerminals;
                }
            }
        }
        QCOMPARE(oldTerminals, 1);
        QCOMPARE(newTerminals, 1);
    }
    void serviceClosesOnceAfterTerminal() {
        QTemporaryDir directory;
        EngineSupervisor service;
        const auto script = engine_test::scenario(directory.filePath(QStringLiteral("worker.json")),
                                                  engine_test::handshake());
        const auto profile = profileFor(service, script);
        ValidatorTestAccess::configure(service, script, 1000);
        QSignalSpy closed(&service, &EngineSupervisor::closed);
        QSignalSpy events(&service, &EngineSupervisor::runtimeChanged);
        connect(&service, &EngineSupervisor::runtimeChanged, &service,
                [&](const RuntimeState& state) {
                    if (state.phase == RuntimePhase::Ready) {
                        service.close();
                        service.close();
                    }
                });
        service.connectEngine(QUuid::createUuid(), profile);
        if (!closed.wait(6000)) {
            qFatal("Supervisor did not join its thread");
        }
        QCOMPARE(closed.size(), 1);
        QVERIFY(!events.isEmpty());
        QCOMPARE(qvariant_cast<RuntimeState>(events.back()[0]).phase, RuntimePhase::Disconnected);
        service.close();
        QCoreApplication::sendPostedEvents();
        QCOMPARE(closed.size(), 1);
    }
    void closeFromNotification_data() {
        QTest::addColumn<QString>("notification");
        QTest::addColumn<bool>("runtime");
        for (const auto* name : {"checking", "starting", "handshaking", "ready", "hash-progress",
                                 "start-progress", "readiness-progress", "integrity-progress"}) {
            QTest::newRow(name) << QString::fromLatin1(name) << true;
        }
        for (const auto* name :
             {"hash-progress", "start-progress", "readiness-progress", "integrity-progress"}) {
            QTest::newRow((QByteArray("registration-") + name).constData())
                << QString::fromLatin1(name) << false;
        }
    }

    void closeFromNotification() {
        QFETCH(QString, notification);
        QFETCH(bool, runtime);
        QTemporaryDir directory;
        engine_test::Validator service;
        const auto script = engine_test::scenario(
            directory.filePath(QStringLiteral("registration.json")), engine_test::handshake());
        const auto profile = profileFor(service.service, script);
        auto steps = engine_test::handshake();
        if (notification == QStringLiteral("handshaking")) {
            steps.removeAt(0);
            steps.removeAt(0);
        } else if (notification == QStringLiteral("readiness-progress")) {
            steps.removeAt(1);
        }
        const auto workerScript =
            engine_test::scenario(directory.filePath(QStringLiteral("worker.json")), steps);
        Attempt attempt(runtime ? AttemptMode::Runtime : AttemptMode::Registration, workerScript);
        QSignalSpy completed(&attempt.worker, &EngineProcessWorker::completed);
        QSignalSpy events(&attempt.worker, &EngineProcessWorker::runtimeChanged);
        bool cancelled = false;
        connect(&attempt.worker, &EngineProcessWorker::runtimeChanged, &attempt.worker,
                [&](const RuntimeState& state) {
                    const bool target = (notification == QStringLiteral("checking") &&
                                         state.phase == RuntimePhase::CheckingExecutable) ||
                                        (notification == QStringLiteral("starting") &&
                                         state.phase == RuntimePhase::Starting) ||
                                        (notification == QStringLiteral("handshaking") &&
                                         state.phase == RuntimePhase::Handshaking) ||
                                        (notification == QStringLiteral("ready") &&
                                         state.phase == RuntimePhase::Ready);
                    if (target && !std::exchange(cancelled, true)) {
                        attempt.worker.close();
                    }
                });
        connect(&attempt.worker, &EngineProcessWorker::progress, &attempt.worker,
                [&](QUuid, const QString& message) {
                    const bool target =
                        (notification == QStringLiteral("hash-progress") &&
                         message == QStringLiteral("Hashing executable…")) ||
                        (notification == QStringLiteral("start-progress") &&
                         message ==
                             QStringLiteral("Starting engine and detecting UCI capabilities…")) ||
                        (notification == QStringLiteral("readiness-progress") &&
                         message == QStringLiteral("Waiting for engine readiness…")) ||
                        (notification == QStringLiteral("integrity-progress") &&
                         message == QStringLiteral("Checking executable is unchanged…"));
                    if (target && !std::exchange(cancelled, true)) {
                        attempt.worker.close();
                    }
                });
        // Observe after the cancelling listener: nested notifications must not mutate snapshots.
        QSignalSpy snapshots(&attempt.worker, &EngineProcessWorker::runtimeChanged);
        QSignalSpy finished(&attempt.worker, &EngineProcessWorker::finished);
        auto* process = attempt.worker.findChild<QProcess*>();
        QVERIFY(process);
        QSignalSpy launches(process, &QProcess::started);
        if (runtime) {
            attempt.worker.connectEngine(QUuid::createUuid(), profile);
        } else {
            attempt.worker.validate(QUuid::createUuid(), profile.engine.executablePath);
        }
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(cancelled);
        if (runtime) {
            const auto terminal = qvariant_cast<RuntimeState>(events.back()[0]);
            QCOMPARE(terminal.phase, RuntimePhase::Disconnected);
            QVERIFY(!terminal.error);
        } else {
            QCOMPARE(completed.size(), 1);
            QCOMPARE(std::get<Error>(qvariant_cast<ValidationResult>(completed.front()[1])).code,
                     ErrorCode::Cancelled);
        }
        QCOMPARE(process->state(), QProcess::NotRunning);
        if (notification == QStringLiteral("checking") ||
            notification == QStringLiteral("starting") ||
            notification == QStringLiteral("hash-progress") ||
            notification == QStringLiteral("start-progress")) {
            QCOMPARE(launches.size(), 0);
        }
        if (notification == QStringLiteral("ready")) {
            bool observedReady = false;
            for (const auto& event : snapshots) {
                const auto state = qvariant_cast<RuntimeState>(event[0]);
                if (state.phase == RuntimePhase::Ready) {
                    observedReady = true;
                    QVERIFY(state.discovery);
                }
            }
            QVERIFY(observedReady);
        }
    }

    void exitStopsProcessDeadline_data() {
        QTest::addColumn<bool>("analyzing");
        QTest::newRow("idle") << false;
        QTest::newRow("analyzing") << true;
    }
    void exitStopsProcessDeadline() {
        QFETCH(bool, analyzing);
        QTemporaryDir directory;
        engine_test::Validator service;
        const auto registration = engine_test::scenario(
            directory.filePath(QStringLiteral("registration.json")), engine_test::handshake());
        const auto profile = profileFor(service.service, registration);
        auto steps = engine_test::handshake();
        auto output = engine_test::write(QString(250000, u'x'));
        output[QStringLiteral("stream")] = QStringLiteral("stderr");
        steps[2] = engine_test::step(
            QStringLiteral("quit"),
            {output, QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}});
        if (analyzing) {
            steps.removeAt(2);
            steps.append(engine_test::step(QStringLiteral("ucinewgame"), {}));
            steps.append(engine_test::step(QStringLiteral("isready"),
                                           {engine_test::write(QStringLiteral("readyok\n"))}));
            steps.append(engine_test::step(QStringLiteral("position startpos"), {}));
            steps.append(engine_test::step(
                QStringLiteral("go infinite"),
                {output, QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}}));
        }
        const auto script =
            engine_test::scenario(directory.filePath(QStringLiteral("worker.json")), steps);
        Attempt attempt(AttemptMode::Runtime, script);
        auto* process = attempt.worker.findChild<QProcess*>();
        QVERIFY(process);
        QSignalSpy events(&attempt.worker, &EngineProcessWorker::runtimeChanged);
        QSignalSpy finished(&attempt.worker, &EngineProcessWorker::finished);
        bool deadlineStoppedAtExit = false;
        bool exitHadBufferedData = false;
        const auto session = QUuid::createUuid();
        connect(&attempt.worker, &EngineProcessWorker::runtimeChanged, &attempt.worker,
                [&](const RuntimeState& state) {
                    if (state.phase == RuntimePhase::Ready) {
                        // Accumulate a real exit buffer so completion requires multiple drain
                        // turns.
                        disconnect(process, &QProcess::readyReadStandardError, &attempt.worker,
                                   nullptr);
                        if (analyzing) {
                            AnalysisRequest request;
                            request.id = QUuid::createUuid();
                            request.limit = InfiniteLimit{};
                            attempt.worker.analyze(session, request);
                        } else {
                            attempt.worker.close();
                        }
                    }
                });
        connect(process, &QProcess::finished, &attempt.worker, [&](int, QProcess::ExitStatus) {
            process->setReadChannel(QProcess::StandardError);
            exitHadBufferedData = process->bytesAvailable() > 65536;
            const auto timers = attempt.worker.findChildren<QTimer*>();
            deadlineStoppedAtExit = std::none_of(timers.begin(), timers.end(),
                                                 [](QTimer* timer) { return timer->isActive(); });
        });
        attempt.worker.connectEngine(session, profile);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(exitHadBufferedData);
        QVERIFY(deadlineStoppedAtExit);
        const auto terminal = qvariant_cast<RuntimeState>(events.back()[0]);
        QCOMPARE(terminal.phase, analyzing ? RuntimePhase::Failed : RuntimePhase::Disconnected);
        if (analyzing) {
            QVERIFY(terminal.error);
            QCOMPARE(terminal.error->code, ErrorCode::Crash);
        } else {
            QVERIFY(!terminal.error);
        }
    }

    void supervisorThreadResponsiveness() {
        QTemporaryDir directory;
        engine_test::Validator service;
        QString declarations = QStringLiteral("id name Test engine\nid author Tests\n");
        for (int i = 0; i < 1024; ++i) {
            declarations += QStringLiteral("option name %1%2 type check default false\n")
                                .arg(QString(512, u'a'))
                                .arg(i, 4, 10, QLatin1Char('0'));
        }
        declarations += QStringLiteral("uciok\n");
        const auto script = engine_test::scenario(directory.filePath(QStringLiteral("stress.json")),
                                                  engine_test::handshake(declarations));
        ValidatorTestAccess::configure(service.service, script, 10000);
        QSignalSpy registration(&service.service, &EngineSupervisor::completed);
        service.service.validate(QUuid::createUuid(),
                                 QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath());
        QVERIFY(registration.wait(15000));
        const auto result = qvariant_cast<ValidationResult>(registration.front()[1]);
        QVERIFY(std::holds_alternative<ValidatedEngine>(result));
        EngineProfile profile;
        profile.id = 1;
        profile.engine = std::get<ValidatedEngine>(result);
        EngineProcessWorker worker(
            AttemptMode::Runtime,
            {{QStringLiteral("--scenario"), script}, 10000, 10000, 1024 * 1024});
        QSignalSpy finished(&worker, &EngineProcessWorker::finished);
        QElapsedTimer clock;
        clock.start();
        qint64 lastTick = 0;
        qint64 maxGap = 0;
        int ticks = 0;
        bool reachedReady = false;
        RuntimeState terminal;
        QTimer heartbeat;
        heartbeat.setInterval(1);
        connect(&heartbeat, &QTimer::timeout, &worker, [&] {
            const auto now = clock.elapsed();
            maxGap = std::max(maxGap, now - lastTick);
            lastTick = now;
            ++ticks;
        });
        connect(&worker, &EngineProcessWorker::runtimeChanged, &worker,
                [&](const RuntimeState& state) {
                    if (state.phase == RuntimePhase::Ready) {
                        reachedReady = true;
                        maxGap = std::max(maxGap, clock.elapsed() - lastTick);
                        worker.close();
                    } else if (!runtimeActive(state.phase)) {
                        terminal = state;
                    }
                });
        heartbeat.start();
        worker.connectEngine(QUuid::createUuid(), profile);
        if (finished.isEmpty() && !finished.wait(15000)) {
            qFatal("Supervisor-thread stress fixture did not finish");
        }
        heartbeat.stop();
        qInfo() << "Supervisor-thread stress:" << clock.elapsed() << "ms," << ticks
                << "ticks, max gap" << maxGap << "ms";
        QVERIFY(ticks > 0);
        QVERIFY(reachedReady);
        QCOMPARE(terminal.phase, RuntimePhase::Disconnected);
        QVERIFY(!terminal.error);
        QCOMPARE(finished.size(), 1);
    }
};
QTEST_GUILESS_MAIN(EngineProcessWorkerTest)
#include "EngineProcessWorkerTest.moc"
