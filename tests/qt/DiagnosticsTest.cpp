#include "TestEnvironment.h"

#include "diagnostics/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QProcess>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef Q_OS_UNIX
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace chess_review::diagnostics;

namespace chess_review::diagnostics {
// Deliberately private: production has no queue control or arbitrary sink API.
class LoggerTestAccess {
  public:
    static std::unique_ptr<Logger> create(const QString& directory,
                                          std::function<bool()> beforeWrite) {
        return std::unique_ptr<Logger>(
            new Logger(directory, false, std::move(beforeWrite), nullptr));
    }
};
} // namespace chess_review::diagnostics

namespace {
void stop(Logger& logger) {
    if (logger.property("testClosed").toBool()) {
        return;
    }
    QEventLoop loop;
    bool completed = false;
    QObject::connect(&logger, &Logger::closed, &loop, [&] {
        completed = true;
        loop.quit();
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(5000);
    logger.close();
    loop.exec();
    if (!completed) {
        qFatal("Test logger failed to stop");
    }
    logger.setProperty("testClosed", true);
}
QByteArray read(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}
bool write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QList<QJsonObject> records(const QByteArray& bytes) {
    QList<QJsonObject> result;
    for (const auto& line : bytes.split('\n')) {
        if (line.isEmpty()) {
            continue;
        }
        const auto document = QJsonDocument::fromJson(line);
        if (!document.isObject()) {
            return {};
        }
        result.push_back(document.object());
    }
    return result;
}
std::atomic<int> restoredCalls{0};
void previous(QtMsgType, const QMessageLogContext&, const QString&) { ++restoredCalls; }
} // namespace

class DiagnosticsTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { QTest::failOnWarning(); }
    void structuredRecordsAndFiltering();
    void debugMode();
    void concurrentSubmission();
    void fatalStillTerminates();
    void frameworkPrivacyAndHandlerLifetime();
    void unavailableDirectory_data();
    void unavailableDirectory();
    void lockContention();
    void rotation();
    void incompleteTailRecovery();
    void unsafeTargets();
    void permissionDenied();
    void boundedQueueAndSlowWriter();
    void recentHistorySurvivesFullQueue();
    void repeatedHandlerInstallation();
    void oversizedLockMetadata();
    void cleanupLockIsNotParsed();
    void handlerOwnership();
    void lossCountSurvivesHistoryEviction();
    void writeFailure();
    void rotationFailure();
};

void DiagnosticsTest::structuredRecordsAndFiltering() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    Logger logger(directory.path());
    const auto cleanup = qScopeGuard([&] { stop(logger); });
    QTRY_COMPARE(logger.health(), Health::Active);
    Context context;
    context.schemaVersion = 1;
    context.elapsedMs = 42;
    logger.record(Event::StorageReady, context);
    logger.record(Event::SchemaChecked, context, Severity::Debug);
    stop(logger);
    const auto content = read(directory.filePath(QStringLiteral("events-0.jsonl")));
    const auto entries = records(content);
    QCOMPARE(entries.size(), 3);
    QCOMPARE(entries[1].value(QStringLiteral("event")).toString(), QStringLiteral("storage.ready"));
    QCOMPARE(entries[1]
                 .value(QStringLiteral("context"))
                 .toObject()
                 .value(QStringLiteral("elapsed_ms"))
                 .toInt(),
             42);
    quint64 sequence = 0;
    QString session;
    for (const auto& entry : entries) {
        QCOMPARE(entry.value(QStringLiteral("format_version")).toInt(), 1);
        QVERIFY(entry.value(QStringLiteral("timestamp")).toString().endsWith('Z'));
        QVERIFY(!entry.value(QStringLiteral("severity")).toString().isEmpty());
        QVERIFY(!entry.value(QStringLiteral("category")).toString().isEmpty());
        QCOMPARE(entry.value(QStringLiteral("sequence")).toString().toULongLong(), ++sequence);
        if (session.isEmpty()) {
            session = entry.value(QStringLiteral("session_id")).toString();
        }
        QVERIFY(!session.isEmpty());
        QCOMPARE(entry.value(QStringLiteral("session_id")).toString(), session);
        QVERIFY(QJsonDocument(entry).toJson(QJsonDocument::Compact).size() < 8192);
    }
    QVERIFY(entries.front().contains(QStringLiteral("app_version")));
    QCOMPARE(entries.back().value(QStringLiteral("event")).toString(),
             QStringLiteral("session.stopped"));
#ifdef Q_OS_UNIX
    QCOMPARE(QFile::permissions(directory.filePath(QStringLiteral("events-0.jsonl"))) &
                 (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther |
                  QFileDevice::WriteOther),
             QFileDevice::Permissions{});
#endif
}

