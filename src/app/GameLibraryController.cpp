#include "app/GameLibraryController.h"
#include <QTimer>
#include <algorithm>
#include <utility>

namespace chess_review::app {
GameLibraryController::GameLibraryController(persistence::Database& database, QObject* parent)
    : QObject(parent), database_(database) {
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(250);
    connect(&refreshTimer_, &QTimer::timeout, this, [this] { refresh(); });
    connect(&database_, &persistence::Database::gamesListed, this,
            [this](persistence::RequestId request, const persistence::ListGamesResult& result) {
                if (pending_.isNull() || request != pending_) {
                    return;
                }
                pending_ = {};
                if (requestedRevision_ != revision_) {
                    requestPage();
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
                emit changed();
            });
}
void GameLibraryController::setReady(bool ready) {
    ready_ = ready;
    ++revision_;
    if (!ready) {
        refreshTimer_.stop();
        pending_ = {};
        rows_.clear();
        hasNext_ = false;
    } else if (active_) {
        requestPage();
    }
    emit changed();
}
void GameLibraryController::activate() {
    if (!active_) {
        active_ = true;
        refresh();
    }
}
void GameLibraryController::refresh(bool newest) {
    refreshTimer_.stop();
    if (newest) {
        offset_ = 0;
    }
    ++revision_;
    requestPage();
}
void GameLibraryController::importedGame() {
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
} // namespace chess_review::app
