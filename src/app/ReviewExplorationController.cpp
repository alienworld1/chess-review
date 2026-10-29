#include "app/ReviewExplorationController.h"
#include "analysis/ReviewEvidenceAdapter.h"
#include "engine/EffectiveSearch.h"
#include <QMetaObject>
#include <algorithm>
#include <exception>
#include <utility>

namespace chess_review::app {
ReviewExplorationController::ReviewExplorationController(persistence::Database& database,
                                                         ReviewNavigationController& navigation,
                                                         EngineSettingsController& settings,
                                                         engine::EngineSupervisor& supervisor)
    : database_(database), navigation_(navigation), settings_(settings), supervisor_(supervisor),
      linkedEngine_(linked_, settings, supervisor) {
    worker_.setMaxThreadCount(1);
    success_.setSingleShot(true);
    retryRead_.setSingleShot(true);
    connect(&success_, &QTimer::timeout, this, &ReviewExplorationController::returnToGame);
    connect(&retryRead_, &QTimer::timeout, this, &ReviewExplorationController::request);
    connect(&navigation_, &ReviewNavigationController::changed, this,
            &ReviewExplorationController::synchronize);
    connect(&sandbox_, &AnalysisController::changed, this, [this] {
        if (!closing_) {
            displayedBoard_ = sandbox_.board();
            state_.pendingBoard = sandbox_.busy();
            displayedBoard_.interactive = sandbox_.board().interactive &&
                                          state_.mode == ExplorationMode::Retry && !workerBusy_;
            if (state_.mode == ExplorationMode::Line && evidence_ && !sandbox_.busy()) {
                state_.step = static_cast<int>(sandbox_.currentNode().value) - evidence_->rootPly;
            }
            publish();
        }
    });
    connect(&sandbox_, &AnalysisController::moveAccepted, this,
            &ReviewExplorationController::grade);
    connect(&sandbox_, &AnalysisController::failed, this, [this](const QString& message) {
        returnToGame();
        state_.message = message;
        publish();
    });
    connect(&sandbox_, &AnalysisController::closed, this, [this] {
        sandboxClosed_ = true;
        finishClose();
    });
    connect(&linked_, &AnalysisController::changed, this, [this] {
        if (openingLinked_ && !linked_.busy()) {
            openingLinked_ = false;
            if (!linked_.error().isEmpty() || !linked_.position()) {
                linkedOrigin_.reset();
                linkedEvidence_.reset();
                linkedState_ = {};
            }
            publish();
        }
    });
    connect(&linked_, &AnalysisController::closed, this, [this] {
        linkedClosed_ = true;
        finishClose();
    });
    connect(&linkedEngine_, &EngineAnalysisController::closed, this, [this] {
        engineClosed_ = true;
        finishClose();
    });
    connect(
        &database_, &persistence::Database::reviewExplorationLoaded, this,
        [this](QUuid id, const persistence::LoadReviewExplorationResult& result) {
            if (id != pending_) {
                return;
            }
            pending_ = {};
            if (closing_) {
                finishClose();
                return;
            }
            if (active()) {
                state_.loading = false;
                publish();
                return;
            }
            if (pendingGeneration_ != generation_) {
                request();
                return;
            }
            state_.loading = false;
            if (const auto* error = std::get_if<persistence::ReviewError>(&result)) {
                if (error->code == persistence::ReviewErrorCode::Busy) {
                    state_.loading = true;
                    retryRead_.start(100);
                } else {
                    state_.message = tr(
                        "Candidate evidence unavailable. Retry loading or choose another moment.");
                }
            } else {
                evidence_ = std::get<std::shared_ptr<const persistence::ReviewExploration>>(result);
                state_.candidates.clear();
                for (std::size_t i = 0; i < evidence_->candidateSans.size(); ++i) {
                    const auto& candidate = evidence_->evidence.candidates[i];
                    const auto score = review::convertEvaluation(
                        candidate.evaluation, evidence_->move.mover,
                        evidence_->run.settings.configuration.values.conversion);
                    const auto evaluation =
                        score.expected && !score.issue
                            ? tr("%1: %2% expected score%3")
                                  .arg(evidence_->move.mover == chess::Color::White ? tr("White")
                                                                                    : tr("Black"))
                                  .arg(score.expected->value * 100, 0, 'f', 1)
                                  .arg(score.expected->source ==
                                               review::ConversionSource::ApproximateCentipawns
                                           ? tr(" (approximate)")
                                           : QString{})
                            : tr("evaluation unavailable");
                    state_.candidates
                        << tr("%1 · %2 · root: %3 · %4")
                               .arg(candidate.rank)
                               .arg(evidence_->candidateSans[i], evaluation,
                                    evidence_->move.classification->stable ? tr("stable")
                                                                           : tr("provisional"));
                }
                state_.message.clear();
            }
            publish();
        });
    connect(&database_, &persistence::Database::analysisFound, this,
            [this](QUuid id, const persistence::FindAnalysisResult& result) {
                if (id != cacheRead_) {
                    return;
                }
                cacheRead_ = {};
                if (closing_ || cacheGeneration_ != generation_ ||
                    state_.mode != ExplorationMode::Grading) {
                    finishClose();
                    publish();
                    return;
                }
                if (const auto* lookup = std::get_if<persistence::CacheLookup>(&result);
                    lookup && lookup->hit) {
                    evaluateChild(*lookup->hit, true);
                } else {
                    scheduleChild();
                }
            });
    connect(&supervisor_, &engine::EngineSupervisor::scheduledFinished, this,
            [this](const engine::ScheduledResult& result) {
                if (search_.isNull() || result.identity.request != search_ ||
                    result.identity.session != session_) {
                    return;
                }
                search_ = {};
                if (closing_ || state_.mode != ExplorationMode::Grading) {
                    finishClose();
                    publish();
                    return;
                }
                if (result.outcome != engine::ScheduledOutcome::Completed || !result.evidence) {
                    showGrade({});
                    return;
                }
                evaluateChild({result.evidence->evidence, engine::AnalysisOutcome::Completed, {}});
            });
}
ReviewExplorationController::~ReviewExplorationController() { Q_ASSERT(worker_.waitForDone(0)); }
void ReviewExplorationController::publish() {
    if (closing_) {
        return;
    }
    const bool current = evidence_ && run_ && evidence_->run.summary.id == run_->id &&
                         evidence_->run.summary.revision == run_->revision &&
                         evidence_->move.positionIndex == target_.position;
    const auto* c =
        evidence_ && evidence_->move.classification ? &*evidence_->move.classification : nullptr;
    state_.canRetry = current && c && c->stable && c->label &&
                      *c->label >= review::Classification::Inaccuracy && evidence_->deep &&
                      evidence_->verified && evidence_->move.mover == run_->selectedSide &&
                      !state_.loading && !workerBusy_ && !sandbox_.busy() && search_.isNull() &&
                      cacheRead_.isNull() && !hidesSolution();
    state_.canAnalyze =
        run_ && (state_.mode == ExplorationMode::Game
                     ? !navigation_.pending() && !navigation_.previewing() && navigation_.position()
                     : state_.mode == ExplorationMode::Line && !state_.pendingBoard &&
                           !sandbox_.busy() && sandbox_.position());
    state_.notice = active() && evidence_ && !current
                        ? tr("Earlier checkpoint pinned for this exploration. Return to Game loads "
                             "current evidence.")
                        : QString{};
    emit changed();
}
const persistence::ReviewMove* ReviewExplorationController::moment() const {
    return active() && evidence_ ? &evidence_->move : navigation_.move(navigation_.selected());
}
bool ReviewExplorationController::hidesSolution() const {
    return state_.mode == ExplorationMode::Retry || state_.mode == ExplorationMode::Grading ||
           state_.mode == ExplorationMode::Success;
}
const BoardViewState& ReviewExplorationController::board() const {
    return active() ? displayedBoard_ : navigation_.board();
}
void ReviewExplorationController::bind(const std::optional<persistence::ReviewSummary>& run) {
    const bool replaced = (run ? run->id : 0) != (run_ ? run_->id : 0);
    const bool changed = replaced || (!active() && run && run_ && run->revision != run_->revision);
    run_ = run;
    if (changed) {
        target_ = {};
    }
    synchronize();
}
void ReviewExplorationController::synchronize() {
    if (closing_) {
        return;
    }
    if (!navigation_.evidenceError().isEmpty()) {
        if (!evidenceReadFailed_) {
            evidenceReadFailed_ = true;
            returnToGame();
            evidence_.reset();
            state_.candidates.clear();
            state_.loading = false;
        }
        state_.message = tr("Move evidence could not be validated. Retry move evidence before "
                            "exploring candidates.");
        publish();
        return;
    }
    if (evidenceReadFailed_) {
        evidenceReadFailed_ = false;
        target_ = {};
    }
    const auto target = navigation_.target(navigation_.selected());
    if (target != target_) {
        target_ = target;
        evidence_.reset();
        state_.candidates.clear();
        state_.message.clear();
        state_.loading = false;
        returnToGame();
        request();
    }
    publish();
}
void ReviewExplorationController::request() {
    if (closing_ || active() || evidenceReadFailed_ || !pending_.isNull() || !run_ ||
        target_.position <= 0) {
        return;
    }
    state_.loading = true;
    pending_ = QUuid::createUuid();
    pendingGeneration_ = generation_;
    database_.loadReviewExploration(pending_, run_->id, run_->revision, target_.position);
}
void ReviewExplorationController::reload() {
    if (closing_ || active()) {
        return;
    }
    ++generation_;
    evidence_.reset();
    state_.candidates.clear();
    state_.message.clear();
    request();
    publish();
}
void ReviewExplorationController::showLine(int candidate) {
    if (closing_ || hidesSolution() || !evidence_ || candidate < 0 ||
        candidate >= static_cast<int>(evidence_->evidence.candidates.size())) {
        return;
    }
    navigation_.clearPreview();
    sandbox_.setOrientation(navigation_.board().blackAtBottom);
    state_.mode = ExplorationMode::Line;
    state_.pendingBoard = true;
    displayedBoard_.interactive = false;
    state_.candidate = candidate;
    state_.step = 0;
    const auto& moves =
        evidence_->evidence.candidates[static_cast<std::size_t>(candidate)].continuation;
    state_.length = static_cast<int>(moves.size());
    sandbox_.openLine(evidence_->before, moves, 0);
    state_.message =
        tr("Candidate continuation · evaluation belongs to its root, not each position.");
    publish();
}
void ReviewExplorationController::step(int position) {
    if (state_.mode != ExplorationMode::Line || !evidence_ || !sandbox_.canNavigate()) {
        return;
    }
    state_.step = std::clamp(position, 0, state_.length);
    sandbox_.navigate(chess::NodeId{static_cast<std::uint64_t>(evidence_->rootPly + state_.step)});
}
void ReviewExplorationController::previous() {
    if (active()) {
        step(state_.step - 1);
    } else {
        navigation_.previous();
    }
}
void ReviewExplorationController::next() {
    if (active()) {
        step(state_.step + 1);
    } else {
        navigation_.next();
    }
}
void ReviewExplorationController::beginning() {
    if (active()) {
        step(0);
    } else {
        navigation_.beginning();
    }
}
void ReviewExplorationController::end() {
    if (active()) {
        step(state_.length);
    } else {
        navigation_.end();
    }
}
void ReviewExplorationController::returnToGame() {
    ++generation_;
    success_.stop();
    retryRead_.stop();
    if (!search_.isNull()) {
        supervisor_.cancelScheduledAnalysis(session_, search_);
    }
    state_.mode = ExplorationMode::Game;
    state_.message.clear();
    state_.pendingBoard = false;
    displayedBoard_.interactive = false;
    if (evidence_ && run_ && evidence_->run.summary.revision != run_->revision) {
        evidence_.reset();
        state_.candidates.clear();
        state_.loading = false;
        request();
    }
    publish();
}
void ReviewExplorationController::tryAgain() {
    if (closing_ || !state_.canRetry || !evidence_ || workerBusy_ || !search_.isNull() ||
        !cacheRead_.isNull()) {
        return;
    }
    navigation_.clearPreview();
    sandbox_.setOrientation(navigation_.board().blackAtBottom);
    ++generation_;
    hint_ = 0;
    state_.mode = ExplorationMode::Retry;
    state_.pendingBoard = true;
    displayedBoard_.interactive = false;
    state_.message = tr("Find a stronger move. You can request a hint or show the solution.");
    sandbox_.openPosition(evidence_->before);
    publish();
}
void ReviewExplorationController::hint() {
    if (state_.mode != ExplorationMode::Retry || !evidence_) {
        return;
    }
    const auto best = evidence_->move.classification->bestMove;
    if (!best) {
        return;
    }
    ++hint_;
    state_.message =
        hint_ == 1 ? tr("Look for a move that preserves more of your expected score.")
        : hint_ == 2
            ? tr("Consider the piece on %1.").arg(QString::fromStdString(best->from.name()))
            : tr("Consider moving it to %1.").arg(QString::fromStdString(best->to.name()));
    publish();
}
void ReviewExplorationController::selectSquare(chess::Square square) {
    if (state_.mode == ExplorationMode::Retry) {
        sandbox_.selectSquare(square);
    }
}
void ReviewExplorationController::clearSelection() { sandbox_.clearSelection(); }
void ReviewExplorationController::attemptMove(chess::Move move, std::uint64_t revision) {
    if (state_.mode == ExplorationMode::Retry && !workerBusy_) {
        sandbox_.attemptMove(move, revision);
    }
}
void ReviewExplorationController::flip() {
    if (active()) {
        sandbox_.flip();
    } else {
        navigation_.flip();
    }
}
void ReviewExplorationController::grade(chess::Move move) {
    if (state_.mode != ExplorationMode::Retry || !evidence_ || workerBusy_ || !search_.isNull()) {
        return;
    }
    state_.mode = ExplorationMode::Grading;
    displayedBoard_.interactive = false;
    state_.message = tr("Checking your move… Cancel or Show Solution remains available.");
    attempted_ = move;
    verifying_ = false;
    deepChild_.reset();
    prepared_.reset();
    const auto evidence = evidence_;
    const auto discovery = settings_.state().runtime.discovery;
    const bool matching =
        settings_.state().runtime.phase == engine::RuntimePhase::Ready &&
        settings_.state().runtime.profileId == evidence->run.settings.engineProfileId;
    session_ = settings_.state().runtime.session;
    const auto generation = generation_;
    workerBusy_ = true;
    worker_.start([this, evidence, discovery, matching, generation, move] {
        review::RetryResult grade;
        std::optional<engine::AnalysisRequest> request;
        std::optional<engine::CacheQuery> query;
        QString error;
        try {
            const auto& config = evidence->run.settings.configuration.values;
            grade = review::gradeRetry(*evidence->before, move, *evidence->deep,
                                       *evidence->verified, config);
            if (!grade.grade) {
                auto child = *evidence->before;
                if (std::holds_alternative<chess::Error>(child.play(move))) {
                    error = tr("The attempted move is unavailable.");
                } else if (child.legalMoves().empty()) {
                    const auto terminal = review::terminalEvidence(child);
                    grade = review::gradeRetry(*evidence->before, move, *evidence->deep,
                                               *evidence->verified, config, &terminal, &terminal);
                } else if (!matching || !discovery) {
                    error = tr("Connect the original review engine in Settings, then try this move "
                               "again.");
                } else {
                    const auto identity = engine::quickReviewFingerprint(*discovery);
                    if (const auto* fingerprint = std::get_if<engine::EngineFingerprint>(&identity);
                        fingerprint && *fingerprint == evidence->run.settings.fingerprint) {
                        request = engine::AnalysisRequest{};
                        request->position = std::move(child);
                        request->limit = engine::NodeLimit{
                            config.budgets[static_cast<std::size_t>(evidence->run.summary.quality)]
                                .deepeningNodes};
                        request->wantWdl = true;
                        query = engine::CacheQuery{
                            request->position.context(), *fingerprint, request->limit, 1, true, {}};
                    } else {
                        error = tr(
                            "The connected engine configuration differs from this saved review.");
                    }
                }
            }
        } catch (const std::exception&) {
            error = tr("Move evaluation could not be prepared.");
        }
        QMetaObject::invokeMethod(
            this,
            [this, generation, grade, request = std::move(request), query = std::move(query),
             error]() mutable {
                workerBusy_ = false;
                if (closing_ || generation != generation_) {
                    finishClose();
                    publish();
                    return;
                }
                if (request) {
                    prepared_ = std::move(request);
                    if (query) {
                        cacheRead_ = QUuid::createUuid();
                        cacheGeneration_ = generation_;
                        database_.findAnalysis(cacheRead_, std::move(*query));
                    } else {
                        scheduleChild();
                    }
                } else {
                    showGrade(grade);
                    if (generation == generation_ && !closing_ && !error.isEmpty()) {
                        state_.message += QLatin1Char('\n') + error;
                        publish();
                    }
                }
            },
            Qt::QueuedConnection);
    });
    publish();
}
void ReviewExplorationController::scheduleChild() {
    if (!prepared_ || closing_ || state_.mode != ExplorationMode::Grading) {
        return;
    }
    if (settings_.state().runtime.phase != engine::RuntimePhase::Ready ||
        settings_.state().runtime.session != session_) {
        showGrade({});
        return;
    }
    search_ = QUuid::createUuid();
    prepared_->id = search_;
    state_.message = verifying_ ? tr("Verifying your move…") : tr("Analyzing your move…");
    supervisor_.scheduleAnalysis(session_, *prepared_, engine::AnalysisPriority::RetryMove);
    publish();
}
void ReviewExplorationController::evaluateChild(engine::AnalysisResult result, bool cached) {
    const auto generation = generation_;
    const auto evidence = evidence_;
    const auto deep = deepChild_;
    const auto move = *attempted_;
    const bool verification = verifying_;
    workerBusy_ = true;
    worker_.start(
        [this, generation, evidence, deep, move, verification, cached, result = std::move(result)] {
            std::optional<review::PositionEvidence> child;
            review::RetryResult grade;
            try {
                const auto adapted = analysis::reviewEvidence(result);
                if (const auto* record = std::get_if<analysis::ReviewEvidenceRecord>(&adapted);
                    record && result.evidence.provenance &&
                    result.evidence.provenance->fingerprint == evidence->run.settings.fingerprint) {
                    child = record->evidence;
                    if (verification && deep) {
                        grade = review::gradeRetry(
                            *evidence->before, move, *evidence->deep, *evidence->verified,
                            evidence->run.settings.configuration.values, &*deep, &*child);
                    }
                }
            } catch (const std::exception&) {
                child.reset();
            }
            // A dominating cache hit can already be as strong as the saved verification
            // budget. Use a fresh first pass rather than guaranteeing an incomparable pair.
            const auto verificationBudget =
                evidence->run.settings.configuration.values
                    .budgets[static_cast<std::size_t>(evidence->run.summary.quality)]
                    .verificationNodes;
            const bool freshFirst = cached && (!child || !child->strength.nodes ||
                                               *child->strength.nodes >= verificationBudget);
            QMetaObject::invokeMethod(
                this,
                [this, generation, verification, freshFirst, child = std::move(child),
                 grade]() mutable {
                    workerBusy_ = false;
                    if (closing_ || generation != generation_) {
                        finishClose();
                        publish();
                        return;
                    }
                    if (freshFirst) {
                        scheduleChild();
                        return;
                    }
                    if (!child || verification) {
                        showGrade(grade);
                        return;
                    }
                    deepChild_ = std::move(child);
                    verifying_ = true;
                    prepared_->limit = engine::NodeLimit{
                        evidence_->run.settings.configuration.values
                            .budgets[static_cast<std::size_t>(evidence_->run.summary.quality)]
                            .verificationNodes};
                    scheduleChild();
                },
                Qt::QueuedConnection);
        });
}
void ReviewExplorationController::showGrade(review::RetryResult result) {
    const auto generation = generation_;
    if (!result.grade) {
        state_.message = tr(
            "Unable to grade: sufficient comparable evidence is unavailable. You can try again.");
    } else {
        switch (*result.grade) {
        case review::RetryGrade::Best:
            state_.message = tr("Best — well found.");
            break;
        case review::RetryGrade::Strong:
            state_.message = tr("Strong — this move preserves equivalent quality.");
            break;
        case review::RetryGrade::Okay:
            state_.message = tr("Okay — look for a stronger move. Try again.");
            break;
        case review::RetryGrade::TryAgain:
            state_.message = tr("Try again.");
            break;
        }
    }
    state_.mode = result.success() ? ExplorationMode::Success : ExplorationMode::Retry;
    if (result.success()) {
        success_.start(1200);
    } else if (evidence_) {
        sandbox_.openPosition(evidence_->before);
    }
    if (closing_ || generation != generation_) {
        return;
    }
    if (result.grade && attempted_) {
        emit moveFeedback(*attempted_, result.success());
    }
    if (!closing_ && generation == generation_) {
        publish();
    }
}
std::optional<ReviewReturnOrigin> ReviewExplorationController::analysisOrigin() const {
    if (!state_.canAnalyze || !run_ || hidesSolution()) {
        return {};
    }
    return ReviewReturnOrigin{run_->gameId,
                              run_->id,
                              navigation_.target(navigation_.selected()),
                              state_.mode == ExplorationMode::Line ? state_.candidate : -1,
                              state_.mode == ExplorationMode::Line ? state_.step : 0,
                              board().blackAtBottom};
}
bool ReviewExplorationController::sameAnalysisOrigin() const {
    return !openingLinked_ && linkedOrigin_ && analysisOrigin() == linkedOrigin_;
}
void ReviewExplorationController::openAnalysis() {
    const auto origin = analysisOrigin();
    if (!origin || closing_) {
        return;
    }
    if (sameAnalysisOrigin()) {
        return;
    }
    const auto position =
        state_.mode == ExplorationMode::Line ? sandbox_.position() : navigation_.position();
    if (!position) {
        return;
    }
    linked_.setOrientation(board().blackAtBottom);
    linkedOrigin_ = origin;
    linkedEvidence_ = evidence_;
    linkedState_ = state_;
    openingLinked_ = true;
    linked_.openPosition(position);
}
void ReviewExplorationController::restoreReviewOrigin() {
    if (closing_ || openingLinked_ || !linkedOrigin_ || !run_ || run_->id != linkedOrigin_->run ||
        navigation_.target(0).session != linkedOrigin_->target.session) {
        return;
    }
    const auto origin = *linkedOrigin_;
    returnToGame();
    navigation_.select(origin.target);
    if (origin.candidate < 0 && navigation_.board().blackAtBottom != origin.blackAtBottom) {
        navigation_.flip();
    }
    if (origin.candidate >= 0 && linkedEvidence_ && !evidenceReadFailed_) {
        sandbox_.setOrientation(origin.blackAtBottom);
        evidence_ = linkedEvidence_;
        state_ = linkedState_;
        state_.pendingBoard = true;
        displayedBoard_.interactive = false;
        sandbox_.openLine(
            evidence_->before,
            evidence_->evidence.candidates[static_cast<std::size_t>(origin.candidate)].continuation,
            origin.step);
    }
    publish();
}
void ReviewExplorationController::close() {
    if (closing_) {
        return;
    }
    closing_ = true;
    returnToGame();
    sandbox_.close();
    linkedEngine_.close();
    linked_.close();
    finishClose();
}
void ReviewExplorationController::finishClose() {
    if (!closing_ || closed_ || workerBusy_ || !search_.isNull() || !pending_.isNull() ||
        !cacheRead_.isNull() || !sandboxClosed_ || !linkedClosed_ || !engineClosed_) {
        return;
    }
    if (!worker_.waitForDone(0)) {
        QTimer::singleShot(5, this, &ReviewExplorationController::finishClose);
        return;
    }
    closed_ = true;
    QTimer::singleShot(0, this, [this] { emit closed(); });
}
} // namespace chess_review::app
