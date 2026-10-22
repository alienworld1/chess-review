#include "engine/private/EngineProcessWorker.h"
#include "EngineTestSupport.h"
#include "TestEnvironment.h"
#include "engine/private/SupervisorCoordinator.h"
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <array>

using namespace chess_review::engine;
namespace {
using Stage = ProcessStage;

class Attempt final {
  public:
    Attempt(AttemptMode mode, const QString& scenario)
        : worker(mode, {{QStringLiteral("--scenario"), scenario}, 1000, 1000, 1024 * 1024}) {
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
                                   std::pair{Stage::Quitting, Stage::Terminating},
                                   std::pair{Stage::Quitting, Stage::CheckingIntegrity},
                                   std::pair{Stage::Terminating, Stage::Killing}};
        constexpr std::array all{Stage::Created,         Stage::CheckingExecutable,
                                 Stage::Starting,        Stage::AwaitingUciOk,
                                 Stage::AwaitingReadyOk, Stage::CheckingIntegrity,
                                 Stage::Ready,           Stage::Quitting,
                                 Stage::Terminating,     Stage::Killing,
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
};
QTEST_GUILESS_MAIN(EngineProcessWorkerTest)
#include "EngineProcessWorkerTest.moc"
