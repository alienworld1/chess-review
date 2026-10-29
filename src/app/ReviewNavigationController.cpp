#include "app/ReviewNavigationController.h"
#include <QTimer>
#include <algorithm>
#include <iterator>

namespace chess_review::app {
ReviewNavigationController::ReviewNavigationController(persistence::Database& database,
                                                       ReviewMovesController& moves)
    : moves_(moves), previewEvidence_(database, false) {
    connect(&replay_, &AnalysisController::gameOpened, this, [this] {
        if (closing_ || !opening_) {
            return;
        }
        opening_ = false;
        line_ = replay_.chosenLine();
        positions_.clear();
        for (std::size_t index = 0; index < line_.size(); ++index) {
            positions_.emplace(line_[index], static_cast<int>(index));
        }
        ready_ = !line_.empty();
        bind(run_, overview_);
        dispatch();
        refreshBoard();
    });
    connect(&replay_, &AnalysisController::changed, this, [this] {
        if (!opening_ && !closing_) {
            refreshBoard();
        }
    });
    connect(&replay_, &AnalysisController::failed, this, [this](const QString& error) {
        opening_ = false;
        ready_ = false;
        error_ = error;
        emit changed();
    });
    connect(&replay_, &AnalysisController::closed, this, [this] {
        replayClosed_ = true;
        finishClose();
    });
    connect(&previewEvidence_, &ReviewMovesController::drainedChanged, this,
            &ReviewNavigationController::finishClose);
    connect(&previewEvidence_, &ReviewMovesController::changed, this,
            [this] { cachePage(previewEvidence_.state()); });
    connect(&moves_, &ReviewMovesController::changed, this, [this] { cachePage(moves_.state()); });
}
void ReviewNavigationController::clear() {
    if (closing_) {
        return;
    }
    ++session_;
    ready_ = false;
    opening_ = false;
    preview_.reset();
    visible_.reset();
    run_.reset();
    overview_.reset();
    line_.clear();
    positions_.clear();
    pages_.clear();
    previewEvidence_.bind(std::nullopt);
    selected_ = displayed_ = 0;
    board_ = {};
    error_.clear();
    userNavigated_ = entryChosen_ = false;
    emit changed();
}
void ReviewNavigationController::open(const persistence::StoredGame& game) {
    clear();
    if (closing_) {
        return;
    }
    opening_ = true;
    replay_.openGame(game);
}
void ReviewNavigationController::bind(const std::optional<persistence::ReviewSummary>& run,
                                      const std::optional<persistence::ReviewOverview>& overview) {
    if (closing_) {
        return;
    }
    const bool replaced = (run ? run->id : 0) != (run_ ? run_->id : 0);
    const bool revised = replaced || (run && run_ && run->revision != run_->revision);
    run_ = run;
    overview_ = overview && run && overview->reviewId == run->id ? overview : std::nullopt;
    if (revised) {
        pages_.clear();
        previewEvidence_.bind(std::nullopt);
    }
    if (replaced) {
        ++session_;
        preview_.reset();
        userNavigated_ = entryChosen_ = false;
        selected_ = 0;
        if (run) {
            replay_.setOrientation(run->selectedSide == chess::Color::Black);
        }
        dispatch();
    }
    cachePage(moves_.state());
    if (overview_ && ready_ && !entryChosen_ && !userNavigated_) {
        entryChosen_ = true;
        navigate(overview_->keyPositions.empty() ? 0 : overview_->keyPositions.front(), false);
    }
}
bool ReviewNavigationController::valid(ReviewTarget target) const {
    return !closing_ && ready_ && target.session == session_ && target.position >= 0 &&
           target.position < static_cast<int>(line_.size());
}
void ReviewNavigationController::navigate(int position, bool user) {
    if (!valid(target(position))) {
        return;
    }
    userNavigated_ = userNavigated_ || user;
    selected_ = position;
    preview_.reset();
    requestEvidence(position, true);
    dispatch();
    emit changed();
    if (user) {
        emit selectionCommitted(position);
    }
}
void ReviewNavigationController::select(ReviewTarget target) {
    if (valid(target)) {
        navigate(target.position, true);
    }
}
void ReviewNavigationController::preview(ReviewTarget target) {
    if (!valid(target) || preview_ == target.position) {
        return;
    }
    preview_ = target.position;
    requestEvidence(target.position, false);
    dispatch();
    emit changed();
}
void ReviewNavigationController::clearPreview() {
    if (!preview_) {
        return;
    }
    preview_.reset();
    dispatch();
    emit changed();
}
void ReviewNavigationController::visible(ReviewTarget first, ReviewTarget last) {
    if (valid(first) && valid(last)) {
        visible_ = std::pair{first.position, last.position};
        requestVisibleEvidence();
    }
}
void ReviewNavigationController::requestVisibleEvidence() {
    if (!visible_ || preview_ || !run_ || closing_) {
        return;
    }
    const int first = (std::max(1, visible_->first) - 1) / 50 * 50;
    const int last = (std::max(1, visible_->second) - 1) / 50 * 50;
    for (int offset = first; offset <= last; offset += 50) {
        if (move(offset + 1) || (moves_.state().offset == offset && moves_.state().loading)) {
            continue;
        }
        if (previewEvidence_.state().loading && previewEvidence_.state().offset == offset) {
            return;
        }
        if (!previewEvidence_.state().error.isEmpty()) {
            return;
        }
        requestEvidence(offset + 1, false);
        return;
    }
}
void ReviewNavigationController::dispatch() {
    if (ready_ && !opening_ && !closing_ && requested() < static_cast<int>(line_.size())) {
        replay_.navigate(replay_.target(line_[static_cast<std::size_t>(requested())]));
    }
}
void ReviewNavigationController::refreshBoard() {
    if (!ready_ || replay_.busy() || line_.empty() ||
        replay_.currentNode() != line_[static_cast<std::size_t>(requested())]) {
        return;
    }
    displayed_ = requested();
    board_ = replay_.board();
    board_.interactive = false;
    board_.legalMoves.clear();
    board_.captureMoves.clear();
    emit changed();
}
void ReviewNavigationController::cachePage(const ReviewMovesState& state) {
    if (closing_) {
        return;
    }
    if (!moves_.state().error.isEmpty() || !previewEvidence_.state().error.isEmpty()) {
        pages_.clear();
        emit changed();
        return;
    }
    if (state.page && run_ && state.page->reviewId == run_->id &&
        state.page->revision == run_->revision) {
        const auto offset = state.page->offset;
        const auto found = std::find_if(pages_.begin(), pages_.end(), [offset](const auto& page) {
            return page.offset == offset;
        });
        if (found != pages_.end()) {
            return;
        }
        pages_.push_front(*state.page);
        if (pages_.size() > 4) {
            // Scrolling must not discard the committed explanation/evaluation.
            // Keep its page pinned within the same four-page memory bound.
            const int selectedOffset = (std::max(1, selected_) - 1) / 50 * 50;
            const auto oldest =
                std::find_if(pages_.rbegin(), pages_.rend(), [selectedOffset](const auto& page) {
                    return page.offset != selectedOffset;
                });
            pages_.erase(std::prev(oldest.base()));
        }
    }
    requestVisibleEvidence();
    emit changed();
}
const persistence::ReviewMove* ReviewNavigationController::move(int position) const {
    for (const auto& page : pages_) {
        const auto index = position - page.offset - 1;
        if (index >= 0 && index < static_cast<int>(page.moves.size())) {
            return &page.moves[static_cast<std::size_t>(index)];
        }
    }
    return nullptr;
}
void ReviewNavigationController::requestEvidence(int position, bool committed) {
    if (!run_) {
        return;
    }
    const int evidencePosition = std::max(1, position);
    const int offset = ((evidencePosition - 1) / 50) * 50;
    if (!committed) {
        if (move(evidencePosition) ||
            (moves_.state().offset == offset && (moves_.state().loading || moves_.state().page))) {
            return;
        }
        previewEvidence_.bind(run_);
    }
    auto& evidence = committed ? moves_ : previewEvidence_;
    if (evidence.state().offset != offset || !evidence.state().page) {
        if (evidence.state().offset != offset || !evidence.state().loading) {
            evidence.showPage(offset);
        }
    }
    if (committed && position > 0 && evidence.state().selectedPosition != position) {
        evidence.select(position);
    }
}
QString ReviewNavigationController::evidenceError() const {
    if (!moves_.state().error.isEmpty()) {
        return moves_.state().error;
    }
    return previewEvidence_.state().error.isEmpty()
               ? QString{}
               : tr("Preview evidence: %1").arg(previewEvidence_.state().error);
}
void ReviewNavigationController::retryEvidence() {
    moves_.retry();
    if (!previewEvidence_.state().error.isEmpty()) {
        previewEvidence_.retry();
    }
}
ReviewEvaluation ReviewNavigationController::evaluation() const {
    return reviewPositionEvaluation(pending() ? nullptr : move(displayed_ == 0 ? 1 : displayed_),
                                    displayed_ == 0);
}
void ReviewNavigationController::previous() { navigate(std::max(0, selected_ - 1), true); }
void ReviewNavigationController::next() {
    navigate(std::min(static_cast<int>(line_.size()) - 1, selected_ + 1), true);
}
void ReviewNavigationController::beginning() { navigate(0, true); }
void ReviewNavigationController::end() { navigate(static_cast<int>(line_.size()) - 1, true); }
void ReviewNavigationController::flip() {
    clearPreview();
    replay_.flip();
    refreshBoard();
}
bool ReviewNavigationController::keyMoment(int position) const {
    return overview_ && std::binary_search(overview_->keyPositions.begin(),
                                           overview_->keyPositions.end(), position);
}
std::optional<int> ReviewNavigationController::opponentContext(int position) const {
    return overview_ && std::binary_search(overview_->opponentContexts.begin(),
                                           overview_->opponentContexts.end(), position)
               ? std::optional{position - 1}
               : std::nullopt;
}
std::optional<int> ReviewNavigationController::positionFor(chess::NodeId node) const {
    const auto found = positions_.find(node);
    return found == positions_.end() ? std::nullopt : std::optional{found->second};
}
bool ReviewNavigationController::canPreviousMoment() const {
    return ready_ && overview_ && !overview_->keyPositions.empty() &&
           overview_->keyPositions.front() < selected_;
}
bool ReviewNavigationController::canNextMoment() const {
    return ready_ && overview_ && !overview_->keyPositions.empty() &&
           overview_->keyPositions.back() > selected_;
}
void ReviewNavigationController::moment(int direction) {
    if (!overview_ || !ready_) {
        return;
    }
    const auto& positions = overview_->keyPositions;
    if (direction > 0) {
        const auto next = std::upper_bound(positions.begin(), positions.end(), selected_);
        if (next != positions.end()) {
            navigate(*next, true);
        }
    } else {
        const auto next = std::lower_bound(positions.begin(), positions.end(), selected_);
        if (next != positions.begin()) {
            navigate(*std::prev(next), true);
        }
    }
}
void ReviewNavigationController::previousMoment() { moment(-1); }
void ReviewNavigationController::nextMoment() { moment(1); }
void ReviewNavigationController::close() {
    if (closing_) {
        return;
    }
    closing_ = true;
    previewEvidence_.close();
    replay_.close();
    finishClose();
}
void ReviewNavigationController::finishClose() {
    if (closing_ && drained()) {
        QTimer::singleShot(0, this, &ReviewNavigationController::closed);
    }
}
} // namespace chess_review::app
