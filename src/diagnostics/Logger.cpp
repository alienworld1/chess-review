#include "diagnostics/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QWaitCondition>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <utility>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace chess_review::diagnostics {
namespace {
constexpr qsizetype maxRecordBytes = 8192;
constexpr std::size_t queueLimit = 1024;
constexpr std::size_t historyLimit = 200;
constexpr qint64 fileLimit = 2 * 1024 * 1024;
QMutex bridgeMutex;
Logger* bridgeLogger = nullptr; // Scoped Qt callback bridge, never a service locator.
QtMessageHandler previousHandler = nullptr;

const char* eventName(Event event) {
    switch (event) {
    case Event::SessionStarted:
        return "session.started";
    case Event::SessionStopped:
        return "session.stopped";
    case Event::StorageStarted:
        return "storage.started";
    case Event::StorageReady:
        return "storage.ready";
    case Event::StorageFailed:
        return "storage.failed";
    case Event::StorageClosed:
        return "storage.closed";
    case Event::SchemaChecked:
        return "schema.checked";
    case Event::BackupStarted:
        return "backup.started";
    case Event::BackupVerified:
        return "backup.verified";
    case Event::MigrationStarted:
        return "migration.started";
    case Event::MigrationCompleted:
        return "migration.completed";
    case Event::QtMessage:
        return "qt.message";
    case Event::LoggingFailed:
        return "logging.failed";
    case Event::QtCaptureUnavailable:
        return "logging.qt_capture_unavailable";
    case Event::EngineValidationStarted:
        return "engine.validation_started";
    case Event::EngineValidationSucceeded:
        return "engine.validation_succeeded";
    case Event::EngineValidationFailed:
        return "engine.validation_failed";
    case Event::EngineRegistrationFinished:
        return "engine.registration_finished";
    case Event::UiStateFailed:
        return "ui_state.failed";
    case Event::EventsDropped:
        return "logging.events_dropped";
    }
    Q_UNREACHABLE();
}

const char* severityName(Severity severity) {
    switch (severity) {
    case Severity::Debug:
        return "debug";
    case Severity::Info:
        return "info";
    case Severity::Warning:
        return "warning";
    case Severity::Error:
        return "error";
    case Severity::Fatal:
        return "fatal";
    }
    Q_UNREACHABLE();
}

QString healthName(Health health) {
    switch (health) {
    case Health::Starting:
        return QStringLiteral("Starting local logging");
    case Health::Active:
        return QStringLiteral("Local logging active");
    case Health::MemoryOnly:
        return QStringLiteral("Local logs unavailable; recent diagnostics are held in memory only");
    case Health::Closed:
        return QStringLiteral("Local logging closed");
    }
    Q_UNREACHABLE();
}

bool safeFile(const QString& path) {
    const QFileInfo information(path);
    if (information.isSymLink() || (information.exists() && !information.isFile())) {
        return false;
    }
#ifdef Q_OS_UNIX
    if (information.exists()) {
        struct stat metadata {};
        const auto encoded = QFile::encodeName(path);
        if (::lstat(encoded.constData(), &metadata) != 0 || metadata.st_nlink != 1) {
            return false;
        }
    }
#endif
    return true;
}

// The sink is used only by the writer, including opening, rotation and destruction.
class FileSink {
  public:
    explicit FileSink(const QString& directory) : directory_(directory) {}
    LogFailure open() {
        if (!QDir::isAbsolutePath(directory_) || directory_.contains(QChar::Null) ||
            QFileInfo(directory_).isSymLink() || !QDir().mkpath(directory_)) {
            return LogFailure::Location;
        }
        const QString lockPath = QDir(directory_).filePath(QStringLiteral("writer.lock"));
        // Qt reads lock metadata with unbounded readLine(). Bound the file before
        // handing it to Qt, and avoid traversing a pre-existing stale-cleanup lock.
        if (!safeFile(lockPath) || QFileInfo(lockPath).size() > 16 * 1024) {
            return LogFailure::UnsafeTarget;
        }
        const QFileInfo cleanupLock(lockPath + QStringLiteral(".rmlock"));
        if (cleanupLock.exists() || cleanupLock.isSymLink()) {
            return LogFailure::Busy;
        }
        lock_ = std::make_unique<QLockFile>(lockPath);
        lock_->setStaleLockTime(0);
        if (!lock_->tryLock(0)) {
            return lock_->error() == QLockFile::LockFailedError ? LogFailure::Busy
                                                                : LogFailure::Open;
        }
        for (int index = 0; index < 5; ++index) {
            if (!safeFile(path(index)) || QFileInfo(path(index)).size() > fileLimit) {
                return LogFailure::UnsafeTarget;
            }
            if (QFileInfo::exists(path(index)) &&
                !QFile::setPermissions(path(index),
                                       QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
                return LogFailure::Open;
            }
        }
        return openCurrent();
    }
    LogFailure write(const QByteArray& record) {
        if (file_.size() + record.size() > fileLimit) {
            if (const auto failure = rotate(); failure != LogFailure::None) {
                return failure;
            }
        }
        return file_.write(record) == record.size() && file_.flush() ? LogFailure::None
                                                                     : LogFailure::Write;
    }

  private:
    QString path(int index) const {
        return QDir(directory_).filePath(QStringLiteral("events-%1.jsonl").arg(index));
    }
    LogFailure openCurrent() {
        const QString current = path(0);
        if (!safeFile(current)) {
            return LogFailure::UnsafeTarget;
        }
        file_.setFileName(current);
        const auto mode = QFileInfo::exists(current) ? QIODevice::ReadWrite
                                                     : QIODevice::ReadWrite | QIODevice::NewOnly;
        if (!file_.open(mode, QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
            !file_.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            return LogFailure::Open;
        }
        const qint64 size = file_.size();
        const qint64 tailStart = std::max(qint64{0}, size - maxRecordBytes);
        if (!file_.seek(tailStart)) {
            return LogFailure::Open;
        }
        const auto tail = file_.read(maxRecordBytes);
        if (file_.error() != QFileDevice::NoError) {
            return LogFailure::Open;
        }
        if (!tail.isEmpty() && !tail.endsWith('\n')) {
            const auto newline = tail.lastIndexOf('\n');
            if (newline < 0 && tailStart != 0) {
                return LogFailure::UnsafeTarget;
            }
            if (!file_.resize(tailStart + newline + 1)) {
                return LogFailure::Open;
            }
        }
        return file_.seek(file_.size()) ? LogFailure::None : LogFailure::Open;
    }
    LogFailure rotate() {
        file_.close();
        for (int index = 0; index < 5; ++index) {
            if (!safeFile(path(index))) {
                return LogFailure::UnsafeTarget;
            }
        }
        if (QFileInfo::exists(path(4)) && !QFile::remove(path(4))) {
            return LogFailure::Rotation;
        }
        for (int index = 3; index >= 0; --index) {
            if (QFileInfo::exists(path(index)) && !QFile::rename(path(index), path(index + 1))) {
                return LogFailure::Rotation;
            }
        }
        const auto result = openCurrent();
        return result == LogFailure::Open ? LogFailure::Rotation : result;
    }
    QString directory_;
    std::unique_ptr<QLockFile> lock_;
    QFile file_;
};
} // namespace

QString loggingFailureCode(LogFailure failure) {
    switch (failure) {
    case LogFailure::None:
        return {};
    case LogFailure::Location:
        return QStringLiteral("logging.location");
    case LogFailure::UnsafeTarget:
        return QStringLiteral("logging.unsafe_target");
    case LogFailure::Busy:
        return QStringLiteral("logging.busy");
    case LogFailure::Open:
        return QStringLiteral("logging.open");
    case LogFailure::Write:
        return QStringLiteral("logging.write");
    case LogFailure::Rotation:
        return QStringLiteral("logging.rotation");
    }
    return QStringLiteral("logging.unknown");
}

struct Logger::Impl final : QThread {
    Impl(Logger& logger, QString directory, bool debug, std::function<bool()> writeHook)
        : owner(logger), beforeWrite(std::move(writeHook)), logDirectory(std::move(directory)),
          debugEnabled(debug), session(QUuid::createUuid().toString(QUuid::Id128)) {}

