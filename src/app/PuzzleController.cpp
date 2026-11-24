#include "app/PuzzleController.h"
#include "app/ReviewMovePresentation.h"
#include "engine/AnalysisCache.h"
#include "engine/EffectiveSearch.h"
#include <QMetaObject>
#include <QRunnable>
#include <algorithm>
#include <utility>

namespace chess_review::app {
PuzzleController::PuzzleController(persistence::Database& database,
                                   EngineSettingsController& settings,
                                   engine::EngineSupervisor& supervisor, QObject* parent)
    : QObject(parent), database_(database), settings_(settings), supervisor_(supervisor),
      linkedEngine_(linked_, settings, supervisor) {
    worker_.setMaxThreadCount(1);
    worker_.setExpiryTimeout(-1);
    reply_.setSingleShot(true);
    reply_.setInterval(300);
    reads_.setSingleShot(true);
    reads_.setInterval(100);
    summaryRetry_.setSingleShot(true);
    summaryRetry_.setInterval(100);
    connect(&summaryRetry_, &QTimer::timeout, this, &PuzzleController::requestSummary);
    reviewSummaryRetry_.setSingleShot(true);
    reviewSummaryRetry_.setInterval(100);
    connect(&reviewSummaryRetry_, &QTimer::timeout, this, &PuzzleController::requestReviewSummary);
    connect(&reply_, &QTimer::timeout, this, &PuzzleController::advance);
    connect(&reads_, &QTimer::timeout, this, [this] { refresh(offset_, filter_); });
    connect(&board_, &AnalysisController::changed, this, &PuzzleController::publish);
    connect(&board_, &AnalysisController::navigationAnimationRequested, this,
            [this](chess::Move move) {
                if (!closing_ && state_.revealed) {
                    emit moveAnimated(move);
                }
            });
    connect(&board_, &AnalysisController::moveAccepted, this, [this](chess::Move move) {
        if (!pendingMove_ || closing_) {
            return;
        }
        const auto accepted = *std::exchange(pendingMove_, std::nullopt);
        node_ = accepted.next;
        state_.hints.clear();
        reply_.start();
        if (accepted.solverMove) {
            emit moveFeedback(move, true);
        }
        emit moveAnimated(move);
        publish();
    });
    connect(&board_, &AnalysisController::failed, this, [this](const QString& message) {
        pendingMove_.reset();
        state_.error = message;
        publish();
    });
    connect(&board_, &AnalysisController::closed, this, [this] {
        boardClosed_ = true;
        finishClose();
    });
    connect(&linked_, &AnalysisController::closed, this, [this] {
        linkedClosed_ = true;
        finishClose();
    });
    connect(&linkedEngine_, &EngineAnalysisController::closed, this, [this] {
        engineClosed_ = true;
        finishClose();
    });
    connect(&database_, &persistence::Database::puzzlesListed, this,
            [this](QUuid id, const persistence::ListPuzzlesResult& result) {
                if (summaryRequest_.isNull() || id != summaryRequest_) {
                    return;
                }
                summaryRequest_ = {};
                if (closing_) {
                    finishClose();
                    return;
                }
                if (!ready_ || summaryDirty_) {
                    requestSummary();
                    return;
                }
                if (const auto* page = std::get_if<persistence::PuzzlePage>(&result)) {
                    savedSummary_ = {static_cast<int>(page->puzzles.size()), page->hasNext, {}};
                } else {
                    const auto& error = std::get<persistence::ReviewError>(result);
                    if (error.code == persistence::ReviewErrorCode::Busy) {
                        summaryDirty_ = true;
                        if (summaryVisible_) {
                            summaryRetry_.start();
                        }
                        return;
                    }
                    savedSummary_ = {std::nullopt, false, error.message};
                }
                emit savedSummaryChanged();
            });
    connect(&database_, &persistence::Database::puzzlesListed, this,
            [this](QUuid id, const persistence::ListPuzzlesResult& result) {
                if (reviewList_.isNull() || id != reviewList_) {
                    return;
                }
                reviewList_ = {};
                if (closing_) {
                    finishClose();
                    return;
                }
                if (ready_ && !reviewSummaryDirty_ && reviewSummaryRequest_ == reviewSummary_.id) {
                    if (const auto* page = std::get_if<persistence::PuzzlePage>(&result)) {
                        reviewSummary_.count = static_cast<int>(page->puzzles.size());
                        reviewSummary_.more = page->hasNext;
                    } else {
                        const auto& error = std::get<persistence::ReviewError>(result);
                        if (error.code == persistence::ReviewErrorCode::Busy) {
                            reviewSummaryDirty_ = true;
                            reviewSummaryRetry_.start();
                        } else {
                            reviewSummary_.error = error.message;
                        }
                    }
                }
                requestReviewSummary();
                emit reviewSummaryChanged();
            });
    connect(&database_, &persistence::Database::puzzleJobsListed, this,
            [this](QUuid id, const persistence::ListPuzzleJobsResult& result) {
                if (reviewJobs_.isNull() || id != reviewJobs_) {
                    return;
                }
                reviewJobs_ = {};
                if (closing_) {
                    finishClose();
                    return;
                }
                if (ready_ && !reviewSummaryDirty_ && reviewSummaryRequest_ == reviewSummary_.id) {
                    if (const auto* jobs =
                            std::get_if<std::vector<persistence::PuzzleJobSummary>>(&result)) {
                        reviewSummary_.generation =
                            jobs->empty() ? std::nullopt : std::optional{jobs->front()};
                    } else {
                        const auto& error = std::get<persistence::ReviewError>(result);
                        if (error.code == persistence::ReviewErrorCode::Busy) {
                            reviewSummaryDirty_ = true;
                            reviewSummaryRetry_.start();
                        } else {
                            reviewSummary_.error = error.message;
                        }
                    }
                }
                requestReviewSummary();
                emit reviewSummaryChanged();
            });
    connect(&database_, &persistence::Database::puzzlesListed, this,
            [this](QUuid id, const persistence::ListPuzzlesResult& result) {
                if (id != list_) {
                    return;
                }
                list_ = {};
                if (closing_) {
                    finishClose();
                    return;
                }
                if (listStale_) {
                    listStale_ = false;
                } else if (const auto* e = std::get_if<persistence::ReviewError>(&result)) {
                    if (e->code == persistence::ReviewErrorCode::Busy) {
                        reads_.start();
                    } else {
                        state_.libraryError = e->message;
                        state_.library = {};
                    }
                } else {
                    state_.library = std::get<persistence::PuzzlePage>(result);
                    state_.libraryError.clear();
                    if (state_.library.puzzles.empty() && offset_ > 0) {
                        refresh(0, filter_);
                    }
                }
                if (refreshPending_) {
                    refreshPending_ = false;
                    QTimer::singleShot(0, this, &PuzzleController::refreshCurrent);
                }
                publish();
            });
    connect(&database_, &persistence::Database::puzzleJobsListed, this,
            [this](QUuid id, const persistence::ListPuzzleJobsResult& result) {
                if (id != jobs_) {
                    return;
                }
                jobs_ = {};
                if (closing_) {
                    finishClose();
                    return;
                }
                if (const auto* value =
                        std::get_if<std::vector<persistence::PuzzleJobSummary>>(&result)) {
                    state_.interrupted = *value;
                } else if (std::get<persistence::ReviewError>(result).code ==
                           persistence::ReviewErrorCode::Busy) {
                    reads_.start();
                }
                publish();
            });
    connect(&database_, &persistence::Database::puzzleLoaded, this,
            [this](QUuid id, const persistence::LoadPuzzleResult& result) {
                if (id != load_) {
                    return;
                }
                load_ = {};
                if (opening_ <= 0) {
                    state_.loading = false;
                    finishClose();
                    return;
                }
                if (closing_) {
                    finishClose();
                    return;
                }
                if (const auto* e = std::get_if<persistence::ReviewError>(&result)) {
                    if (e->code == persistence::ReviewErrorCode::Busy) {
                        const auto requested = opening_;
                        QTimer::singleShot(100, this, [this, requested] {
                            if (!closing_ && opening_ == requested && load_.isNull()) {
                                open(requested);
                            }
                        });
                    } else {
                        state_.loading = false;
                        state_.solving = false;
                        state_.error = e->message;
                    }
                    publish();
                    return;
                }
                puzzle_ = std::get<std::shared_ptr<const persistence::StoredPuzzle>>(result);
                state_.selected = puzzle_->id;
                hintLevels_.assign(puzzle_->document.solution.nodes.size(), 0);
                resetBoard();
            });
    connect(&database_, &persistence::Database::puzzleDeleted, this,
            [this](QUuid id, const persistence::DeletePuzzleResult& result) {
                if (id != deletion_) {
                    return;
                }
                deletion_ = {};
                if (closing_) {
                    finishClose();
                    return;
                }
                if (const auto* e = std::get_if<persistence::ReviewError>(&result)) {
                    state_.error = e->message;
                } else {
                    const auto deleted = deleting_;
                    if (state_.selected == deleted) {
                        ++epoch_;
                        cancelAlternative();
                        reply_.stop();
                        state_.solving = false;
                        state_.selected.reset();
                        puzzle_.reset();
                        root_.reset();
                    }
                    invalidateSummary();
                    emit puzzleRemoved(deleted);
                }
                refresh(offset_, filter_);
                inspectReview(reviewSummary_.id, true);
                publish();
            });
    connect(&supervisor_, &engine::EngineSupervisor::scheduledFinished, this,
            [this](const engine::ScheduledResult& result) {
                if (result.identity.request != search_ || result.identity.session != session_) {
                    return;
                }
                search_ = {};
                if (closing_ || !alternative_) {
                    finishClose();
                    publish();
                    return;
                }
                if (result.outcome != engine::ScheduledOutcome::Completed || !result.evidence) {
                    alternativeUnavailable();
                    return;
                }
                alternative_->passes.push_back(
                    {result.evidence->evidence, engine::AnalysisOutcome::Completed, {}});
                evaluateAlternative();
            });
    connect(&settings_, &EngineSettingsController::changed, this, [this] {
        if (state_.grading && settings_.state().runtime.phase != engine::RuntimePhase::Ready) {
            cancelAlternative();
            state_.message =
                tr("Grading unavailable. Reconnect the original engine or try a saved solution.");
            publish();
        }
    });
}
PuzzleController::~PuzzleController() {
    Q_ASSERT(!workerBusy_ && search_.isNull() && list_.isNull() && jobs_.isNull() &&
             load_.isNull() && deletion_.isNull() && summaryRequest_.isNull());
}
void PuzzleController::inspectReview(review::ReviewId id, bool refresh) {
    if (!ready_ || closing_ || id <= 0 || (reviewSummary_.id == id && !refresh)) {
        return;
    }
    if (reviewSummary_.id != id) {
        reviewSummary_ = {};
        reviewSummary_.id = id;
    }
    reviewSummaryDirty_ = true;
    requestReviewSummary();
}
void PuzzleController::requestReviewSummary() {
    if (!ready_ || closing_ || !reviewSummaryDirty_ || !reviewList_.isNull() ||
        !reviewJobs_.isNull() || reviewSummaryRetry_.isActive()) {
        return;
    }
    reviewSummaryDirty_ = false;
    reviewSummary_.error.clear();
    reviewSummaryRequest_ = reviewSummary_.id;
    reviewList_ = QUuid::createUuid();
    reviewJobs_ = QUuid::createUuid();
    database_.listPuzzles(reviewList_, 0, 50, reviewSummaryRequest_);
    database_.listPuzzleJobs(reviewJobs_, 0, 1, reviewSummaryRequest_);
}
void PuzzleController::setSummaryVisible(bool visible) {
    if (visible && !summaryVisible_ && !savedSummary_.error.isEmpty()) {
        summaryDirty_ = true;
    }
    summaryVisible_ = visible;
    if (visible) {
        requestSummary();
    } else {
        summaryRetry_.stop();
    }
}
void PuzzleController::invalidateSummary() {
    summaryDirty_ = true;
    requestSummary();
}
void PuzzleController::requestSummary() {
    if (!ready_ || closing_ || !summaryVisible_ || !summaryDirty_ || !summaryRequest_.isNull() ||
        summaryRetry_.isActive()) {
        return;
    }
    summaryDirty_ = false;
    summaryRequest_ = QUuid::createUuid();
    database_.listPuzzles(summaryRequest_, 0, 50, std::nullopt);
}
void PuzzleController::setReady(bool ready) {
    ready_ = ready;
    if (ready) {
        refresh();
        requestSummary();
        requestReviewSummary();
    } else {
        summaryRetry_.stop();
        reviewSummaryRetry_.stop();
        reviewSummaryDirty_ = reviewSummary_.id > 0;
        reads_.stop();
        summaryDirty_ = true;
        savedSummary_ = {};
        emit savedSummaryChanged();
        publish();
    }
}
void PuzzleController::refresh(std::int64_t offset, std::optional<review::ReviewId> review) {
    if (!ready_ || closing_) {
        return;
    }
    offset = std::max<std::int64_t>(0, offset);
    if (offset != offset_ || review != filter_) {
        listStale_ = !list_.isNull();
        state_.library = {};
        state_.library.offset = offset;
    }
    offset_ = offset;
    filter_ = review;
    if (list_.isNull()) {
        list_ = QUuid::createUuid();
        database_.listPuzzles(list_, offset_, 50, filter_);
    } else {
        refreshPending_ = true;
    }
    if (jobs_.isNull()) {
        jobs_ = QUuid::createUuid();
        database_.listPuzzleJobs(jobs_, 0, 50, {});
    }
    publish();
}
void PuzzleController::open(persistence::PuzzleId id) {
    if (!ready_ || closing_ || id <= 0) {
        return;
    }
    if (opening_ == id && !load_.isNull()) {
        return;
    }
    if (state_.selected == id && puzzle_ && !state_.loading) {
        state_.solving = true;
        publish();
        return;
    }
    ++epoch_;
    cancelAlternative();
    reply_.stop();
    pendingMove_.reset();
    state_.loading = true;
    state_.solving = true;
    state_.revealed = false;
    state_.complete = false;
    state_.error.clear();
    state_.hints.clear();
    state_.selected.reset();
    puzzle_.reset();
    root_.reset();
    opening_ = id;
    load_ = QUuid::createUuid();
    database_.loadPuzzle(load_, id);
    publish();
}
void PuzzleController::remove(persistence::PuzzleId id) {
    if (!ready_ || closing_ || !deletion_.isNull()) {
        return;
    }
    deleting_ = id;
    if (opening_ == id) {
        opening_ = 0;
        state_.loading = false;
        state_.solving = false;
        ++epoch_;
    }
    if (state_.selected == id) {
        cancelAlternative();
        reply_.stop();
    }
    deletion_ = QUuid::createUuid();
    database_.deletePuzzle(deletion_, id);
    publish();
}
void PuzzleController::showLibrary() { returnToLibrary(false); }
void PuzzleController::showAllPuzzles() { returnToLibrary(true); }
void PuzzleController::returnToLibrary(bool clearFilter) {
    if (closing_) {
        return;
    }
    ++epoch_;
    cancelAlternative();
    if (state_.loading) {
        opening_ = 0;
        pendingReset_ = false;
        state_.loading = false;
        state_.selected.reset();
        puzzle_.reset();
        root_.reset();
    }
    state_.solving = false;
    if (clearFilter) {
        state_.error.clear();
        refresh();
    } else {
        refreshCurrent();
    }
    publish();
}
void PuzzleController::resetBoard() {
    if (!puzzle_ || closing_) {
        return;
    }
    if (workerBusy_) {
        pendingReset_ = true;
        return;
    }
    ++epoch_;
    const auto epoch = epoch_;
    cancelAlternative();
    reply_.stop();
    node_ = 0;
    pendingMove_.reset();
    state_.revealed = false;
    state_.complete = false;
    state_.loading = true;
    state_.hints.clear();
    state_.error.clear();
    switch (puzzle_->document.solution.goal) {
    case puzzles::Goal::Mate:
        state_.message = tr("Find checkmate.");
        break;
    case puzzles::Goal::Material:
        state_.message = tr("Find a way to win material.");
        break;
    case puzzles::Goal::Defence:
        state_.message = tr("Find the move that holds the position.");
        break;
    case puzzles::Goal::Practice:
        state_.message = tr("Find the best continuation.");
        break;
    case puzzles::Goal::Tactic:
        state_.message = tr("Find the winning tactic.");
        break;
    }
    workerBusy_ = true;
    const auto puzzle = puzzle_;
    worker_.start(QRunnable::create([this, puzzle, epoch] {
        const auto restored = engine::restorePosition(puzzle->document.definition.position);
        std::shared_ptr<const chess::Position> position;
        if (const auto* p = std::get_if<chess::Position>(&restored)) {
            position = std::make_shared<const chess::Position>(*p);
        }
        QMetaObject::invokeMethod(
            this,
            [this, position, epoch] {
                workerBusy_ = false;
                if (pendingReset_ && !closing_) {
                    pendingReset_ = false;
                    resetBoard();
                    return;
                }
                if (closing_ || epoch != epoch_) {
                    finishClose();
                    return;
                }
                root_ = position;
                if (root_) {
                    const bool black = puzzle_->document.definition.position.board.sideToMove ==
                                       chess::Color::Black;
                    board_.openPosition(root_);
                    if (closing_ || epoch != epoch_) {
                        return;
                    }
                    board_.setOrientation(black);
                } else {
                    state_.loading = false;
                    state_.error = tr("The saved position could not be reconstructed.");
                }
                publish();
            },
            Qt::QueuedConnection);
    }));
    publish();
}
void PuzzleController::retry() {
    if (closing_ || workerBusy_ || board_.busy() || !puzzle_) {
        return;
    }
    hintLevels_.assign(puzzle_->document.solution.nodes.size(), 0);
    resetBoard();
}
bool PuzzleController::canHint() const {
    return displayed_.interactive && puzzle_ && node_ < hintLevels_.size() &&
           hintLevels_[node_] < 3 && !puzzle_->document.solution.nodes[node_].moves.empty();
}
void PuzzleController::hint() {
    if (!canHint()) {
        return;
    }
    const auto& n = puzzle_->document.solution.nodes[node_];
    const auto move = n.moves.front().move;
    const auto piece = displayed_.pieces[static_cast<std::size_t>(move.from.index())];
    if (!piece) {
        return;
    }
    const int level = ++hintLevels_[node_];
    state_.hints.push_back(
        reviewRetryHint({n.idea, move, piece->type}, static_cast<review::RetryHintLevel>(level)));
    publish();
}
void PuzzleController::reveal() {
    if (!puzzle_ || !root_ || workerBusy_ || board_.busy() || pendingMove_ || closing_) {
        return;
    }
    ++epoch_;
    cancelAlternative();
    reply_.stop();
    state_.revealed = true;
    state_.loading = true;
    state_.complete = false;
    state_.hints.clear();
    revealedLine_.clear();
    auto index = std::size_t{0};
    const auto& nodes = puzzle_->document.solution.nodes;
    while (index < nodes.size() && !nodes[index].complete && !nodes[index].moves.empty()) {
        const auto edge = nodes[index].moves.front();
        revealedLine_.push_back(edge.move);
        index = edge.next;
    }
    board_.openLine(root_, revealedLine_, 0);
    state_.message = tr("Solution revealed. Step through the moves or retry.");
    publish();
}
bool PuzzleController::canStepBack() const {
    return state_.revealed && !state_.loading && !board_.busy() && puzzle_ &&
           board_.currentNode().value > puzzle_->document.definition.position.moves.size();
}
bool PuzzleController::canStepForward() const {
    return state_.revealed && !state_.loading && !board_.busy() && !board_.continuations().empty();
}
void PuzzleController::previous() {
    if (canStepBack()) {
        board_.previous();
    }
}
void PuzzleController::next() {
    if (canStepForward()) {
        board_.next();
    }
}
void PuzzleController::selectSquare(chess::Square square) {
    if (displayed_.interactive) {
        board_.selectSquare(square);
    }
}
void PuzzleController::clearSelection() { board_.clearSelection(); }
void PuzzleController::flip() { board_.flip(); }
void PuzzleController::play(puzzles::Edge edge, bool solverMove) {
    if (!puzzle_ || edge.next >= puzzle_->document.solution.nodes.size()) {
        return;
    }
    const auto move = edge.move;
    pendingMove_ = PendingMove{edge.next, solverMove};
    board_.attemptMove(move, board_.board().revision);
}
void PuzzleController::advance() {
    if (!puzzle_ || closing_ || state_.revealed || board_.busy() || pendingMove_) {
        return;
    }
    const auto& n = puzzle_->document.solution.nodes[node_];
    if (n.complete) {
        state_.complete = true;
        state_.message = tr("Puzzle solved!");
    } else if (n.path.size() % 2 == 1 && !n.moves.empty()) {
        play(n.moves.front(), false);
        return;
    } else {
        state_.message = tr("Good move! Keep going.");
    }
    publish();
}
void PuzzleController::attemptMove(chess::Move move, std::uint64_t revision) {
    if (!displayed_.interactive || revision != displayed_.revision || !puzzle_) {
        return;
    }
    const auto& n = puzzle_->document.solution.nodes[node_];
    const auto found = std::find_if(n.moves.begin(), n.moves.end(),
                                    [&](const auto& edge) { return edge.move == move; });
    if (found != n.moves.end()) {
        play(*found, true);
        return;
    }
    // Legality and arbitrary input validation stay on the analysis worker.
    alternative_ = std::make_shared<PuzzleAlternative>(PuzzleAlternative{
        puzzle_, board_.position(), move, static_cast<int>(n.path.size() / 2), {}});
    state_.grading = true;
    state_.message = tr("Checking your move…");
    analysisJob_ = QUuid::createUuid();
    evaluateAlternative();
}
void PuzzleController::cancelAlternative() {
    if (!search_.isNull()) {
        supervisor_.cancelAnalysisJob(session_, analysisJob_);
    }
    alternative_.reset();
    state_.grading = false;
}
void PuzzleController::alternativeUnavailable() {
    cancelAlternative();
    state_.message = tr("Grading unavailable. Connect the original engine or try a saved solution; "
                        "this move has not been marked wrong.");
    publish();
}
void PuzzleController::evaluateAlternative() {
    if (!alternative_ || workerBusy_ || closing_) {
        return;
    }
    const auto attempt = alternative_;
    const auto epoch = epoch_;
    const auto currentNode = node_;
    workerBusy_ = true;
    worker_.start(QRunnable::create([this, attempt, epoch, currentNode] {
        auto result = evaluatePuzzleAlternative(*attempt);
        std::shared_ptr<const persistence::StoredPuzzle> extended;
        if (result.grade == puzzles::AlternativeGrade::Strong && result.continuation &&
            attempt->puzzle->document.solution.nodes.size() + result.continuation->nodes.size() <=
                static_cast<std::size_t>(
                    attempt->puzzle->document.definition.policy.maximumPositions) &&
            attempt->puzzle->document.passes.size() + attempt->passes.size() <= 192) {
            auto copy = std::make_shared<persistence::StoredPuzzle>(*attempt->puzzle);
            auto& nodes = copy->document.solution.nodes;
            auto prefix = nodes[currentNode].path;
            prefix.push_back(attempt->move);
            const auto base = nodes.size();
            nodes[currentNode].moves.push_back({attempt->move, base});
            for (auto n : result.continuation->nodes) {
                auto path = prefix;
                path.insert(path.end(), n.path.begin(), n.path.end());
                n.path = std::move(path);
                for (auto& edge : n.moves) {
                    edge.next += base;
                }
                nodes.push_back(std::move(n));
            }
            copy->document.passes.insert(copy->document.passes.end(), attempt->passes.begin(),
                                         attempt->passes.end());
            extended = std::move(copy);
        }
        QMetaObject::invokeMethod(
            this,
            [this, attempt, epoch, extended, result = std::move(result)]() mutable {
                workerBusy_ = false;
                if (pendingReset_ && !closing_) {
                    pendingReset_ = false;
                    resetBoard();
                    return;
                }
                if (closing_ || epoch != epoch_ || alternative_ != attempt) {
                    finishClose();
                    publish();
                    return;
                }
                if (result.next) {
                    scheduleAlternative(std::move(*result.next));
                    return;
                }
                if (result.grade == puzzles::AlternativeGrade::Strong && extended) {
                    puzzle_ = extended;
                    hintLevels_.resize(extended->document.solution.nodes.size(), 0);
                    const auto edge = puzzle_->document.solution.nodes[node_].moves.back();
                    alternative_.reset();
                    state_.grading = false;
                    state_.message = tr("Good alternative! Keep going.");
                    play(edge, true);
                    return;
                } else if (result.grade == puzzles::AlternativeGrade::TryAgain) {
                    const auto move = attempt->move;
                    alternative_.reset();
                    state_.grading = false;
                    state_.message = tr("Try again.");
                    emit moveFeedback(move, false);
                } else {
                    alternativeUnavailable();
                }
                publish();
            },
            Qt::QueuedConnection);
    }));
    publish();
}
void PuzzleController::scheduleAlternative(puzzles::SearchNeed need) {
    const auto& runtime = settings_.state().runtime;
    if (!alternative_ || runtime.phase != engine::RuntimePhase::Ready || !runtime.discovery ||
        !search_.isNull()) {
        alternativeUnavailable();
        return;
    }
    const auto discovery = *runtime.discovery;
    const auto overrides = runtime.optionOverrides;
    const auto expected = runtime.session;
    const auto attempt = alternative_;
    const auto epoch = epoch_;
    workerBusy_ = true;
    worker_.start(QRunnable::create([this, discovery, overrides, expected, attempt, epoch,
                                     need = std::move(need)]() mutable {
        const auto fingerprint = engine::quickReviewFingerprint(discovery, overrides);
        const bool compatible = std::holds_alternative<engine::EngineFingerprint>(fingerprint) &&
                                std::get<engine::EngineFingerprint>(fingerprint) ==
                                    attempt->puzzle->document.fingerprint;
        QMetaObject::invokeMethod(
            this,
            [this, expected, attempt, epoch, compatible, need = std::move(need)]() mutable {
                workerBusy_ = false;
                if (pendingReset_ && !closing_) {
                    pendingReset_ = false;
                    resetBoard();
                    return;
                }
                if (closing_ || epoch != epoch_ || alternative_ != attempt) {
                    finishClose();
                    publish();
                    return;
                }
                if (!compatible || settings_.state().runtime.session != expected ||
                    settings_.state().runtime.phase != engine::RuntimePhase::Ready) {
                    alternativeUnavailable();
                    return;
                }
                engine::AnalysisRequest request;
                request.id = search_ = QUuid::createUuid();
                request.position = std::move(need.position);
                request.limit = engine::NodeLimit{need.nodes};
                request.multiPv = need.width;
                request.wantWdl = true;
                session_ = expected;
                supervisor_.scheduleAnalysis(session_, request, engine::AnalysisPriority::RetryMove,
                                             analysisJob_);
                publish();
            },
            Qt::QueuedConnection);
    }));
}
void PuzzleController::openAnalysis() {
    if (!state_.solving || state_.loading || !puzzle_ || board_.busy() || !board_.position() ||
        closing_) {
        return;
    }
    analysisOrigin_ = board_.position();
    analysisEpoch_ = epoch_;
    emit analysisRequested();
}
bool PuzzleController::enterAnalysis() {
    if (closing_ || !analysisOrigin_ || analysisEpoch_ != epoch_) {
        return false;
    }
    ++epoch_;
    cancelAlternative();
    linked_.openPosition(analysisOrigin_);
    linked_.setOrientation(displayed_.blackAtBottom);
    return true;
}
void PuzzleController::publish() {
    if (closing_) {
        return;
    }
    if (state_.loading && load_.isNull() && !workerBusy_ && root_ && !board_.busy() &&
        board_.canNavigate()) {
        state_.loading = false;
    }
    displayed_ = state_.loading || !root_ ? BoardViewState{} : board_.board();
    displayed_.interactive = displayed_.interactive && state_.solving && !state_.loading &&
                             state_.error.isEmpty() && !state_.revealed && !state_.complete &&
                             !state_.grading && !pendingMove_ && !reply_.isActive() &&
                             !workerBusy_ && search_.isNull() && deletion_.isNull() && puzzle_ &&
                             node_ < puzzle_->document.solution.nodes.size() &&
                             puzzle_->document.solution.nodes[node_].path.size() % 2 == 0;
    emit changed();
}
void PuzzleController::close() {
    if (closing_) {
        return;
    }
    closing_ = true;
    ++epoch_;
    reply_.stop();
    reads_.stop();
    summaryRetry_.stop();
    reviewSummaryRetry_.stop();
    cancelAlternative();
    board_.close();
    linked_.close();
    linkedEngine_.close();
    finishClose();
}
void PuzzleController::finishClose() {
    if (!closing_ || closed_ || workerBusy_ || !search_.isNull() || !list_.isNull() ||
        !jobs_.isNull() || !load_.isNull() || !deletion_.isNull() || !reviewList_.isNull() ||
        !reviewJobs_.isNull() || !summaryRequest_.isNull() || !boardClosed_ || !linkedClosed_ ||
        !engineClosed_) {
        return;
    }
    if (!worker_.waitForDone(0)) {
        QTimer::singleShot(5, this, &PuzzleController::finishClose);
        return;
    }
    closed_ = true;
    QTimer::singleShot(0, this, [this] { emit closed(); });
}
} // namespace chess_review::app
