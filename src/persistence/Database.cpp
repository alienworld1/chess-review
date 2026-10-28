#include "persistence/Database.h"

#include "diagnostics/Logger.h"
#include "persistence/private/Migrations.h"
#include "persistence/private/StorageSession.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <exception>
#include <type_traits>
#include <utility>

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

    template <typename Result, typename Operation> Result execute(Operation operation) {
        if (!session_) {
            return GameError{GameErrorCode::Unavailable,
                             QStringLiteral("Game storage is unavailable.")};
        }
        try {
            return operation(*session_);
        } catch (const std::exception&) {
            return GameError{GameErrorCode::Storage,
                             QStringLiteral("Game storage could not complete the request.")};
        }
    }
    template <typename Result, typename Operation> Result executeEngine(Operation operation) {
        if (!session_) {
            return engine::Error{engine::ErrorCode::Storage,
                                 QStringLiteral("Engine storage is unavailable.")};
        }
        try {
            return operation(*session_);
        } catch (const std::exception&) {
            return engine::Error{engine::ErrorCode::Storage,
                                 QStringLiteral("Engine storage could not complete the request.")};
        }
    }
    template <typename Result, typename Operation> Result executeCache(Operation operation) {
        if (!session_) {
            return CacheError{CacheErrorCode::Unavailable,
                              QStringLiteral("Analysis cache is unavailable.")};
        }
        try {
            auto result = operation(*session_);
            if constexpr (std::is_same_v<Result, FindAnalysisResult>) {
                const auto* lookup = std::get_if<CacheLookup>(&result);
                if (lookup && lookup->discardedRecords && logger_) {
                    diagnostics::Context context;
                    context.count = static_cast<quint64>(lookup->discardedRecords);
                    logger_->record(diagnostics::Event::CacheRecordsDiscarded, context,
                                    diagnostics::Severity::Warning);
                }
            }
            return result;
        } catch (const std::exception&) {
            return CacheError{CacheErrorCode::Storage,
                              QStringLiteral("Analysis cache could not complete the request.")};
        }
    }
    template <typename Result, typename Operation> Result executeReview(Operation operation) {
        if (!session_) {
            return ReviewError{ReviewErrorCode::Unavailable,
                               QStringLiteral("Review storage is unavailable.")};
        }
        try {
            return operation(*session_);
        } catch (const std::exception&) {
            return ReviewError{ReviewErrorCode::Storage,
                               QStringLiteral("Review storage could not complete the request.")};
        }
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
    int pending = 0;
    bool ready = false;
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
                impl_->ready = true;
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

std::optional<GameError> Database::acceptRequest() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->closing) {
        return GameError{GameErrorCode::Closing, QStringLiteral("Game storage is closing.")};
    }
    if (!impl_->ready) {
        return GameError{GameErrorCode::Unavailable,
                         QStringLiteral("Game storage is unavailable.")};
    }
    if (impl_->pending >= 16) {
        return GameError{GameErrorCode::Busy, QStringLiteral("Game storage is busy.")};
    }
    ++impl_->pending;
    return std::nullopt;
}
void Database::insertGame(RequestId request, std::shared_ptr<const pgn::PgnDocument> document,
                          GameSource source) {
    if (auto error = acceptRequest()) {
        QTimer::singleShot(0, this,
                           [this, request, error = *error] { emit gameInserted(request, error); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, request, document = std::move(document), source = std::move(source)] {
            const auto result = impl_->worker->execute<InsertGameResult>(
                [&](detail::StorageSession& session) -> InsertGameResult {
                    if (!document) {
                        return GameError{GameErrorCode::InvalidSnapshot,
                                         QStringLiteral("Missing imported snapshot.")};
                    }
                    return session.insertGame(*document, source);
                });
            QMetaObject::invokeMethod(
                this,
                [this, request, result] {
                    --impl_->pending;
                    emit gameInserted(request, result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void Database::loadGame(RequestId request, GameId id) {
    if (auto error = acceptRequest()) {
        QTimer::singleShot(0, this,
                           [this, request, error = *error] { emit gameLoaded(request, error); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, request, id] {
            const auto result = impl_->worker->execute<LoadGameResult>(
                [&](detail::StorageSession& session) { return session.loadGame(id); });
            QMetaObject::invokeMethod(
                this,
                [this, request, result] {
                    --impl_->pending;
                    emit gameLoaded(request, result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void Database::listGames(RequestId request, std::int64_t offset, int limit) {
    if (auto error = acceptRequest()) {
        QTimer::singleShot(0, this,
                           [this, request, error = *error] { emit gamesListed(request, error); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, request, offset, limit] {
            const auto result = impl_->worker->execute<ListGamesResult>(
                [&](detail::StorageSession& session) { return session.listGames(offset, limit); });
            QMetaObject::invokeMethod(
                this,
                [this, request, result] {
                    --impl_->pending;
                    emit gamesListed(request, result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void Database::registerEngine(RequestId request, engine::ValidatedEngine engine) {
    if (auto error = acceptRequest()) {
        const engine::Error failure{
            error->code == GameErrorCode::Busy ? engine::ErrorCode::Busy
                                               : engine::ErrorCode::Storage,
            QStringLiteral("Engine storage is unavailable, closing, or busy.")};
        QTimer::singleShot(0, this,
                           [this, request, failure] { emit engineRegistered(request, failure); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, request, engine = std::move(engine)] {
            const auto result = impl_->worker->executeEngine<RegisterEngineResult>(
                [&](detail::StorageSession& session) { return session.registerEngine(engine); });
            QMetaObject::invokeMethod(
                this,
                [this, request, result] {
                    --impl_->pending;
                    emit engineRegistered(request, result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void Database::listEngines(RequestId request, std::int64_t offset, int limit) {
    if (auto error = acceptRequest()) {
        const engine::Error failure{
            error->code == GameErrorCode::Busy ? engine::ErrorCode::Busy
                                               : engine::ErrorCode::Storage,
            QStringLiteral("Engine storage is unavailable, closing, or busy.")};
        QTimer::singleShot(0, this,
                           [this, request, failure] { emit enginesListed(request, failure); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, request, offset, limit] {
            const auto result = impl_->worker->executeEngine<ListEnginesResult>(
                [&](detail::StorageSession& session) {
                    return session.listEngines(offset, limit);
                });
            QMetaObject::invokeMethod(
                this,
                [this, request, result] {
                    --impl_->pending;
                    emit enginesListed(request, result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void Database::storeAnalysis(RequestId request, engine::AnalysisResult result) {
    if (auto error = acceptRequest()) {
        CacheErrorCode code = CacheErrorCode::Unavailable;
        if (error->code == GameErrorCode::Busy) {
            code = CacheErrorCode::Busy;
        }
        if (error->code == GameErrorCode::Closing) {
            code = CacheErrorCode::Closing;
        }
        const CacheError failure{
            code, QStringLiteral("Analysis cache is unavailable, closing, or busy.")};
        QTimer::singleShot(0, this,
                           [this, request, failure] { emit analysisStored(request, failure); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, request, input = std::move(result)] {
            const auto outcome = impl_->worker->executeCache<StoreAnalysisResult>(
                [&](detail::StorageSession& session) { return session.storeAnalysis(input); });
            QMetaObject::invokeMethod(
                this,
                [this, request, outcome] {
                    --impl_->pending;
                    emit analysisStored(request, outcome);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void Database::findAnalysis(RequestId request, engine::CacheQuery query) {
    if (auto error = acceptRequest()) {
        CacheErrorCode code = CacheErrorCode::Unavailable;
        if (error->code == GameErrorCode::Busy) {
            code = CacheErrorCode::Busy;
        }
        if (error->code == GameErrorCode::Closing) {
            code = CacheErrorCode::Closing;
        }
        const CacheError failure{
            code, QStringLiteral("Analysis cache is unavailable, closing, or busy.")};
        QTimer::singleShot(0, this,
                           [this, request, failure] { emit analysisFound(request, failure); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, request, input = std::move(query)] {
            const auto outcome = impl_->worker->executeCache<FindAnalysisResult>(
                [&](detail::StorageSession& session) { return session.findAnalysis(input); });
            QMetaObject::invokeMethod(
                this,
                [this, request, outcome] {
                    --impl_->pending;
                    emit analysisFound(request, outcome);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void Database::createReview(RequestId requestId, CreateReview request) {
    if (auto error = acceptRequest()) {
        const auto code =
            error->code == GameErrorCode::Busy
                ? ReviewErrorCode::Busy
                : (error->code == GameErrorCode::Closing ? ReviewErrorCode::Closing
                                                         : ReviewErrorCode::Unavailable);
        const ReviewError failure{
            code, QStringLiteral("Review storage is unavailable, closing, or busy.")};
        QTimer::singleShot(0, this,
                           [this, requestId, failure] { emit reviewCreated(requestId, failure); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, requestId, request = std::move(request)] {
            const auto result = impl_->worker->executeReview<CreateReviewResult>(
                [&](detail::StorageSession& session) { return session.createReview(request); });
            QMetaObject::invokeMethod(
                this,
                [this, requestId, result] {
                    --impl_->pending;
                    emit reviewCreated(requestId, result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void Database::loadReview(RequestId requestId, ReviewId id) {
    if (auto error = acceptRequest()) {
        const auto code =
            error->code == GameErrorCode::Busy
                ? ReviewErrorCode::Busy
                : (error->code == GameErrorCode::Closing ? ReviewErrorCode::Closing
                                                         : ReviewErrorCode::Unavailable);
        const ReviewError failure{
            code, QStringLiteral("Review storage is unavailable, closing, or busy.")};
        QTimer::singleShot(0, this,
                           [this, requestId, failure] { emit reviewLoaded(requestId, failure); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, requestId, id] {
            const auto result = impl_->worker->executeReview<LoadReviewResult>(
                [&](detail::StorageSession& session) { return session.loadReview(id); });
            QMetaObject::invokeMethod(
                this,
                [this, requestId, result] {
                    --impl_->pending;
                    emit reviewLoaded(requestId, result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void Database::listReviews(RequestId requestId, GameId gameId, std::int64_t offset, int limit) {
    if (auto error = acceptRequest()) {
        const auto code =
            error->code == GameErrorCode::Busy
                ? ReviewErrorCode::Busy
                : (error->code == GameErrorCode::Closing ? ReviewErrorCode::Closing
                                                         : ReviewErrorCode::Unavailable);
        const ReviewError failure{
            code, QStringLiteral("Review storage is unavailable, closing, or busy.")};
        QTimer::singleShot(0, this,
                           [this, requestId, failure] { emit reviewsListed(requestId, failure); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, requestId, gameId, offset, limit] {
            const auto result = impl_->worker->executeReview<ListReviewsResult>(
                [&](detail::StorageSession& session) {
                    return session.listReviews(gameId, offset, limit);
                });
            QMetaObject::invokeMethod(
                this,
                [this, requestId, result] {
                    --impl_->pending;
                    emit reviewsListed(requestId, result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void Database::checkpointReview(RequestId requestId, ReviewCheckpoint checkpoint) {
    if (auto error = acceptRequest()) {
        const auto code =
            error->code == GameErrorCode::Busy
                ? ReviewErrorCode::Busy
                : (error->code == GameErrorCode::Closing ? ReviewErrorCode::Closing
                                                         : ReviewErrorCode::Unavailable);
        const ReviewError failure{
            code, QStringLiteral("Review storage is unavailable, closing, or busy.")};
        QTimer::singleShot(
            0, this, [this, requestId, failure] { emit reviewCheckpointed(requestId, failure); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, requestId, checkpoint = std::move(checkpoint)] {
            const auto result = impl_->worker->executeReview<CheckpointReviewResult>(
                [&](detail::StorageSession& session) {
                    return session.checkpointReview(checkpoint);
                });
            QMetaObject::invokeMethod(
                this,
                [this, requestId, result] {
                    --impl_->pending;
                    emit reviewCheckpointed(requestId, result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void Database::continueReview(RequestId requestId, ContinueReview continuation) {
    if (auto error = acceptRequest()) {
        const auto code =
            error->code == GameErrorCode::Busy
                ? ReviewErrorCode::Busy
                : (error->code == GameErrorCode::Closing ? ReviewErrorCode::Closing
                                                         : ReviewErrorCode::Unavailable);
        const ReviewError failure{
            code, QStringLiteral("Review storage is unavailable, closing, or busy.")};
        QTimer::singleShot(
            0, this, [this, requestId, failure] { emit reviewContinued(requestId, failure); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, requestId, continuation = std::move(continuation)] {
            const auto result = impl_->worker->executeReview<ContinueReviewResult>(
                [&](detail::StorageSession& session) {
                    return session.continueReview(continuation);
                });
            QMetaObject::invokeMethod(
                this,
                [this, requestId, result] {
                    --impl_->pending;
                    emit reviewContinued(requestId, result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void Database::deriveReviewMoves(RequestId requestId, ReviewId id, std::int64_t expectedRevision,
                                 int offset, int limit) {
    if (auto error = acceptRequest()) {
        const auto code =
            error->code == GameErrorCode::Busy
                ? ReviewErrorCode::Busy
                : (error->code == GameErrorCode::Closing ? ReviewErrorCode::Closing
                                                         : ReviewErrorCode::Unavailable);
        const ReviewError failure{
            code, QStringLiteral("Review storage is unavailable, closing, or busy.")};
        QTimer::singleShot(
            0, this, [this, requestId, failure] { emit reviewMovesLoaded(requestId, failure); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, requestId, id, expectedRevision, offset, limit] {
            const auto result = impl_->worker->executeReview<LoadReviewMovesResult>(
                [&](detail::StorageSession& session) {
                    return session.deriveReviewMoves(id, expectedRevision, offset, limit);
                });
            QMetaObject::invokeMethod(
                this,
                [this, requestId, result] {
                    --impl_->pending;
                    emit reviewMovesLoaded(requestId, result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void Database::loadReviewOverview(RequestId requestId, ReviewId id, std::int64_t expectedRevision,
                                  int keyOffset) {
    if (auto error = acceptRequest()) {
        const auto code =
            error->code == GameErrorCode::Busy
                ? ReviewErrorCode::Busy
                : (error->code == GameErrorCode::Closing ? ReviewErrorCode::Closing
                                                         : ReviewErrorCode::Unavailable);
        const ReviewError failure{
            code, QStringLiteral("Review storage is unavailable, closing, or busy.")};
        QTimer::singleShot(
            0, this, [this, requestId, failure] { emit reviewOverviewLoaded(requestId, failure); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, requestId, id, expectedRevision, keyOffset] {
            const auto result = impl_->worker->executeReview<LoadReviewOverviewResult>(
                [&](detail::StorageSession& session) {
                    return session.loadReviewOverview(id, expectedRevision, keyOffset);
                });
            QMetaObject::invokeMethod(
                this,
                [this, requestId, result] {
                    --impl_->pending;
                    emit reviewOverviewLoaded(requestId, result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void Database::loadReviewEvidence(RequestId requestId, ReviewId id, int positionIndex,
                                  review::EvidenceStage stage) {
    if (auto error = acceptRequest()) {
        const auto code =
            error->code == GameErrorCode::Busy
                ? ReviewErrorCode::Busy
                : (error->code == GameErrorCode::Closing ? ReviewErrorCode::Closing
                                                         : ReviewErrorCode::Unavailable);
        const ReviewError failure{
            code, QStringLiteral("Review storage is unavailable, closing, or busy.")};
        QTimer::singleShot(
            0, this, [this, requestId, failure] { emit reviewEvidenceLoaded(requestId, failure); });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, requestId, id, positionIndex, stage] {
            const auto result = impl_->worker->executeReview<LoadReviewEvidenceResult>(
                [&](detail::StorageSession& session) {
                    return session.loadReviewEvidence(id, positionIndex, stage);
                });
            QMetaObject::invokeMethod(
                this,
                [this, requestId, result] {
                    --impl_->pending;
                    emit reviewEvidenceLoaded(requestId, result);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}
void Database::listReviewTransitions(RequestId requestId, ReviewId id, std::int64_t offset,
                                     int limit) {
    if (auto error = acceptRequest()) {
        const auto code =
            error->code == GameErrorCode::Busy
                ? ReviewErrorCode::Busy
                : (error->code == GameErrorCode::Closing ? ReviewErrorCode::Closing
                                                         : ReviewErrorCode::Unavailable);
        const ReviewError failure{
            code, QStringLiteral("Review storage is unavailable, closing, or busy.")};
        QTimer::singleShot(0, this, [this, requestId, failure] {
            emit reviewTransitionsListed(requestId, failure);
        });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker,
        [this, requestId, id, offset, limit] {
            const auto result = impl_->worker->executeReview<ListReviewTransitionsResult>(
                [&](detail::StorageSession& session) {
                    return session.listReviewTransitions(id, offset, limit);
                });
            QMetaObject::invokeMethod(
                this,
                [this, requestId, result] {
                    --impl_->pending;
                    emit reviewTransitionsListed(requestId, result);
                },
                Qt::QueuedConnection);
        },
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
    if (impl_->pending != 0 || !impl_->thread.wait(0)) {
        QTimer::singleShot(5, this, &Database::finishClose);
        return;
    }
    impl_->worker = nullptr;
    emit closed();
}

} // namespace chess_review::persistence

#include "Database.moc"
