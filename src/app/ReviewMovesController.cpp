#include "app/ReviewMovesController.h"
#include <algorithm>
namespace chess_review::app {
ReviewMovesController::ReviewMovesController(persistence::Database& database)
    : database_(database) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &ReviewMovesController::requestNext);
    connect(&database_, &persistence::Database::reviewMovesLoaded, this,
            [this](QUuid request, const persistence::LoadReviewMovesResult& result) {
                if (request != pending_) {
                    return;
                }
                pending_ = {};
                if (closing_) {
                    emit drainedChanged();
                    return;
                }
                if (const auto* page = std::get_if<persistence::ReviewMovePage>(&result)) {
                    retryMs_ = 25;
                    if (run_ && page->reviewId == run_->id && page->revision == run_->revision) {
                        if (page->offset == backfillOffset_) {
                            backfillOffset_ += static_cast<int>(page->moves.size());
                        }
                        if (page->offset == state_.offset) {
                            state_.page = *page;
                            state_.loading = false;
                            state_.error.clear();
                            if (state_.selectedPosition <= page->offset ||
                                state_.selectedPosition >
                                    page->offset + static_cast<int>(page->moves.size())) {
                                state_.selectedPosition =
                                    page->moves.empty() ? 0 : page->moves.front().positionIndex;
                            }
                        }
                    }
                } else if (const auto* error = std::get_if<persistence::ReviewError>(&result)) {
                    if (error->code == persistence::ReviewErrorCode::Busy) {
                        dirty_ = true;
                        timer_.start(retryMs_);
                        retryMs_ = std::min(retryMs_ * 2, 1000);
                        return;
                    }
                    // A newer bind can supersede a conflicted/stale request. Other errors stay
                    // visible.
                    if (!dirty_) {
                        state_.error = error->message;
                        state_.loading = false;
                        backfillOffset_ = run_ ? run_->totalPositions - 1 : 0;
                        emit changed();
                        return;
                    }
                }
                timer_.start(0);
                emit changed();
            });
}
void ReviewMovesController::bind(const std::optional<persistence::ReviewSummary>& run) {
    if (closing_) {
        return;
    }
    if ((!run && !run_) ||
        (run && run_ && run->id == run_->id && run->revision == run_->revision)) {
        return;
    }
    const bool same = run && run_ && run->id == run_->id;
    run_ = run;
    if (!same) {
        state_ = {};
        backfillOffset_ = 0;
    }
    dirty_ = run.has_value();
    if (dirty_) {
        state_.loading = true;
    }
    if (pending_.isNull()) {
        timer_.start(0);
    }
}
void ReviewMovesController::showPage(int offset) {
    if (closing_ || !run_ || offset < 0 || offset >= run_->totalPositions || offset % 50 != 0) {
        return;
    }
    state_.offset = offset;
    state_.page.reset();
    state_.selectedPosition = 0;
    state_.loading = true;
    state_.error.clear();
    dirty_ = true;
    if (pending_.isNull()) {
        timer_.start(0);
    }
    emit changed();
}
void ReviewMovesController::select(int positionIndex) {
    if (!state_.page || positionIndex <= state_.page->offset ||
        positionIndex > state_.page->offset + static_cast<int>(state_.page->moves.size())) {
        return;
    }
    state_.selectedPosition = positionIndex;
    emit changed();
}
void ReviewMovesController::requestNext() {
    if (closing_ || !pending_.isNull() || !run_) {
        return;
    }
    if (!dirty_ && backfillOffset_ >= run_->totalPositions - 1) {
        return;
    }
    const int offset = dirty_ ? state_.offset : backfillOffset_;
    dirty_ = false;
    pending_ = QUuid::createUuid();
    database_.deriveReviewMoves(pending_, run_->id, run_->revision, offset);
}
void ReviewMovesController::close() {
    closing_ = true;
    timer_.stop();
}
} // namespace chess_review::app
