#pragma once

#include "persistence/Database.h"
#include <QObject>
#include <QString>
#include <QTimer>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace chess_review::app {
// GUI-thread presentation facade. The injected Database must outlive this controller.
class GameLibraryController final : public QObject {
    Q_OBJECT
  public:
    using DeleteGuard = std::function<QString(persistence::GameId)>;
    explicit GameLibraryController(persistence::Database& database, DeleteGuard deleteGuard = {},
                                   QObject* parent = nullptr);
    void setReady(bool ready);
    // Home only needs recent games, without deriving review summaries.
    void activate(bool reviewDetails = true);
    void deactivate();
    void open(persistence::GameId id);
    bool opening() const { return !pendingOpen_.isNull(); }
    void refresh(bool newest = false);
    void importedGame();
    void setQuery(persistence::GameLibraryQuery query);
    const persistence::GameLibraryQuery& query() const { return query_; }
    void selectPreview(std::optional<persistence::GameId> game);
    const std::optional<persistence::GamePreview>& preview() const { return preview_; }
    const QString& previewError() const { return previewError_; }
    bool previewLoading() const { return previewGame_ && !preview_ && previewError_.isEmpty(); }
    void remove(persistence::GameId id);
    bool deleting() const { return !deletePending_.isNull(); }
    std::uint64_t rowsVersion() const { return rowsVersion_; }
    QString reviewStatus(persistence::GameId id) const;
    QString reviewAccuracy(persistence::GameId id) const;
    std::optional<chess::Color> reviewedSide(persistence::GameId id) const;
    void previous();
    void next();
    const std::vector<persistence::GameSummary>& rows() const { return rows_; }
    bool busy() const { return !pending_.isNull(); }
    bool ready() const { return ready_; }
    bool showingGames() const { return reviewDetails_; }
    bool hasNext() const { return hasNext_; }
    std::int64_t page() const { return offset_ / pageSize; }
    const QString& error() const { return openError_.isEmpty() ? error_ : openError_; }
  signals:
    void changed();
    void reviewsChanged();
    void previewChanged();
    void deletionFailed(const QString& message);
    void removed(persistence::GameId id);
    void openingChanged(persistence::GameId id, const persistence::OpeningResolution& opening);
    void openFailed(const QString& message);
    void opened(const chess_review::persistence::StoredGame& game);

  private:
    static constexpr int pageSize = 50;
    static constexpr int recentGamesLimit = 5;
    void refreshMetadata(bool newest, bool invalidateAccuracy);
    void requestPage();
    void requestReviews();
    void requestAccuracy();
    void requestOpening();
    void requestPreview();
    struct RowRequest {
        persistence::RequestId id;
        persistence::GameId game;
        std::uint64_t revision;
    };
    std::optional<RowRequest> openingRequest_;
    std::size_t nextOpeningRow_ = 0;
    struct ReviewRow {
        persistence::LibraryReview metadata;
        std::optional<review::SideSummary> side;
        QString error;
    };
    std::map<persistence::GameId, ReviewRow> reviews_;
    persistence::RequestId reviewsPending_;
    persistence::RequestId accuracyPending_;
    std::vector<persistence::ReviewSummary> accuracyQueue_;
    std::optional<persistence::ReviewSummary> accuracyRun_;
    std::uint64_t reviewsRevision_ = 0;
    std::uint64_t accuracyRevision_ = 0;
    persistence::Database& database_;
    QTimer refreshTimer_;
    QTimer previewTimer_;
    persistence::GameLibraryQuery query_;
    std::optional<persistence::GameId> previewGame_;
    std::optional<RowRequest> previewPending_;
    std::optional<persistence::GamePreview> preview_;
    QString previewError_;
    std::uint64_t previewGeneration_ = 0;
    DeleteGuard deleteGuard_;
    persistence::RequestId deletePending_;
    persistence::GameId deletingGame_ = 0;
    std::uint64_t rowsVersion_ = 0;
    persistence::RequestId pending_;
    persistence::RequestId pendingOpen_;
    std::vector<persistence::GameSummary> rows_;
    QString error_;
    QString openError_;
    std::uint64_t openGeneration_ = 0;
    std::uint64_t requestedOpenGeneration_ = 0;
    std::int64_t offset_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t requestedRevision_ = 0;
    bool ready_ = false;
    bool active_ = false;
    bool reviewDetails_ = true;
    bool pageStale_ = true;
    bool hasNext_ = false;
};
} // namespace chess_review::app
