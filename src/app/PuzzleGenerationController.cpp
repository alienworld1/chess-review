#include "app/PuzzleGenerationController.h"
#include "engine/EffectiveSearch.h"
#include <QMetaObject>
#include <QRunnable>
#include <algorithm>

namespace chess_review::app {
namespace {
persistence::PuzzleJobSummary summary(const persistence::PuzzleJob& job) {
    return {job.id,
            job.reviewId,
            job.status,
            job.current,
            static_cast<int>(job.candidates.size()),
            static_cast<int>(job.accepted.size())};
}
} // namespace
PuzzleGenerationController::PuzzleGenerationController(persistence::Database& database,
                                                       EngineSettingsController& settings,
                                                       engine::EngineSupervisor& supervisor,
                                                       ReviewConfiguration& configuration,
                                                       QObject* parent)
    : QObject(parent), database_(database), settings_(settings), supervisor_(supervisor),
      configuration_(configuration) {
    worker_.setMaxThreadCount(1);
    worker_.setExpiryTimeout(-1);
    retry_.setSingleShot(true);
    retry_.setInterval(50);
    connect(&retry_, &QTimer::timeout, this, [this] {
        if (operation_) {
            pending_ = QUuid::createUuid();
            operation_(pending_);
        }
    });
    const auto accept = [this](QUuid id, const persistence::SavePuzzleJobResult& result) {
        if (id != pending_) {
            return;
        }
        if (const auto* e = std::get_if<persistence::ReviewError>(&result);
            e && e->code == persistence::ReviewErrorCode::Busy) {
            retry_.start();
            return;
        }
        pending_ = {};
        operation_ = {};
        loaded(result);
    };
    connect(&database_, &persistence::Database::puzzleJobPrepared, this, accept);
    connect(&database_, &persistence::Database::puzzleJobLoaded, this, accept);
    connect(&database_, &persistence::Database::puzzleJobCheckpointed, this, accept);
    connect(&settings_, &EngineSettingsController::changed, this, [this] {
        emit changed();
        if (!search_.isNull() && (settings_.state().runtime.session != session_ ||
                                  settings_.state().runtime.phase != engine::RuntimePhase::Ready)) {
            cancel();
        }
    });
    connect(&supervisor_, &engine::EngineSupervisor::scheduledFinished, this,
            [this](const engine::ScheduledResult& result) {
                if (result.identity.request != search_ || result.identity.session != session_) {
                    return;
                }
                search_ = {};
                if (state_.cancelling || closing_) {
                    advance();
                    return;
                }
                if (result.outcome != engine::ScheduledOutcome::Completed || !result.evidence) {
                    stopWithError(tr("Puzzle verification stopped. Your completed work is saved; "
                                     "reconnect the original engine and resume."));
                    return;
                }
                if (!job_ || job_->current >= static_cast<int>(job_->candidates.size())) {
                    idle();
                    return;
                }
                job_->candidates[static_cast<std::size_t>(job_->current)].passes.push_back(
                    {result.evidence->evidence, engine::AnalysisOutcome::Completed, {}});
                evaluate();
            });
}
PuzzleGenerationController::~PuzzleGenerationController() {
    Q_ASSERT(!workerBusy_ && pending_.isNull() && search_.isNull());
}
void PuzzleGenerationController::setReady(bool ready) { ready_ = ready; }
void PuzzleGenerationController::request(std::function<void(QUuid)> operation) {
    Q_ASSERT(pending_.isNull());
    operation_ = std::move(operation);
    pending_ = QUuid::createUuid();
    operation_(pending_);
}
void PuzzleGenerationController::generate(review::ReviewId id) {
    if (!ready_ || closing_ || state_.active || id <= 0) {
        return;
    }
    state_ = {};
    state_.active = true;
    state_.message = tr("Finding worthwhile puzzles…");
    engineJob_ = QUuid::createUuid();
    const auto config = configuration_.snapshot();
    const auto settings = config ? config->puzzles : config::PuzzleConfiguration{};
    request([this, id, settings](QUuid requestId) {
        database_.preparePuzzleJob(requestId, id, settings);
    });
    emit changed();
}
void PuzzleGenerationController::enqueue(review::ReviewId id) {
    if (closing_ || !ready_) {
        return;
    }
    if (state_.active) {
        if (queued_.size() < 64 && std::find(queued_.begin(), queued_.end(), id) == queued_.end()) {
            queued_.push_back(id);
        }
        return;
    }
    generate(id);
}
void PuzzleGenerationController::savePosition(std::shared_ptr<const chess::Position> position,
                                              std::optional<persistence::GameId> gameId) {
    if (!ready_ || closing_ || state_.active || !position) {
        return;
    }
    const auto config = configuration_.snapshot();
    const auto& runtime = settings_.state().runtime;
    if (!config || runtime.phase != engine::RuntimePhase::Ready || !runtime.discovery) {
        state_.error =
            tr("Connect a trusted engine and load valid settings before saving a puzzle.");
        emit changed();
        return;
    }
    state_ = {};
    state_.active = true;
    state_.message = tr("Preparing the selected position…");
    const auto discovery = *runtime.discovery;
    const auto profile = runtime.profileId;
    engineJob_ = QUuid::createUuid();
    workerBusy_ = true;
    worker_.start(QRunnable::create(
        [this, position = std::move(position), gameId, config, discovery, profile] {
            auto job = std::make_shared<persistence::PuzzleJob>();
            const auto fingerprint = engine::quickReviewFingerprint(discovery);
            if (const auto* f = std::get_if<engine::EngineFingerprint>(&fingerprint)) {
                persistence::PuzzleDocument d;
                d.definition.position = position->context();
                d.definition.origin = puzzles::Origin::Manual;
                d.definition.policy.review = config->values;
                config::applyPuzzleLimits(d.definition.policy, config->puzzles);
                d.definition.policy.baselineNodes =
                    config->values.budgets[static_cast<std::size_t>(config->values.quality)]
                        .verificationNodes;
                d.fingerprint = *f;
                d.engineProfileId = profile;
                d.gameId = gameId;
                job->candidates.push_back(std::move(d));
            }
            QMetaObject::invokeMethod(
                this,
                [this, job = std::move(job)] {
                    workerBusy_ = false;
                    if (job->candidates.empty()) {
                        stopWithError(tr("The engine cannot verify this puzzle."));
                        return;
                    }
                    job_ = job;
                    checkpoint();
                },
                Qt::QueuedConnection);
        }));
    emit changed();
}
void PuzzleGenerationController::resume(persistence::PuzzleJobId id) {
    if (!ready_ || closing_ || state_.active || id <= 0) {
        return;
    }
    state_ = {};
    state_.active = true;
    state_.message = tr("Loading saved puzzle verification…");
    engineJob_ = QUuid::createUuid();
    request([this, id](QUuid requestId) { database_.loadPuzzleJob(requestId, id); });
    emit changed();
}
void PuzzleGenerationController::loaded(const persistence::SavePuzzleJobResult& result) {
    if (const auto* e = std::get_if<persistence::ReviewError>(&result)) {
        state_.error = e->message;
        idle();
        return;
    }
    job_ = std::make_shared<persistence::PuzzleJob>(std::get<persistence::PuzzleJob>(result));
    durablePasses_ = job_->current < static_cast<int>(job_->candidates.size())
                         ? job_->candidates[static_cast<std::size_t>(job_->current)].passes.size()
                         : 0;
    state_.job = summary(*job_);
    if (!job_->accepted.empty()) {
        state_.lastSaved = job_->accepted.back();
    }
    emit libraryChanged();
    if (job_->status == persistence::PuzzleJobStatus::Complete) {
        const bool manual = !job_->candidates.empty() &&
                            job_->candidates.front().definition.origin == puzzles::Origin::Manual;
        const bool narrow = std::any_of(
            job_->candidates.begin(), job_->candidates.end(), [](const auto& candidate) {
                return std::any_of(
                    candidate.passes.begin(), candidate.passes.end(), [](const auto& pass) {
                        return pass.evidence.effectiveMultiPv < pass.evidence.requestedMultiPv;
                    });
            });
        if (manual && job_->reusedExisting) {
            state_.message = tr("This position is already saved. Open the existing puzzle.");
        } else if (job_->accepted.empty() && narrow) {
            state_.message = tr("No puzzle saved: the engine did not report enough alternatives. "
                                "Use an engine with wider MultiPV support.");
        } else if (job_->accepted.empty()) {
            state_.message =
                manual ? tr("This position has no verified puzzle solution within the current "
                            "analysis limits.")
                       : tr("No suitable puzzles found. Only clear, verified lessons are saved.");
        } else {
            state_.message = tr("%n puzzle(s) available in your library.", nullptr,
                                static_cast<int>(job_->accepted.size()));
        }
        idle();
        return;
    }
    advance();
}
void PuzzleGenerationController::advance() {
    if (workerBusy_ || !pending_.isNull() || !search_.isNull()) {
        return;
    }
    if (!job_) {
        idle();
        return;
    }
    if (state_.cancelling || closing_) {
        if (job_->status == persistence::PuzzleJobStatus::Running) {
            job_->status = persistence::PuzzleJobStatus::Paused;
            checkpoint();
        } else {
            state_.message = tr("Puzzle generation paused. Resume when you are ready.");
            idle();
        }
        return;
    }
    job_->status = persistence::PuzzleJobStatus::Running;
    if (job_->current >= static_cast<int>(job_->candidates.size()) || job_->accepted.size() >= 2) {
        checkpoint();
        return;
    }
    state_.job = summary(*job_);
    state_.message = tr("Preparing puzzles… (%1/%2 positions)")
                         .arg(job_->current + 1)
                         .arg(job_->candidates.size());
    evaluate();
}
void PuzzleGenerationController::evaluate() {
    if (workerBusy_ || !job_ || !pending_.isNull()) {
        return;
    }
    workerBusy_ = true;
    const auto job = job_;
    worker_.start(QRunnable::create([this, job] {
        auto& candidate = job->candidates[static_cast<std::size_t>(job->current)];
        auto result = persistence::verifyPuzzle(candidate);
        QMetaObject::invokeMethod(
            this,
            [this, job, result = std::move(result)]() mutable {
                workerBusy_ = false;
                if (const auto* e = std::get_if<persistence::ReviewError>(&result)) {
                    // Do not checkpoint malformed output into an otherwise resumable job.
                    auto& passes = job->candidates[static_cast<std::size_t>(job->current)].passes;
                    if (passes.size() > durablePasses_) {
                        passes.pop_back();
                    }
                    stopWithError(e->message);
                    return;
                }
                auto verified = std::get<puzzles::Verification>(std::move(result));
                if (verified.outcome == puzzles::Outcome::Pending && verified.next) {
                    if (state_.cancelling || closing_) {
                        advance();
                        return;
                    }
                    // Save every completed search before requesting more work.
                    if (job->candidates[static_cast<std::size_t>(job->current)].passes.size() >
                        durablePasses_) {
                        checkpoint();
                        return;
                    }
                    schedule(std::move(*verified.next));
                } else {
                    std::optional<persistence::PuzzleDocument> accepted;
                    if (verified.outcome == puzzles::Outcome::Accepted) {
                        auto& d = job->candidates[static_cast<std::size_t>(job->current)];
                        d.solution = std::move(verified.solution);
                        accepted = d;
                    }
                    ++job->current;
                    checkpoint(std::move(accepted));
                }
            },
            Qt::QueuedConnection);
    }));
    emit changed();
}
void PuzzleGenerationController::checkpoint(std::optional<persistence::PuzzleDocument> accepted) {
    if (!job_ || !pending_.isNull()) {
        return;
    }
    // The database takes ownership of its copy; workers never mutate this job until
    // the accepted write returns. This also serializes cancellation with persistence.
    const auto job = job_;
    request([this, job, accepted = std::move(accepted)](QUuid id) {
        database_.checkpointPuzzleJob(id, {*job, accepted});
    });
}
void PuzzleGenerationController::schedule(puzzles::SearchNeed need) {
    if (state_.cancelling || closing_) {
        advance();
        return;
    }
    const auto& runtime = settings_.state().runtime;
    if (runtime.phase != engine::RuntimePhase::Ready || !runtime.discovery) {
        stopWithError(tr("Connect the original engine, then resume puzzle generation."));
        return;
    }
    const auto fingerprintExpected =
        job_->candidates[static_cast<std::size_t>(job_->current)].fingerprint;
    const auto expectedSession = runtime.session;
    const auto discovery = *runtime.discovery;
    workerBusy_ = true;
    worker_.start(QRunnable::create([this, fingerprintExpected, discovery, expectedSession,
                                     need = std::move(need)]() mutable {
        const auto fingerprint = engine::quickReviewFingerprint(discovery);
        const bool compatible =
            std::holds_alternative<engine::EngineFingerprint>(fingerprint) &&
            std::get<engine::EngineFingerprint>(fingerprint) == fingerprintExpected;
        QMetaObject::invokeMethod(
            this,
            [this, compatible, expectedSession, need = std::move(need)]() mutable {
                workerBusy_ = false;
                if (state_.cancelling || closing_) {
                    advance();
                    return;
                }
                const auto& currentRuntime = settings_.state().runtime;
                if (!compatible || currentRuntime.session != expectedSession ||
                    currentRuntime.phase != engine::RuntimePhase::Ready) {
                    stopWithError(
                        tr("Reconnect the engine and settings used to create this puzzle."));
                    return;
                }
                engine::AnalysisRequest request;
                request.id = search_ = QUuid::createUuid();
                session_ = currentRuntime.session;
                request.position = std::move(need.position);
                request.limit = engine::NodeLimit{need.nodes};
                request.multiPv = need.width;
                request.wantWdl = true;
                supervisor_.scheduleAnalysis(
                    session_, request, engine::AnalysisPriority::PuzzleVerification, engineJob_);
                emit changed();
            },
            Qt::QueuedConnection);
    }));
}
void PuzzleGenerationController::stopWithError(const QString& message) {
    state_.error = message;
    state_.cancelling = true;
    advance();
}
void PuzzleGenerationController::cancel() {
    if (!state_.active || state_.cancelling) {
        return;
    }
    state_.cancelling = true;
    queued_.clear();
    if (!search_.isNull()) {
        supervisor_.cancelAnalysisJob(session_, engineJob_);
    }
    advance();
    emit changed();
}
void PuzzleGenerationController::idle() {
    state_.active = false;
    state_.cancelling = false;
    job_.reset();
    emit changed();
    finishClose();
    if (!closing_ && !queued_.empty()) {
        const auto id = queued_.front();
        queued_.pop_front();
        QTimer::singleShot(0, this, [this, id] { enqueue(id); });
    }
}
void PuzzleGenerationController::close() {
    if (closing_) {
        return;
    }
    closing_ = true;
    queued_.clear();
    cancel();
    finishClose();
}
void PuzzleGenerationController::finishClose() {
    if (!closing_ || closed_ || state_.active || workerBusy_ || !pending_.isNull() ||
        !search_.isNull()) {
        return;
    }
    if (!worker_.waitForDone(0)) {
        QTimer::singleShot(5, this, &PuzzleGenerationController::finishClose);
        return;
    }
    closed_ = true;
    QTimer::singleShot(0, this, [this] { emit closed(); });
}
} // namespace chess_review::app