void DiagnosticsTest::debugMode() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    Logger logger(directory.path(), true);
    const auto cleanup = qScopeGuard([&] { stop(logger); });
    logger.record(Event::SchemaChecked, {}, Severity::Debug);
    stop(logger);
    QVERIFY(read(directory.filePath(QStringLiteral("events-0.jsonl"))).contains("schema.checked"));
}

void DiagnosticsTest::concurrentSubmission() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    Logger logger(directory.path());
    const auto cleanup = qScopeGuard([&] { stop(logger); });
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&] {
            for (int event = 0; event < 200; ++event) {
                logger.record(Event::StorageStarted);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    stop(logger);
    const auto entries = records(read(directory.filePath(QStringLiteral("events-0.jsonl"))));
    QCOMPARE(entries.size(), 802);
    quint64 expected = 0;
    for (const auto& entry : entries) {
        QCOMPARE(entry.value(QStringLiteral("sequence")).toString().toULongLong(), ++expected);
    }
}

void DiagnosticsTest::fatalStillTerminates() {
#ifdef Q_OS_UNIX
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    QProcess child;
    child.setWorkingDirectory(directory.path());
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--fatal-probe"), directory.path()});
    QVERIFY(child.waitForFinished(5000));
    const auto errorOutput = child.readAllStandardError();
    QCOMPARE(child.exitStatus(), QProcess::CrashExit);
    QCOMPARE(child.exitCode(), SIGABRT);
    QVERIFY2(errorOutput.contains("fatal-probe-ready"), errorOutput.constData());
    QVERIFY2(!errorOutput.contains("Sanitizer") && !errorOutput.contains("runtime error:"),
             errorOutput.constData());
#else
    QSKIP("Fatal subprocess validation uses Unix core-dump suppression");
#endif
}

void DiagnosticsTest::frameworkPrivacyAndHandlerLifetime() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const auto old = qInstallMessageHandler(previous);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(old); });
    Logger logger(directory.path(), true);
    const auto cleanup = qScopeGuard([&] { stop(logger); });
    QVERIFY(logger.installQtHandler());
    const QString secret =
        QString::fromUtf8("/home/private/棋\nhttps://personal.example/game [Event \"PGN\"]\x01");
    QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO, "qt.sql.private-secret")
        .warning("%s", qPrintable(secret));
    QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO, "arbitrary-private-category")
        .warning("%s", qPrintable(secret));
    QByteArray longCategory("qt.widgets.");
    longCategory += QByteArray(2 * 1024 * 1024, 'x');
    QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO, longCategory.constData())
        .warning("%s", qPrintable(secret));
    const auto report = logger.report();
    QVERIFY(report.size() < 8192);
    QVERIFY(!report.contains(QStringLiteral("private")));
    QVERIFY(!report.contains(QStringLiteral("PGN")));
    stop(logger);
    const auto bytes = read(directory.filePath(QStringLiteral("events-0.jsonl")));
    QVERIFY(!bytes.contains("private"));
    QVERIFY(!bytes.contains("PGN"));
    QVERIFY(bytes.contains("qt.sql"));
    QVERIFY(bytes.contains("other"));
    const int before = restoredCalls.load();
    qWarning("handler restoration probe");
    QCOMPARE(restoredCalls.load(), before + 1);
}

void DiagnosticsTest::unavailableDirectory_data() {
    QTest::addColumn<QString>("directory");
    QTest::newRow("empty") << QString();
    QTest::newRow("relative") << QStringLiteral("relative/logs");
    QTest::newRow("nul") << (QDir::tempPath() + QChar::Null + QStringLiteral("/logs"));
}
void DiagnosticsTest::unavailableDirectory() {
    QFETCH(QString, directory);
    Logger logger(directory);
    const auto cleanup = qScopeGuard([&] { stop(logger); });
    QTRY_COMPARE(logger.health(), Health::MemoryOnly);
    QCOMPARE(logger.failure(), LogFailure::Location);
    QVERIFY(logger.report().contains(QStringLiteral("logging.location")));
    QVERIFY(logger.report().contains(QStringLiteral("session.started")));
    QVERIFY(!QFileInfo::exists(QStringLiteral("relative/logs")));
}

