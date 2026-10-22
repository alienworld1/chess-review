#include "EngineTestSupport.h"
#include "TestEnvironment.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <utility>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif
using namespace chess_review::engine;
class EngineValidationTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { QTest::failOnWarning(); }
    void optionTypesAndCodec() {
        Capabilities capabilities;
        const QStringList lines{
            QStringLiteral("option name Threads type spin default 1 min 1 max 4"),
            QStringLiteral("option name Hash type spin default 16 min 1 max 128"),
            QStringLiteral("option name MultiPV type spin default 1 min 1 max 3"),
            QStringLiteral("option name UCI_ShowWDL type check default false"),
            QStringLiteral(
                "option name Style type combo default Very  Quiet var Normal var Very  Quiet"),
            QStringLiteral("option name Book Path type string default <empty>"),
            QStringLiteral("option name Clear Hash type button")};
        for (const auto& line : lines) {
            const auto parsed = parseOption(line);
            QVERIFY(std::holds_alternative<UciOption>(parsed));
            QVERIFY(!appendOption(capabilities, std::get<UciOption>(parsed)));
        }
        QVERIFY(capabilities.supportsMultiPv && capabilities.supportsWdl &&
                capabilities.supportsThreads && capabilities.supportsHash);
        QCOMPARE(capabilities.options[4].defaultValue, QStringLiteral("Very  Quiet"));
        const auto pathOption = std::get<UciOption>(parseOption(
            QStringLiteral("option name EvalFile type string default /tmp/two  spaces.nnue")));
        QCOMPARE(pathOption.defaultValue, QStringLiteral("/tmp/two  spaces.nnue"));
        const auto encoded = encodeCapabilities(capabilities);
        const auto decoded = decodeCapabilities(encoded);
        QVERIFY(decoded);
        QCOMPARE(encodeCapabilities(*decoded), encoded);
        QVERIFY(appendOption(capabilities, capabilities.options.front()));
        Capabilities unusualMinimum;
        QVERIFY(!appendOption(unusualMinimum,
                              std::get<UciOption>(parseOption(QStringLiteral(
                                  "option name Threads type spin default 2 min 2 max 8")))));
        QVERIFY(unusualMinimum.supportsThreads);
        QVERIFY(!decodeCapabilities(QByteArray("{\"version\":2,\"options\":[]}")));
        QVERIFY(std::holds_alternative<Error>(
            parseOption(QStringLiteral("option name X type spin default 2 min 3 max 4"))));
    }

    void codecRejectsCaseInsensitiveDuplicates() {
        const QList<std::pair<QString, QString>> pairs{
            {QStringLiteral("Threads"), QStringLiteral("THREADS")},
            {QStringLiteral("Name \u03a3"), QStringLiteral("Name \u03c3")},
            {QStringLiteral("Name \U00010400"), QStringLiteral("Name \U00010428")}};
        for (const auto& [first, second] : pairs) {
            QCOMPARE(first.compare(second, Qt::CaseInsensitive), 0);
            Capabilities capabilities;
            UciOption one;
            one.name = first;
            one.type = OptionType::Check;
            one.defaultValue = QStringLiteral("false");
            auto two = one;
            two.name = second;
            capabilities.options = {one, two};
            QVERIFY(!decodeCapabilities(encodeCapabilities(capabilities)));
            capabilities.options = {one};
            const auto decoded = decodeCapabilities(encodeCapabilities(capabilities));
            QVERIFY(decoded);
            QCOMPARE(decoded->options, capabilities.options);
        }
        Capabilities ordered;
        for (const auto* name : {"Zebra", "alpha", "Middle"}) {
            auto option = std::get<UciOption>(
                parseOption(QStringLiteral("option name %1 type check default false")
                                .arg(QString::fromLatin1(name))));
            QVERIFY(!appendOption(ordered, std::move(option)));
        }
        const auto decoded = decodeCapabilities(encodeCapabilities(ordered));
        QVERIFY(decoded);
        QCOMPARE(*decoded, ordered); // Sorting duplicate-check keys must not reorder declarations.
    }
    void profileConsistency() {
        ValidatedEngine value;
        value.executablePath = QStringLiteral("/engine");
        value.executableSha256 = QByteArray(32, 'x');
        value.reportedName = QStringLiteral("Engine");
        value.lastValidatedAt = QDateTime::currentDateTimeUtc();
        QVERIFY(validEngine(value));
        value.capabilities.supportsMultiPv = true;
        QVERIFY(!validEngine(value));
        value.capabilities.supportsMultiPv = false;
        value.reportedName = QStringLiteral(" \t ");
        QVERIFY(!validEngine(value));
    }
    void unsolicitedReadyInUciResponse() {
        QTemporaryDir directory;
        auto steps = engine_test::handshake(QStringLiteral("id name Test\nuciok\nreadyok\n"));
        steps[1] = engine_test::step(QStringLiteral("isready"),
                                     {engine_test::write(QStringLiteral("info ignored\n"))});
        engine_test::Validator validator;
        ValidatorTestAccess::configure(
            validator.service,
            engine_test::scenario(directory.filePath(QStringLiteral("script.json")), steps));
        QSignalSpy done(&validator.service, &EngineSupervisor::completed);
        validator.service.validate(
            QUuid::createUuid(), QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath());
        QTRY_COMPARE(done.size(), 1);
        const auto result = qvariant_cast<ValidationResult>(done.front()[1]);
        QVERIFY(std::holds_alternative<Error>(result));
        QCOMPARE(std::get<Error>(result).code, ErrorCode::Protocol);
        // A rejected process must be fully reaped and its protocol state discarded before reuse.
        ValidatorTestAccess::configure(
            validator.service,
            engine_test::scenario(directory.filePath(QStringLiteral("retry.json")),
                                  engine_test::handshake()));
        validator.service.validate(
            QUuid::createUuid(), QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath());
        QTRY_COMPARE(done.size(), 2);
        QVERIFY(std::holds_alternative<ValidatedEngine>(
            qvariant_cast<ValidationResult>(done.back()[1])));
    }
    void maximumComboChoices() {
        QString line = QStringLiteral("option name Style type combo default Choice  0");
        for (int index = 0; index < 1024; ++index) {
            line += QStringLiteral(" var Choice  %1").arg(index);
        }
        const auto parsed = parseOption(line);
        QVERIFY(std::holds_alternative<UciOption>(parsed));
        const auto option = std::get<UciOption>(parsed);
        QCOMPARE(option.choices.size(), 1024);
        QCOMPARE(option.choices.back(), QStringLiteral("Choice  1023"));
        QVERIFY(std::holds_alternative<Error>(parseOption(line + QStringLiteral(" var Overflow"))));
        QVERIFY(std::holds_alternative<Error>(parseOption(
            QStringLiteral("option name Style type combo default Normal var Normal var"))));
    }
    void handshake() {
        QTemporaryDir directory;
        auto steps = engine_test::handshake(
            QStringLiteral("id name  Exact <name>  \nid author Tests\noption name MultiPV type "
                           "spin default 1 min 1 max 4\nuciok\n"));
        engine_test::Validator validator;
        ValidatorTestAccess::configure(
            validator.service,
            engine_test::scenario(directory.filePath(QStringLiteral("script.json")), steps));
        QSignalSpy done(&validator.service, &EngineSupervisor::completed);
        int heartbeat = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] { ++heartbeat; });
        timer.start(1);
        const auto request = QUuid::createUuid();
        validator.service.validate(
            request, QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath());
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(done.front()[0].toUuid(), request);
        const auto result = qvariant_cast<ValidationResult>(done.front()[1]);
        QVERIFY2(std::holds_alternative<ValidatedEngine>(result),
                 std::holds_alternative<Error>(result) ? qPrintable(std::get<Error>(result).message)
                                                       : "");
        const auto profile = std::get<ValidatedEngine>(result);
        QCOMPARE(profile.reportedName, QStringLiteral(" Exact <name>  "));
        QVERIFY(profile.capabilities.supportsMultiPv);
        QVERIFY(profile.lastValidatedAt.isValid());
        QFile executable(QString::fromUtf8(FAKE_UCI_ENGINE));
        QVERIFY(executable.open(QIODevice::ReadOnly));
        QCOMPARE(profile.executableSha256,
                 QCryptographicHash::hash(executable.readAll(), QCryptographicHash::Sha256));
        QVERIFY(heartbeat > 0);
    }
    void hostileHandshake_data() {
        QTest::addColumn<QString>("output");
        QTest::addColumn<int>("expected");
        QString manyOptions = QStringLiteral("id name Test\n");
        for (int index = 0; index < 1025; ++index) {
            manyOptions += QStringLiteral("option name Option%1 type button\n").arg(index);
        }
        QTest::newRow("option-count") << manyOptions << static_cast<int>(ErrorCode::OutputLimit);
        QTest::newRow("bare-option") << QStringLiteral("id name Test\noption\nuciok\n")
                                     << static_cast<int>(ErrorCode::Protocol);
        QTest::newRow("premature-bestmove")
            << QStringLiteral("id name Test\nbestmove e2e4\nuciok\n")
            << static_cast<int>(ErrorCode::Protocol);
        QTest::newRow("no-name") << QStringLiteral("uciok\n")
                                 << static_cast<int>(ErrorCode::Protocol);
        QTest::newRow("premature-ready")
            << QStringLiteral("id name Test\nreadyok\n") << static_cast<int>(ErrorCode::Protocol);
        QTest::newRow("duplicate-name") << QStringLiteral("id name Test\nid name Other\nuciok\n")
                                        << static_cast<int>(ErrorCode::Protocol);
        QTest::newRow("bad-option")
            << QStringLiteral(
                   "id name Test\noption name Hash type spin default 9 min 1 max 2\nuciok\n")
            << static_cast<int>(ErrorCode::Protocol);
        QTest::newRow("duplicate-option")
            << QStringLiteral(
                   "id name Test\noption name X type button\noption name x type button\nuciok\n")
            << static_cast<int>(ErrorCode::Protocol);
        QTest::newRow("long-line")
            << QString(65537, u'x') + u'\n' << static_cast<int>(ErrorCode::OutputLimit);
    }
    void hostileHandshake() {
        QFETCH(QString, output);
        QFETCH(int, expected);
        QTemporaryDir directory;
        const QJsonArray steps{
            engine_test::step(QStringLiteral("uci"), {engine_test::write(output)}),
            engine_test::step(QStringLiteral("quit"),
                              {QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}})};
        engine_test::Validator validator;
        ValidatorTestAccess::configure(
            validator.service,
            engine_test::scenario(directory.filePath(QStringLiteral("script.json")), steps));
        QSignalSpy done(&validator.service, &EngineSupervisor::completed);
        validator.service.validate(
            QUuid::createUuid(), QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath());
        QTRY_COMPARE(done.size(), 1);
        const auto result = qvariant_cast<ValidationResult>(done.front()[1]);
        QVERIFY(std::holds_alternative<Error>(result));
        QCOMPARE(static_cast<int>(std::get<Error>(result).code), expected);
    }
    void fragmentedWhitespace() {
        QTemporaryDir directory;
        auto steps = engine_test::handshake();
        steps[0] = engine_test::step(
            QStringLiteral("uci"),
            {engine_test::write(QStringLiteral(
                 "\t id\tname Test\r\noption name Book File type string default some path\nuc")),
             engine_test::write(QStringLiteral("iok\r\n"), 20)});
        engine_test::Validator validator;
        ValidatorTestAccess::configure(
            validator.service,
            engine_test::scenario(directory.filePath(QStringLiteral("script.json")), steps));
        QSignalSpy done(&validator.service, &EngineSupervisor::completed);
        validator.service.validate(
            QUuid::createUuid(), QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath());
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(std::holds_alternative<ValidatedEngine>(
            qvariant_cast<ValidationResult>(done.front()[1])));
    }
    void timeoutCancelCrashAndCleanup_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("timeout") << QStringLiteral("timeout");
        QTest::newRow("cancel") << QStringLiteral("cancel");
        QTest::newRow("crash") << QStringLiteral("crash");
        QTest::newRow("quit-hang") << QStringLiteral("quit-hang");
        QTest::newRow("quit-stubborn") << QStringLiteral("quit-stubborn");
        QTest::newRow("ready-timeout") << QStringLiteral("ready-timeout");
    }
    void timeoutCancelCrashAndCleanup() {
        QFETCH(QString, mode);
        QTemporaryDir directory;
        QJsonArray steps;
        if (mode == QStringLiteral("quit-hang") || mode == QStringLiteral("quit-stubborn")) {
            steps = engine_test::handshake();
            steps[2] = engine_test::step(
                QStringLiteral("quit"),
                {QJsonObject{{QStringLiteral("type"), mode == QStringLiteral("quit-stubborn")
                                                          ? QStringLiteral("stubbornHang")
                                                          : QStringLiteral("hang")}}});
        } else if (mode == QStringLiteral("ready-timeout")) {
            steps = engine_test::handshake();
            steps[1] =
                engine_test::step(QStringLiteral("isready"),
                                  {QJsonObject{{QStringLiteral("type"), QStringLiteral("hang")}}});
        } else {
            const auto action =
                mode == QStringLiteral("crash") ? QStringLiteral("crash") : QStringLiteral("hang");
            steps = {engine_test::step(QStringLiteral("uci"),
                                       {QJsonObject{{QStringLiteral("type"), action}}})};
        }
        engine_test::Validator validator;
        ValidatorTestAccess::configure(
            validator.service,
            engine_test::scenario(directory.filePath(QStringLiteral("script.json")), steps),
            // Sanitizer abort reporting can outlive the deliberately short timeout fixtures.
            mode == QStringLiteral("crash") ? 5000 : 300);
        QSignalSpy progress(&validator.service, &EngineSupervisor::progress);
        QSignalSpy done(&validator.service, &EngineSupervisor::completed);
        validator.service.validate(
            QUuid::createUuid(), QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath());
        if (mode == QStringLiteral("cancel")) {
            QTRY_VERIFY(progress.size() >= 2);
            validator.service.cancel();
        }
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        const auto result = qvariant_cast<ValidationResult>(done.front()[1]);
        QVERIFY(std::holds_alternative<Error>(result));
        const auto expected =
            mode == QStringLiteral("crash")    ? ErrorCode::Crash
            : mode == QStringLiteral("cancel") ? ErrorCode::Cancelled
            : (mode == QStringLiteral("quit-hang") || mode == QStringLiteral("quit-stubborn"))
                ? ErrorCode::Cleanup
                : ErrorCode::Timeout;
        QCOMPARE(std::get<Error>(result).code, expected);
    }
    void growingExecutableIsRejectedDuringHashing() {
        QTemporaryDir directory;
        const auto executable = directory.filePath(QStringLiteral("engine"));
        QVERIFY(QFile::copy(QString::fromUtf8(FAKE_UCI_ENGINE), executable));
        QFile file(executable);
        QVERIFY(file.open(QIODevice::Append));
        const QByteArray padding(1024 * 1024, '\0');
        for (int index = 0; index < 128; ++index) {
            QCOMPARE(file.write(padding), qint64(padding.size()));
        }
        file.close();
        engine_test::Validator validator;
        ValidatorTestAccess::configure(
            validator.service,
            engine_test::scenario(directory.filePath(QStringLiteral("script.json")),
                                  engine_test::handshake()),
            1000);
        bool appended = false;
        connect(&validator.service, &EngineSupervisor::progress, this,
                [&](QUuid, const QString& message) {
                    if (message == QStringLiteral("Hashing executable…")) {
                        appended = file.open(QIODevice::Append) && file.write("changed") == 7;
                        file.close();
                    }
                });
        QSignalSpy done(&validator.service, &EngineSupervisor::completed);
        validator.service.validate(QUuid::createUuid(), executable);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QVERIFY(appended);
        const auto result = qvariant_cast<ValidationResult>(done.front()[1]);
        QVERIFY(std::holds_alternative<Error>(result));
        QCOMPARE(std::get<Error>(result).code, ErrorCode::Changed);
    }
    void replacementDuringHandshake() {
        QTemporaryDir directory;
        const auto executable = directory.filePath(QStringLiteral("engine"));
        const auto replacement = directory.filePath(QStringLiteral("replacement"));
        QVERIFY(QFile::copy(QString::fromUtf8(FAKE_UCI_ENGINE), executable));
        QVERIFY(QFile::copy(executable, replacement));
        QFile changed(replacement);
        QVERIFY(changed.open(QIODevice::Append));
        QCOMPARE(changed.write("changed"), qint64(7));
        changed.close();
        auto steps = engine_test::handshake();
        steps[1] = engine_test::step(QStringLiteral("isready"),
                                     {engine_test::write(QStringLiteral("readyok\n"), 300)});
        engine_test::Validator validator;
        ValidatorTestAccess::configure(
            validator.service,
            engine_test::scenario(directory.filePath(QStringLiteral("script.json")), steps), 1000);
        QSignalSpy progress(&validator.service, &EngineSupervisor::progress);
        QSignalSpy done(&validator.service, &EngineSupervisor::completed);
        validator.service.validate(QUuid::createUuid(), executable);
        QTRY_VERIFY(progress.size() >= 3);
        QVERIFY(QFile::remove(executable));
        QVERIFY(QFile::rename(replacement, executable));
        QTRY_COMPARE(done.size(), 1);
        const auto result = qvariant_cast<ValidationResult>(done.front()[1]);
        QVERIFY(std::holds_alternative<Error>(result));
        QCOMPARE(std::get<Error>(result).code, ErrorCode::Changed);
    }
    void aggregateOutputLimit() {
        QTemporaryDir directory;
        auto write = engine_test::write(QString(16384, u'x'));
        write[QStringLiteral("stream")] = QStringLiteral("stderr");
        const QJsonArray steps{
            engine_test::step(QStringLiteral("uci"), {write}),
            engine_test::step(QStringLiteral("quit"),
                              {QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}})};
        engine_test::Validator validator;
        ValidatorTestAccess::configure(
            validator.service,
            engine_test::scenario(directory.filePath(QStringLiteral("script.json")), steps), 1000,
            8192);
        QSignalSpy done(&validator.service, &EngineSupervisor::completed);
        validator.service.validate(
            QUuid::createUuid(), QFileInfo(QString::fromUtf8(FAKE_UCI_ENGINE)).canonicalFilePath());
        QTRY_COMPARE(done.size(), 1);
        const auto result = qvariant_cast<ValidationResult>(done.front()[1]);
        QVERIFY(std::holds_alternative<Error>(result));
        QCOMPARE(std::get<Error>(result).code, ErrorCode::OutputLimit);
    }
    void pathsAndReplacement() {
        QTemporaryDir directory;
        engine_test::Validator validator;
        QSignalSpy inspected(&validator.service, &EngineSupervisor::inspected);
        validator.service.inspect(QUuid::createUuid(), directory.path());
        QTRY_COMPARE(inspected.size(), 1);
        QVERIFY(std::holds_alternative<Error>(qvariant_cast<PathResult>(inspected.back()[1])));
        const auto binary = directory.filePath(QStringLiteral("棋 engine ';$().bin"));
        QVERIFY(QFile::copy(QString::fromUtf8(FAKE_UCI_ENGINE), binary));
        const auto link = directory.filePath(QStringLiteral("link"));
        QVERIFY(QFile::link(binary, link));
        validator.service.inspect(QUuid::createUuid(), link);
        QTRY_COMPARE(inspected.size(), 2);
        QCOMPARE(std::get<QString>(qvariant_cast<PathResult>(inspected.back()[1])), binary);
        QFile invalid(directory.filePath(QStringLiteral("not-an-engine")));
        QVERIFY(invalid.open(QIODevice::WriteOnly));
        QCOMPARE(invalid.write("invalid executable"), qint64(18));
        invalid.close();
        QVERIFY(invalid.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        QSignalSpy invalidDone(&validator.service, &EngineSupervisor::completed);
        validator.service.validate(QUuid::createUuid(), invalid.fileName());
        QTRY_COMPARE(invalidDone.size(), 1);
        const auto invalidResult = qvariant_cast<ValidationResult>(invalidDone.front()[1]);
        QVERIFY(std::holds_alternative<Error>(invalidResult));
        QCOMPARE(std::get<Error>(invalidResult).code, ErrorCode::Start);
        const auto script = engine_test::scenario(directory.filePath(QStringLiteral("script.json")),
                                                  engine_test::handshake());
        ValidatorTestAccess::configure(validator.service, script);
        QSignalSpy done(&validator.service, &EngineSupervisor::completed);
        validator.service.validate(QUuid::createUuid(), binary);
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(std::holds_alternative<ValidatedEngine>(
            qvariant_cast<ValidationResult>(done.back()[1])));
        QVERIFY(QFile::setPermissions(binary, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        validator.service.inspect(QUuid::createUuid(), binary);
        QTRY_COMPARE(inspected.size(), 3);
        QVERIFY(std::holds_alternative<Error>(qvariant_cast<PathResult>(inspected.back()[1])));
#ifdef Q_OS_UNIX
        const auto fifo = directory.filePath(QStringLiteral("fifo")).toLocal8Bit();
        QCOMPARE(::mkfifo(fifo.constData(), 0600), 0);
        validator.service.inspect(QUuid::createUuid(), QString::fromLocal8Bit(fifo));
        QTRY_COMPARE(inspected.size(), 4);
        QVERIFY(std::holds_alternative<Error>(qvariant_cast<PathResult>(inspected.back()[1])));
#endif
    }
};
QTEST_GUILESS_MAIN(EngineValidationTest)
#include "EngineValidationTest.moc"
