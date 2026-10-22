#include "TestEnvironment.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
// Own the real child even when a QVERIFY returns early. No process survives fixture cleanup.
class Child final {
  public:
    Child() {
        QObject::connect(&process, &QProcess::readyReadStandardOutput, &process,
                         [this] { output += process.readAllStandardOutput(); });
        QObject::connect(&process, &QProcess::readyReadStandardError, &process,
                         [this] { errors += process.readAllStandardError(); });
    }
    ~Child() {
        if (process.state() != QProcess::NotRunning) {
            process.terminate();
            if (!process.waitForFinished(1000)) {
                process.kill();
                if (!process.waitForFinished(1000)) {
                    qFatal("Cannot reap fake engine child");
                }
            }
        }
    }
    void start(const QStringList& arguments) {
        process.start(QString::fromUtf8(FAKE_UCI_ENGINE), arguments);
    }
    void fixture(const QString& name) {
        start({QStringLiteral("--scenario"),
               QString::fromUtf8(FAKE_UCI_SCENARIOS) + u'/' + name + QStringLiteral(".json")});
    }
    bool send(const QByteArray& bytes) { return process.write(bytes) == bytes.size(); }
    QProcess process;
    QByteArray output;
    QByteArray errors;
};
// Deliberately bypass QProcess stdout buffering to exercise an actually full/broken pipe.
class OutputPipe final {
  public:
    OutputPipe() {
        valid_ = ::pipe(fds_) == 0;
        if (valid_) {
            const auto flags = ::fcntl(fds_[0], F_GETFL);
            valid_ = flags != -1 && ::fcntl(fds_[0], F_SETFL, flags | O_NONBLOCK) != -1;
        }
    }
    ~OutputPipe() {
        for (int fd : fds_) {
            if (fd != -1) {
                (void)::close(fd);
            }
        }
    }
    OutputPipe(const OutputPipe&) = delete;
    OutputPipe& operator=(const OutputPipe&) = delete;
    bool valid() const { return valid_; }
    void redirect(Child& child) const {
        const auto readFd = fds_[0];
        const auto writeFd = fds_[1];
        child.process.setChildProcessModifier([readFd, writeFd] {
            if (::dup2(writeFd, STDOUT_FILENO) == -1) {
                ::_exit(120);
            }
            if (readFd != -1) {
                (void)::close(readFd);
            }
            (void)::close(writeFd);
        });
    }
    void closeReader() {
        if (fds_[0] != -1) {
            (void)::close(fds_[0]);
            fds_[0] = -1;
        }
    }
    void closeWriter() {
        if (fds_[1] != -1) {
            (void)::close(fds_[1]);
            fds_[1] = -1;
        }
    }
    bool drain(QByteArray& bytes) const {
        char buffer[65536];
        const auto count = ::read(fds_[0], buffer, sizeof(buffer));
        if (count > 0) {
            bytes.append(buffer, static_cast<qsizetype>(count));
            return true;
        }
        return count == 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }

  private:
    int fds_[2]{-1, -1};
    bool valid_{};
};
QJsonObject writeAction(const QString& data, int afterMs = 0) {
    return {{"type", "write"}, {"stream", "stdout"}, {"data", data}, {"afterMs", afterMs}};
}
QJsonObject step(const QString& expect, const QJsonArray& actions = {}) {
    return {{"expect", expect}, {"actions", actions}};
}
QJsonObject scenario(const QJsonArray& steps) { return {{"version", 1}, {"steps", steps}}; }
QJsonObject exitAction() { return {{"type", "exit"}}; }
bool save(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}
bool save(const QString& path, const QJsonObject& object) {
    return save(path, QJsonDocument(object).toJson(QJsonDocument::Compact));
}
bool cleanDiagnostics(const QByteArray& bytes) {
    return !bytes.contains("AddressSanitizer") && !bytes.contains("LeakSanitizer") &&
           !bytes.contains("runtime error:");
}
} // namespace

class FakeUciTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { QTest::failOnWarning(); }
    void invocation();
    void invalidScenario_data();
    void invalidScenario();
    void scriptedAnalysis();
    void delayedReadiness();
    void rawOutput();
    void cancellation_data();
    void cancellation();
    void silentAndHang_data();
    void silentAndHang();
    void intentionalCrash();
    void mismatchAndEof();
    void framingAndBounds();
    void backpressureAndExit();
    void limitsAccepted();
    void pendingInputAndEof();
    void independentProcesses();
    void outputFailure();
    void ordinarySilenceCannotCompleteConversation();
    void sharedFileFlags();
    void nonregularScenario();
    void actualBackpressure();
    void mergedPipes();
    void supplementaryUnicode();
    void maximumPendingActions();
};

void FakeUciTest::invocation() {
    for (const auto& arguments : {QStringList{}, QStringList{"--help"}, QStringList{"--scenario"},
                                  QStringList{"--unknown"}}) {
        Child child;
        child.start(arguments);
        QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
        QCOMPARE(child.process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.process.exitCode(), arguments == QStringList{"--help"} ? 0 : 2);
        QVERIFY((child.output + child.errors).contains("Usage:"));
    }
    Child missing;
    missing.start({"--scenario", "/no/such/fake-uci-scenario.json"});
    QTRY_COMPARE(missing.process.state(), QProcess::NotRunning);
    QCOMPARE(missing.process.exitCode(), 2);
    QVERIFY(missing.output.isEmpty());
}
void FakeUciTest::invalidScenario_data() {
    QTest::addColumn<QByteArray>("bytes");
    const auto valid = scenario({step("quit", {exitAction()})});
    const auto add = [](const char* name, const QJsonObject& value) {
        QTest::newRow(name) << QJsonDocument(value).toJson(QJsonDocument::Compact);
    };
    QTest::newRow("syntax") << QByteArray("{");
    QTest::newRow("non-object") << QByteArray("[]");
    QTest::newRow("invalid-utf8") << QByteArray("\xff");
    QTest::newRow("unpaired-output-surrogate") << QByteArray(
        R"({"version":1,"steps":[{"expect":"quit","actions":[{"type":"write","stream":"stdout","data":"\ud800"},{"type":"exit"}]}]})");
    QTest::newRow("unpaired-command-surrogate") << QByteArray(
        R"({"version":1,"steps":[{"expect":"\udc00quit","actions":[{"type":"exit"}]}]})");
    QTest::newRow("oversized") << QByteArray(1024 * 1024 + 1, ' ');
    auto changed = valid;
    changed["version"] = 2;
    add("version", changed);
    changed["version"] = true;
    add("boolean-version", changed);
    changed = valid;
    changed["unknown"] = 1;
    add("root-key", changed);
    add("empty", scenario({}));
    add("nonboolean-eof",
        scenario({QJsonObject{{"expect", "go"}, {"actions", QJsonArray{}}, {"allowEof", "yes"}},
                  step("quit", {exitAction()})}));
    add("eof-with-actions",
        scenario({QJsonObject{
            {"expect", "quit"}, {"actions", QJsonArray{exitAction()}}, {"allowEof", true}}}));
    add("missing-actions", scenario({QJsonObject{{"expect", "quit"}}}));
    add("empty-command", scenario({step(" \t", {exitAction()})}));
    add("newline-command", scenario({step("quit\n", {exitAction()})}));
    add("oversized-command", scenario({step(QString(65537, u'x'), {exitAction()})}));
    add("no-terminal", scenario({step("uci", {writeAction("uciok\n")})}));
    add("unknown-step-key",
        scenario({QJsonObject{
            {"expect", "quit"}, {"actions", QJsonArray{exitAction()}}, {"extra", true}}}));
    add("unknown-action",
        scenario({step("quit", {QJsonObject{{"type", "execute"}}, exitAction()})}));
    add("unknown-action-key",
        scenario({step("quit", {QJsonObject{{"type", "exit"}, {"data", "bad"}}})}));
    for (const auto& delay : {QJsonValue(-1), QJsonValue(0.5), QJsonValue(2147483648.0),
                              QJsonValue("1"), QJsonValue(true)}) {
        auto action = exitAction();
        action["afterMs"] = delay;
        const auto name = "invalid-delay-" + QByteArray::number(delay.type()) + '-' +
                          QByteArray::number(delay.toDouble());
        add(name.constData(), scenario({step("quit", {action})}));
    }
    add("delayed-cancel",
        scenario({step("stop",
                       {QJsonObject{{"type", "cancelPending"}, {"afterMs", 1}}, exitAction()})}));
    add("bad-stream",
        scenario({step("quit", {QJsonObject{{"type", "write"}, {"stream", "log"}, {"data", "x"}},
                                exitAction()})}));
    add("missing-data",
        scenario(
            {step("quit", {QJsonObject{{"type", "write"}, {"stream", "stdout"}}, exitAction()})}));
    QJsonArray many;
    for (int i = 0; i < 4097; ++i) {
        many.append(exitAction());
    }
    add("too-many-actions", scenario({step("quit", many)}));
    many = {};
    for (int i = 0; i < 4097; ++i) {
        many.append(step("quit", i == 0 ? QJsonArray{exitAction()} : QJsonArray{}));
    }
    add("too-many-steps", scenario(many));
}
void FakeUciTest::invalidScenario() {
    QFETCH(QByteArray, bytes);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath("scenario with spaces.json");
    QVERIFY(save(path, bytes));
    Child child;
    child.start({"--scenario", path});
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.process.exitCode(), 2);
    QVERIFY(child.output.isEmpty());
    QVERIFY(child.errors.contains("configuration:"));
    QVERIFY(cleanDiagnostics(child.errors));
}
void FakeUciTest::scriptedAnalysis() {
    QByteArray reference;
    for (int run = 0; run < 3; ++run) {
        Child child;
        child.fixture("analysis");
        QTRY_COMPARE(child.process.state(), QProcess::Running);
        QVERIFY(child.send("uci\nsetoption name MultiPV value 2\nsetoption name UCI_ShowWDL value "
                           "true\nisready\nucinewgame\nposition startpos\ngo nodes 200\nposition "
                           "startpos moves f2f3 e7e5 g2g4\ngo depth 2\nquit\n"));
        QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
        QCOMPARE(child.process.exitCode(), 0);
        QCOMPARE(child.process.exitStatus(), QProcess::NormalExit);
        QVERIFY(child.errors.isEmpty());
        QVERIFY(child.output.contains("multipv 2 score cp 10 wdl 180 700 120 nodes 100"));
        QVERIFY(child.output.contains("nodes 200 time 2"));
        QVERIFY(child.output.contains("score mate 1"));
        QCOMPARE(child.output.count("bestmove"), 2);
        if (run == 0) {
            reference = child.output;
        } else {
            QCOMPARE(child.output, reference);
        }
    }
    Child generic;
    generic.fixture("missing-wdl");
    QTRY_COMPARE(generic.process.state(), QProcess::Running);
    QVERIFY(generic.send("uci\nposition startpos\ngo nodes 100\nquit\n"));
    QTRY_COMPARE(generic.process.state(), QProcess::NotRunning);
    QCOMPARE(generic.process.exitCode(), 0);
    QVERIFY(!generic.output.contains("wdl"));
    QVERIFY(generic.output.contains("score cp 12"));
}
void FakeUciTest::delayedReadiness() {
    Child child;
    child.fixture("delayed");
    QTRY_COMPARE(child.process.state(), QProcess::Running);
    QVERIFY(child.send("uci\nisready\n"));
    QTRY_VERIFY(child.output.contains("uciok\n") && child.output.contains("readyok\n"));
    QVERIFY(child.output.indexOf("readyok") < child.output.indexOf("uciok"));
    QVERIFY(child.send("position startpos\ngo nodes 100\n"));
    QTRY_VERIFY(child.output.contains("bestmove e2e4\n"));
    QVERIFY(child.send("quit\n"));
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitCode(), 0);
    QVERIFY(child.errors.isEmpty());
}
void FakeUciTest::rawOutput() {
    Child child;
    child.fixture("malformed");
    QTRY_COMPARE(child.process.state(), QProcess::Running);
    QVERIFY(child.send("uci\nisready\nposition startpos\ngo nodes 100\nquit\n"));
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitCode(), 0);
    QCOMPARE(child.output, QByteArray("id name fake\r\nuciok\nreadyok\nnonsense\ninfo depth nope "
                                      "score cp ???\ninfo string ") +
                               QByteArray(1, '\0') + "\tUnicode \xe2\x99\x9f\nbestmove z9z9");
    QCOMPARE(child.errors, QByteArray("private test stderr\n"));
}
void FakeUciTest::cancellation_data() {
    QTest::addColumn<QString>("name");
    QTest::newRow("clear") << QString("cancel");
    QTest::newRow("preserve") << QString("late-output");
}
void FakeUciTest::cancellation() {
    QFETCH(QString, name);
    Child child;
    child.fixture(name);
    QTRY_COMPARE(child.process.state(), QProcess::Running);
    QVERIFY(child.send("uci\nisready\nposition startpos\ngo infinite\nisready\nstop\n"));
    QTRY_VERIFY(child.output.contains("info string settled\n"));
    QCOMPARE(child.output.count("readyok"), 2);
    QVERIFY(child.output.contains("bestmove e2e4\n"));
    QCOMPARE(child.output.contains("nodes 999"), name == "late-output");
    if (name == "late-output") {
        QVERIFY(child.output.indexOf("bestmove") < child.output.indexOf("nodes 999"));
    }
    QVERIFY(child.send("quit\n"));
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitCode(), 0);
    QVERIFY(child.errors.isEmpty());
}
void FakeUciTest::silentAndHang_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("closeInput");
    QTest::newRow("silent-eof") << QString("silence") << true;
    QTest::newRow("silent-stop") << QString("silence") << false;
    QTest::newRow("hang-eof") << QString("hang") << true;
    QTest::newRow("hang-terminate") << QString("hang") << false;
}
void FakeUciTest::silentAndHang() {
    QFETCH(QString, name);
    QFETCH(bool, closeInput);
    Child child;
    child.fixture(name);
    QTRY_COMPARE(child.process.state(), QProcess::Running);
    QVERIFY(child.send("uci\ngo infinite\n"));
    QTRY_VERIFY(child.output.contains("uciok\n"));
    if (name == "hang") {
        QTRY_VERIFY(child.output.contains("entering hang"));
    }
    if (closeInput) {
        child.process.closeWriteChannel();
    } else if (name == "silence") {
        QVERIFY(child.send("stop\nquit\n"));
    } else {
        QVERIFY(child.send("stop\nquit\n"));
        // The hang has no exit action; only external termination can end it with stdin open.
        child.process.terminate();
    }
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    if (name == "hang" && !closeInput) {
        QCOMPARE(child.process.exitStatus(), QProcess::CrashExit);
        QVERIFY(!child.output.contains("bestmove"));
    } else {
        QCOMPARE(child.process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.process.exitCode(), 0);
    }
    QVERIFY(child.errors.isEmpty());
}
void FakeUciTest::intentionalCrash() {
    Child child;
    child.fixture("crash");
    QTRY_COMPARE(child.process.state(), QProcess::Running);
    QVERIFY(child.send("uci\ngo nodes 100\n"));
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitStatus(), QProcess::CrashExit);
    QCOMPARE(child.process.exitCode(), SIGABRT);
    QVERIFY(child.errors.contains("fake-uci: intentional crash\n"));
    QVERIFY(child.output.contains("info nodes 10\n"));
    QVERIFY(cleanDiagnostics(child.errors));
}
void FakeUciTest::mismatchAndEof() {
    for (bool eof : {false, true}) {
        Child child;
        child.fixture("analysis");
        QTRY_COMPARE(child.process.state(), QProcess::Running);
        if (eof) {
            QVERIFY(child.send("uci\n"));
            QTRY_VERIFY(child.output.contains("uciok"));
            child.process.closeWriteChannel();
        } else {
            QVERIFY(child.send("isready\n"));
        }
        QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
        QCOMPARE(child.process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.process.exitCode(), eof ? 4 : 3);
        QVERIFY(child.errors.contains(eof ? "step 2" : "step 1"));
        QVERIFY(cleanDiagnostics(child.errors));
    }
}
void FakeUciTest::framingAndBounds() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath("commands.json");
    const auto document = scenario({step("uci", {writeAction("uciok\n")}),
                                    step("position startpos", {writeAction("position accepted\n")}),
                                    step("quit", {exitAction()})});
    QVERIFY(save(path, document));
    Child child;
    child.start({"--scenario", path});
    QTRY_COMPARE(child.process.state(), QProcess::Running);
    QVERIFY(child.send("\t uc"));
    QVERIFY(child.send("i \t\r\n \tposition\t  startpos\rquit\n"));
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitCode(), 0);
    QCOMPARE(child.output, QByteArray("uciok\nposition accepted\n"));
    Child large;
    large.start({"--scenario", path});
    QTRY_COMPARE(large.process.state(), QProcess::Running);
    QVERIFY(large.send(QByteArray(65537, 'x')));
    QTRY_COMPARE(large.process.state(), QProcess::NotRunning);
    QCOMPARE(large.process.exitCode(), 3);
    QVERIFY(large.errors.contains("64 KiB"));
    QVERIFY(large.errors.size() < 1024);
}
void FakeUciTest::backpressureAndExit() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath("output.json");
    const QString payload(512 * 1024, u'x');
    QVERIFY(save(path, scenario({step("go", {writeAction(payload), writeAction("never", 1000),
                                             exitAction()})})));
    Child child;
    child.start({"--scenario", path});
    QTRY_COMPARE(child.process.state(), QProcess::Running);
    QVERIFY(child.send("go\n"));
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitCode(), 0);
    QCOMPARE(child.output, payload.toUtf8());
    QVERIFY(child.errors.isEmpty());
}

