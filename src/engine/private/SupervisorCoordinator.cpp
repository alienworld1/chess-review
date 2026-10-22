#include "engine/private/SupervisorCoordinator.h"
#include <QThread>
#include <memory>
#include <utility>
namespace chess_review::engine {
void SupervisorCoordinator::inspect(QUuid request, const QString& path) {
    emit inspected(request, inspectExecutable(path));
}

void SupervisorCoordinator::validate(QUuid request, const QString& path) {
    if (closing_ || worker_) {
        emit completed(request, unavailable());
        return;
    }
    createWorker(AttemptMode::Registration, request);
    worker_->validate(request, path);
}

void SupervisorCoordinator::connectEngine(QUuid session, const EngineProfile& profile) {
    if (closing_ || worker_) {
        emit runtimeChanged(
            RuntimeState{session, profile.id, RuntimePhase::Failed, {}, unavailable()});
        return;
    }
    if (session.isNull() || profile.id <= 0 || !validEngine(profile.engine) ||
        !profile.optionOverrides.isEmpty()) {
        emit runtimeChanged(RuntimeState{
            session,
            profile.id,
            RuntimePhase::Failed,
            {},
            Error{ErrorCode::InvalidRecord,
                  QStringLiteral("The saved engine is invalid or has unsupported overrides.")}});
        return;
    }
    createWorker(AttemptMode::Runtime, session);
    worker_->connectEngine(session, profile);
}

void SupervisorCoordinator::disconnectEngine(QUuid session) {
    if (worker_) {
        worker_->disconnectEngine(session);
    }
}

void SupervisorCoordinator::cancel() {
    if (worker_) {
        worker_->cancel();
    }
}

void SupervisorCoordinator::close() {
    if (std::exchange(closing_, true)) {
        return;
    }
    if (worker_) {
        worker_->close();
    } else {
        QThread::currentThread()->quit();
    }
}

void SupervisorCoordinator::configure(QStringList arguments, int startMs, int handshakeMs,
                                      qsizetype outputLimit) {
    if (!worker_ && !closing_) {
        configuration_ = {std::move(arguments), startMs, handshakeMs, outputLimit};
    }
}

Error SupervisorCoordinator::unavailable() const {
    return {closing_ ? ErrorCode::Closing : ErrorCode::Busy,
            QStringLiteral("Engine supervision is unavailable or busy.")};
}

void SupervisorCoordinator::createWorker(AttemptMode mode, QUuid request) {
    // Transfer exclusive ownership to the coordinator's QObject tree before use.
    auto owned = std::make_unique<EngineProcessWorker>(mode, configuration_);
    worker_ = owned.get();
    worker_->setParent(this);
    owned.release();
    auto* const attempt = worker_;
    connect(attempt, &EngineProcessWorker::progress, this,
            [this, attempt](QUuid id, const QString& message) {
                if (worker_ == attempt && !retiring_) {
                    emit progress(id, message);
                }
            });
    connect(attempt, &EngineProcessWorker::completed, this,
            [this, attempt](QUuid, const ValidationResult& result) {
                if (worker_ == attempt && !retiring_) {
                    result_ = result;
                }
            });
    connect(attempt, &EngineProcessWorker::runtimeChanged, this,
            [this, attempt](const RuntimeState& state) {
                if (worker_ != attempt || retiring_) {
                    return;
                }
                if (runtimeActive(state.phase)) {
                    emit runtimeChanged(state);
                } else {
                    result_ = state;
                }
            });
    connect(attempt, &EngineProcessWorker::finished, this, [this, attempt] {
        if (worker_ == attempt && !std::exchange(retiring_, true)) {
            attempt->deleteLater();
        }
    });
    connect(attempt, &QObject::destroyed, this, [this, attempt, request] {
        if (worker_ != attempt) {
            return;
        }
        worker_ = nullptr;
        retiring_ = false;
        const auto result = std::exchange(result_, std::monostate{});
        if (const auto* validation = std::get_if<ValidationResult>(&result)) {
            emit completed(request, *validation);
        } else if (const auto* runtime = std::get_if<RuntimeState>(&result)) {
            emit runtimeChanged(*runtime);
        }
        if (closing_) {
            QThread::currentThread()->quit();
        }
    });
}
} // namespace chess_review::engine
