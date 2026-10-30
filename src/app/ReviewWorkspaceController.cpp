#include "app/ReviewWorkspaceController.h"
#include "app/ReviewGameController.h"
#include <QScopedValueRollback>
#include <algorithm>
#include <utility>
namespace chess_review::app {
using namespace persistence;
ReviewWorkspaceController::ReviewWorkspaceController(Database& database,
                                                     EngineSettingsController& settings,
                                                     engine::EngineSupervisor& supervisor,
                                                     ReviewConfiguration& configuration,
                                                     ReviewGameController& task)
    : database_(database), configuration_(configuration), task_(task), moves_(database),
      navigation_(database, moves_), exploration_(database, navigation_, settings, supervisor) {
    retry_.setSingleShot(true);
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(250);
    recoveryRetry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, [this] {
        if (operation_) {
            pending_ = QUuid::createUuid();
            operation_(pending_);
        }
    });
    connect(&saveTimer_, &QTimer::timeout, this, &ReviewWorkspaceController::flush);
    connect(&recoveryRetry_, &QTimer::timeout, this, [this] { refreshRecovery(recoveryPage_); });
    connect(&task_, &ReviewGameController::taskChanged, this,
            &ReviewWorkspaceController::taskChanged);
    connect(&configuration_, &ReviewConfiguration::changed, this, [this] {
        updateCompatibility();
        publish();
    });
    connect(&moves_, &ReviewMovesController::changed, this, &ReviewWorkspaceController::publish);
    connect(&moves_, &ReviewMovesController::drainedChanged, this,
            &ReviewWorkspaceController::finishClose);
    connect(&navigation_, &ReviewNavigationController::closed, this, [this] {
        navigationClosed_ = true;
        finishClose();
    });
    connect(&exploration_, &ReviewExplorationController::closed, this, [this] {
        explorationClosed_ = true;
        finishClose();
    });
    connect(&navigation_, &ReviewNavigationController::selectionCommitted, this, [this](int index) {
        userMoment_ = index;
        restorePending_ = false;
        remember();
    });
    connect(&navigation_, &ReviewNavigationController::changed, this, [this] {
        if (restorePending_ && navigation_.ready()) {
            const auto index = restoreMoment_.value_or(0);
            restorePending_ = false;
            if (index < 0 || index >= static_cast<int>(navigation_.line().size())) {
                state_.notice = tr("The saved moment is unavailable. Returned to the game root.");
                navigation_.select(navigation_.target(0));
            } else {
                navigation_.select(navigation_.target(index));
            }
        }
        if (!contextLoading_ && !restorePending_) {
            remember();
        }
    });
    connect(&database_, &Database::gameLoaded, this,
            [this](QUuid id, const LoadGameResult& result) {
                if (!accept(id)) {
                    return;
                }
                if (const auto* issue = std::get_if<GameError>(&result)) {
                    if (issue->code == GameErrorCode::Busy) {
                        retry_.start(100);
                        return;
                    }
                    state_.error = issue->message;
                    operation_ = {};
                    state_.phase = ReviewPhase::Finished;
                    publish();
                    emit restoreFailed();
                    return;
                }
                operation_ = {};
                const auto generation = generation_;
                game_ = std::get<StoredGame>(result);
                const auto player = [](const QString& name, const QString& fallback) {
                    auto text = name.left(256);
                    if (!text.isEmpty() && text.back().isHighSurrogate()) {
                        text.chop(1);
                    }
                    text = text.simplified();
                    return text.isEmpty() ? fallback : text;
                };
                state_.game = tr("Game %1: %2 — %3")
                                  .arg(game_->summary.id)
                                  .arg(player(game_->summary.white, tr("White")),
                                       player(game_->summary.black, tr("Black")));
                navigation_.open(*game_);
                if (closing_ || generation != generation_) {
                    return;
                }
                loadContext(desiredRun_.value_or(0));
            });
    connect(&database_, &Database::reviewContextLoaded, this,
            [this](QUuid id, const LoadReviewContextResult& result) {
                if (id == restoreRequest_ && !id.isNull()) {
                    restoreRequest_ = {};
                    if (const auto* issue = std::get_if<ReviewError>(&result)) {
                        if (issue->code == ReviewErrorCode::Busy && !closing_) {
                            QTimer::singleShot(100, this, &ReviewWorkspaceController::recover);
                            return;
                        }
                        state_.notice = issue->message;
                        publish();
                        if (!closing_) {
                            emit restoreFailed();
                        }
                        emit recoveryFinished(false);
                        finishClose();
                        return;
                    }
                    const auto context = std::get<ReviewContext>(result);
                    const bool restore = !closing_ && restoreAllowed_ && generation_ == 0 &&
                                         context.reopen && context.gameId;
                    if (restore) {
                        selectGame(*context.gameId, context.reviewId);
                        emit showRequested();
                    }
                    emit recoveryFinished(restore);
                    finishClose();
                    return;
                }
                if (!accept(id)) {
                    return;
                }
                if (const auto* issue = std::get_if<ReviewError>(&result)) {
                    if (issue->code == ReviewErrorCode::Busy) {
                        retry_.start(100);
                        return;
                    }
                    operation_ = {};
                    contextLoading_ = false;
                    pendingResume_.reset();
                    state_.error = issue->message;
                    publish();
                    return;
                }
                operation_ = {};
                auto context = std::get<ReviewContext>(result);
                const auto matches = [this](const ReviewContext& pending) {
                    return pending.gameId == state_.gameId &&
                           (!desiredRun_ || pending.reviewId == desiredRun_);
                };
                if (saving_ && matches(*saving_)) {
                    context = *saving_;
                }
                for (const auto& pending : writes_) {
                    if (matches(pending)) {
                        context = pending;
                    }
                }
                if (context.invalidPosition && !userMoment_) {
                    state_.notice =
                        tr("The saved moment is unavailable. Returned to the game root.");
                    restoreMoment_ = 0;
                } else {
                    restoreMoment_ = userMoment_ ? userMoment_ : context.position;
                }
                if (context.reviewId) {
                    loadRun(*context.reviewId);
                } else {
                    contextLoading_ = false;
                    refreshRuns();
                }
            });
    connect(&database_, &Database::reviewLoaded, this,
            [this](QUuid id, const LoadReviewResult& result) {
                if (!accept(id)) {
                    return;
                }
                if (const auto* issue = std::get_if<ReviewError>(&result)) {
                    if (issue->code == ReviewErrorCode::Busy) {
                        retry_.start(100);
                        return;
                    }
                    operation_ = {};
                    contextLoading_ = false;
                    pendingResume_.reset();
                    state_.error = issue->message;
                    state_.phase = ReviewPhase::Finished;
                    publish();
                    refreshRuns();
                    return;
                }
                operation_ = {};
                auto run = std::get<StoredReview>(result);
                if (run.summary.gameId != state_.gameId) {
                    pendingResume_.reset();
                    state_.error = tr("This run belongs to another game.");
                    contextLoading_ = false;
                    publish();
                    return;
                }
                stored_ = std::move(run);
                updateCompatibility();
                if (!state_.run || state_.run->id != stored_->summary.id ||
                    state_.run->revision <= stored_->summary.revision) {
                    state_.run = stored_->summary;
                }
                // A matching live job may have advanced while this read was queued.
                const auto& liveTask = task_.taskState();
                if (liveTask.run && liveTask.run->id == state_.run->id &&
                    liveTask.run->revision > state_.run->revision) {
                    state_.run = liveTask.run;
                }
                state_.engine = stored_->settings.fingerprint.reportedName.left(256);
                state_.phase = ReviewPhase::Finished;
                state_.message = reviewStatusText(state_.run->status);
                restorePending_ = restoreMoment_.has_value();
                contextLoading_ = false;
                // Select before the overview can choose its default key moment.
                const auto generation = generation_;
                publish();
                if (closing_ || generation != generation_) {
                    return;
                }
                if (restorePending_ && navigation_.ready()) {
                    const auto index = *restoreMoment_;
                    restorePending_ = false;
                    if (index < 0 || index >= static_cast<int>(navigation_.line().size())) {
                        state_.notice =
                            tr("The saved moment is unavailable. Returned to the game root.");
                        selectMoment(0);
                    } else {
                        selectMoment(index);
                    }
                }
                remember();
                if (pendingResume_ && state_.run && *pendingResume_ == state_.run->id) {
                    const auto resumedId = *std::exchange(pendingResume_, std::nullopt);
                    resume(resumedId);
                    return;
                }
                refreshOverview();
            });
    connect(&database_, &Database::reviewsListed, this,
            [this](QUuid id, const ListReviewsResult& result) {
                if (!accept(id)) {
                    return;
                }
                state_.listing = false;
                if (const auto* issue = std::get_if<ReviewError>(&result)) {
                    if (issue->code == ReviewErrorCode::Busy) {
                        retry_.start(100);
                        return;
                    }
                    state_.notice = issue->message;
                    operation_ = {};
                    publish();
                    return;
                }
                operation_ = {};
                state_.runs = std::get<std::vector<ReviewSummary>>(result);
                state_.hasNext = state_.runs.size() > 50;
                if (state_.hasNext) {
                    state_.runs.pop_back();
                }
                if (!state_.run && !desiredRun_ && state_.error.isEmpty() && !state_.runs.empty()) {
                    desiredRun_ = state_.runs.front().id;
                    loadContext(*desiredRun_);
                    return;
                }
                state_.phase = state_.run ? ReviewPhase::Finished : ReviewPhase::Idle;
                publish();
            });
    connect(&database_, &Database::reviewOverviewLoaded, this,
            [this](QUuid id, const LoadReviewOverviewResult& result) {
                if (!accept(id)) {
                    return;
                }
                if (const auto* issue = std::get_if<ReviewError>(&result)) {
                    if (issue->code == ReviewErrorCode::Busy) {
                        retry_.start(100);
                        return;
                    }
                    operation_ = {};
                    state_.overviewError = issue->message;
                    if (issue->code == ReviewErrorCode::Conflict && state_.run) {
                        loadRun(state_.run->id);
                        return;
                    }
                } else {
                    operation_ = {};
                    const auto& overview = std::get<ReviewOverview>(result);
                    if (state_.run && overview.reviewId == state_.run->id &&
                        overview.revision == state_.run->revision) {
                        state_.overview = overview;
                        state_.overviewError.clear();
                    }
                }
                publish();
                if (state_.runs.empty()) {
                    refreshRuns();
                }
            });
    connect(&database_, &Database::reviewContextSaved, this,
            [this](QUuid id, const SaveReviewContextResult& result) {
                if (id != write_ || id.isNull()) {
                    return;
                }
                write_ = {};
                if (const auto* issue = std::get_if<ReviewError>(&result)) {
                    if (issue->code == ReviewErrorCode::Busy) {
                        writes_.push_front(*saving_);
                        saveTimer_.start(100);
                        saving_.reset();
                        return;
                    }
                    state_.notice =
                        tr("The last review selection could not be saved. %1").arg(issue->message);
                    publish();
                } else {
                    const QScopedValueRollback notification(notifying_, true);
                    emit contextCommitted();
                }
                saving_.reset();
                flush();
                finishClose();
            });
    connect(&database_, &Database::interruptedReviewsListed, this,
            [this](QUuid id, const ListReviewsResult& result) {
                if (id != recoveryPending_ || id.isNull()) {
                    return;
                }
                recoveryPending_ = {};
                if (const auto* issue = std::get_if<ReviewError>(&result)) {
                    if (issue->code == ReviewErrorCode::Busy && !closing_) {
                        recoveryRetry_.start(100);
                        return;
                    }
                    recoveryError_ = issue->message;
                } else {
                    const auto& runs = std::get<std::vector<ReviewSummary>>(result);
                    if (runs.empty() && recoveryPage_ > 0 && !closing_) {
                        refreshRecovery();
                        return;
                    }
                    interrupted_ = runs;
                    recoveryHasNext_ = interrupted_.size() > 50;
                    if (recoveryHasNext_) {
                        interrupted_.pop_back();
                    }
                    recoveryError_.clear();
                }
                emit recoveryChanged();
                finishClose();
            });
}
void ReviewWorkspaceController::request(std::function<void(QUuid)> operation) {
    if (closing_) {
        return;
    }
    if (!pending_.isNull()) {
        next_ = std::move(operation);
        return;
    }
    retry_.stop();
    operation_ = std::move(operation);
    requestedGeneration_ = generation_;
    pending_ = QUuid::createUuid();
    operation_(pending_);
    publish();
}
bool ReviewWorkspaceController::accept(QUuid id) {
    if (id.isNull() || id != pending_) {
        return false;
    }
    pending_ = {};
    if (closing_) {
        operation_ = {};
        finishClose();
        return false;
    }
    if (next_) {
        auto next = std::exchange(next_, {});
        operation_ = {};
        request(std::move(next));
        return false;
    }
    if (requestedGeneration_ != generation_) {
        operation_ = {};
        publish();
        return false;
    }
    return true;
}
bool ReviewWorkspaceController::displaysTask() const {
    const auto& task = task_.taskState();
    return game_ && task.gameId == state_.gameId &&
           (followingTask_ || (state_.run && task.run && state_.run->id == task.run->id));
}
void ReviewWorkspaceController::publish() {
    if (closed_) {
        return;
    }
    const QScopedValueRollback notification(notifying_, true);
    const auto& task = task_.taskState();
    state_.active = displaysTask() && task.active;
    state_.cancelling = state_.active && task.cancelling;
    state_.inspecting = !pending_.isNull() || retry_.isActive();
    state_.busy = state_.active || state_.inspecting;
    moves_.bind(state_.run);
    navigation_.bind(state_.run, state_.overview);
    exploration_.bind(state_.run);
    emit changed();
}
void ReviewWorkspaceController::updateCompatibility() {
    state_.compatibilityNotice.clear();
    if (stored_ && configuration_.snapshot()) {
        auto values = configuration_.snapshot()->values;
        values.quality = stored_->settings.configuration.values.quality;
        const auto validated =
            config::validate(values, stored_->settings.configuration.classificationIdentityVersion);
        if (const auto* current =
                std::get_if<std::shared_ptr<const config::ValidatedConfiguration>>(&validated)) {
            const auto& old = stored_->settings.configuration.identities;
            const auto& now = (**current).identities;
            if (old.analysis != now.analysis || old.classification != now.classification ||
                old.conversion != now.conversion ||
                stored_->settings.configuration.classificationIdentityVersion !=
                    configuration_.snapshot()->classificationIdentityVersion) {
                state_.compatibilityNotice =
                    tr("Historical review: results use the settings saved with this run. Resume "
                       "retains those settings; a new review uses current settings.");
            }
        }
    }
}
void ReviewWorkspaceController::setReady(bool ready) {
    state_.ready = ready;
    publish();
}
void ReviewWorkspaceController::selectGame(GameId id, std::optional<review::ReviewId> run) {
    if (closing_ || !state_.ready || id <= 0) {
        return;
    }
    remember();
    const auto generation = ++generation_;
    followingTask_ = false;
    restoreAllowed_ = false;
    pendingResume_.reset();
    retry_.stop();
    operation_ = {};
    next_ = {};
    game_.reset();
    stored_.reset();
    desiredRun_ = run;
    restoreMoment_.reset();
    userMoment_.reset();
    restorePending_ = false;
    contextLoading_ = true;
    navigation_.clear();
    if (closing_ || generation != generation_) {
        return;
    }
    state_ = ReviewGameState{};
    state_.ready = true;
    state_.gameId = id;
    state_.phase = ReviewPhase::Loading;
    request([this, id](QUuid request) { database_.loadGame(request, id); });
}
void ReviewWorkspaceController::loadContext(review::ReviewId id) {
    contextLoading_ = true;
    const auto game = state_.gameId;
    request([this, game, id](QUuid request) { database_.loadReviewContext(request, game, id); });
}
void ReviewWorkspaceController::inspect(review::ReviewId id) { inspectRun(id, false); }
void ReviewWorkspaceController::inspectRun(review::ReviewId id, bool resumeAfterLoad) {
    if (closing_ || !game_ || id <= 0) {
        return;
    }
    remember();
    const auto generation = ++generation_;
    followingTask_ = false;
    restoreAllowed_ = false;
    pendingResume_ = resumeAfterLoad ? std::optional{id} : std::nullopt;
    contextLoading_ = true;
    state_.listing = false;
    desiredRun_ = id;
    state_.error.clear();
    state_.notice.clear();
    state_.compatibilityNotice.clear();
    state_.engine.clear();
    state_.overviewError.clear();
    state_.message.clear();
    restoreMoment_.reset();
    userMoment_.reset();
    restorePending_ = false;
    state_.run.reset();
    state_.overview.reset();
    stored_.reset();
    navigation_.bind({}, {});
    if (closing_ || generation != generation_) {
        return;
    }
    loadContext(id);
}
void ReviewWorkspaceController::loadRun(review::ReviewId id) {
    state_.phase = ReviewPhase::Loading;
    request([this, id](QUuid request) { database_.loadReview(request, id); });
}
void ReviewWorkspaceController::refreshRuns(std::int64_t page) {
    if (closing_ || !game_ || page < 0 || page > 1000000 || !pending_.isNull() ||
        retry_.isActive()) {
        return;
    }
    state_.page = page;
    state_.listing = true;
    const auto game = state_.gameId;
    request([this, game, page](QUuid id) { database_.listReviews(id, game, page * 50, 51); });
}
void ReviewWorkspaceController::refreshOverview(int offset) {
    if (closing_ || !state_.run || offset < 0 || !pending_.isNull() || retry_.isActive()) {
        return;
    }
    const auto run = *state_.run;
    request([this, run, offset](QUuid id) {
        database_.loadReviewOverview(id, run.id, run.revision, offset);
    });
}
void ReviewWorkspaceController::selectMoment(int index) {
    restorePending_ = false;
    restoreMoment_.reset();
    navigation_.select(navigation_.target(index));
    remember();
}
void ReviewWorkspaceController::start(chess::Color side, std::int64_t profile,
                                      review::Quality quality) {
    if (closing_ || !game_ || state_.busy || !task_.startBlocker(profile).isEmpty()) {
        return;
    }
    if ((side != chess::Color::White && side != chess::Color::Black) ||
        (quality != review::Quality::Fast && quality != review::Quality::Standard &&
         quality != review::Quality::Thorough)) {
        state_.error = tr("Invalid review perspective or quality.");
        publish();
        return;
    }
    followingTask_ = true;
    pendingResume_.reset();
    task_.game_ = game_;
    task_.state_ = ReviewGameState{};
    task_.state_.ready = state_.ready;
    task_.state_.gameId = state_.gameId;
    task_.state_.game = state_.game;
    state_.run.reset();
    state_.overview.reset();
    stored_.reset();
    updateCompatibility();
    desiredRun_.reset();
    task_.startJob(side, profile, quality);
}
void ReviewWorkspaceController::resume(review::ReviewId id) {
    if (closing_ || !game_) {
        return;
    }
    const auto blocker = task_.resumeBlocker();
    if (id <= 0 || !blocker.isEmpty()) {
        state_.error = id <= 0 ? tr("Select a valid saved review to resume.") : blocker;
        publish();
        return;
    }
    if (!state_.run || state_.run->id != id) {
        inspectRun(id, true);
        return;
    }
    if (state_.run->status < review::JobStatus::Partial) {
        state_.error = tr("This run cannot be resumed. Start a new review if needed.");
        publish();
        return;
    }
    remember();
    followingTask_ = true;
    task_.game_ = game_;
    task_.state_ = ReviewGameState{};
    task_.state_.ready = state_.ready;
    task_.state_.gameId = state_.gameId;
    task_.state_.game = state_.game;
    desiredRun_ = id;
    task_.resumeJob(id);
}
void ReviewWorkspaceController::taskChanged() {
    const auto& task = task_.taskState();
    if (displaysTask() && (!task.run || !state_.run || task.run->id != state_.run->id ||
                           task.run->revision >= state_.run->revision)) {
        const bool revised =
            task.run && (!state_.run || task.run->revision != state_.run->revision);
        if (task.run) {
            state_.run = task.run;
        }
        state_.engine = task.engine;
        state_.phase = task.phase;
        state_.message = task.message;
        state_.error = task.error;
        state_.refinementDone = task.refinementDone;
        state_.refinementTotal = task.refinementTotal;
        if (task.overview &&
            (!state_.overview || task.overview->reviewId != state_.overview->reviewId ||
             task.overview->revision >= state_.overview->revision)) {
            state_.overview = task.overview;
        }
        if (revised) {
            remember();
        }
        if (!task.active && revised && pending_.isNull()) {
            refreshOverview();
        }
    }
    if (task.run && task.run->status != review::JobStatus::Interrupted &&
        std::any_of(interrupted_.begin(), interrupted_.end(),
                    [&task](const ReviewSummary& run) { return run.id == task.run->id; })) {
        refreshRecovery(recoveryPage_);
    }
    publish();
}
void ReviewWorkspaceController::returnToTask() {
    const auto& task = task_.taskState();
    if (task.gameId) {
        const bool sameRun =
            task.run ? (state_.run && task.run->id == state_.run->id) : !state_.run;
        if (state_.gameId != task.gameId || !sameRun) {
            std::optional<review::ReviewId> run;
            if (task.run) {
                run = task.run->id;
            }
            selectGame(task.gameId, run);
        }
        emit showRequested();
    }
}
void ReviewWorkspaceController::setVisible(bool visible) {
    if (visible_ != visible) {
        visible_ = visible;
        remember();
    }
}
void ReviewWorkspaceController::remember() {
    if (closing_ || contextLoading_ || restorePending_ || !game_) {
        return;
    }
    ReviewContext context;
    context.gameId = state_.gameId;
    context.reopen = visible_;
    if (state_.run) {
        context.reviewId = state_.run->id;
        context.position = navigation_.selected();
    }
    if (remembered_ && remembered_->gameId == context.gameId &&
        remembered_->reviewId == context.reviewId && remembered_->position == context.position &&
        remembered_->reopen == context.reopen) {
        return;
    }
    remembered_ = context;
    // Coalesce each run's latest selection, preserving the most recent workspace last.
    std::erase_if(writes_, [&context](const ReviewContext& old) {
        return old.reviewId == context.reviewId && old.gameId == context.gameId;
    });
    writes_.push_back(context);
    saveTimer_.start();
}
void ReviewWorkspaceController::flush() {
    if (!write_.isNull() || writes_.empty()) {
        finishClose();
        return;
    }
    saving_ = writes_.front();
    writes_.pop_front();
    write_ = QUuid::createUuid();
    database_.saveReviewContext(write_, *saving_);
}
void ReviewWorkspaceController::recover() {
    if (closing_ || !restoreRequest_.isNull()) {
        return;
    }
    restoreRequest_ = QUuid::createUuid();
    database_.loadReviewContext(restoreRequest_, 0, 0);
    refreshRecovery();
}
void ReviewWorkspaceController::refreshRecovery(std::int64_t page) {
    if (closing_ || !state_.ready || !recoveryPending_.isNull() || page < 0 || page > 1000000) {
        return;
    }
    recoveryPage_ = page;
    recoveryPending_ = QUuid::createUuid();
    database_.listInterruptedReviews(recoveryPending_, page * 50, 51);
    emit recoveryChanged();
}
void ReviewWorkspaceController::close() {
    if (closing_) {
        return;
    }
    remember();
    closing_ = true;
    retry_.stop();
    recoveryRetry_.stop();
    saveTimer_.stop();
    next_ = {};
    operation_ = {};
    moves_.close();
    exploration_.close();
    navigation_.close();
    flush();
    finishClose();
}
void ReviewWorkspaceController::finishClose() {
    if (!closing_ || closed_ || !navigationClosed_ || !explorationClosed_ || !moves_.drained() ||
        !pending_.isNull() || !restoreRequest_.isNull() || !recoveryPending_.isNull() ||
        !write_.isNull() || !writes_.empty() || saveTimer_.isActive()) {
        return;
    }
    if (notifying_) {
        QTimer::singleShot(5, this, &ReviewWorkspaceController::finishClose);
        return;
    }
    closed_ = true;
    QTimer::singleShot(0, this, &ReviewWorkspaceController::closed);
}
} // namespace chess_review::app
