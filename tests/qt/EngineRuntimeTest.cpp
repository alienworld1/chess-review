#include "EngineTestSupport.h"
#include "TestEnvironment.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
using namespace chess_review::engine;
namespace {
const QString identity =
    QStringLiteral("id name Runtime test\nid author Tests\n"
                   "option name Threads type spin default 1 min 1 max 4\n"
                   "option name UCI_ShowWDL type check default false\nuciok\n");
EngineProfile registered(EngineSupervisor& supervisor, QTemporaryDir& directory,
                         const QString& declarations = identity,
                         const QString& executable = QString::fromUtf8(FAKE_UCI_ENGINE)) {
    ValidatorTestAccess::configure(
        supervisor, engine_test::scenario(directory.filePath(QStringLiteral("registration.json")),
                                          engine_test::handshake(declarations)));
    QSignalSpy done(&supervisor, &EngineSupervisor::completed);
    supervisor.validate(QUuid::createUuid(), QFileInfo(executable).canonicalFilePath());
    if (!done.wait(5000)) {
        qFatal("Registration fixture timed out");
    }
    const auto result = qvariant_cast<ValidationResult>(done.front()[1]);
    if (!std::holds_alternative<ValidatedEngine>(result)) {
        qFatal("Registration fixture failed");
    }
    EngineProfile profile;
    profile.id = 1;
    profile.engine = std::get<ValidatedEngine>(result);
    return profile;
}
void configure(EngineSupervisor& supervisor, QTemporaryDir& directory, QJsonArray steps,
               int deadline = 300, qsizetype outputLimit = 1024 * 1024) {
    ValidatorTestAccess::configure(
        supervisor,
        engine_test::scenario(directory.filePath(QStringLiteral("runtime.json")), steps), deadline,
        outputLimit);
}
RuntimeState latest(const QSignalSpy& events) {
    return events.empty() ? RuntimeState{} : qvariant_cast<RuntimeState>(events.back()[0]);
}
} // namespace
class EngineRuntimeTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { QTest::failOnWarning(); }
    void persistentReadyAndAdmission() {
        QTemporaryDir directory;
        engine_test::Validator fixture;
        auto& service = fixture.service;
        const auto profile = registered(service, directory);
        configure(service, directory, engine_test::handshake(identity));
        QSignalSpy events(&service, &EngineSupervisor::runtimeChanged);
        QSignalSpy validation(&service, &EngineSupervisor::completed);
        const auto session = QUuid::createUuid();
        service.connectEngine(session, profile);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Ready);
        QVERIFY(latest(events).discovery);
        QCOMPARE(latest(events).discovery->capabilities, profile.engine.capabilities);
        QCOMPARE(latest(events).profileId, profile.id);
        const auto readyCount = events.size();
        QTest::qWait(100);
        QCOMPARE(events.size(), readyCount); // No automatic quit or readiness polling.
        service.validate(QUuid::createUuid(), profile.engine.executablePath);
        QTRY_COMPARE(validation.size(), 1);
        QCOMPARE(std::get<Error>(qvariant_cast<ValidationResult>(validation.front()[1])).code,
                 ErrorCode::Busy);
        const auto competitor = QUuid::createUuid();
        service.connectEngine(competitor, profile);
        QTRY_COMPARE(latest(events).session, competitor);
        QCOMPARE(latest(events).error->code, ErrorCode::Busy);
        service.disconnectEngine(competitor); // An unrelated session cannot stop the child.
        service.disconnectEngine(session);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Disconnected);
        QCOMPARE(latest(events).session, session);
        QVERIFY(!latest(events).error);
        service.connectEngine(QUuid::createUuid(), profile);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Ready);
    }
    void discoveryMatching_data() {
        QTest::addColumn<QString>("output");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("reordered-and-case")
            << QStringLiteral("id name Runtime test\nid author Tests\n"
                              "option name uci_showwdl type check default false\n"
                              "option name threads type spin default 1 min 1 max 4\nuciok\n")
            << true;
        QTest::newRow("name") << QString(identity).replace(QStringLiteral("Runtime test"),
                                                           QStringLiteral("Other"))
                              << false;
        QTest::newRow("default") << QString(identity).replace(QStringLiteral("default 1"),
                                                              QStringLiteral("default 2"))
                                 << false;
        QTest::newRow("missing-option")
            << QStringLiteral("id name Runtime test\nid author Tests\nuciok\n") << false;
        QTest::newRow("malformed")
            << QStringLiteral("id name Runtime test\noption name Broken\nuciok\n") << false;
        QTest::newRow("bundled-ready") << identity + QStringLiteral("readyok\n") << false;
    }
    void discoveryMatching() {
        QFETCH(QString, output);
        QFETCH(bool, accepted);
        QTemporaryDir directory;
        engine_test::Validator fixture;
        const auto profile = registered(fixture.service, directory);
        configure(fixture.service, directory, engine_test::handshake(output));
        QSignalSpy events(&fixture.service, &EngineSupervisor::runtimeChanged);
        fixture.service.connectEngine(QUuid::createUuid(), profile);
        QTRY_COMPARE(latest(events).phase, accepted ? RuntimePhase::Ready : RuntimePhase::Failed);
        if (!accepted) {
            QVERIFY(latest(events).error);
            QVERIFY(!latest(events).discovery);
        }
    }
    void minimalAndFragmentedDiscovery() {
        QTemporaryDir directory;
        engine_test::Validator fixture;
        const auto profile =
            registered(fixture.service, directory, QStringLiteral("id name Minimal\nuciok\n"));
        auto steps = engine_test::handshake();
        steps[0] = engine_test::step(QStringLiteral("uci"),
                                     {engine_test::write(QStringLiteral("id na")),
                                      engine_test::write(QStringLiteral("me Minimal\r\nuc"), 10),
                                      engine_test::write(QStringLiteral("iok\n"), 20)});
        configure(fixture.service, directory, steps);
        QSignalSpy events(&fixture.service, &EngineSupervisor::runtimeChanged);
        fixture.service.connectEngine(QUuid::createUuid(), profile);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Ready);
        QVERIFY(latest(events).discovery->capabilities.options.empty());
        QVERIFY(!latest(events).discovery->capabilities.supportsWdl);
    }
    void invalidSavedState_data() {
        QTest::addColumn<bool>("overrides");
        QTest::newRow("changed-hash") << false;
        QTest::newRow("unsupported-overrides") << true;
    }
    void invalidSavedState() {
        QFETCH(bool, overrides);
        QTemporaryDir directory;
        engine_test::Validator fixture;
        auto profile = registered(fixture.service, directory);
        if (overrides) {
            profile.optionOverrides.insert(QStringLiteral("Threads"), QStringLiteral("2"));
        } else {
            profile.engine.executableSha256.fill('x');
        }
        // If anything launches, the unconfigured executable fails instead of handshaking.
        ValidatorTestAccess::configure(fixture.service,
                                       directory.filePath(QStringLiteral("missing.json")));
        QSignalSpy events(&fixture.service, &EngineSupervisor::runtimeChanged);
        fixture.service.connectEngine(QUuid::createUuid(), profile);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Failed);
        QCOMPARE(latest(events).error->code,
                 overrides ? ErrorCode::InvalidRecord : ErrorCode::Changed);
    }
    void idleFailure_data() {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<int>("expected");
        QTest::newRow("crash") << QStringLiteral("crash") << int(ErrorCode::Crash);
        QTest::newRow("unsolicited-bestmove")
            << QStringLiteral("bestmove") << int(ErrorCode::Protocol);
        QTest::newRow("output-flood") << QStringLiteral("flood") << int(ErrorCode::OutputLimit);
        QTest::newRow("oversized-line") << QStringLiteral("line") << int(ErrorCode::OutputLimit);
    }
    void idleFailure() {
        QFETCH(QString, kind);
        QFETCH(int, expected);
        QTemporaryDir directory;
        engine_test::Validator fixture;
        const auto profile = registered(fixture.service, directory);
        auto steps = engine_test::handshake(identity);
        QJsonArray actions{engine_test::write(QStringLiteral("readyok\n"))};
        if (kind == QStringLiteral("crash")) {
            actions.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("crash")},
                                       {QStringLiteral("afterMs"), 150}});
        } else {
            QString output = QStringLiteral("bestmove e2e4\n");
            if (kind == QStringLiteral("flood")) {
                output.clear();
                for (int i = 0; i < 1000; ++i) {
                    output += QStringLiteral("info string ignored\n");
                }
            } else if (kind == QStringLiteral("line")) {
                output = QString(65537, u'x');
            }
            actions.append(engine_test::write(output, 150));
        }
        steps[1] = engine_test::step(QStringLiteral("isready"), actions);
        configure(fixture.service, directory, steps, 5000,
                  kind == QStringLiteral("flood") ? 8192 : 1024 * 1024);
        QSignalSpy events(&fixture.service, &EngineSupervisor::runtimeChanged);
        fixture.service.connectEngine(QUuid::createUuid(), profile);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Ready);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Failed);
        QCOMPARE(int(latest(events).error->code), expected);
        configure(fixture.service, directory, engine_test::handshake(identity));
        fixture.service.connectEngine(QUuid::createUuid(), profile);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Ready);
    }
    void disconnectDuringHandshakeAndForcedCleanup_data() {
        QTest::addColumn<bool>("stubborn");
        QTest::addColumn<bool>("waitingForReady");
        QTest::newRow("cancel-handshake") << false << false;
        QTest::newRow("cancel-readiness") << false << true;
        QTest::newRow("kill-ready-child") << true << false;
    }
    void disconnectDuringHandshakeAndForcedCleanup() {
        QFETCH(bool, stubborn);
        QFETCH(bool, waitingForReady);
        QTemporaryDir directory;
        engine_test::Validator fixture;
        const auto profile = registered(fixture.service, directory);
        auto steps = engine_test::handshake(identity);
        if (stubborn) {
            steps[2] = engine_test::step(
                QStringLiteral("quit"),
                {QJsonObject{{QStringLiteral("type"), QStringLiteral("stubbornHang")}}});
        } else if (waitingForReady) {
            steps[1] = engine_test::step(QStringLiteral("isready"), {});
        } else {
            // Handshaking is published before uciok, so quit may precede isready.
            // Keep the child in discovery and expect cancellation at that boundary.
            steps[0] = engine_test::step(QStringLiteral("uci"), {});
            steps.removeAt(1);
        }
        configure(fixture.service, directory, steps, 5000);
        QSignalSpy events(&fixture.service, &EngineSupervisor::runtimeChanged);
        QSignalSpy progress(&fixture.service, &EngineSupervisor::progress);
        const auto session = QUuid::createUuid();
        fixture.service.connectEngine(session, profile);
        QTRY_COMPARE(latest(events).phase,
                     stubborn ? RuntimePhase::Ready : RuntimePhase::Handshaking);
        if (waitingForReady) {
            QTRY_VERIFY(!progress.empty() && progress.back()[1].toString() ==
                                                 QStringLiteral("Waiting for engine readiness…"));
        }
        fixture.service.disconnectEngine(session);
        QTRY_COMPARE(latest(events).phase,
                     stubborn ? RuntimePhase::Failed : RuntimePhase::Disconnected);
        if (stubborn) {
            QVERIFY(latest(events).error);
            QCOMPARE(latest(events).error->code, ErrorCode::Cleanup);
        } else {
            QVERIFY(!latest(events).error);
        }
        QCOMPARE(latest(events).session, session);
    }
    void disconnectBeforeLaunchAndReuse() {
        QTemporaryDir directory;
        engine_test::Validator fixture;
        const auto profile = registered(fixture.service, directory);
        configure(fixture.service, directory, engine_test::handshake(identity));
        QSignalSpy events(&fixture.service, &EngineSupervisor::runtimeChanged);
        const auto session = QUuid::createUuid();
        fixture.service.connectEngine(session, profile);
        fixture.service.disconnectEngine(session);
        QTRY_COMPARE(latest(events).session, session);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Disconnected);
        for (const auto& event : events) {
            QVERIFY(qvariant_cast<RuntimeState>(event[0]).phase != RuntimePhase::Starting);
        }
        fixture.service.connectEngine(QUuid::createUuid(), profile);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Ready);
    }
    void closeDuringStartup_data() {
        QTest::addColumn<bool>("hashing");
        QTest::newRow("hashing") << true;
        QTest::newRow("starting") << false;
    }
    void closeDuringStartup() {
        QFETCH(bool, hashing);
        QTemporaryDir directory;
        EngineSupervisor service;
        const auto profile = registered(service, directory);
        configure(service, directory, engine_test::handshake(identity));
        QSignalSpy closed(&service, &EngineSupervisor::closed);
        connect(&service, &EngineSupervisor::runtimeChanged, this, [&](const RuntimeState& state) {
            if (state.phase ==
                (hashing ? RuntimePhase::CheckingExecutable : RuntimePhase::Starting)) {
                service.close();
            }
        });
        service.connectEngine(QUuid::createUuid(), profile);
        const bool finished = closed.wait(6000);
        if (!finished) {
            qFatal("Supervisor did not close");
        }
        QSignalSpy events(&service, &EngineSupervisor::runtimeChanged);
        service.connectEngine(QUuid::createUuid(), profile);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Failed);
        QCOMPARE(latest(events).error->code, ErrorCode::Closing);
    }
    void lateOutputDuringDisconnect_data() {
        QTest::addColumn<QString>("boundary");
        QTest::newRow("discovery-reply-after-quit") << QStringLiteral("discovery");
        QTest::newRow("readiness-reply-after-quit") << QStringLiteral("readiness");
        QTest::newRow("partial-idle-line-at-quit") << QStringLiteral("idle");
    }
    void lateOutputDuringDisconnect() {
        QFETCH(QString, boundary);
        QTemporaryDir directory;
        engine_test::Validator fixture;
        const auto profile = registered(fixture.service, directory);
        auto steps = engine_test::handshake(identity);
        if (boundary == QStringLiteral("discovery")) {
            steps[0] = engine_test::step(QStringLiteral("uci"), {});
            steps.removeAt(1);
            steps[1] =
                engine_test::step(QStringLiteral("quit"),
                                  {engine_test::write(identity),
                                   QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}});
        } else if (boundary == QStringLiteral("readiness")) {
            steps[1] = engine_test::step(QStringLiteral("isready"), {});
            steps[2] =
                engine_test::step(QStringLiteral("quit"),
                                  {engine_test::write(QStringLiteral("readyok\n")),
                                   QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}});
        } else {
            steps[1] =
                engine_test::step(QStringLiteral("isready"),
                                  {engine_test::write(QStringLiteral("readyok\ninfo incomplete"))});
        }
        configure(fixture.service, directory, steps, 5000);
        QSignalSpy events(&fixture.service, &EngineSupervisor::runtimeChanged);
        QSignalSpy progress(&fixture.service, &EngineSupervisor::progress);
        const auto session = QUuid::createUuid();
        fixture.service.connectEngine(session, profile);
        QTRY_COMPARE(latest(events).phase, boundary == QStringLiteral("idle")
                                               ? RuntimePhase::Ready
                                               : RuntimePhase::Handshaking);
        if (boundary == QStringLiteral("readiness")) {
            QTRY_VERIFY(!progress.empty() && progress.back()[1].toString() ==
                                                 QStringLiteral("Waiting for engine readiness…"));
        }
        fixture.service.disconnectEngine(session);
        QTRY_VERIFY(!runtimeActive(latest(events).phase));
        QCOMPARE(latest(events).phase, RuntimePhase::Disconnected);
        QVERIFY(!latest(events).error);
        configure(fixture.service, directory, engine_test::handshake(identity));
        fixture.service.connectEngine(QUuid::createUuid(), profile);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Ready);
    }
    void disconnectKeepsOutputAndExitChecks_data() {
        QTest::addColumn<bool>("flood");
        QTest::newRow("late-output-limit") << true;
        QTest::newRow("nonzero-quit-exit") << false;
    }
    void disconnectKeepsOutputAndExitChecks() {
        QFETCH(bool, flood);
        QTemporaryDir directory;
        engine_test::Validator fixture;
        const auto profile = registered(fixture.service, directory);
        auto steps = engine_test::handshake(identity);
        if (flood) {
            steps[2] =
                engine_test::step(QStringLiteral("quit"),
                                  {engine_test::write(QString(16384, u'x')),
                                   QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}});
        } else {
            // A command mismatch exits the fixture normally with a nonzero status.
            steps[2] =
                engine_test::step(QStringLiteral("not-quit"),
                                  {QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}});
        }
        configure(fixture.service, directory, steps, 5000, 8192);
        QSignalSpy events(&fixture.service, &EngineSupervisor::runtimeChanged);
        const auto session = QUuid::createUuid();
        fixture.service.connectEngine(session, profile);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Ready);
        fixture.service.disconnectEngine(session);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Failed);
        QVERIFY(latest(events).error);
        QCOMPARE(latest(events).error->code, flood ? ErrorCode::OutputLimit : ErrorCode::Crash);
    }
    void changedRuntimeExecutable_data() {
        QTest::addColumn<bool>("afterReadiness");
        QTest::newRow("changed-before-launch") << false;
        QTest::newRow("replaced-during-integrity-check") << true;
    }
    void changedRuntimeExecutable() {
#ifndef Q_OS_LINUX
        QSKIP("Replacement of a running executable is verified on Linux.");
#else
        QFETCH(bool, afterReadiness);
        QTemporaryDir directory;
        engine_test::Validator fixture;
        const auto copiedPath = directory.filePath(QStringLiteral("engine"));
        QVERIFY(QFile::copy(QString::fromUtf8(FAKE_UCI_ENGINE), copiedPath));
        if (afterReadiness) {
            // Keep hashing in flight long enough to replace the path after its
            // descriptor opens. The copied ELF remains a valid executable.
            QFile padded(copiedPath);
            QVERIFY(padded.open(QIODevice::ReadWrite));
            QVERIFY(padded.resize(padded.size() + 128 * 1024 * 1024));
        }
        const auto profile = registered(fixture.service, directory, identity, copiedPath);
        configure(fixture.service, directory, engine_test::handshake(identity));
        QSignalSpy events(&fixture.service, &EngineSupervisor::runtimeChanged);
        const auto session = QUuid::createUuid();
        bool replaced = false;
        if (afterReadiness) {
            connect(&fixture.service, &EngineSupervisor::progress, this,
                    [&](QUuid request, const QString& message) {
                        if (request == session &&
                            message == QStringLiteral("Checking executable is unchanged…")) {
                            QSaveFile replacement(copiedPath);
                            QVERIFY(replacement.open(QIODevice::WriteOnly));
                            QCOMPARE(replacement.write("replacement"), qint64(11));
                            QVERIFY(replacement.commit());
                            replaced = true;
                        }
                    });
        } else {
            QFile changed(copiedPath);
            QVERIFY(changed.open(QIODevice::Append));
            QCOMPARE(changed.write("changed"), qint64(7));
        }
        fixture.service.connectEngine(session, profile);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Failed);
        QVERIFY(latest(events).error);
        QCOMPARE(latest(events).error->code, ErrorCode::Changed);
        QCOMPARE(replaced, afterReadiness);
        for (const auto& event : events) {
            const auto phase = qvariant_cast<RuntimeState>(event[0]).phase;
            QVERIFY(phase != RuntimePhase::Ready);
            if (!afterReadiness) {
                QVERIFY(phase != RuntimePhase::Starting);
            }
        }
