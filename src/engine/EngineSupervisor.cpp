#include "engine/EngineSupervisor.h"
#include "engine/private/SupervisorCoordinator.h"
#include <QThread>
#include <QTimer>
#include <utility>

namespace chess_review::engine {
struct EngineSupervisor::Impl {
    QThread thread;
    SupervisorCoordinator* coordinator = nullptr;
    bool closing = false;
};
EngineSupervisor::EngineSupervisor(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
    qRegisterMetaType<PathResult>();
    qRegisterMetaType<ValidationResult>();
    qRegisterMetaType<RuntimeState>();
    qRegisterMetaType<SearchState>();
    qRegisterMetaType<AnalysisSnapshot>();
    qRegisterMetaType<AnalysisResult>();
    auto coordinator = std::make_unique<SupervisorCoordinator>();
    impl_->coordinator = coordinator.get();
    impl_->coordinator->moveToThread(&impl_->thread);
    impl_->thread.setObjectName(QStringLiteral("engine-supervisor"));
    connect(&impl_->thread, &QThread::finished, impl_->coordinator, &QObject::deleteLater);
    connect(&impl_->thread, &QThread::finished, this, &EngineSupervisor::finishClose,
            Qt::QueuedConnection);
    connect(impl_->coordinator, &SupervisorCoordinator::inspected, this,
            &EngineSupervisor::inspected, Qt::QueuedConnection);
    connect(impl_->coordinator, &SupervisorCoordinator::progress, this, &EngineSupervisor::progress,
            Qt::QueuedConnection);
    connect(impl_->coordinator, &SupervisorCoordinator::completed, this,
            &EngineSupervisor::completed, Qt::QueuedConnection);
    connect(impl_->coordinator, &SupervisorCoordinator::runtimeChanged, this,
            &EngineSupervisor::runtimeChanged, Qt::QueuedConnection);
    connect(impl_->coordinator, &SupervisorCoordinator::searchChanged, this,
            &EngineSupervisor::searchChanged, Qt::QueuedConnection);
    connect(impl_->coordinator, &SupervisorCoordinator::analysisUpdated, this,
            &EngineSupervisor::analysisUpdated, Qt::QueuedConnection);
    connect(impl_->coordinator, &SupervisorCoordinator::analysisFinished, this,
            &EngineSupervisor::analysisFinished, Qt::QueuedConnection);
    // QThread::finished owns deletion on the engine thread.
    coordinator.release();
    impl_->thread.start();
}
EngineSupervisor::~EngineSupervisor() { Q_ASSERT(!impl_->thread.isRunning()); }
void EngineSupervisor::inspect(QUuid request, const QString& path) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->closing) {
        QTimer::singleShot(0, this, [this, request] {
            emit inspected(request, Error{ErrorCode::Closing,
                                          QStringLiteral("Engine validation is closing.")});
        });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->coordinator,
        [coordinator = impl_->coordinator, request, path] { coordinator->inspect(request, path); },
        Qt::QueuedConnection);
}
void EngineSupervisor::validate(QUuid request, const QString& path) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->closing) {
        QTimer::singleShot(0, this, [this, request] {
            emit completed(request, Error{ErrorCode::Closing,
                                          QStringLiteral("Engine validation is closing.")});
        });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->coordinator,
        [coordinator = impl_->coordinator, request, path] { coordinator->validate(request, path); },
        Qt::QueuedConnection);
}
void EngineSupervisor::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->closing) {
        return;
    }
    QMetaObject::invokeMethod(impl_->coordinator, &SupervisorCoordinator::cancel,
                              Qt::QueuedConnection);
}
void EngineSupervisor::connectEngine(QUuid session, const EngineProfile& profile) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->closing) {
        QTimer::singleShot(0, this, [this, session, id = profile.id] {
            emit runtimeChanged(RuntimeState{
                session,
                id,
                RuntimePhase::Failed,
                {},
                Error{ErrorCode::Closing, QStringLiteral("Engine supervision is closing.")}});
        });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->coordinator,
        [coordinator = impl_->coordinator, session, profile] {
            coordinator->connectEngine(session, profile);
        },
        Qt::QueuedConnection);
}
void EngineSupervisor::disconnectEngine(QUuid session) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!impl_->closing) {
        QMetaObject::invokeMethod(
            impl_->coordinator,
            [coordinator = impl_->coordinator, session] { coordinator->disconnectEngine(session); },
            Qt::QueuedConnection);
    }
}
void EngineSupervisor::analyze(QUuid session, const AnalysisRequest& request) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->closing) {
        AnalysisSnapshot snapshot;
        snapshot.session = session;
        snapshot.request = request.id;
        snapshot.limit = request.limit;
        snapshot.requestedMultiPv = request.multiPv;
        snapshot.wdlRequested = request.wantWdl;
        QTimer::singleShot(0, this, [this, snapshot] {
            emit analysisFinished(
                {snapshot, AnalysisOutcome::Rejected,
                 Error{ErrorCode::Closing, QStringLiteral("Engine supervision is closing.")}});
        });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->coordinator,
        [coordinator = impl_->coordinator, session, request] {
            coordinator->analyze(session, request);
        },
        Qt::QueuedConnection);
}
void EngineSupervisor::stopAnalysis(QUuid session, QUuid requestId) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!impl_->closing) {
        QMetaObject::invokeMethod(
            impl_->coordinator,
            [coordinator = impl_->coordinator, session, requestId] {
                coordinator->stopAnalysis(session, requestId);
            },
            Qt::QueuedConnection);
    }
}
void EngineSupervisor::setTestSearchDeadlines(int idleMs, int responseMs) {
    QMetaObject::invokeMethod(
        impl_->coordinator,
        [coordinator = impl_->coordinator, idleMs, responseMs] {
            coordinator->configureSearch(idleMs, responseMs);
        },
        Qt::QueuedConnection);
}
void EngineSupervisor::close() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (std::exchange(impl_->closing, true)) {
        return;
    }
    QMetaObject::invokeMethod(impl_->coordinator, &SupervisorCoordinator::close,
                              Qt::QueuedConnection);
}
void EngineSupervisor::finishClose() {
    if (!impl_->thread.wait(0)) {
        QTimer::singleShot(5, this, &EngineSupervisor::finishClose);
        return;
    }
    impl_->coordinator = nullptr;
    emit closed();
}
void EngineSupervisor::setTestArguments(QStringList arguments, int startMs, int handshakeMs,
                                        qsizetype outputLimit) {
    QMetaObject::invokeMethod(
        impl_->coordinator,
        [coordinator = impl_->coordinator, arguments = std::move(arguments), startMs, handshakeMs,
         outputLimit] { coordinator->configure(arguments, startMs, handshakeMs, outputLimit); },
        Qt::QueuedConnection);
}
} // namespace chess_review::engine