void FakeUciTest::limitsAccepted() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath("limits.json");
    const QString command(65536, u'x');
    QJsonArray steps;
    QByteArray input;
    for (int i = 0; i < 4096; ++i) {
        const auto expected = i == 0 ? command : QStringLiteral("next");
        steps.append(
            step(expected, i == 4095 ? QJsonArray{exitAction()} : QJsonArray{writeAction("")}));
        input += expected.toUtf8() + '\n';
    }
    QVERIFY(save(path, scenario(steps)));
    Child child;
    child.start({"--scenario", path});
    QTRY_COMPARE(child.process.state(), QProcess::Running);
    QVERIFY(child.send(input));
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitCode(), 0);
    QVERIFY(child.output.isEmpty());
    QVERIFY(child.errors.isEmpty());
}
void FakeUciTest::pendingInputAndEof() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath("eof.json");
    QVERIFY(save(path, scenario({step("go"), step("quit", {exitAction()})})));
    Child partial;
    partial.start({"--scenario", path});
    QTRY_COMPARE(partial.process.state(), QProcess::Running);
    QVERIFY(partial.send("go\nqui"));
    partial.process.closeWriteChannel();
    QTRY_COMPARE(partial.process.state(), QProcess::NotRunning);
    QCOMPARE(partial.process.exitCode(), 4);

    // An executed exit ignores buffered later commands and cancels future writes.
    QVERIFY(save(path, scenario({step("go", {exitAction(), writeAction("unreachable", 1000)})})));
    Child afterExit;
    afterExit.start({"--scenario", path});
    QTRY_COMPARE(afterExit.process.state(), QProcess::Running);
    QVERIFY(afterExit.send("go\nextra\n"));
    QTRY_COMPARE(afterExit.process.state(), QProcess::NotRunning);
    QCOMPARE(afterExit.process.exitCode(), 0);
    QVERIFY(afterExit.output.isEmpty());

    auto delayedExit = exitAction();
    delayedExit["afterMs"] = 1000;
    QVERIFY(save(path, scenario({step("go", {writeAction("accepted\n"), delayedExit})})));
    Child exhausted;
    exhausted.start({"--scenario", path});
    QTRY_COMPARE(exhausted.process.state(), QProcess::Running);
    QVERIFY(exhausted.send("go\nextra\n"));
    QTRY_COMPARE(exhausted.process.state(), QProcess::NotRunning);
    QCOMPARE(exhausted.process.exitCode(), 3);
    QVERIFY(exhausted.errors.contains("step 2"));
}
void FakeUciTest::independentProcesses() {
    Child first;
    Child second;
    first.fixture("silence");
    second.fixture("missing-wdl");
    QTRY_COMPARE(first.process.state(), QProcess::Running);
    QTRY_COMPARE(second.process.state(), QProcess::Running);
    QVERIFY(first.send("uci\ngo infinite\n"));
    QVERIFY(second.send("uci\nposition startpos\ngo nodes 100\nquit\n"));
    QTRY_VERIFY(first.output.contains("uciok"));
    QTRY_COMPARE(second.process.state(), QProcess::NotRunning);
    QCOMPARE(second.process.exitCode(), 0);
    QCOMPARE(first.process.state(), QProcess::Running);
    QVERIFY(first.send("stop\nquit\n"));
    QTRY_COMPARE(first.process.state(), QProcess::NotRunning);
    QCOMPARE(first.process.exitCode(), 0);
    QVERIFY(first.errors.isEmpty());
    QVERIFY(second.errors.isEmpty());
}

