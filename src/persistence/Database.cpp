#include "persistence/Database.h"

#include "diagnostics/Logger.h"
#include "persistence/private/Migrations.h"
#include "persistence/private/StorageSession.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

namespace chess_review::persistence {
namespace detail {

class StorageWorker final : public QObject {
    Q_OBJECT

  public:
    explicit StorageWorker(diagnostics::Logger* logger) : logger_(logger) {}
    void initialize(const QString& path) {
        QElapsedTimer elapsed;
        elapsed.start();
        if (logger_) {
            logger_->record(diagnostics::Event::StorageStarted);
        }
        session_ = std::make_unique<StorageSession>();
        const auto& registry = migrations();
        const auto error = session_->initialize(
            path, registry, [this](const QString& message) { emit progress(message); },
            [this](StorageEvent event, int version) {
                if (!logger_) {
                    return;
                }
                using diagnostics::Event;
                Event logEvent = Event::SchemaChecked;
                switch (event) {
                case StorageEvent::SchemaChecked:
                    logEvent = Event::SchemaChecked;
                    break;
                case StorageEvent::BackupStarted:
                    logEvent = Event::BackupStarted;
                    break;
                case StorageEvent::BackupVerified:
                    logEvent = Event::BackupVerified;
                    break;
                case StorageEvent::MigrationStarted:
                    logEvent = Event::MigrationStarted;
                    break;
                case StorageEvent::MigrationCompleted:
                    logEvent = Event::MigrationCompleted;
                    break;
                }
                diagnostics::Context context;
                if (event == StorageEvent::MigrationStarted ||
                    event == StorageEvent::MigrationCompleted) {
                    context.migrationVersion = version;
                } else {
                    context.schemaVersion = version;
                }
                logger_->record(logEvent, context,
                                event == StorageEvent::SchemaChecked ? diagnostics::Severity::Debug
                                                                     : diagnostics::Severity::Info);
            });
        diagnostics::Context context;
        context.elapsedMs = elapsed.elapsed();
        if (error) {
            context.errorCode = static_cast<int>(error->code);
            context.migrationVersion = error->migrationVersion;
            if (logger_) {
                logger_->record(diagnostics::Event::StorageFailed, context,
                                diagnostics::Severity::Error);
            }
            session_.reset();
            emit failed(*error);
            return;
        }
        context.schemaVersion = registry.back().version;
        if (logger_) {
            logger_->record(diagnostics::Event::StorageReady, context);
        }
        emit ready(registry.back().version);
    }

    void close() {
        session_.reset();
        if (logger_) {
            logger_->record(diagnostics::Event::StorageClosed);
        }
        QThread::currentThread()->quit();
    }

  signals:
    void progress(const QString& message);
    void ready(int schemaVersion);
    void failed(const chess_review::persistence::DatabaseError& error);

  private:
    diagnostics::Logger* logger_; // Injected owner outlives storage closure.
    std::unique_ptr<StorageSession> session_;
};

} // namespace detail

struct Database::Impl {
    diagnostics::Logger* logger = nullptr;
    QThread thread;
    detail::StorageWorker* worker =
        nullptr; // Owned by the thread's finished/deleteLater connection.
    bool started = false;
    bool closing = false;
};

Database::Database(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>()) {
    qRegisterMetaType<DatabaseError>();
    impl_->thread.setObjectName(QStringLiteral("storage"));
}

Database::Database(diagnostics::Logger& logger, QObject* parent) : Database(parent) {
    impl_->logger = &logger;
}

Database::~Database() {
    Q_ASSERT_X(!impl_->thread.isRunning(), "Database", "Await closed() before destroying Database");
}

void Database::initialize(const QString& databasePath) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->started || impl_->closing) {
        return;
    }
    impl_->started = true;
    impl_->worker = new detail::StorageWorker(impl_->logger);
    impl_->worker->moveToThread(&impl_->thread);
    connect(&impl_->thread, &QThread::finished, impl_->worker, &QObject::deleteLater);
    connect(&impl_->thread, &QThread::finished, this, &Database::finishClose, Qt::QueuedConnection);
    connect(
        impl_->worker, &detail::StorageWorker::progress, this,
        [this](const QString& message) {
            if (!impl_->closing) {
                emit progress(message);
            }
        },
        Qt::QueuedConnection);
    connect(
        impl_->worker, &detail::StorageWorker::ready, this,
        [this](int version) {
            if (!impl_->closing) {
                emit ready(version);
            }
        },
        Qt::QueuedConnection);
    connect(
        impl_->worker, &detail::StorageWorker::failed, this,
        [this](const DatabaseError& error) {
            if (!impl_->closing) {
                emit failed(error);
            }
        },
        Qt::QueuedConnection);
    impl_->thread.start();
    QMetaObject::invokeMethod(
        impl_->worker, [worker = impl_->worker, databasePath] { worker->initialize(databasePath); },
        Qt::QueuedConnection);
}

void Database::close() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->closing) {
        return;
    }
    impl_->closing = true;
    if (!impl_->started) {
        QTimer::singleShot(0, this, &Database::closed);
        return;
    }
    QMetaObject::invokeMethod(impl_->worker, &detail::StorageWorker::close, Qt::QueuedConnection);
}

void Database::finishClose() {
    // finished() precedes native thread-local cleanup. Poll a nonblocking join so closed()
    // means the thread is fully gone, without waiting on it in the GUI thread.
    if (!impl_->thread.wait(0)) {
        QTimer::singleShot(5, this, &Database::finishClose);
        return;
    }
    impl_->worker = nullptr;
    emit closed();
}

} // namespace chess_review::persistence

#include "Database.moc"
