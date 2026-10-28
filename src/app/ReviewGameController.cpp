#include "app/ReviewGameController.h"
#include "engine/EffectiveSearch.h"
#include <QMetaObject>
#include <QScopedValueRollback>
#include <algorithm>
#include <exception>
#include <utility>
#include <variant>

namespace chess_review::app {
using persistence::ReviewError;
using persistence::ReviewErrorCode;
using review::JobStatus;
namespace {
QString displayName(const QString& value) {
    auto result = value.left(256);
    if (!result.isEmpty() && result.back().isHighSurrogate()) {
        result.chop(1);
    }
    result = result.simplified();
    if (value.size() > 256) {
        result += QChar(0x2026);
    }
    return result;
}
} // namespace
QString reviewStatusText(JobStatus status) {
    switch (status) {
    case JobStatus::Queued:
        return QObject::tr("Queued");
    case JobStatus::QuickScanning:
        return QObject::tr("Quick scanning");
    case JobStatus::Reviewable:
        return QObject::tr("Reviewable");
    case JobStatus::Deepening:
        return QObject::tr("Deepening");
    case JobStatus::Deriving:
        return QObject::tr("Deriving");
    case JobStatus::Complete:
        return QObject::tr("Processing complete");
    case JobStatus::Partial:
        return QObject::tr("Partial");
    case JobStatus::Cancelled:
        return QObject::tr("Cancelled");
    case JobStatus::Interrupted:
        return QObject::tr("Interrupted");
    case JobStatus::Failed:
        return QObject::tr("Failed");
    }
    return QObject::tr("Unknown");
}
struct ReviewGameController::Cursor {
    std::shared_ptr<const pgn::PgnDocument> document;
    std::optional<chess::Position> position;
    int index = 0;
};
ReviewGameController::ReviewGameController(persistence::Database& database,
                                           EngineSettingsController& settings,
                                           engine::EngineSupervisor& supervisor,
                                           ReviewConfiguration& configuration, QObject* parent)
    : QObject(parent), database_(database), settings_(settings), supervisor_(supervisor),
      configuration_(configuration), moves_(database), cursor_(std::make_unique<Cursor>()) {
    connect(&moves_, &ReviewMovesController::changed, this, &ReviewGameController::publish);
    connect(&moves_, &ReviewMovesController::drainedChanged, this,
            &ReviewGameController::finishClose);
    worker_.setMaxThreadCount(1);
    worker_.setExpiryTimeout(-1);
    retry_.setSingleShot(true);
    listRetry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, [this] {
        if (operation_) {
            pending_ = QUuid::createUuid();
            operation_(pending_);
        }
    });
    connect(&listRetry_, &QTimer::timeout, this, [this] { refreshRuns(state_.page); });
    connect(&settings_, &EngineSettingsController::changed, this,
            &ReviewGameController::runtimeChanged);
    connect(&configuration_, &ReviewConfiguration::changed, this, &ReviewGameController::publish);
    connect(&database_, &persistence::Database::gameLoaded, this,
            [this](QUuid id, const persistence::LoadGameResult& result) {
                if (!accept(id)) {
                    return;
                }
                if (const auto* error = std::get_if<persistence::GameError>(&result)) {
                    if (error->code == persistence::GameErrorCode::Busy) {
                        handleError({ReviewErrorCode::Busy, error->message});
                    } else {
                        fail(error->message, review::JobFailure::Storage);
                    }
                    return;
                }
                operation_ = {};
                game_ = std::get<persistence::StoredGame>(result);
                state_.game =
                    tr("Game %1: %2 — %3")
                        .arg(game_->summary.id)
                        .arg(game_->summary.white.isEmpty() ? tr("White")
                                                            : displayName(game_->summary.white),
                             game_->summary.black.isEmpty() ? tr("Black")
                                                            : displayName(game_->summary.black));
                state_.phase = ReviewPhase::Idle;
                publish();
                refreshRuns();
                finishClose();
            });
    connect(&database_, &persistence::Database::reviewsListed, this,
            [this](QUuid id, const persistence::ListReviewsResult& result) {
                if (id != listing_) {
                    return;
                }
                listing_ = {};
                state_.listing = false;
                if (const auto* error = std::get_if<ReviewError>(&result)) {
                    if (error->code == ReviewErrorCode::Busy && !closing_) {
                        listRetry_.start(100);
                    } else {
                        state_.notice = error->message;
                    }
                } else {
                    state_.runs = std::get<std::vector<persistence::ReviewSummary>>(result);
                    state_.hasNext = state_.runs.size() > 50;
                    if (state_.hasNext) {
                        state_.runs.pop_back();
                    }
                }
                publish();
                finishClose();
            });
    connect(&database_, &persistence::Database::reviewLoaded, this,
            [this](QUuid id, const persistence::LoadReviewResult& result) {
                if (!accept(id)) {
                    return;
                }
                if (!state_.active && !closing_ && nextInspection_) {
                    const auto next = *std::exchange(nextInspection_, std::nullopt);
                    loadInspection(next);
                    return;
                }
                if (const auto* error = std::get_if<ReviewError>(&result)) {
                    handleError(*error);
                    return;
                }
                operation_ = {};
                stored_ = std::get<persistence::StoredReview>(result);
                if (stored_->summary.gameId != state_.gameId) {
                    stored_.reset();
                    fail(tr("This run belongs to another game."),
                         review::JobFailure::Configuration);
                    return;
                }
                state_.run = stored_->summary;
                state_.engine = displayName(stored_->settings.fingerprint.reportedName);
                if (!state_.active) {
                    state_.phase = ReviewPhase::Finished;
                    state_.message = reviewStatusText(state_.run->status);
                    publish();
                    refreshOverview();
                    finishClose();
                    return;
                }
                const auto& run = *stored_;
                if (run.summary.status < JobStatus::Partial ||
                    run.evidenceModelVersion != review::evidenceModelVersion ||
                    run.expectedScoreModelVersion != review::expectedScoreModelVersion) {
                    fail(tr("This run cannot be resumed. Start a new run."),
                         review::JobFailure::Configuration);
                    return;
                }
                profileId_ = run.settings.engineProfileId;
                side_ = run.settings.selectedSide;
                refinementEnabled_ = run.settings.refinement;
                runConfiguration_ = run.settings.configuration;
                connectSelected(true);
            });
    connect(&database_, &persistence::Database::reviewOverviewLoaded, this,
            [this](QUuid id, const persistence::LoadReviewOverviewResult& result) {
                if (!accept(id)) {
                    return;
                }
                if (!state_.active && !closing_ && nextInspection_) {
                    const auto next = *std::exchange(nextInspection_, std::nullopt);
                    loadInspection(next);
                    return;
                }
                if (const auto* error = std::get_if<ReviewError>(&result)) {
                    if (error->code == ReviewErrorCode::Busy) {
                        handleError(*error);
                        return;
                    }
                    operation_ = {};
                    if (state_.active) {
                        planning_ = false;
                        fail(error->message, review::JobFailure::Evidence);
                    } else {
                        state_.overviewError = error->message;
                        publish();
                        finishClose();
                    }
                    return;
                }
                operation_ = {};
                const auto& overview = std::get<persistence::ReviewOverview>(result);
                if (!state_.run || overview.reviewId != state_.run->id ||
                    overview.revision != state_.run->revision) {
                    finishClose();
                    return;
                }
                state_.overview = overview;
                state_.overviewError.clear();
                if (planning_) {
                    planning_ = false;
                    planned_ = true;
                    refinementPositions_ = overview.refinementPositions;
                    refinementIndex_ = 0;
                    stage_ = review::EvidenceStage::Deepening;
                    state_.refinementTotal = static_cast<int>(refinementPositions_.size()) * 2;
                    state_.refinementDone = 0;
                    statusWrite_ = true;
                    checkpoint(JobStatus::Reviewable);
                } else {
                    publish();
                    if (state_.active) {
                        advance();
                    }
                    finishClose();
                }
            });
    connect(&database_, &persistence::Database::reviewCreated, this,
            [this](QUuid id, const persistence::CreateReviewResult& result) {
                if (!accept(id)) {
                    return;
                }
                if (const auto* error = std::get_if<ReviewError>(&result)) {
                    handleError(*error);
                    return;
                }
                operation_ = {};
                state_.run = std::get<persistence::ReviewSummary>(result);
                advance();
            });
    connect(&database_, &persistence::Database::reviewContinued, this,
            [this](QUuid id, const persistence::ContinueReviewResult& result) {
                if (!accept(id)) {
                    return;
                }
                if (const auto* error = std::get_if<ReviewError>(&result)) {
                    handleError(*error);
                    return;
                }
                operation_ = {};
                state_.run = std::get<persistence::ReviewSummary>(result);
                advance();
            });
    connect(&database_, &persistence::Database::reviewEvidenceLoaded, this,
            [this](QUuid id, const persistence::LoadReviewEvidenceResult& result) {
                if (!accept(id)) {
                    return;
                }
                if (const auto* error = std::get_if<ReviewError>(&result)) {
                    if (error->code == ReviewErrorCode::Missing) {
                        operation_ = {};
                        if (state_.cancelling || failurePending_) {
                            advance();
                        } else {
                            preparePosition();
                        }
                    } else {
                        handleError(*error);
                    }
                    return;
                }
                operation_ = {};
                advancePosition();
            });
    connect(&database_, &persistence::Database::analysisFound, this,
            [this](QUuid id, const persistence::FindAnalysisResult& result) {
                if (!accept(id)) {
                    return;
                }
                if (const auto* error = std::get_if<persistence::CacheError>(&result)) {
                    if (error->code == persistence::CacheErrorCode::Busy) {
                        handleError({ReviewErrorCode::Busy, error->message});
                        return;
                    }
                    // Disposable-cache failure does not prevent durable evidence collection.
                    operation_ = {};
                    state_.notice = tr("Cache lookup unavailable; collecting fresh evidence.");
                    search();
                    return;
                }
                operation_ = {};
                if (const auto& hit = std::get<persistence::CacheLookup>(result).hit; hit) {
                    checkpoint(stage_ == review::EvidenceStage::Quick ? JobStatus::QuickScanning
                                                                      : JobStatus::Deepening,
                               review::JobFailure::None, *hit);
                } else {
                    search();
                }
            });
    connect(
        &database_, &persistence::Database::analysisStored, this,
        [this](QUuid id, const persistence::StoreAnalysisResult& result) {
            if (id != cacheWrite_) {
                return;
            }
            cacheWrite_ = {};
            if (std::holds_alternative<persistence::CacheError>(result)) {
                state_.notice = tr(
                    "The disposable cache was not updated; committed review evidence is retained.");
            }
            publish();
            finishClose();
        });
    connect(&database_, &persistence::Database::reviewCheckpointed, this,
            [this](QUuid id, const persistence::CheckpointReviewResult& result) {
                if (!accept(id)) {
                    return;
                }
                if (const auto* error = std::get_if<ReviewError>(&result)) {
                    handleError(*error);
                    return;
                }
                operation_ = {};
                state_.run = std::get<persistence::ReviewSummary>(result);
                if (terminalWrite_) {
                    finish();
                    return;
                }
                if (statusWrite_) {
                    statusWrite_ = false;
                    advance();
                    return;
                }
                advancePosition();
            });
    connect(&supervisor_, &engine::EngineSupervisor::scheduledChanged, this,
            [this](const engine::ScheduledState& value) {
                if (value.session == session_ && value.request == search_ && value.job == job_) {
                    attempt_ = value.attempt;
                }
            });
    connect(&supervisor_, &engine::EngineSupervisor::scheduledFinished, this,
            [this](const engine::ScheduledResult& result) {
                if (result.identity.session != session_ || result.identity.request != search_ ||
                    result.identity.job != job_) {
                    return;
                }
                search_ = {};
                if (result.outcome == engine::ScheduledOutcome::Completed && result.evidence &&
                    result.evidence->identity.session == session_ &&
                    result.evidence->identity.request == result.identity.request &&
                    result.evidence->identity.job == job_ && result.identity.attempt &&
                    result.evidence->evidence.session == session_ &&
                    result.evidence->evidence.request == *result.identity.attempt &&
                    result.identity.attempt == result.evidence->identity.attempt &&
                    (!attempt_ || result.identity.attempt == attempt_) &&
                    result.evidence->evidence.provenance &&
                    result.evidence->evidence.provenance->fingerprint == searchFingerprint_) {
                    save(engine::AnalysisResult{
                        result.evidence->evidence, engine::AnalysisOutcome::Completed, {}});
                } else if (state_.cancelling &&
                           result.outcome == engine::ScheduledOutcome::Cancelled) {
                    advance();
                } else {
                    fail(tr("Engine analysis failed. Reconnect the original engine and explicitly "
                            "resume."),
                         review::JobFailure::Engine);
                }
            });
}
ReviewGameController::~ReviewGameController() {
    Q_ASSERT_X(worker_.waitForDone(0), "ReviewGameController", "Await closed()");
}
review::Quality ReviewGameController::defaultQuality() const {
    const auto configuration = configuration_.snapshot();
    return configuration ? configuration->values.quality : review::Quality::Standard;
}
void ReviewGameController::publish() {
    if (closed_) {
        return;
    }
    moves_.bind(state_.run);
    state_.busy = state_.active || workerBusy_ || !pending_.isNull() || retry_.isActive();
    const QScopedValueRollback notification(notifying_, true);
    emit changed();
}
void ReviewGameController::setReady(bool ready) {
    state_.ready = ready;
    publish();
}
void ReviewGameController::request(std::function<void(QUuid)> operation, bool durable) {
    durableOperation_ = durable;
    Q_ASSERT(pending_.isNull());
    retryMs_ = 25;
    operation_ = std::move(operation);
    pending_ = QUuid::createUuid();
    operation_(pending_);
    publish();
}
bool ReviewGameController::accept(QUuid id) {
    if (id.isNull() || id != pending_) {
        return false;
    }
    pending_ = {};
    return true;
}
void ReviewGameController::handleError(const ReviewError& error) {
    if (error.code == ReviewErrorCode::Busy && operation_) {
        if ((state_.cancelling || closing_) && !durableOperation_) {
            operation_ = {};
            if (state_.active) {
                advance();
            } else {
                finishClose();
            }
            return;
        }
        retry_.start(retryMs_);
        retryMs_ = std::min(500, retryMs_ * 2);
        publish();
        return;
    }
    operation_ = {};
    if (error.code == ReviewErrorCode::Conflict) {
        const auto id = state_.run ? state_.run->id : 0;
        state_.error = tr("The saved run changed. Reloaded state requires explicit resume.");
        finish();
        if (!closing_ && id) {
            loadInspection(id);
        }
    } else if (terminalWrite_) {
        state_.error = tr("Run status could not be saved. Previously committed evidence is "
                          "retained; restart may mark this run Interrupted.");
        finish();
    } else {
        fail(error.message, review::JobFailure::Storage);
    }
}
void ReviewGameController::selectGame(persistence::GameId id) {
    if (closing_ || state_.busy || !state_.ready || id <= 0) {
        return;
    }
    ++generation_;
    listing_ = {};
    listRetry_.stop();
    game_.reset();
    stored_.reset();
    state_.run.reset();
    state_.overview.reset();
    state_.overviewError.clear();
    state_.runs.clear();
    state_.page = 0;
    state_.hasNext = false;
    state_.listing = false;
    nextInspection_.reset();
    state_.gameId = id;
    state_.engine.clear();
    state_.error.clear();
    state_.notice.clear();
    state_.message.clear();
    state_.phase = ReviewPhase::Loading;
    request([this, id](QUuid requestId) { database_.loadGame(requestId, id); });
}
void ReviewGameController::refreshRuns(std::int64_t page) {
    if (closing_ || !state_.ready || !state_.gameId || !listing_.isNull() || page < 0 ||
        page > 1000000) {
        return;
    }
    state_.page = page;
    state_.listing = true;
    listing_ = QUuid::createUuid();
    database_.listReviews(listing_, state_.gameId, page * 50, 51);
    publish();
}
void ReviewGameController::inspect(review::ReviewId id) {
    if (closing_ || state_.active || !game_ || id <= 0) {
        return;
    }
    if (state_.busy) {
        nextInspection_ = id;
        return;
    }
    state_.error.clear();
    loadInspection(id);
}
void ReviewGameController::loadInspection(review::ReviewId id) {
    terminalWrite_ = false;
    state_.overview.reset();
    state_.overviewError.clear();
    state_.run.reset();
    stored_.reset();
    state_.engine.clear();
    state_.phase = ReviewPhase::Loading;
    request([this, id](QUuid requestId) { database_.loadReview(requestId, id); });
}
QString ReviewGameController::startBlocker(std::int64_t profileId) const {
    if (!state_.ready || closing_) {
        return tr("Local storage is unavailable.");
    }
    if (state_.busy) {
        return tr("Wait for the current operation or cancel the active run.");
    }
    if (!game_) {
        return tr("Select a saved game from Games.");
    }
    if (configuration_.state() != ConfigurationState::Valid) {
        return tr("Fix the review configuration and restart.");
    }
    const auto& settings = settings_.state();
    if (!settings.ready || settings.registration != EngineRegistrationState::Idle) {
        return tr("Wait for engine registration or connection setup to finish in Settings.");
    }
    if (std::none_of(settings.profiles.begin(), settings.profiles.end(),
                     [profileId](const auto& profile) { return profile.id == profileId; })) {
        return tr("Choose a registered engine. Manage engines in Settings.");
    }
    if (engine::runtimeActive(settings.runtime.phase) && settings.runtime.profileId != profileId) {
        return tr("Disconnect the other engine in Settings before running this review.");
    }
    if (engine::runtimeActive(settings.runtime.phase) &&
        settings.runtime.phase != engine::RuntimePhase::Ready) {
        return tr("Wait for the engine connection to finish.");
    }
    return {};
}
void ReviewGameController::start(chess::Color side, std::int64_t profileId,
                                 review::Quality quality) {
    if (!startBlocker(profileId).isEmpty() ||
        (side != chess::Color::White && side != chess::Color::Black)) {
        return;
    }
    auto values = configuration_.snapshot()->values;
    values.quality = quality;
    auto validated = config::validate(values);
    const auto* result =
        std::get_if<std::shared_ptr<const config::ValidatedConfiguration>>(&validated);
    if (!result) {
        return;
    }
    runConfiguration_ = **result;
    side_ = side;
    profileId_ = profileId;
    refinementEnabled_ = true;
    resuming_ = false;
    stored_.reset();
    state_.run.reset();
    state_.active = true;
    state_.cancelling = false;
    state_.error.clear();
    state_.notice.clear();
    failurePending_ = false;
    terminalWrite_ = false;
    identityReady_ = false;
    index_ = 0;
    stage_ = review::EvidenceStage::Quick;
    planned_ = false;
    planning_ = false;
    statusWrite_ = false;
    refinementPositions_.clear();
    state_.overview.reset();
    state_.overviewError.clear();
    state_.refinementDone = 0;
    state_.refinementTotal = 0;
    job_ = QUuid::createUuid();
    stop_ = std::stop_source{};
    ++generation_;
    connectSelected(false);
}
void ReviewGameController::resume(review::ReviewId id) {
    if (closing_ || state_.busy || !game_ || !state_.ready ||
        configuration_.state() != ConfigurationState::Valid || id <= 0) {
        return;
    }
    state_.active = true;
    state_.cancelling = false;
    state_.error.clear();
    state_.notice.clear();
    failurePending_ = false;
    terminalWrite_ = false;
    identityReady_ = false;
    state_.overview.reset();
    state_.run.reset();
    stored_.reset();
    resuming_ = true;
    index_ = 0;
    stage_ = review::EvidenceStage::Quick;
    planned_ = false;
    planning_ = false;
    statusWrite_ = false;
    refinementPositions_.clear();
    state_.overview.reset();
    state_.overviewError.clear();
    state_.refinementDone = 0;
    state_.refinementTotal = 0;
    job_ = QUuid::createUuid();
    stop_ = std::stop_source{};
    ++generation_;
    state_.phase = ReviewPhase::Loading;
    request([this, id](QUuid requestId) { database_.loadReview(requestId, id); });
}
void ReviewGameController::connectSelected(bool fresh) {
    // Settings emits synchronously; other listeners may nest the event loop before return.
    const QScopedValueRollback notification(notifying_, true);
    if (state_.cancelling) {
        advance();
        return;
    }
    const auto& runtime = settings_.state().runtime;
    if (engine::runtimeActive(runtime.phase) && runtime.profileId != profileId_) {
        fail(tr("Disconnect the other engine in Settings, then resume or start a new run."),
             review::JobFailure::Configuration);
        return;
    }
    if (std::none_of(settings_.state().profiles.begin(), settings_.state().profiles.end(),
                     [this](const auto& profile) { return profile.id == profileId_; })) {
        fail(tr("Load the original engine profile page in Settings before resuming."),
             review::JobFailure::Configuration);
        return;
    }
    if (!settings_.state().ready ||
        settings_.state().registration != EngineRegistrationState::Idle) {
        fail(tr("Finish engine registration in Settings before starting this run."),
             review::JobFailure::Configuration);
        return;
    }
    state_.phase = ReviewPhase::Connecting;
    if (runtime.phase == engine::RuntimePhase::Disconnecting) {
        reconnecting_ = true;
        session_ = runtime.session;
    } else if (fresh && runtime.phase == engine::RuntimePhase::Ready) {
        reconnecting_ = true;
        session_ = runtime.session;
        settings_.disconnectEngine();
    } else if (runtime.phase == engine::RuntimePhase::Ready) {
        session_ = runtime.session;
        prepareIdentity();
    } else {
        reconnecting_ = false;
        settings_.connectEngine(profileId_);
        session_ = settings_.state().runtime.session;
    }
    publish();
}
void ReviewGameController::runtimeChanged() {
    if (!state_.active || identityReady_ || workerBusy_ || failurePending_) {
        publish();
        return;
    }
    const auto& runtime = settings_.state().runtime;
    if (state_.phase != ReviewPhase::Connecting) {
        publish();
        return;
    }
    if (reconnecting_ && runtime.phase == engine::RuntimePhase::Disconnected) {
        reconnecting_ = false;
        if (state_.cancelling) {
            advance();
            return;
        }
        connectSelected(false);
    } else if (runtime.profileId == profileId_ && runtime.phase == engine::RuntimePhase::Ready) {
        session_ = runtime.session;
        prepareIdentity();
    } else if (runtime.phase == engine::RuntimePhase::Failed ||
               runtime.phase == engine::RuntimePhase::Disconnected) {
        fail(tr("Engine connection ended before review started. Check Settings and try again."),
             review::JobFailure::Engine);
    }
    publish();
}
void ReviewGameController::prepareIdentity() {
    const auto discovery = settings_.state().runtime.discovery;
    if (!discovery) {
        fail(tr("Engine discovery is unavailable."), review::JobFailure::Engine);
        return;
    }
    workerBusy_ = true;
    state_.phase = ReviewPhase::Preparing;
    const auto generation = generation_;
    worker_.start([this, discovery, generation] {
        std::variant<engine::EngineFingerprint, engine::Error> result =
            engine::Error{engine::ErrorCode::InvalidRequest,
                          tr("Could not prepare the review engine configuration.")};
        try {
            result = engine::quickReviewFingerprint(*discovery);
        } catch (const std::exception&) { /* Keep the explicit preparation failure. */
        }
        QMetaObject::invokeMethod(
            this,
            [this, generation, result = std::move(result)] {
                workerBusy_ = false;
                if (generation != generation_) {
                    finishClose();
                    return;
                }
                if (const auto* error = std::get_if<engine::Error>(&result)) {
                    fail(error->message, review::JobFailure::Configuration);
                    return;
                }
                fingerprint_ = std::get<engine::EngineFingerprint>(result);
                state_.engine = displayName(fingerprint_.reportedName);
                identityReady_ = true;
                if (state_.cancelling) {
                    advance();
                    return;
                }
                if (settings_.state().runtime.phase != engine::RuntimePhase::Ready ||
                    settings_.state().runtime.session != session_) {
                    fail(tr("The review engine disconnected during preparation. Reconnect and "
                            "explicitly resume."),
                         review::JobFailure::Engine);
                    return;
                }
                if (resuming_) {
                    if (fingerprint_ != stored_->settings.fingerprint) {
                        // No continuation has committed: leave the original run untouched.
                        state_.error = tr("The engine fingerprint differs from this run. Use the "
                                          "original engine or start a new run.");
                        finish();
                        return;
                    }
                    persistence::ContinueReview continuation{stored_->summary.id,
                                                             stored_->summary.revision,
                                                             fingerprint_, runConfiguration_};
                    request([this, continuation](
                                QUuid id) { database_.continueReview(id, continuation); },
                            true);
                } else {
                    persistence::CreateReview creation{state_.gameId,     profileId_,
                                                       fingerprint_,      side_,
                                                       runConfiguration_, refinementEnabled_};
                    request([this, creation](QUuid id) { database_.createReview(id, creation); },
                            true);
                }
            },
            Qt::QueuedConnection);
    });
    publish();
}
void ReviewGameController::advance() {
    if (!pending_.isNull() || workerBusy_ || !search_.isNull() || retry_.isActive()) {
        return;
    }
    if (state_.cancelling || failurePending_) {
        if (state_.run && (!resuming_ || identityReady_) &&
            state_.run->status < JobStatus::Complete) {
            checkpoint(failurePending_ ? JobStatus::Failed : JobStatus::Cancelled,
                       failurePending_ ? failure_ : review::JobFailure::None);
        } else {
            finish();
        }
        return;
    }
    if (stage_ != review::EvidenceStage::Quick) {
        if (state_.run->status == JobStatus::Deriving) {
            checkpoint(JobStatus::Complete);
            return;
        }
        if (refinementIndex_ >= refinementPositions_.size()) {
            if (stage_ == review::EvidenceStage::Deepening) {
                stage_ = review::EvidenceStage::Verification;
                refinementIndex_ = 0;
                statusWrite_ = true;
                checkpoint(JobStatus::Deepening);
            } else {
                statusWrite_ = true;
                checkpoint(JobStatus::Deriving);
            }
            return;
        }
        index_ = refinementPositions_[refinementIndex_];
    } else if (index_ >= state_.run->totalPositions) {
        if (refinementEnabled_ && !planned_) {
            planning_ = true;
            refreshOverview();
        } else {
            checkpoint(JobStatus::Complete);
        }
        return;
    }
    if (resuming_ || stage_ != review::EvidenceStage::Quick) {
        state_.phase = ReviewPhase::LookingUp;
        const auto id = state_.run->id;
        const auto index = index_;
        request([this, id, index](QUuid requestId) {
            database_.loadReviewEvidence(requestId, id, index, stage_);
        });
    } else {
        preparePosition();
    }
}
void ReviewGameController::advancePosition() {
    if (stage_ == review::EvidenceStage::Quick) {
        ++index_;
    } else {
        ++refinementIndex_;
        ++state_.refinementDone;
    }
    if (!state_.cancelling && !failurePending_ &&
        ((stage_ == review::EvidenceStage::Quick && (index_ == 2 || index_ % 50 == 0)) ||
         (stage_ != review::EvidenceStage::Quick && refinementIndex_ % 50 == 0))) {
        refreshOverview();
        return;
    }
    advance();
}
std::uint64_t ReviewGameController::nodeBudget() const {
    const auto& b = runConfiguration_.values
                        .budgets[static_cast<std::size_t>(runConfiguration_.values.quality)];
    if (stage_ == review::EvidenceStage::Deepening) {
        return b.deepeningNodes;
    }
    if (stage_ == review::EvidenceStage::Verification) {
        return b.verificationNodes;
    }
    return b.quickScanNodes;
}
void ReviewGameController::refreshOverview(int keyOffset) {
    if (closing_ || !state_.run || !pending_.isNull() || !search_.isNull() || workerBusy_) {
        return;
    }
    const auto id = state_.run->id;
    const auto revision = state_.run->revision;
    request([this, id, revision, keyOffset](QUuid requestId) {
        database_.loadReviewOverview(requestId, id, revision, keyOffset);
    });
}
void ReviewGameController::selectMoment(int index) {
    if (!state_.run || index < 1 || index >= state_.run->totalPositions) {
        return;
    }
    moves_.showPage(((index - 1) / 50) * 50);
    // Selection is retained until the requested asynchronous page arrives.
    moves_.select(index);
}
void ReviewGameController::preparePosition() {
    workerBusy_ = true;
    state_.phase = ReviewPhase::Preparing;
    const auto document = game_->document;
    const auto index = index_;
    const auto generation = generation_;
    const auto stop = stop_.get_token();
    const auto fingerprint = fingerprint_;
    const auto nodes = nodeBudget();
    const auto stage = stage_;
    int maximumWidth = 1;
    if (stage != review::EvidenceStage::Quick && settings_.state().runtime.discovery) {
        const auto& capabilities = settings_.state().runtime.discovery->capabilities;
        for (const auto& option : capabilities.options) {
            if (option.name.compare(QStringLiteral("MultiPV"), Qt::CaseInsensitive) == 0 &&
                option.maximum && capabilities.supportsMultiPv) {
                maximumWidth = static_cast<int>(
                    std::min<qint64>(*option.maximum, runConfiguration_.values.refinement.multiPv));
            }
        }
    }
    worker_.start([this, document, index, generation, stop, fingerprint, nodes, maximumWidth,
                   stage] {
        engine::CacheQuery query;
        std::shared_ptr<const chess::Position> position;
        bool terminal = false;
        QString error;
        try {
            if (cursor_->document != document || !cursor_->position || cursor_->index > index) {
                auto root = document->tree.position(document->tree.rootId());
                if (auto* value = std::get_if<chess::Position>(&root)) {
                    cursor_->position = std::move(*value);
                    cursor_->index = 0;
                    cursor_->document = document;
                } else {
                    error = tr("Could not prepare the saved game root.");
                }
            }
            while (error.isEmpty() && cursor_->index < index && !stop.stop_requested()) {
                const auto moves = document->originalMainline();
                if (static_cast<std::size_t>(cursor_->index) >= moves.size() ||
                    std::holds_alternative<chess::Error>(
                        cursor_->position->play(moves[static_cast<std::size_t>(cursor_->index)]))) {
                    error = tr("The original mainline could not be prepared.");
                    break;
                }
                ++cursor_->index;
            }
            if (error.isEmpty() && !stop.stop_requested()) {
                terminal = cursor_->position->legalMoves().empty();
                position = std::make_shared<const chess::Position>(*cursor_->position);
                const int width = std::max(
                    1, std::min(maximumWidth, static_cast<int>(position->legalMoves().size())));
                query = engine::CacheQuery{position->context(),
                                           stage == review::EvidenceStage::Quick
                                               ? fingerprint
                                               : engine::reviewWidthFingerprint(fingerprint, width),
                                           engine::NodeLimit{nodes},
                                           width,
                                           true,
                                           {}};
            }
        } catch (const std::exception&) {
            error = tr("Position preparation failed.");
        }
        QMetaObject::invokeMethod(
            this,
            [this, generation, position = std::move(position), terminal, error,
             query = std::move(query)]() mutable {
                workerBusy_ = false;
                if (generation != generation_) {
                    finishClose();
                    return;
                }
                if (!error.isEmpty()) {
                    fail(error, review::JobFailure::Evidence);
                    return;
                }
                if (state_.cancelling || !position) {
                    advance();
                    return;
                }
                positionReady(std::move(position), terminal, std::move(query));
            },
            Qt::QueuedConnection);
    });
    publish();
}
void ReviewGameController::positionReady(std::shared_ptr<const chess::Position> position,
                                         bool terminal, engine::CacheQuery query) {
    position_ = std::move(position);
    searchFingerprint_ = query.fingerprint;
    searchWidth_ = query.multiPv;
    if (terminal) {
        save({}, true);
        return;
    }
    if (stage_ == review::EvidenceStage::Verification ||
        !engine::verifiedResources(searchFingerprint_)) {
        search();
        return;
    }
    state_.phase = ReviewPhase::LookingUp;
    request([this, query = std::move(query)](QUuid id) { database_.findAnalysis(id, query); });
}
void ReviewGameController::search() {
    if (state_.cancelling || failurePending_) {
        advance();
        return;
    }
    if (settings_.state().runtime.phase != engine::RuntimePhase::Ready ||
        settings_.state().runtime.session != session_) {
        fail(tr("The review engine disconnected. Reconnect and explicitly resume."),
             review::JobFailure::Engine);
        return;
    }
    state_.phase =
        stage_ == review::EvidenceStage::Quick ? ReviewPhase::Scanning : ReviewPhase::Refining;
    search_ = QUuid::createUuid();
    attempt_.reset();
    engine::AnalysisRequest request;
    request.id = search_;
    request.position = *position_;
    request.limit = engine::NodeLimit{nodeBudget()};
    request.multiPv = searchWidth_;
    request.wantWdl = true;
    supervisor_.scheduleAnalysis(session_, request,
                                 stage_ == review::EvidenceStage::Quick
                                     ? engine::AnalysisPriority::ReviewQuickScan
                                     : engine::AnalysisPriority::ReviewBackgroundDeepening,
                                 job_);
    publish();
}
void ReviewGameController::save(std::optional<engine::AnalysisResult> evidence, bool terminal) {
    if (evidence && cacheWrite_.isNull()) {
        cacheWrite_ = QUuid::createUuid();
        database_.storeAnalysis(cacheWrite_, *evidence);
    }
    checkpoint(stage_ == review::EvidenceStage::Quick ? JobStatus::QuickScanning
                                                      : JobStatus::Deepening,
               review::JobFailure::None, std::move(evidence), terminal);
}
void ReviewGameController::checkpoint(JobStatus status, review::JobFailure failure,
                                      std::optional<engine::AnalysisResult> evidence,
                                      bool terminal) {
    terminalWrite_ = status >= JobStatus::Complete;
    persistence::ReviewCheckpoint checkpoint;
    checkpoint.reviewId = state_.run->id;
    checkpoint.expectedRevision = state_.run->revision;
    checkpoint.status = status;
    checkpoint.failure = failure;
    if (evidence || terminal) {
        checkpoint.positionIndex = index_;
    }
    checkpoint.analysis = std::move(evidence);
    checkpoint.stage = checkpoint.positionIndex ? stage_ : review::EvidenceStage::Quick;
    checkpoint.terminal = terminal;
    state_.phase = ReviewPhase::Saving;
    request([this, checkpoint = std::move(checkpoint)](
                QUuid id) { database_.checkpointReview(id, checkpoint); },
            true);
}
void ReviewGameController::fail(const QString& message, review::JobFailure failure) {
    state_.error = message;
    failurePending_ = true;
    failure_ = failure;
    operation_ = {};
    retry_.stop();
    if (!search_.isNull()) {
        supervisor_.cancelAnalysisJob(session_, job_);
    }
    if (!state_.active) {
        finish();
    } else {
        advance();
    }
}
void ReviewGameController::finish() {
    planning_ = false;
    statusWrite_ = false;
    state_.active = false;
    state_.cancelling = false;
    state_.phase = ReviewPhase::Finished;
    operation_ = {};
    retry_.stop();
    position_.reset();
    state_.message = state_.run ? reviewStatusText(state_.run->status) : tr("Review not started");
    if (state_.run && state_.run->status == JobStatus::Complete) {
        state_.message = tr("Processing complete. Stable, provisional and withheld results are "
                            "identified separately.");
    }
    publish();
    if (!closing_) {
        refreshRuns();
        if (!state_.active && state_.error.isEmpty()) {
            refreshOverview();
        }
    }
    finishClose();
}
void ReviewGameController::cancel() {
    if (!state_.active || state_.cancelling) {
        return;
    }
    state_.cancelling = true;
    stop_.request_stop();
    if (retry_.isActive() && !durableOperation_) {
        retry_.stop();
        operation_ = {};
    }
    if (!search_.isNull()) {
        supervisor_.cancelAnalysisJob(session_, job_);
    }
    advance();
    publish();
}
void ReviewGameController::close() {
    if (closing_) {
        return;
    }
    closing_ = true;
    moves_.close();
    nextInspection_.reset();
    listRetry_.stop();
    if (retry_.isActive() && !durableOperation_) {
        retry_.stop();
        operation_ = {};
    }
    cancel();
    finishClose();
}
void ReviewGameController::finishClose() {
    if (!closing_ || closed_ || closeQueued_ || !moves_.drained() || state_.active || workerBusy_ ||
        !pending_.isNull() || !search_.isNull() || !cacheWrite_.isNull() || !listing_.isNull() ||
        retry_.isActive()) {
        return;
    }
    closeQueued_ = true;
    QTimer::singleShot(0, this, &ReviewGameController::deliverClose);
}
void ReviewGameController::deliverClose() {
    // A changed() consumer may close us and run a nested event loop. Do not authorize
    // destruction until its notification stack and the last worker have retired.
    if (notifying_ || !worker_.waitForDone(0)) {
        QTimer::singleShot(5, this, &ReviewGameController::deliverClose);
        return;
    }
    closed_ = true;
    emit closed();
}
} // namespace chess_review::app