void FakeUciTest::outputFailure() {
    OutputPipe pipe;
    QVERIFY(pipe.valid());
    pipe.closeReader();
    Child child;
    pipe.redirect(child);
    child.fixture("analysis");
    QTRY_COMPARE(child.process.state(), QProcess::Running);
    pipe.closeWriter();
    QVERIFY(child.send("uci\n"));
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.process.exitCode(), 5);
    QVERIFY(child.errors.contains("step 1: output write failed"));
    QVERIFY(cleanDiagnostics(child.errors));
}
void FakeUciTest::ordinarySilenceCannotCompleteConversation() {
    Child child;
    child.fixture("analysis");
    QTRY_COMPARE(child.process.state(), QProcess::Running);
    QVERIFY(child.send("uci\nsetoption name MultiPV value 2\n"));
    child.process.closeWriteChannel();
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitCode(), 4);
    QVERIFY(child.errors.contains("step 3"));
}
void FakeUciTest::sharedFileFlags() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QFile output(root.filePath("shared.log"));
    QVERIFY(output.open(QIODevice::WriteOnly));
    const auto fd = output.handle();
    const auto flags = ::fcntl(fd, F_GETFL);
    QVERIFY(flags != -1);
    Child child;
    child.process.setChildProcessModifier([fd] {
        if (::dup2(fd, STDOUT_FILENO) == -1 || ::dup2(fd, STDERR_FILENO) == -1) {
            ::_exit(120);
        }
    });
    child.fixture("missing-wdl");
    // Unsupported shared files must be rejected before changing inherited file flags.
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(::fcntl(fd, F_GETFL), flags);
    QCOMPARE(child.process.exitCode(), 5);
}
void FakeUciTest::nonregularScenario() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath("scenario.fifo");
    QVERIFY(::mkfifo(QFile::encodeName(path).constData(), 0600) == 0);
    Child child;
    child.start({"--scenario", path});
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitCode(), 2);
    QVERIFY(child.errors.contains("regular file"));
}
void FakeUciTest::actualBackpressure() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath("blocked.json");
    const QString payload(512 * 1024, u'x');
    auto ready = writeAction("ready despite full stdout\n");
    ready["stream"] = "stderr";
    QVERIFY(save(
        path, scenario({step("go", {writeAction(payload), writeAction("must be cancelled", 60000)}),
                        step("isready", {QJsonObject{{"type", "cancelPending"}}, ready}),
                        step("quit", {exitAction()})})));
    OutputPipe pipe;
    QVERIFY(pipe.valid());
    Child child;
    pipe.redirect(child);
    child.start({"--scenario", path});
    QTRY_COMPARE(child.process.state(), QProcess::Running);
    pipe.closeWriter();
    QVERIFY(child.send("go\nisready\n"));
    // No reads from stdout until stderr proves the next command was processed.
    QTRY_VERIFY(child.errors.contains("ready despite full stdout"));
    QCOMPARE(child.process.state(), QProcess::Running);
    QVERIFY(child.send("quit\n"));
    QByteArray received;
    QTRY_VERIFY(pipe.drain(received) && received.size() == payload.size());
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitCode(), 0);
    QCOMPARE(received, payload.toUtf8());
    QCOMPARE(child.errors, QByteArray("ready despite full stdout\n"));
}