void DiagnosticsTest::lockContention() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    Logger first(directory.path());
    const auto cleanupFirst = qScopeGuard([&] { stop(first); });
    QTRY_COMPARE(first.health(), Health::Active);
    Logger second(directory.path());
    const auto cleanupSecond = qScopeGuard([&] { stop(second); });
    QTRY_COMPARE(second.health(), Health::MemoryOnly);
    QCOMPARE(second.failure(), LogFailure::Busy);
    QVERIFY(second.report().contains(QStringLiteral("logging.busy")));
    stop(second);
    QProcess child;
    child.setWorkingDirectory(directory.path());
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--logging-probe"), directory.path()});
    QVERIFY(child.waitForFinished(5000));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 2);
    stop(first);
    const auto entries = records(read(directory.filePath(QStringLiteral("events-0.jsonl"))));
    QCOMPARE(entries.size(), 2);
}

void DiagnosticsTest::rotation() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    // Exactly one complete JSON line at the size boundary; rotation must occur
    // before the next record, leaving unrelated files untouched.
    QByteArray full("{\"padding\":\"");
    full += QByteArray(2 * 1024 * 1024 - full.size() - 3, 'x');
    full += "\"}\n";
    QCOMPARE(full.size(), 2 * 1024 * 1024);
    for (int index = 0; index < 5; ++index) {
        QVERIFY(write(directory.filePath(QStringLiteral("events-%1.jsonl").arg(index)), full));
    }
    QVERIFY(write(directory.filePath(QStringLiteral("keep.txt")), "keep"));
    Logger logger(directory.path());
    const auto cleanup = qScopeGuard([&] { stop(logger); });
    QTRY_COMPARE(logger.health(), Health::Active);
    stop(logger);
    const QDir logs(directory.path());
    QCOMPARE(logs.entryList({QStringLiteral("events-*.jsonl")}, QDir::Files).size(), 5);
    for (int index = 0; index < 5; ++index) {
        const auto path = directory.filePath(QStringLiteral("events-%1.jsonl").arg(index));
        QVERIFY(QFileInfo(path).size() <= 2 * 1024 * 1024);
        QVERIFY(!records(read(path)).isEmpty());
    }
    QCOMPARE(read(directory.filePath(QStringLiteral("keep.txt"))), QByteArray("keep"));
}

void DiagnosticsTest::incompleteTailRecovery() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("events-0.jsonl"));
    const QByteArray complete("{\"event\":\"old.complete\"}\n");
    QVERIFY(write(path, complete + "{\"event\":\"incomplete"));
    Logger logger(directory.path());
    const auto cleanup = qScopeGuard([&] { stop(logger); });
    QTRY_COMPARE(logger.health(), Health::Active);
    stop(logger);
    const auto bytes = read(path);
    QVERIFY(bytes.startsWith(complete));
    QVERIFY(!bytes.contains("incomplete"));
    QCOMPARE(records(bytes).size(), 3);
}

void DiagnosticsTest::unsafeTargets() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString log = directory.filePath(QStringLiteral("events-0.jsonl"));
    QVERIFY(QDir().mkdir(log));
    {
        Logger logger(directory.path());
        const auto cleanup = qScopeGuard([&] { stop(logger); });
        QTRY_COMPARE(logger.health(), Health::MemoryOnly);
    }
    QVERIFY(QDir().rmdir(log));