#endif
    }
    void maximumDiscoveryRemainsAsynchronous() {
        QTemporaryDir directory;
        engine_test::Validator fixture;
        QString declarations = QStringLiteral("id name Bulk\n");
        const QString prefix(512, u'x');
        for (int index = 0; index < 1024; ++index) {
            declarations += QStringLiteral("option name %1%2 type button\n").arg(prefix).arg(index);
        }
        declarations += QStringLiteral("uciok\n");
        configure(fixture.service, directory, engine_test::handshake(declarations), 10000);
        QSignalSpy validated(&fixture.service, &EngineSupervisor::completed);
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 lastTick = 0;
        qint64 maximumGap = 0;
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] {
            const auto now = elapsed.elapsed();
            maximumGap = std::max(maximumGap, now - lastTick);
            lastTick = now;
            ++ticks;
        });
        heartbeat.start(10);
        fixture.service.validate(QUuid::createUuid(),
                                 QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath());
        QTRY_COMPARE_WITH_TIMEOUT(validated.size(), 1, 15000);
        const auto result = qvariant_cast<ValidationResult>(validated.front()[1]);
        QVERIFY(std::holds_alternative<ValidatedEngine>(result));
        EngineProfile profile;
        profile.id = 1;
        profile.engine = std::get<ValidatedEngine>(result);
        QCOMPARE(profile.engine.capabilities.options.size(), 1024);
        QSignalSpy events(&fixture.service, &EngineSupervisor::runtimeChanged);
        fixture.service.connectEngine(QUuid::createUuid(), profile);
        QTRY_COMPARE_WITH_TIMEOUT(latest(events).phase, RuntimePhase::Ready, 15000);
        QCOMPARE(latest(events).discovery->capabilities.options.size(), 1024);
        QVERIFY(ticks > 0);
        qInfo() << "1024-option registration/connection milliseconds:" << elapsed.elapsed()
                << "caller heartbeat ticks:" << ticks << "maximum gap milliseconds:" << maximumGap;
    }
    void handshakeTimeout() {
        QTemporaryDir directory;
        engine_test::Validator fixture;
        const auto profile = registered(fixture.service, directory);
        auto steps = engine_test::handshake(identity);
        steps[1] = engine_test::step(QStringLiteral("isready"), {});
        configure(fixture.service, directory, steps);
        QSignalSpy events(&fixture.service, &EngineSupervisor::runtimeChanged);
        fixture.service.connectEngine(QUuid::createUuid(), profile);
        QTRY_COMPARE(latest(events).phase, RuntimePhase::Failed);
        QCOMPARE(latest(events).error->code, ErrorCode::Timeout);
    }
};
QTEST_GUILESS_MAIN(EngineRuntimeTest)
#include "EngineRuntimeTest.moc"
