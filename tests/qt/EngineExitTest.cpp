#include "EngineTestSupport.h"
#include "TestEnvironment.h"
#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <tuple>
class EngineExitTest final : public QObject {
    Q_OBJECT
  private slots:
    void actualExit_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("during-validation") << QStringLiteral("validating");
        QTest::newRow("during-save") << QStringLiteral("saving");
        QTest::newRow("after-add") << QStringLiteral("add");
    }
    void actualExit() {
#ifndef Q_OS_LINUX
        QSKIP("Full application profile isolation uses Linux XDG locations.");
#else
        QFETCH(QString, mode);
        QTemporaryDir directory;
        auto steps = engine_test::handshake();
        if (mode == QStringLiteral("validating")) {
            steps[1] = engine_test::step(QStringLiteral("isready"),
                                         {engine_test::write(QStringLiteral("readyok\n"), 5000)});
        }
        const auto script =
            engine_test::scenario(directory.filePath(QStringLiteral("scenario.json")), steps);
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("HOME"), directory.path());
        environment.insert(QStringLiteral("XDG_DATA_HOME"),
                           directory.filePath(QStringLiteral("data")));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"),
                           directory.filePath(QStringLiteral("config")));
        environment.insert(QStringLiteral("XDG_CACHE_HOME"),
                           directory.filePath(QStringLiteral("cache")));
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        environment.insert(QStringLiteral("CHESS_REVIEW_ENGINE_PROBE_MODE"), mode);
        environment.insert(QStringLiteral("CHESS_REVIEW_ENGINE_PROBE_SCENARIO"), script);
        environment.insert(QStringLiteral("CHESS_REVIEW_ENGINE_PROBE_BINARY"),
                           QString::fromUtf8(FAKE_UCI_ENGINE));
        auto run = [&](const QProcessEnvironment& env) {
            QProcess process;
            process.setProcessEnvironment(env);
            process.start(QString::fromUtf8(ENGINE_EXIT_PROBE), {});
            const bool finished = process.waitForFinished(8000);
            if (!finished) {
                process.kill();
                process.waitForFinished(2000);
            }
            const auto output = process.readAllStandardOutput();
            const auto errors = process.readAllStandardError();
            return std::tuple{finished && process.exitStatus() == QProcess::NormalExit &&
                                  process.exitCode() == 0,
                              output, errors};
        };
        const auto [ok, output, errors] = run(environment);
        QVERIFY2(ok, errors.constData());
        QVERIFY(output.contains("engine-close-requested"));
        if (mode != QStringLiteral("validating")) {
            environment.insert(QStringLiteral("CHESS_REVIEW_ENGINE_PROBE_MODE"),
                               QStringLiteral("reopen"));
            const auto [reopened, reopenOutput, reopenErrors] = run(environment);
            QVERIFY2(reopened, reopenErrors.constData());
            QVERIFY(reopenOutput.contains("engine-reopened"));
        }
#endif
    }
};
QTEST_GUILESS_MAIN(EngineExitTest)
#include "EngineExitTest.moc"
