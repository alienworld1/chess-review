#include "app/GameLibraryController.h"
#include "app/ReviewGameController.h"
#include <QTimer>
#include <algorithm>
#include <utility>

namespace chess_review::app {
GameLibraryController::GameLibraryController(persistence::Database& database,
                                             DeleteGuard deleteGuard, QObject* parent)
    : QObject(parent), database_(database), deleteGuard_(std::move(deleteGuard)) {
    connect(&database_, &persistence::Database::gameLoaded, this,
            [this](persistence::RequestId request, const persistence::LoadGameResult& result) {
                if (pendingOpen_.isNull() || request != pendingOpen_) {
                    return;
                }
                pendingOpen_ = {};
                if (ready_ && requestedOpenGeneration_ == openGeneration_) {
                    if (const auto* game = std::get_if<persistence::StoredGame>(&result)) {
                        openError_.clear();
                        emit opened(*game);
                    } else {
                        openError_ = std::get<persistence::GameError>(result).message;
                        emit openFailed(openError_);
                    }
                }
                emit changed();
                requestPreview();
                requestOpening();
            });
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(250);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] { refreshMetadata(false, false); });
    previewTimer_.setSingleShot(true);
    previewTimer_.setInterval(120);
    connect(&previewTimer_, &QTimer::timeout, this, &GameLibraryController::requestPreview);
    connect(&database_, &persistence::Database::gamePreviewed, this,
            [this](QUuid id, const persistence::GamePreviewResult& result) {
                if (!previewPending_ || previewPending_->id != id) {
                    return;
                }
                const auto request = *std::exchange(previewPending_, std::nullopt);
                if (ready_ && active_ && reviewDetails_ && previewGame_ == request.game &&
                    previewGeneration_ == request.revision) {
                    if (const auto* preview = std::get_if<persistence::GamePreview>(&result)) {
                        preview_ = *preview;
                        const auto row =
                            std::ranges::find(rows_, request.game, &persistence::GameSummary::id);
                        if (row != rows_.end() && !row->opening && preview->summary.opening) {
                            row->opening = preview->summary.opening;
                            emit openingChanged(request.game, *preview->summary.opening);
                        }
                        emit previewChanged();
                    } else {
                        const auto& error = std::get<persistence::GameError>(result);
                        if (error.code == persistence::GameErrorCode::Busy) {
                            previewTimer_.start();
                        } else {
                            previewError_ = error.message;
                            emit previewChanged();
                        }
                    }
                }
                requestPreview();
                requestOpening();
                requestAccuracy();
            });
    connect(&database_, &persistence::Database::gameDeleted, this,
            [this](QUuid id, const persistence::DeleteGameResult& result) {
                if (!deleting() || deletePending_ != id) {
                    return;
                }
                deletePending_ = {};
                const auto game = std::exchange(deletingGame_, 0);
                if (const auto* error = std::get_if<persistence::GameError>(&result)) {
                    emit deletionFailed(error->message);
                    if (error->code == persistence::GameErrorCode::Missing) {
                        refreshMetadata(false, false);
                    }
                } else {
                    pageStale_ = true;
                    if (previewGame_ == game) {
                        selectPreview(std::nullopt);
                    }
                    reviews_.erase(game);
                    openError_.clear();
                    refreshMetadata(false, false);
                    emit removed(game);
                }
                emit changed();
                requestPreview();
                requestOpening();
                requestAccuracy();
            });
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
                ++rowsVersion_;
                nextOpeningRow_ = 0;
                hasNext_ = false;
                if (const auto* rows =
                        std::get_if<std::vector<persistence::GameSummary>>(&result)) {
                    hasNext_ = reviewDetails_ && rows->size() > pageSize;
                    const auto size = std::min<std::size_t>(pageSize, rows->size());
                    rows_.assign(rows->begin(), rows->begin() + static_cast<std::ptrdiff_t>(size));
                    error_.clear();
                    pageStale_ = false;
                    if (rows_.empty() && reviewDetails_ && offset_ > 0) {
                        offset_ -= pageSize;
                        ++revision_;
                        requestPage();
                        return;
                    }
                } else {
                    error_ = std::get<persistence::GameError>(result).message;
                }
                requestReviews();
                emit changed();
                requestPreview();
                requestOpening();
            });
    connect(&database_, &persistence::Database::gameOpeningResolved, this,
            [this](QUuid id, const persistence::ResolveGameOpeningResult& result) {
                if (!openingRequest_ || openingRequest_->id != id) {
                    return;
                }
                const auto request = *std::exchange(openingRequest_, std::nullopt);
                if (ready_ && active_ && reviewDetails_ && request.revision == revision_) {
                    if (const auto* opening =
                            std::get_if<std::optional<persistence::OpeningResolution>>(&result)) {
                        if (!*opening) {
                            nextOpeningRow_ = rows_.size();
                            return;
                        }
                        const auto row =
                            std::ranges::find(rows_, request.game, &persistence::GameSummary::id);
                        if (row != rows_.end()) {
                            row->opening = **opening;
                            emit openingChanged(request.game, **opening);
                        }
                    } else if (std::get<persistence::GameError>(result).code ==
                               persistence::GameErrorCode::Busy) {
                        --nextOpeningRow_;
                        QTimer::singleShot(100, this, &GameLibraryController::requestOpening);
                        return;
                    }
                }
                requestOpening();
            });
    connect(&database_, &persistence::Database::libraryReviewsListed, this,
            [this](QUuid id, const persistence::ListLibraryReviewsResult& result) {
                if (id != reviewsPending_ || id.isNull()) {
                    return;
                }
                reviewsPending_ = {};
                if (!ready_ || !active_ || !reviewDetails_ || reviewsRevision_ != revision_) {
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
                emit reviewsChanged();
                if (std::holds_alternative<persistence::ReviewError>(result)) {
                    emit changed();
                }
            });
    connect(
        &database_, &persistence::Database::reviewOverviewLoaded, this,
        [this](QUuid id, const persistence::LoadReviewOverviewResult& result) {
            if (id != accuracyPending_ || id.isNull()) {
                return;
            }
            accuracyPending_ = {};
            if (ready_ && active_ && reviewDetails_ && accuracyRevision_ == revision_ &&
                accuracyRun_) {
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
                emit reviewsChanged();
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
        ++rowsVersion_;
        selectPreview(std::nullopt);
        pageStale_ = true;
        hasNext_ = false;
    } else if (active_) {
        requestPage();
    }
    emit changed();
}
void GameLibraryController::open(persistence::GameId id) {
    if (!ready_ || opening() || (deleting() && deletingGame_ == id) || id <= 0) {
        return;
    }
    pendingOpen_ = QUuid::createUuid();
    requestedOpenGeneration_ = openGeneration_;
    openError_.clear();
    database_.loadGame(pendingOpen_, id);
    emit changed();
}
void GameLibraryController::activate(bool reviewDetails) {
    if (reviewDetails_ != reviewDetails) {
        reviewDetails_ = reviewDetails;
        active_ = true;
        selectPreview(std::nullopt);
        refreshMetadata(false, false);
        return;
    }
    if (!active_) {
        active_ = true;
        if (pageStale_) {
            refresh();
        } else {
            requestReviews();
            emit changed();
        }
    } else if (reviewDetails_) {
        requestReviews();
    }
    requestOpening();
}
void GameLibraryController::deactivate() {
    if (!active_) {
        return;
    }
    active_ = false;
    ++revision_;
    nextOpeningRow_ = 0;
    refreshTimer_.stop();
    accuracyQueue_.clear();
    selectPreview(std::nullopt);
}
void GameLibraryController::refresh(bool newest) { refreshMetadata(newest, true); }
void GameLibraryController::refreshMetadata(bool newest, bool invalidateAccuracy) {
    pageStale_ = true;
    refreshTimer_.stop();
    if (invalidateAccuracy) {
        reviews_.clear();
        emit reviewsChanged();
    }
    if (newest) {
        offset_ = 0;
    }
    ++revision_;
    accuracyQueue_.clear();
    requestPage();
}
void GameLibraryController::setQuery(persistence::GameLibraryQuery query) {
    query.players = query.players.simplified();
    if (query_ == query) {
        return;
    }
    query_ = std::move(query);
    offset_ = 0;
    openError_.clear();
    if (reviewDetails_) {
        refreshMetadata(false, false);
    }
}
void GameLibraryController::selectPreview(std::optional<persistence::GameId> game) {
    if (game && (*game <= 0 || !active_ || !reviewDetails_ || !ready_)) {
        game.reset();
    }
    if (game == previewGame_) {
        return;
    }
    previewTimer_.stop();
    ++previewGeneration_;
    previewGame_ = game;
    preview_.reset();
    previewError_.clear();
    if (game) {
        previewTimer_.start();
    } else {
        requestOpening();
        requestAccuracy();
    }
    emit previewChanged();
}
void GameLibraryController::requestPreview() {
    if (busy() || opening() || deleting() || previewPending_ || previewTimer_.isActive() ||
        !previewLoading()) {
        return;
    }
    previewPending_ = RowRequest{QUuid::createUuid(), *previewGame_, previewGeneration_};
    database_.previewGame(previewPending_->id, previewPending_->game);
}
void GameLibraryController::remove(persistence::GameId id) {
    if (!ready_ || deleting() || opening() || id <= 0) {
        return;
    }
    if (deleteGuard_) {
        const auto error = deleteGuard_(id);
        if (!error.isEmpty()) {
            emit deletionFailed(error);
            return;
        }
    }
    deletingGame_ = id;
    deletePending_ = QUuid::createUuid();
    database_.deleteGame(deletePending_, id);
    emit changed();
}
void GameLibraryController::importedGame() {
    pageStale_ = true;
    if (ready_ && active_ && !refreshTimer_.isActive()) {
        refreshTimer_.start();
    }
}
void GameLibraryController::previous() {
    if (busy() || !reviewDetails_ || offset_ == 0) {
        return;
    }
    offset_ -= pageSize;
    refresh();
}
void GameLibraryController::next() {
    if (busy() || !hasNext_) {
        return;
    }
    offset_ += pageSize;
    refresh();
}
void GameLibraryController::requestPage() {
    if (!ready_ || !active_ || busy()) {
        return;
    }
    pending_ = QUuid::createUuid();
    requestedRevision_ = revision_;
    persistence::GameLibraryQuery recent;
    recent.sort = persistence::GameSort::RecentActivity;
    database_.listGames(pending_, reviewDetails_ ? offset_ : 0,
                        reviewDetails_ ? pageSize + 1 : recentGamesLimit,
                        reviewDetails_ ? query_ : recent);
    emit changed();
}
void GameLibraryController::requestReviews() {
    if (!ready_ || !active_ || !reviewDetails_ || !reviewsPending_.isNull() || !pending_.isNull()) {
        return;
    }
    std::vector<persistence::GameId> ids;
    for (const auto& row : rows_) {
        if (row.id > 0) {
            ids.push_back(row.id);
        }
    }
    if (ids.empty()) {
        return;
    }
    reviewsRevision_ = revision_;
    reviewsPending_ = QUuid::createUuid();
    database_.listLibraryReviews(reviewsPending_, std::move(ids));
}
void GameLibraryController::requestAccuracy() {
    if (!ready_ || !active_ || !reviewDetails_ || busy() || deleting() || pageStale_ ||
        !accuracyPending_.isNull() || accuracyQueue_.empty() || previewLoading()) {
        return;
    }
    if (previewGame_) {
        const auto selected =
            std::ranges::find(accuracyQueue_, *previewGame_, &persistence::ReviewSummary::gameId);
        if (selected != accuracyQueue_.end()) {
            std::iter_swap(selected, accuracyQueue_.end() - 1);
        }
    }
    accuracyRun_ = accuracyQueue_.back();
    accuracyQueue_.pop_back();
    accuracyRevision_ = revision_;
    accuracyPending_ = QUuid::createUuid();
    database_.loadReviewOverview(accuracyPending_, accuracyRun_->id, accuracyRun_->revision);
}
void GameLibraryController::requestOpening() {
    if (!ready_ || !active_ || !reviewDetails_ || busy() || opening() || pageStale_ || deleting() ||
        previewLoading() || openingRequest_) {
        return;
    }
    while (nextOpeningRow_ < rows_.size()) {
        const auto& row = rows_[nextOpeningRow_++];
        if (!row.opening && !row.metadataError) {
            openingRequest_ = RowRequest{QUuid::createUuid(), row.id, revision_};
            database_.resolveGameOpening(openingRequest_->id, row.id);
            return;
        }
    }
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
    return reviewStatusText(run.status) +
           (row.metadata.hasInterrupted && run.status != review::JobStatus::Interrupted
                ? tr(" (unfinished review available)")
                : QString{});
}
std::optional<chess::Color> GameLibraryController::reviewedSide(persistence::GameId id) const {
    const auto found = reviews_.find(id);
    return found != reviews_.end() && found->second.metadata.run
               ? std::optional{found->second.metadata.run->selectedSide}
               : std::nullopt;
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
        return tr("Unavailable (%1/%2 analyzed)").arg(side.evaluated).arg(side.total);
    }
    const auto text = tr("%1%").arg(*side.accuracy, 0, 'f', 1);
    return text + (side.evaluated < side.total    ? tr(" (partial)")
                   : side.stable < side.evaluated ? tr(" (preliminary)")
                                                  : QString{});
}
} // namespace chess_review::app