    QByteArray encode(Event event, const Context& context, Severity severity) {
        QJsonObject fields;
        if (context.qtCategory) {
            const char* name = "other";
            switch (*context.qtCategory) {
            case QtCategory::Default:
                name = "default";
                break;
            case QtCategory::Core:
                name = "qt.core";
                break;
            case QtCategory::Gui:
                name = "qt.gui";
                break;
            case QtCategory::Widgets:
                name = "qt.widgets";
                break;
            case QtCategory::Sql:
                name = "qt.sql";
                break;
            case QtCategory::Other:
                break;
            }
            fields.insert(QStringLiteral("qt_category"), QString::fromLatin1(name));
        }
        if (context.schemaVersion) {
            fields.insert(QStringLiteral("schema_version"), *context.schemaVersion);
        }
        if (context.migrationVersion) {
            fields.insert(QStringLiteral("migration_version"), *context.migrationVersion);
        }
        if (context.errorCode) {
            fields.insert(QStringLiteral("error_code"), *context.errorCode);
        }
        if (context.elapsedMs) {
            fields.insert(QStringLiteral("elapsed_ms"), *context.elapsedMs);
        }
        if (context.count) {
            fields.insert(QStringLiteral("count"), QString::number(*context.count));
        }
        QJsonObject object{
            {QStringLiteral("format_version"), 1},
            {QStringLiteral("timestamp"),
             QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("severity"), QString::fromLatin1(severityName(severity))},
            {QStringLiteral("category"),
             event == Event::QtMessage ? QStringLiteral("qt") : QStringLiteral("app")},
            {QStringLiteral("event"), QString::fromLatin1(eventName(event))},
            {QStringLiteral("session_id"), session},
            {QStringLiteral("sequence"), QString::number(++sequence)},
            {QStringLiteral("context"), fields}};
        if (event == Event::SessionStarted) {
            object.insert(QStringLiteral("app_version"), QStringLiteral(CHESS_REVIEW_VERSION));
            object.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
            object.insert(QStringLiteral("debug_logging"), debugEnabled);
        }
        return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    }
    void remember(const QByteArray& record) {
        if (history.size() == historyLimit) {
            history.pop_front();
        }
        history.push_back(record);
    }
    void setHealth(Health value, LogFailure issue = LogFailure::None) {
        {
            QMutexLocker guard(&mutex);
            health = value;
            if (issue != LogFailure::None) {
                failure = issue;
                Context context;
                context.errorCode = static_cast<int>(issue);
                remember(encode(Event::LoggingFailed, context, Severity::Warning));
            }
        }
        emit owner.healthChanged();
    }
    void run() override {
        std::unique_ptr<FileSink> sink = std::make_unique<FileSink>(logDirectory);
        const auto openingFailure = sink->open();
        if (openingFailure != LogFailure::None) {
            sink.reset();
        }
        setHealth(sink ? Health::Active : Health::MemoryOnly, openingFailure);
        for (;;) {
            QByteArray record;
            bool last = false;
            {
                QMutexLocker guard(&mutex);
                while (queue.empty() && !stopping && dropped == 0) {
                    available.wait(&mutex);
                }
                if (queue.empty() && dropped == 0 && stopping) {
                    record = encode(Event::SessionStopped, {}, Severity::Info);
                    remember(record);
                    last = true;
                } else if (!queue.empty()) {
                    record = std::move(queue.front());
                    queue.pop_front();
                } else {
                    Context context;
                    context.count = dropped;
                    dropped = 0;
                    record = encode(Event::EventsDropped, context, Severity::Warning);
                    remember(record);
                }
            }
            if (sink && beforeWrite && !beforeWrite()) {
                sink.reset();
                setHealth(Health::MemoryOnly, LogFailure::Write);
            }
            if (sink) {
                if (const auto issue = sink->write(record); issue != LogFailure::None) {
                    sink.reset();
                    setHealth(Health::MemoryOnly, issue);
                }
            }
            if (last) {
                break;
            }
        }
        sink.reset();
        setHealth(Health::Closed);
    }
    Logger& owner;
    const std::function<bool()> beforeWrite;
    const QString logDirectory;
    const bool debugEnabled;
    const QString session;
    mutable QMutex mutex;
    QWaitCondition available;
    std::deque<QByteArray> queue;
    std::deque<QByteArray> history;
    quint64 sequence = 0;
    quint64 dropped = 0;
    quint64 droppedTotal = 0;
    Health health = Health::Starting;
    LogFailure failure = LogFailure::None;
    bool stopping = false;
};

Logger::Logger(const QString& logDirectory, bool debugEnabled, QObject* parent)
    : Logger(logDirectory, debugEnabled, {}, parent) {}

Logger::Logger(const QString& logDirectory, bool debugEnabled, std::function<bool()> beforeWrite,
               QObject* parent)
    : QObject(parent),
      impl_(std::make_unique<Impl>(*this, logDirectory, debugEnabled, std::move(beforeWrite))) {
    connect(impl_.get(), &QThread::finished, this, &Logger::finishClose, Qt::QueuedConnection);
    record(Event::SessionStarted);
    impl_->setObjectName(QStringLiteral("diagnostics"));
    impl_->start();
}

Logger::~Logger() {
    restoreQtHandler();
    Q_ASSERT_X(!impl_->isRunning(), "Logger", "Await closed() before destroying Logger");
}

void Logger::record(Event event, const Context& context, Severity severity) {
    if (severity == Severity::Debug && !impl_->debugEnabled) {
        return;
    }
    QMutexLocker guard(&impl_->mutex);
    if (impl_->stopping) {
        return;
    }
    if (impl_->dropped != 0 && impl_->queue.size() < queueLimit &&
        impl_->health != Health::MemoryOnly) {
        Context loss;
        loss.count = impl_->dropped;
        impl_->dropped = 0;
        auto lost = impl_->encode(Event::EventsDropped, loss, Severity::Warning);
        impl_->remember(lost);
        impl_->queue.push_back(std::move(lost));
    }
    auto record = impl_->encode(event, context, severity);
    if (record.size() <= maxRecordBytes) {
        // Memory history is independent of disk backpressure: keep the latest
        // events even when the writer is slow or has permanently failed.
        impl_->remember(record);
        if (impl_->health == Health::MemoryOnly) {
            return;
        }
        if (impl_->queue.size() < queueLimit) {
            impl_->queue.push_back(std::move(record));
        } else {
            ++impl_->dropped;
            ++impl_->droppedTotal;
        }
    } else {
        ++impl_->dropped;
        ++impl_->droppedTotal;
    }
    impl_->available.wakeOne();
    // No per-record queued signal: a burst must not create an unbounded Qt event queue.
}

QString Logger::report() const {
    // Copy implicitly shared record buffers while locked; format the report
    // outside the producer lock so a reader cannot stall message submission.
    std::deque<QByteArray> history;
    Health health;
    LogFailure failure;
    quint64 dropped;
    quint64 droppedTotal;
    {
        QMutexLocker guard(&impl_->mutex);
        history = impl_->history;
        health = impl_->health;
        failure = impl_->failure;
        dropped = impl_->dropped;
        droppedTotal = impl_->droppedTotal;
    }
    QString report = QStringLiteral("chess-review %1\nQt %2\nSession: %3\n%4\nRedacted "
                                    "diagnostics; no paths or native error text.\n")
                         .arg(QStringLiteral(CHESS_REVIEW_VERSION), QString::fromLatin1(qVersion()),
                              impl_->session, healthName(health));
    if (failure != LogFailure::None) {
        report += QStringLiteral("Logging issue: %1\n").arg(loggingFailureCode(failure));
    }
    if (droppedTotal != 0) {
        report += QStringLiteral("Dropped disk events (queue overflow): %1\n").arg(droppedTotal);
    }
    if (dropped != 0) {
        report += QStringLiteral("Pending dropped events: %1\n").arg(dropped);
    }
    for (const auto& record : history) {
        report += QString::fromUtf8(record);
    }
    return report;
}
Health Logger::health() const {
    QMutexLocker guard(&impl_->mutex);
    return impl_->health;
}
LogFailure Logger::failure() const {
    QMutexLocker guard(&impl_->mutex);
    return impl_->failure;
}
QString Logger::logDirectory() const { return impl_->logDirectory; }

void Logger::close() {
    Q_ASSERT(QThread::currentThread() == thread());
    {
        QMutexLocker guard(&impl_->mutex);
        if (impl_->stopping) {
            return;
        }
    }
    restoreQtHandler();
    QMutexLocker guard(&impl_->mutex);
    impl_->stopping = true;
    impl_->available.wakeOne();
}
void Logger::finishClose() {
    if (!impl_->wait(0)) {
        QTimer::singleShot(5, this, &Logger::finishClose);
        return;
    }
    emit closed();
}
bool Logger::installQtHandler() {
    Q_ASSERT(QThread::currentThread() == thread());
    QMutexLocker guard(&bridgeMutex);
    {
        QMutexLocker stateGuard(&impl_->mutex);
        if (impl_->stopping) {
            return false;
        }
    }
    if (bridgeLogger == this) {
        return true;
    }
    if (bridgeLogger != nullptr) {
        return false;
    }
    bridgeLogger = this;
    previousHandler = qInstallMessageHandler(&Logger::qtMessage);
    return true;
}
void Logger::restoreQtHandler() {
    QMutexLocker guard(&bridgeMutex);
    if (bridgeLogger == this) {
        qInstallMessageHandler(previousHandler);
        bridgeLogger = nullptr;
        previousHandler = nullptr;
    }
}
void Logger::qtMessage(QtMsgType type, const QMessageLogContext& qtContext, const QString&) {
    const Severity severity = type == QtDebugMsg     ? Severity::Debug
                              : type == QtInfoMsg    ? Severity::Info
                              : type == QtWarningMsg ? Severity::Warning
                              : type == QtFatalMsg   ? Severity::Fatal
                                                     : Severity::Error;
    QMutexLocker guard(&bridgeMutex);
    if (bridgeLogger != nullptr) {
        Context context;
        context.count = 1;
        context.qtCategory = QtCategory::Other;
        if (qtContext.category) {
            // Compare only the fixed family prefixes; never scan an arbitrary
            // category to its end or retain any source-provided category text.
            const char* category = qtContext.category;
            if (std::strcmp(category, "default") == 0) {
                context.qtCategory = QtCategory::Default;
            } else if (std::strcmp(category, "qt.core") == 0 ||
                       std::strncmp(category, "qt.core.", 8) == 0) {
                context.qtCategory = QtCategory::Core;
            } else if (std::strcmp(category, "qt.gui") == 0 ||
                       std::strncmp(category, "qt.gui.", 7) == 0) {
                context.qtCategory = QtCategory::Gui;
            } else if (std::strcmp(category, "qt.widgets") == 0 ||
                       std::strncmp(category, "qt.widgets.", 11) == 0) {
                context.qtCategory = QtCategory::Widgets;
            } else if (std::strcmp(category, "qt.sql") == 0 ||
                       std::strncmp(category, "qt.sql.", 7) == 0) {
                context.qtCategory = QtCategory::Sql;
            }
        }
        bridgeLogger->record(Event::QtMessage, context, severity);
    }
}
} // namespace chess_review::diagnostics
