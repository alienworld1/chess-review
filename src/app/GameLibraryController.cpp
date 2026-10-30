#include "app/GameLibraryController.h"
#include "app/ReviewGameController.h"
#include <QTimer>
#include <algorithm>
#include <utility>

namespace chess_review::app {
GameLibraryController::GameLibraryController(persistence::Database& database, QObject* parent)
    : QObject(parent), database_(database) {
    connect(&database_, &persistence::Database::gameLoaded, this,
            [this](persistence::RequestId request, const persistence::LoadGameResult& result) {
                if (pendingOpen_.isNull() || request != pendingOpen_) {
                    return;
                }
                pendingOpen_ = {};
                if (!ready_ || requestedOpenGeneration_ != openGeneration_) {
                    emit changed();
                    return;
                }
                if (const auto* game = std::get_if<persistence::StoredGame>(&result)) {
                    openError_.clear();
                    emit opened(*game);
                } else {
                    openError_ = std::get<persistence::GameError>(result).message;
                    emit openFailed(openError_);
                }
                emit changed();
            });
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(250);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] { refreshMetadata(false, false); });
    connect(&database_, &persistence::Database::gamesListed, this,
            [this](persistence::RequestId request, const persistence::ListGamesResult& result) {
                if (pending_.isNull() || request != pending_) {
                    return;
                }
                pending_ = {};
                if (requestedRevision_ != revision_ || !ready_ || !active_) {
                    requestPage();
                    if (!busy()) {
                        emit changed();
                    }
                    return;
                }
                rows_.clear();
                hasNext_ = false;
                if (const auto* rows =
                        std::get_if<std::vector<persistence::GameSummary>>(&result)) {
                    hasNext_ = rows->size() > 50;
                    const auto size = std::min<std::size_t>(50, rows->size());
                    rows_.assign(rows->begin(), rows->begin() + static_cast<std::ptrdiff_t>(size));
                    error_.clear();
                    pageStale_ = false;
                    // Concurrent imports can shift pages; never strand the user on an empty page.
                    if (rows_.empty() && offset_ > 0) {
                        offset_ = 0;
                        ++revision_;
                        requestPage();
                        return;
                    }
                } else {
                    error_ = std::get<persistence::GameError>(result).message;
                }
                requestReviews();
                emit changed();
            });
    connect(&database_, &persistence::Database::libraryReviewsListed, this,
            [this](QUuid id, const persistence::ListLibraryReviewsResult& result) {
                if (id != reviewsPending_ || id.isNull()) {
                    return;
                }
                reviewsPending_ = {};
                if (!ready_ || !active_ || reviewsRevision_ != revision_) {
                    requestReviews();
                    return;
                }
                if (const auto* issue = std::get_if<persistence::ReviewError>(&result)) {
                    error_ = issue->message;
                    if (issue->code == persistence::ReviewErrorCode::Busy) {
                        refreshTimer_.start();
                    }
                } else {
                    auto previous = std::move(reviews_);
                    reviews_.clear();
                    accuracyQueue_.clear();
                    for (const auto& row :
                         std::get<std::vector<persistence::LibraryReview>>(result)) {
                        auto& presentation = reviews_[row.gameId];
                        presentation = ReviewRow{row, {}, {}};
                        const auto old = previous.find(row.gameId);
                        if (row.run && old != previous.end() && old->second.metadata.run &&
                            old->second.metadata.run->id == row.run->id &&
                            old->second.metadata.run->revision == row.run->revision) {
                            presentation.side = old->second.side;
                        }
                        if (row.run && !presentation.side) {
                            accuracyQueue_.push_back(*row.run);
                        }
                    }
                    std::reverse(accuracyQueue_.begin(), accuracyQueue_.end());
                    requestAccuracy();
                }
                emit changed();
            });
    connect(
        &database_, &persistence::Database::reviewOverviewLoaded, this,
        [this](QUuid id, const persistence::LoadReviewOverviewResult& result) {
            if (id != accuracyPending_ || id.isNull()) {
                return;
            }
            accuracyPending_ = {};
            if (ready_ && active_ && accuracyRevision_ == revision_ && accuracyRun_) {
                auto found = reviews_.find(accuracyRun_->gameId);
                if (found != reviews_.end() && found->second.metadata.run &&
                    found->second.metadata.run->id == accuracyRun_->id &&
                    found->second.metadata.run->revision == accuracyRun_->revision) {
                    if (const auto* overview = std::get_if<persistence::ReviewOverview>(&result)) {
                        if (overview->reviewId == accuracyRun_->id &&
                            overview->revision == accuracyRun_->revision) {
                            found->second.side =
                                overview
                                    ->sides[static_cast<std::size_t>(accuracyRun_->selectedSide)];
                        }
                    } else {
                        const auto& issue = std::get<persistence::ReviewError>(result);
                        found->second.error = issue.message;
                        if (issue.code == persistence::ReviewErrorCode::Busy ||
                            issue.code == persistence::ReviewErrorCode::Conflict) {
                            refreshTimer_.start();
                        }
                    }
                }
                emit changed();
            }
            accuracyRun_.reset();
            requestAccuracy();
        });
}
void GameLibraryController::setReady(bool ready) {
    ready_ = ready;
    ++revision_;
    ++openGeneration_;
    if (!ready) {
        refreshTimer_.stop();
        accuracyQueue_.clear();
        reviews_.clear();
        rows_.clear();
        pageStale_ = true;
        hasNext_ = false;
    } else if (active_) {
        requestPage();
    }
    emit changed();
}
void GameLibraryController::open(persistence::GameId id) {
    if (!ready_ || opening() || id <= 0) {
        return;
    }
    pendingOpen_ = QUuid::createUuid();
    requestedOpenGeneration_ = openGeneration_;
    openError_.clear();
    database_.loadGame(pendingOpen_, id);
    emit changed();
}
void GameLibraryController::activate() {
    if (!active_) {
        active_ = true;
        if (pageStale_) {
            refresh();
        } else {
            requestReviews();
        }
    }
}
void GameLibraryController::deactivate() {
    if (!active_) {
        return;
    }
    active_ = false;
    ++revision_;
    refreshTimer_.stop();
    accuracyQueue_.clear();
}
void GameLibraryController::refresh(bool newest) { refreshMetadata(newest, true); }
void GameLibraryController::refreshMetadata(bool newest, bool invalidateAccuracy) {
    pageStale_ = true;
    refreshTimer_.stop();
    if (invalidateAccuracy) {
        reviews_.clear();
    }
    if (newest) {
        offset_ = 0;
    }
    ++revision_;
    accuracyQueue_.clear();
    requestPage();
}
void GameLibraryController::importedGame() {
    pageStale_ = true;
    if (ready_ && active_ && !refreshTimer_.isActive()) {
        refreshTimer_.start();
    }
}
void GameLibraryController::previous() {
    if (busy() || offset_ == 0) {
        return;
    }
    offset_ -= 50;
    refresh();
}
void GameLibraryController::next() {
    if (busy() || !hasNext_) {
        return;
    }
    offset_ += 50;
    refresh();
}
void GameLibraryController::requestPage() {
    if (!ready_ || !active_ || busy()) {
        return;
    }
    pending_ = QUuid::createUuid();
    requestedRevision_ = revision_;
    database_.listGames(pending_, offset_, 51);
    emit changed();
}
void GameLibraryController::requestReviews() {
    if (!ready_ || !active_ || !reviewsPending_.isNull() || !pending_.isNull()) {
        return;
    }
    std::vector<persistence::GameId> ids;
    for (const auto& row : rows_) {
        ids.push_back(row.id);
    }
    reviewsRevision_ = revision_;
    reviewsPending_ = QUuid::createUuid();
    database_.listLibraryReviews(reviewsPending_, std::move(ids));
}
void GameLibraryController::requestAccuracy() {
    if (!ready_ || !active_ || !accuracyPending_.isNull() || accuracyQueue_.empty()) {
        return;
    }
    accuracyRun_ = accuracyQueue_.back();
    accuracyQueue_.pop_back();
    accuracyRevision_ = revision_;
    accuracyPending_ = QUuid::createUuid();
    database_.loadReviewOverview(accuracyPending_, accuracyRun_->id, accuracyRun_->revision);
}
QString GameLibraryController::reviewStatus(persistence::GameId id) const {
    const auto found = reviews_.find(id);
    if (found == reviews_.end()) {
        return tr("Loading review status…");
    }
    const auto& row = found->second;
    if (row.metadata.error) {
        return row.metadata.error->message;
    }
    if (!row.metadata.run) {
        return tr("Not reviewed");
    }
    const auto& run = *row.metadata.run;
    const auto text = tr("%1 · %2 · saved %3/%4")
                          .arg(reviewStatusText(run.status),
                               run.selectedSide == chess::Color::White ? tr("White") : tr("Black"))
                          .arg(run.completedPositions)
                          .arg(run.totalPositions);
    return text + (row.metadata.hasInterrupted && run.status != review::JobStatus::Interrupted
                       ? tr(" · interrupted review available")
                       : QString{});
}
QString GameLibraryController::reviewAccuracy(persistence::GameId id) const {
    const auto found = reviews_.find(id);
    if (found == reviews_.end() || !found->second.metadata.run) {
        return QStringLiteral("—");
    }
    const auto& row = found->second;
    if (!row.error.isEmpty()) {
        return tr("Unavailable: %1").arg(row.error);
    }
    if (!row.side) {
        return tr("Loading accuracy…");
    }
    const auto& side = *row.side;
    if (!side.accuracy) {
        return tr("Unavailable · %1/%2 evaluated").arg(side.evaluated).arg(side.total);
    }
    const auto text = tr("%1% · %2/%3 evaluated")
                          .arg(*side.accuracy, 0, 'f', 1)
                          .arg(side.evaluated)
                          .arg(side.total);
    return text + (side.evaluated < side.total ? tr(" · partial") : QString{}) +
           (side.stable < side.evaluated ? tr(" · provisional") : QString{});
}
} // namespace chess_review::app