void FakeUciTest::mergedPipes() {
    Child child;
    child.process.setProcessChannelMode(QProcess::MergedChannels);
    child.fixture("malformed");
    QTRY_COMPARE(child.process.state(), QProcess::Running);
    QVERIFY(child.send("uci\nisready\nposition startpos\ngo nodes 100\nquit\n"));
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitCode(), 0);
    QVERIFY(child.output.contains("private test stderr\n"));
    QVERIFY(child.output.contains("bestmove z9z9"));
    QVERIFY(child.errors.isEmpty());
}
void FakeUciTest::supplementaryUnicode() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath("unicode.json");
    const auto text = QString::fromUcs4(U"horse 🐴");
    QVERIFY(save(path, scenario({step(text, {writeAction(text), exitAction()})})));
    Child child;
    child.start({"--scenario", path});
    QTRY_COMPARE(child.process.state(), QProcess::Running);
    QVERIFY(child.send(text.toUtf8() + '\n'));
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitCode(), 0);
    QCOMPARE(child.output, text.toUtf8());
    QVERIFY(child.errors.isEmpty());
}

void FakeUciTest::maximumPendingActions() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto path = root.filePath("pending.json");
    QJsonArray actions;
    for (int i = 0; i < 4095; ++i) {
        actions.append(writeAction("must not emit", 60000));
    }
    QJsonArray steps{step("go", actions)};
    QByteArray input("go\n");
    for (int i = 0; i < 4094; ++i) {
        steps.append(step("next"));
        input += "next\n";
    }
    steps.append(step("quit", {exitAction()}));
    input += "quit\n";
    QVERIFY(save(path, scenario(steps)));
    Child child;
    child.start({"--scenario", path});
    QTRY_COMPARE(child.process.state(), QProcess::Running);
    QVERIFY(child.send(input));
    QTRY_COMPARE(child.process.state(), QProcess::NotRunning);
    QCOMPARE(child.process.exitCode(), 0);
    QVERIFY(child.output.isEmpty());
    QVERIFY(child.errors.isEmpty());
}

int main(int argc, char** argv) {
    TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QCoreApplication application(argc, argv);
    FakeUciTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "FakeUciTest.moc"