#ifdef Q_OS_UNIX
    const QString target = directory.filePath(QStringLiteral("private.txt"));
    QVERIFY(write(target, "private contents"));
    QVERIFY(QFile::link(target, log));
    {
        Logger logger(directory.path());
        const auto cleanup = qScopeGuard([&] { stop(logger); });
        QTRY_COMPARE(logger.health(), Health::MemoryOnly);
    }
    QCOMPARE(read(target), QByteArray("private contents"));
    QVERIFY(QFile::remove(log));
    QVERIFY(::link(QFile::encodeName(target).constData(), QFile::encodeName(log).constData()) == 0);
    {
        Logger logger(directory.path());
        const auto cleanup = qScopeGuard([&] { stop(logger); });
        QTRY_COMPARE(logger.health(), Health::MemoryOnly);
    }
    QCOMPARE(read(target), QByteArray("private contents"));
    QVERIFY(QFile::remove(log));
    QVERIFY(::mkfifo(QFile::encodeName(log).constData(), 0600) == 0);
    {
        Logger logger(directory.path());
        const auto cleanup = qScopeGuard([&] { stop(logger); });
        QTRY_COMPARE(logger.health(), Health::MemoryOnly);
    }
#endif
}

void DiagnosticsTest::permissionDenied() {
#ifdef Q_OS_UNIX
    if (::geteuid() == 0) {
        QSKIP("Root bypasses directory permissions");
    }
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    QVERIFY(
        QFile::setPermissions(directory.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const auto restore = qScopeGuard([&] {
        if (!QFile::setPermissions(directory.path(), QFileDevice::ReadOwner |
                                                         QFileDevice::WriteOwner |
                                                         QFileDevice::ExeOwner)) {
            qFatal("Cannot restore temporary directory permissions");
        }
    });
    Logger logger(directory.path());
    const auto cleanup = qScopeGuard([&] { stop(logger); });
    QTRY_COMPARE(logger.health(), Health::MemoryOnly);
#else
    QSKIP("Permission denial fixture uses Unix directory permissions");
#endif
}

void DiagnosticsTest::boundedQueueAndSlowWriter() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    QSemaphore entered;
    QSemaphore release;
    std::atomic<bool> first{true};
    auto logger = LoggerTestAccess::create(directory.path(), [&] {
        if (first.exchange(false)) {
            entered.release();
            release.acquire();
        }
        return true;
    });
    const auto cleanup = qScopeGuard([&] {
        release.release();
        stop(*logger);
    });
    QTRY_VERIFY(entered.available() > 0);
    QElapsedTimer elapsed;
    elapsed.start();
    for (int index = 0; index < 5000; ++index) {
        logger->record(Event::StorageStarted);
    }
    QVERIFY(elapsed.elapsed() < 2000);
    QVERIFY(logger->report().contains(QStringLiteral("Pending dropped events:")));
    QTimer heartbeat;
    QSignalSpy pulses(&heartbeat, &QTimer::timeout);
    heartbeat.start(0);
    QTRY_VERIFY(pulses.count() > 0);
    // close() must also return while the sink is blocked.
    logger->close();
    QCOMPARE(logger->health(), Health::Active);
    release.release();
    stop(*logger);
    const auto entries = records(read(directory.filePath(QStringLiteral("events-0.jsonl"))));
    QVERIFY(entries.size() <= 1027);
    quint64 dropped = 0;
    quint64 lastSequence = 0;
    for (const auto& entry : entries) {
        const auto sequence = entry.value(QStringLiteral("sequence")).toString().toULongLong();
        QVERIFY(sequence > lastSequence);
        lastSequence = sequence;
        if (entry.value(QStringLiteral("event")).toString() ==
            QStringLiteral("logging.events_dropped")) {
            dropped += entry.value(QStringLiteral("context"))
                           .toObject()
                           .value(QStringLiteral("count"))
                           .toString()
                           .toULongLong();
        }
    }
    QVERIFY(dropped >= 3976);
    QByteArray history;
    for (const auto& line : logger->report().split('\n')) {
        if (line.startsWith('{')) {
            history += line.toUtf8() + '\n';
        }
    }
    QCOMPARE(records(history).size(), 200);
    QVERIFY(logger->report().contains(QStringLiteral("Dropped disk events (queue overflow):")));
}

void DiagnosticsTest::recentHistorySurvivesFullQueue() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    QSemaphore entered;
    QSemaphore release;
    std::atomic<bool> first{true};
    auto logger = LoggerTestAccess::create(directory.path(), [&] {
        if (first.exchange(false)) {
            entered.release();
            release.acquire();
        }
        return true;
    });
    const auto cleanup = qScopeGuard([&] {
        release.release();
        stop(*logger);
    });
    QTRY_VERIFY(entered.available() > 0);
    for (int index = 0; index < 1500; ++index) {
        logger->record(Event::StorageStarted);
    }
    Context failure;
    failure.errorCode = 6;
    failure.migrationVersion = 42;
    logger->record(Event::StorageFailed, failure, Severity::Error);
    // Memory diagnostics must retain the latest failure even while disk is stalled.
    QVERIFY(logger->report().contains(QStringLiteral("storage.failed")));
    QVERIFY(logger->report().contains(QStringLiteral("\"migration_version\":42")));
}

