#include "persistence/Database.h"

#include "persistence/private/Migrations.h"
#include "persistence/private/StorageSession.h"

#include <QMetaObject>
#include <QThread>
#include <QTimer>

namespace chess_review::persistence {
namespace detail {

class StorageWorker final : public QObject {
    Q_OBJECT

  public:
    void initialize(const QString& path) {
        session_ = std::make_unique<StorageSession>();
        const auto& registry = migrations();
        const auto error = session_->initialize(
            path, registry, [this](const QString& message) { emit progress(message); });
        if (error) {
            session_.reset();
            emit failed(*error);
            return;
        }
        emit ready(registry.back().version);
    }

    void close() {
        session_.reset();
        QThread::currentThread()->quit();
    }

  signals:
    void progress(const QString& message);
    void ready(int schemaVersion);
    void failed(const chess_review::persistence::DatabaseError& error);

  private:
    std::unique_ptr<StorageSession> session_;
};

} // namespace detail

struct Database::Impl {
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

Database::~Database() {
    Q_ASSERT_X(!impl_->thread.isRunning(), "Database", "Await closed() before destroying Database");
}

void Database::initialize(const QString& databasePath) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->started || impl_->closing) {
        return;
    }
    impl_->started = true;
    impl_->worker = new detail::StorageWorker;
    impl_->worker->moveToThread(&impl_->thread);
    connect(&impl_->thread, &QThread::finished, impl_->worker, &QObject::deleteLater);
    connect(&impl_->thread, &QThread::finished, this, &Database::closed, Qt::QueuedConnection);
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

} // namespace chess_review::persistence

#include "Database.moc"
