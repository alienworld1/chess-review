#include "engine/private/SupervisorCoordinator.h"
#include <QThread>
#include <algorithm>
#include <memory>
#include <utility>

namespace chess_review::engine {
namespace {
bool sameDiscovery(const ValidatedEngine& a, const ValidatedEngine& b) {
    return a.executableSha256 == b.executableSha256 && a.reportedName == b.reportedName &&
           a.reportedAuthor == b.reportedAuthor && a.detectedVersion == b.detectedVersion &&
           a.capabilities == b.capabilities;
}
} // namespace

bool SupervisorCoordinator::workersRunning() const {
    return std::ranges::any_of(workers_, [](const Worker& worker) { return worker.process; });
}

void SupervisorCoordinator::inspect(QUuid request, const QString& path) {
    emit inspected(request, inspectExecutable(path));
}

void SupervisorCoordinator::detectEngines(QUuid request) {
    if (closing_) {
        emit enginesDetected(request, {});
        return;
    }
    emit enginesDetected(request,
                         detectLocalEngines(detectionDirectories_, detectionIncludeDefaults_));
}

void SupervisorCoordinator::checkExecutables(
    QUuid request, const QList<std::pair<std::int64_t, QString>>& profiles) {
    QList<std::int64_t> missing;
    if (!closing_) {
        for (const auto& [id, path] : profiles) {
            if (std::holds_alternative<Error>(inspectExecutable(path))) {
                missing.push_back(id);
            }
        }
    }
    emit executablesChecked(request, missing);
}

void SupervisorCoordinator::configureDetection(QStringList directories, bool includeDefaults) {
    if (!closing_) {
        detectionDirectories_ = std::move(directories);
        detectionIncludeDefaults_ = includeDefaults;
    }
}

void SupervisorCoordinator::validate(QUuid request, const QString& path) {
    if (closing_ || workersRunning()) {
        emit completed(request, unavailable());
        return;
    }
    runtime_.reset();
    auto& worker = workers_.front();
    createWorker(worker, AttemptMode::Registration, request);
    worker.process->validate(request, path);
}

void SupervisorCoordinator::connectEngine(QUuid session, const EngineProfile& profile,
                                          EnginePoolLimits limits) {
    const auto fail = [this, session, id = profile.id](Error error) {
        emit runtimeChanged(RuntimeState{session, id, RuntimePhase::Failed, {}, std::move(error)});
    };
    if (closing_ || workersRunning()) {
        fail(unavailable());
        return;
    }
    if (session.isNull() || profile.id <= 0 || !validEngine(profile.engine) ||
        !validOverrides(profile.engine.capabilities, profile.optionOverrides)) {
        fail({ErrorCode::InvalidRecord,
              QStringLiteral("The saved engine is invalid or has unsupported overrides.")});
        return;
    }
    // Search preparation uses one thread unless this validated profile sets Threads.
    const int threads =
        profile.optionOverrides.value(QStringLiteral("threads"), QStringLiteral("1")).toInt();
    const int automatic = std::max({1, threads, QThread::idealThreadCount() / 2});
    const int budget = limits.cpuThreads.value_or(std::min(automatic, maximumPoolThreads));
    if (threads < 1 || limits.workers < 1 || limits.workers > maximumEngineWorkers || budget < 1 ||
        budget > maximumPoolThreads || limits.workers > budget / threads) {
        fail(
            {ErrorCode::InvalidRequest,
             QStringLiteral("Engine workers exceed the CPU thread budget. In Settings, reduce "
                            "the worker count or Threads per engine, or raise the total budget.")});
        return;
    }
    runtime_ = RuntimeState{session,
                            profile.id,
                            RuntimePhase::CheckingExecutable,
                            {},
                            {},
                            profile.optionOverrides,
                            limits.workers};
    connectingProfile_ = profile;
    startedWorkers_ = 0;
    runtimeFailure_.reset();
    draining_ = false;
    scheduledIds_.clear();
    cancelledJobs_.clear();
    submissionOrder_ = 0;
    schedulingStopped_ = true;
    startRuntimeWorker();
}

void SupervisorCoordinator::startRuntimeWorker() {
    Q_ASSERT(runtime_ && connectingProfile_ &&
             startedWorkers_ < static_cast<std::size_t>(runtime_->workers));
    auto& worker = workers_[startedWorkers_++];
    const auto profile = *connectingProfile_;
    const auto session = runtime_->session;
    createWorker(worker, AttemptMode::Runtime, session);
    worker.process->connectEngine(session, profile);
}

void SupervisorCoordinator::disconnectEngine(QUuid session) {
    if (runtime_ && runtime_->session == session && workersRunning()) {
        drainRuntime();
    }
}

void SupervisorCoordinator::analyze(QUuid session, const AnalysisRequest& request) {
    auto& primary = workers_.front();
    if (closing_ || schedulingStopped_ || !primary.process || !primary.ready ||
        std::ranges::any_of(workers_,
                            [](const Worker& worker) { return worker.active.has_value(); }) ||
        !pending_.empty()) {
        AnalysisSnapshot snapshot;
        snapshot.session = session;
        snapshot.request = request.id;
        snapshot.limit = request.limit;
        snapshot.requestedMultiPv = request.multiPv;
        snapshot.wdlRequested = request.wantWdl;
        emit analysisFinished({snapshot, AnalysisOutcome::Rejected, unavailable()});
        return;
    }
    primary.process->analyze(session, request);
}
void SupervisorCoordinator::stopAnalysis(QUuid session, QUuid requestId) {
    auto& primary = workers_.front();
    if (primary.process && !primary.retiring && directSearch_ == requestId) {
        primary.process->stopAnalysis(session, requestId);
    }
}
void SupervisorCoordinator::configureSearch(int idleMs, int responseMs) {
    if (!workersRunning() && !closing_) {
        configuration_.searchIdleMs = idleMs;
        configuration_.searchResponseMs = responseMs;
    }
}
void SupervisorCoordinator::cancel() {
    if (!runtime_ && workers_.front().process) {
        workers_.front().process->cancel();
    }
}
void SupervisorCoordinator::close() {
    if (std::exchange(closing_, true)) {
        return;
    }
    if (!workersRunning()) {
        QThread::currentThread()->quit();
    } else if (runtime_) {
        drainRuntime();
    } else {
        workers_.front().process->close();
    }
}
void SupervisorCoordinator::configure(QStringList arguments, int startMs, int handshakeMs,
                                      qsizetype outputLimit) {
    if (!workersRunning() && !closing_) {
        configuration_.arguments = std::move(arguments);
        configuration_.startMs = startMs;
        configuration_.handshakeMs = handshakeMs;
        configuration_.outputLimit = outputLimit;
    }
}
Error SupervisorCoordinator::unavailable() const {
    return {closing_ ? ErrorCode::Closing : ErrorCode::Busy,
            QStringLiteral("Engine supervision is unavailable or busy.")};
}

void SupervisorCoordinator::onRuntimeChanged(Worker& worker, const RuntimeState& state) {
    Q_ASSERT(runtime_ && runtime_->session == state.session);
    const bool primary = &worker == &workers_.front();
    switch (state.phase) {
    case RuntimePhase::Failed:
        drainRuntime(state.error.value_or(
            Error{ErrorCode::Protocol, QStringLiteral("An engine worker failed.")}));
        return;
    case RuntimePhase::Disconnected:
        return;
    case RuntimePhase::Disconnecting:
        if (runtime_->phase != RuntimePhase::Disconnecting) {
            // An unsolicited stop means the worker is cleaning up a failure.
            // Preserve pending requests until the worker reports the cause.
            schedulingStopped_ = true;
            runtime_->phase = RuntimePhase::Disconnecting;
            runtime_->discovery.reset();
            emit runtimeChanged(*runtime_);
        }
        return;
    case RuntimePhase::Ready:
        if (runtime_->phase == RuntimePhase::Disconnecting) {
            return;
        }
        if (!state.discovery ||
            (!primary &&
             (!runtime_->discovery || !sameDiscovery(*runtime_->discovery, *state.discovery) ||
              runtime_->optionOverrides != state.optionOverrides))) {
            drainRuntime(Error{ErrorCode::Protocol,
                               QStringLiteral("The engine workers reported different settings "
                                              "or capabilities.")});
            return;
        }
        if (primary) {
            runtime_->discovery = state.discovery;
        }
        worker.ready = true;
        if (startedWorkers_ < static_cast<std::size_t>(runtime_->workers)) {
            startRuntimeWorker();
            return;
        }
        runtime_->phase = RuntimePhase::Ready;
        schedulingStopped_ = false;
        connectingProfile_.reset();
        emit runtimeChanged(*runtime_);
        return;
    default:
        if (primary && runtime_->phase != RuntimePhase::Disconnecting) {
            runtime_->phase = state.phase;
            emit runtimeChanged(*runtime_);
        }
    }
}

void SupervisorCoordinator::drainRuntime(std::optional<Error> error) {
    if (error && !runtimeFailure_) {
        runtimeFailure_ = std::move(error);
    }
    if (std::exchange(draining_, true)) {
        return;
    }
    stopScheduling();
    connectingProfile_.reset();
    if (runtime_->phase != RuntimePhase::Disconnecting) {
        runtime_->phase = RuntimePhase::Disconnecting;
        runtime_->discovery.reset();
        emit runtimeChanged(*runtime_);
    }
    for (auto& worker : workers_) {
        if (worker.process && !worker.retiring) {
            worker.process->disconnectEngine(runtime_->session);
        }
    }
}

void SupervisorCoordinator::workerDestroyed(Worker& worker, QUuid request) {
    worker.process = nullptr;
    worker.retiring = false;
    worker.ready = false;
    if (worker.failedSearch) {
        finishSearch(worker, *std::exchange(worker.failedSearch, std::nullopt));
    }
    if (workersRunning()) {
        return;
    }
    if (registrationResult_) {
        emit completed(request, *std::exchange(registrationResult_, std::nullopt));
    } else if (runtime_) {
        Q_ASSERT(!std::ranges::any_of(workers_,
                                      [](const Worker& item) { return item.active.has_value(); }));
        const bool failed = runtimeFailure_.has_value();
        cancelPending(failed ? ScheduledOutcome::Failed : ScheduledOutcome::Cancelled,
                      runtimeFailure_);
        directSearch_.reset();
        runtime_->phase = failed ? RuntimePhase::Failed : RuntimePhase::Disconnected;
        runtime_->error = std::exchange(runtimeFailure_, std::nullopt);
        emit runtimeChanged(*runtime_);
    }
    if (closing_) {
        QThread::currentThread()->quit();
    }
}

void SupervisorCoordinator::createWorker(Worker& worker, AttemptMode mode, QUuid request) {
    Q_ASSERT(!worker.process);
    auto owned = std::make_unique<EngineProcessWorker>(mode, configuration_);
    worker.process = owned.get();
    worker.process->setParent(this);
    owned.release();
    auto* const attempt = worker.process;
    connect(attempt, &EngineProcessWorker::progress, this,
            [this, &worker](QUuid id, const QString& message) {
                if (!worker.retiring) {
                    emit progress(id, message);
                }
            });
    connect(attempt, &EngineProcessWorker::completed, this,
            [this, &worker](QUuid, const ValidationResult& result) {
                if (!worker.retiring) {
                    registrationResult_ = result;
                }
            });
    connect(attempt, &EngineProcessWorker::runtimeChanged, this,
            [this, &worker](const RuntimeState& state) {
                if (!worker.retiring) {
                    onRuntimeChanged(worker, state);
                }
            });
    connect(attempt, &EngineProcessWorker::searchChanged, this,
            [this, &worker](const SearchState& state) {
                if (worker.retiring) {
                    return;
                }
                if (worker.active && worker.active->state.attempt == state.request) {
                    switch (state.phase) {
                    case SearchPhase::Configuring:
                        worker.active->state.phase = ScheduledPhase::Configuring;
                        break;
                    case SearchPhase::Analyzing:
                        worker.active->state.phase = ScheduledPhase::Analyzing;
                        break;
                    case SearchPhase::Stopping:
                        worker.active->state.phase = ScheduledPhase::Stopping;
                        break;
                    case SearchPhase::Synchronizing:
                        worker.active->state.phase = ScheduledPhase::Synchronizing;
                        break;
                    }
                    emit scheduledChanged(worker.active->state);
                } else {
                    directSearch_ = state.request;
                    emit searchChanged(state);
                }
            });
    connect(attempt, &EngineProcessWorker::analysisUpdated, this,
            [this, &worker](const AnalysisSnapshot& snapshot) {
                if (!worker.retiring) {
                    if (worker.active && worker.active->state.attempt == snapshot.request) {
                        emit scheduledUpdated({worker.active->state, snapshot});
                    } else {
                        emit analysisUpdated(snapshot);
                    }
                }
            });
    connect(attempt, &EngineProcessWorker::analysisFinished, this,
            [this, &worker](const AnalysisResult& result) {
                if (!worker.retiring) {
                    if (result.outcome == AnalysisOutcome::Failed) {
                        worker.failedSearch = result;
                    } else {
                        finishSearch(worker, result);
                    }
                }
            });
    connect(attempt, &EngineProcessWorker::finished, this, [&worker, attempt] {
        if (!std::exchange(worker.retiring, true)) {
            attempt->deleteLater();
        }
    });
    connect(attempt, &QObject::destroyed, this,
            [this, &worker, request] { workerDestroyed(worker, request); });
}
} // namespace chess_review::engine
