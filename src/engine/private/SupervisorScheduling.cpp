#include "engine/private/SupervisorCoordinator.h"
#include <QTimer>
#include <algorithm>
#include <cstddef>
#include <tuple>
#include <utility>

namespace chess_review::engine {
namespace {
constexpr std::size_t pendingLimit = 256;
constexpr std::size_t registryLimit = 65536;
Error invalidSchedule() {
    return {ErrorCode::InvalidRequest,
            QStringLiteral("Use fresh request/job identifiers and a valid scheduling priority; "
                           "reconnect if the session identifier capacity is exhausted.")};
}
} // namespace

void SupervisorCoordinator::scheduleAnalysis(QUuid session, const AnalysisRequest& request,
                                             AnalysisPriority priority, std::optional<QUuid> job) {
    ScheduledState state{session, request.id, job, {}, ScheduledPhase::Queued};
    const auto reject = [this, &state](Error error) {
        emit scheduledFinished({state, ScheduledOutcome::Rejected, {}, std::move(error)});
    };
    if (closing_ || !worker_ || retiring_ || schedulingStopped_ || !runtime_ ||
        runtime_->session != session) {
        reject(unavailable());
        return;
    }
    if (priority < AnalysisPriority::InteractiveAnalysis ||
        priority > AnalysisPriority::Maintenance || request.id.isNull() || (job && job->isNull()) ||
        scheduledIds_.contains(request.id) || scheduledIds_.size() >= registryLimit ||
        cancelledJobs_.size() >= registryLimit) {
        reject(invalidSchedule());
        return;
    }
    if (job && cancelledJobs_.contains(*job)) {
        reject({ErrorCode::Cancelled, QStringLiteral("This analysis job has been cancelled.")});
        return;
    }
    if (pending_.size() >= pendingLimit) {
        reject({ErrorCode::Busy, QStringLiteral("The analysis queue is full. Submit work later.")});
        return;
    }
    // Validate before preemption. All chess replay/serialization stays off the GUI thread.
    if (const auto error = validateSearchRequest(request, runtime_->discovery->capabilities)) {
        reject(*error);
        return;
    }
    scheduledIds_.insert(request.id);
    pending_.push_back({request, priority, state, submissionOrder_++, false, false, {}});
    emit scheduledChanged(state);
    considerPreemption();
    dispatchScheduled();
}

void SupervisorCoordinator::considerPreemption() {
    if (!active_ || active_->cancelled || active_->preempted ||
        active_->state.phase == ScheduledPhase::Synchronizing ||
        active_->priority < AnalysisPriority::ReviewBackgroundDeepening) {
        return;
    }
    const bool interactive = std::any_of(pending_.begin(), pending_.end(), [](const auto& request) {
        return request.priority <= AnalysisPriority::RetryMove;
    });
    if (interactive) {
        active_->preempted = true;
        worker_->stopAnalysis(active_->state.session, *active_->state.attempt);
    }
}

void SupervisorCoordinator::dispatchScheduled() {
    if (closing_ || schedulingStopped_ || !worker_ || retiring_ || active_ || directSearch_ ||
        pending_.empty()) {
        return;
    }
    const auto next =
        std::min_element(pending_.begin(), pending_.end(), [](const auto& a, const auto& b) {
            return std::tie(a.priority, a.order) < std::tie(b.priority, b.order);
        });
    active_ = std::move(*next);
    pending_.erase(next);
    active_->cancelled = false;
    active_->preempted = false;
    active_->state.attempt = QUuid::createUuid();
    active_->state.phase = ScheduledPhase::Configuring;
    auto attempt = active_->request;
    attempt.id = *active_->state.attempt;
    worker_->analyze(active_->state.session, attempt);
}

void SupervisorCoordinator::cancelScheduledAnalysis(QUuid session, QUuid requestId) {
    if (!runtime_ || runtime_->session != session || schedulingStopped_) {
        return;
    }
    const auto found =
        std::find_if(pending_.begin(), pending_.end(),
                     [requestId](const auto& value) { return value.request.id == requestId; });
    if (found != pending_.end()) {
        auto cancelled = std::move(*found);
        pending_.erase(found);
        emit scheduledFinished(
            {cancelled.state, ScheduledOutcome::Cancelled, std::move(cancelled.evidence), {}});
    } else if (active_ && active_->request.id == requestId && !active_->cancelled &&
               (active_->state.phase != ScheduledPhase::Synchronizing || active_->preempted)) {
        active_->cancelled = true;
        worker_->stopAnalysis(session, *active_->state.attempt);
    }
}

void SupervisorCoordinator::cancelAnalysisJob(QUuid session, QUuid jobId) {
    if (!runtime_ || runtime_->session != session || jobId.isNull()) {
        return;
    }
    // Once full, all new admissions fail closed, including unrecorded job IDs.
    if (cancelledJobs_.size() < registryLimit) {
        cancelledJobs_.insert(jobId);
    }
    std::vector<QUuid> requests;
    for (const auto& value : pending_) {
        if (value.state.job == jobId) {
            requests.push_back(value.request.id);
        }
    }
    if (active_ && active_->state.job == jobId) {
        requests.push_back(active_->request.id);
    }
    for (const auto request : requests) {
        cancelScheduledAnalysis(session, request);
    }
}

void SupervisorCoordinator::finishSearch(const AnalysisResult& result) {
    if (!active_ || active_->state.attempt != result.evidence.request) {
        if (result.outcome != AnalysisOutcome::Rejected &&
            directSearch_ == result.evidence.request) {
            directSearch_.reset();
        }
        emit analysisFinished(result);
    } else {
        auto finished = std::move(*active_);
        active_.reset();
        const bool completed = result.outcome == AnalysisOutcome::Completed ||
                               (result.outcome == AnalysisOutcome::Stopped &&
                                finished.state.phase == ScheduledPhase::Synchronizing &&
                                !finished.cancelled && !finished.preempted);
        // Keep the most recent useful attempt when cancellation occurs before a restart's go.
        if (completed || !result.evidence.lines.empty() || result.evidence.bestMove ||
            !finished.evidence) {
            finished.evidence = ScheduledEvidence{finished.state, result.evidence};
        }
        if (finished.preempted && !finished.cancelled && !schedulingStopped_ &&
            result.outcome == AnalysisOutcome::Stopped) {
            emit scheduledInterrupted({finished.state, result.evidence});
            finished.state.phase = ScheduledPhase::Queued;
            finished.state.attempt = {};
            pending_.push_back(std::move(finished));
            emit scheduledChanged(pending_.back().state);
        } else {
            const auto outcome = completed ? ScheduledOutcome::Completed
                                 : result.outcome == AnalysisOutcome::Stopped
                                     ? ScheduledOutcome::Cancelled
                                     : ScheduledOutcome::Failed;
            emit scheduledFinished(
                {finished.state, outcome, std::move(finished.evidence), result.error});
        }
    }
    // Worker completion notifications may be direct/reentrant. Start the next search only
    // after the worker has returned from its current callback and released search state.
    QTimer::singleShot(0, this, &SupervisorCoordinator::dispatchScheduled);
}

void SupervisorCoordinator::cancelPending(ScheduledOutcome outcome, std::optional<Error> error) {
    const auto pending = std::exchange(pending_, {});
    for (const auto& value : pending) {
        emit scheduledFinished({value.state, outcome, value.evidence, error});
    }
}

void SupervisorCoordinator::stopScheduling() {
    // A worker already retiring after failure owns the queued work's failure outcome.
    // Later close/disconnect must not downgrade that first failure to cancellation.
    const bool alreadyRetiring =
        schedulingStopped_ && runtime_ &&
        (runtime_->phase == RuntimePhase::Disconnecting || runtime_->phase == RuntimePhase::Failed);
    schedulingStopped_ = true;
    if (!alreadyRetiring) {
        cancelPending(ScheduledOutcome::Cancelled);
    }
    if (active_ && active_->state.phase != ScheduledPhase::Synchronizing) {
        active_->cancelled = true;
    }
}
} // namespace chess_review::engine