void DiagnosticsTest::repeatedHandlerInstallation() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    QProcess child;
    child.setWorkingDirectory(directory.path());
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--handler-probe"), directory.path()});
    QVERIFY(child.waitForFinished(5000));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 0);
}

void DiagnosticsTest::oversizedLockMetadata() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("writer.lock"));
    const QByteArray oversized(16 * 1024 + 1, 'x');
    QVERIFY(write(path, oversized));
    Logger logger(directory.path());
    const auto cleanup = qScopeGuard([&] { stop(logger); });
    QTRY_COMPARE(logger.health(), Health::MemoryOnly);
    QCOMPARE(logger.failure(), LogFailure::UnsafeTarget);
    QCOMPARE(read(path), oversized);
    QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("events-0.jsonl"))));
}

void DiagnosticsTest::cleanupLockIsNotParsed() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    const QString path = directory.filePath(QStringLiteral("writer.lock.rmlock"));
    const QByteArray oversized(20 * 1024, 'x');
    QVERIFY(write(path, oversized));
    Logger logger(directory.path());
    const auto cleanup = qScopeGuard([&] { stop(logger); });
    QTRY_COMPARE(logger.health(), Health::MemoryOnly);
    QCOMPARE(logger.failure(), LogFailure::Busy);
    QCOMPARE(read(path), oversized);
}

void DiagnosticsTest::handlerOwnership() {
    QTemporaryDir firstDirectory;
    QVERIFY2(firstDirectory.isValid(), qPrintable(firstDirectory.errorString()));
    QTemporaryDir secondDirectory;
    QVERIFY2(secondDirectory.isValid(), qPrintable(secondDirectory.errorString()));
    const auto old = qInstallMessageHandler(previous);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(old); });
    Logger first(firstDirectory.path());
    const auto firstCleanup = qScopeGuard([&] { stop(first); });
    Logger second(secondDirectory.path());
    const auto secondCleanup = qScopeGuard([&] { stop(second); });
    QVERIFY(first.installQtHandler());
    QVERIFY(!second.installQtHandler());
    stop(second); // A non-owner must not remove the first logger's handler.
    const int before = restoredCalls.load();
    qWarning("private ownership probe");
    QCOMPARE(restoredCalls.load(), before);
    QVERIFY(first.report().contains(QStringLiteral("qt.message")));
    stop(first);
    QVERIFY(!first.installQtHandler());
    qWarning("handler restoration probe");
    QCOMPARE(restoredCalls.load(), before + 1);
}

void DiagnosticsTest::lossCountSurvivesHistoryEviction() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    QSemaphore entered;
    QSemaphore release;
    std::atomic<bool> first{true};
    auto logger = LoggerTestAccess::create(directory.path(), [&] {
        if (first.exchange(false)) {
            entered.release();
            release.acquire();
        }
        return true;
    });
    const auto cleanup = qScopeGuard([&] {
        release.release();
        stop(*logger);
    });
    QTRY_VERIFY(entered.available() > 0);
    for (int index = 0; index < 1500; ++index) {
        logger->record(Event::StorageStarted);
    }
    release.release();
    QTRY_VERIFY(!logger->report().contains(QStringLiteral("Pending dropped events:")));
    for (int index = 0; index < 250; ++index) {
        logger->record(Event::StorageReady);
    }
    stop(*logger);
    const auto report = logger->report();
    QVERIFY(!report.contains(QStringLiteral("logging.events_dropped")));
    QVERIFY(report.contains(QStringLiteral("Dropped disk events (queue overflow): 476")));
}

