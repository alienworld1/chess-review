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
#include <functional>
#include <memory>
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
    void writeFailure();
    void rotationFailure();
};

void DiagnosticsTest::structuredRecordsAndFiltering() {
    QTemporaryDir directory;
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
    Logger logger(directory.path(), true);
    const auto cleanup = qScopeGuard([&] { stop(logger); });
    logger.record(Event::SchemaChecked, {}, Severity::Debug);
    stop(logger);
    QVERIFY(read(directory.filePath(QStringLiteral("events-0.jsonl"))).contains("schema.checked"));
}

void DiagnosticsTest::concurrentSubmission() {
    QTemporaryDir directory;
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
    QProcess child;
    child.setWorkingDirectory(directory.path());
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--fatal-probe"), directory.path()});
    QVERIFY(child.waitForFinished(5000));
    QCOMPARE(child.exitStatus(), QProcess::CrashExit);
#else
    QSKIP("Fatal subprocess validation uses Unix core-dump suppression");
#endif
}

void DiagnosticsTest::frameworkPrivacyAndHandlerLifetime() {
    QTemporaryDir directory;
    const auto old = qInstallMessageHandler(previous);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(old); });
    Logger logger(directory.path(), true);
    const auto cleanup = qScopeGuard([&] { stop(logger); });
    logger.installQtHandler();
    const QString secret =
        QString::fromUtf8("/home/private/棋\nhttps://personal.example/game [Event \"PGN\"]\x01");
    QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO, "qt.sql.private-secret")
        .warning("%s", qPrintable(secret));
    QMessageLogger(__FILE__, __LINE__, Q_FUNC_INFO, "arbitrary-private-category")
        .warning("%s", qPrintable(secret));
    const auto report = logger.report();
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
    QVERIFY(logger.report().contains(QStringLiteral("session.started")));
    QVERIFY(!QFileInfo::exists(QStringLiteral("relative/logs")));
}

void DiagnosticsTest::lockContention() {
    QTemporaryDir directory;
    Logger first(directory.path());
    const auto cleanupFirst = qScopeGuard([&] { stop(first); });
    QTRY_COMPARE(first.health(), Health::Active);
    Logger second(directory.path());
    const auto cleanupSecond = qScopeGuard([&] { stop(second); });
    QTRY_COMPARE(second.health(), Health::MemoryOnly);
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
    QCOMPARE(records(logger->report().section('\n', 5).toUtf8()).size(), 200);
}

void DiagnosticsTest::writeFailure() {
    QTemporaryDir directory;
    auto logger = LoggerTestAccess::create(directory.path(), [] { return false; });
    const auto cleanup = qScopeGuard([&] { stop(*logger); });
    QTRY_COMPARE(logger->health(), Health::MemoryOnly);
    QVERIFY(logger->report().contains(QStringLiteral("session.started")));
}

void DiagnosticsTest::rotationFailure() {
    QTemporaryDir directory;
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
        logger.installQtHandler();
        qFatal("private fatal probe");
    }
#endif
    DiagnosticsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "DiagnosticsTest.moc"