void DiagnosticsTest::writeFailure() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    auto logger = LoggerTestAccess::create(directory.path(), [] { return false; });
    const auto cleanup = qScopeGuard([&] { stop(*logger); });
    QTRY_COMPARE(logger->health(), Health::MemoryOnly);
    QCOMPARE(logger->failure(), LogFailure::Write);
    QVERIFY(logger->report().contains(QStringLiteral("logging.write")));
    QVERIFY(logger->report().contains(QStringLiteral("session.started")));
    stop(*logger);
    QCOMPARE(logger->failure(), LogFailure::Write);
}

void DiagnosticsTest::rotationFailure() {
    QTemporaryDir directory;
    QVERIFY2(directory.isValid(), qPrintable(directory.errorString()));
    QVERIFY(write(directory.filePath(QStringLiteral("events-0.jsonl")),
                  QByteArray(2 * 1024 * 1024, '\n')));
    QSemaphore entered;
    QSemaphore release;
    std::atomic<bool> first{true};
    auto logger = LoggerTestAccess::create(directory.path(), [&] {
        if (first.exchange(false)) {
            entered.release();
            release.acquire();
        }
        return true;
    });
    const auto cleanup = qScopeGuard([&] {
        release.release();
        stop(*logger);
    });
    QTRY_VERIFY(entered.available() > 0);
    QVERIFY(QDir().mkdir(directory.filePath(QStringLiteral("events-4.jsonl"))));
    release.release();
    QTRY_COMPARE(logger->health(), Health::MemoryOnly);
    QVERIFY(logger->report().contains(QStringLiteral("session.started")));
}

int main(int argc, char* argv[]) {
    // Internal child probes inherit the parent's isolated environment and cwd.
    // A fatal child must not allocate a new root that abort() cannot clean up.
    const bool isProbe = argc == 3 && (std::string_view{argv[1]} == "--logging-probe" ||
                                       std::string_view{argv[1]} == "--fatal-probe" ||
                                       std::string_view{argv[1]} == "--handler-probe");
    std::optional<TestEnvironment> environment;
    if (!isProbe) {
        environment.emplace();
        if (!environment->isValid()) {
            return 1;
        }
    }
    QCoreApplication application(argc, argv);
    if (application.arguments().size() == 3 &&
        application.arguments()[1] == QStringLiteral("--logging-probe")) {
        Logger logger(application.arguments()[2]);
        int result = 1;
        const auto update = [&] {
            if (logger.health() == Health::MemoryOnly || logger.health() == Health::Active) {
                result = logger.health() == Health::MemoryOnly ? 2 : 0;
                logger.close();
            }
        };
        QObject::connect(&logger, &Logger::healthChanged, &application, update);
        QObject::connect(&logger, &Logger::closed, &application, &QCoreApplication::quit);
        update(); // The writer may have initialized before the signal connection.
        application.exec();
        return result;
    }
#ifdef Q_OS_UNIX
    if (application.arguments().size() == 3 &&
        application.arguments()[1] == QStringLiteral("--fatal-probe")) {
        const struct rlimit limit {
            0, 0
        };
        if (::setrlimit(RLIMIT_CORE, &limit) != 0) {
            return 1;
        }
        Logger logger(application.arguments()[2]);
        if (!logger.installQtHandler()) {
            stop(logger);
            return 3;
        }
        if (std::fputs("fatal-probe-ready\n", stderr) == EOF || std::fflush(stderr) != 0) {
            stop(logger);
            return 4;
        }
        qFatal("private fatal probe");
    }
#endif
    if (application.arguments().size() == 3 &&
        application.arguments()[1] == QStringLiteral("--handler-probe")) {
#ifdef Q_OS_UNIX
        const struct rlimit limit {
            0, 0
        };
        if (::setrlimit(RLIMIT_CORE, &limit) != 0) {
            return 1;
        }
#endif
        const auto old = qInstallMessageHandler(previous);
        Logger logger(application.arguments()[2]);
        if (!logger.installQtHandler() || !logger.installQtHandler()) {
            stop(logger);
            return 3;
        }
        stop(logger);
        const int before = restoredCalls.load();
        qWarning("restoration probe");
        qInstallMessageHandler(old);
        return restoredCalls.load() == before + 1 ? 0 : 2;
    }
    DiagnosticsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "DiagnosticsTest.moc"
