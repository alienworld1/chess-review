#pragma once
#include "app/ReviewConfiguration.h"
#include "app/ReviewExplorationController.h"
#include "app/ReviewGameState.h"
#include "app/ReviewNavigationController.h"
#include <deque>
#include <functional>
namespace chess_review::app {
class ReviewGameController;
// GUI-thread workspace. Storage, replay and engine work retain their existing owners.
class ReviewWorkspaceController final : public QObject {
    Q_OBJECT
  public:
    ReviewWorkspaceController(persistence::Database& database, EngineSettingsController& settings,
                              engine::EngineSupervisor& supervisor,
                              ReviewConfiguration& configuration, ReviewGameController& task);
    const ReviewGameState& state() const { return state_; }
    ReviewMovesController& moves() { return moves_; }
    ReviewNavigationController& navigation() { return navigation_; }
    ReviewExplorationController& exploration() { return exploration_; }
    bool hasGame() const { return game_.has_value(); }
    void setReady(bool ready);
    void selectGame(persistence::GameId id, std::optional<review::ReviewId> run = std::nullopt);
    void inspect(review::ReviewId id);
    void refreshRuns(std::int64_t page = 0);
    void refreshOverview(int offset = 0);
    void selectMoment(int index);
    void start(chess::Color side, std::int64_t profile, review::Quality quality);
    void resume(review::ReviewId id);
    void returnToTask();
    void setVisible(bool visible);
    void recover();
    void suppressRestoration() { restoreAllowed_ = false; }
    void refreshRecovery(std::int64_t page = 0);
    const std::vector<persistence::ReviewSummary>& interrupted() const { return interrupted_; }
    std::int64_t recoveryPage() const { return recoveryPage_; }
    bool recoveryHasNext() const { return recoveryHasNext_; }
    bool recoveryBusy() const { return !recoveryPending_.isNull(); }
    const QString& recoveryError() const { return recoveryError_; }
    void close();
    bool drained() const { return closed_; }
  signals:
    void changed();
    void closed();
    void showRequested();
    void restoreFailed();
    void recoveryFinished(bool restored);
    void recoveryChanged();
    void contextCommitted();

  private:
    friend class ReviewGameController;
    void publish();
    void updateCompatibility();
    void taskChanged();
    void request(std::function<void(QUuid)> operation);
    bool accept(QUuid id);
    void loadRun(review::ReviewId id);
    void loadContext(review::ReviewId id = 0);
    void remember();
    void flush();
    void finishClose();
    persistence::Database& database_;
    ReviewConfiguration& configuration_;
    ReviewGameController& task_;
    ReviewGameState state_;
    ReviewMovesController moves_;
    ReviewNavigationController navigation_;
    ReviewExplorationController exploration_;
    std::optional<persistence::StoredGame> game_;
    std::optional<persistence::StoredReview> stored_;
    std::optional<review::ReviewId> desiredRun_;
    std::optional<int> restoreMoment_;
    std::optional<int> userMoment_;
    bool restorePending_ = false;
    bool contextLoading_ = false;
    bool visible_ = false;
    bool restoreAllowed_ = true;
    bool closing_ = false;
    bool notifying_ = false;
    bool closed_ = false;
    bool navigationClosed_ = false;
    bool explorationClosed_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t requestedGeneration_ = 0;
    QUuid pending_;
    QUuid restoreRequest_;
    QUuid recoveryPending_;
    QUuid write_;
    std::function<void(QUuid)> operation_;
    std::function<void(QUuid)> next_;
    QTimer retry_;
    QTimer saveTimer_;
    QTimer recoveryRetry_;
    std::deque<persistence::ReviewContext> writes_;
    std::optional<persistence::ReviewContext> saving_;
    std::optional<persistence::ReviewContext> remembered_;
    std::vector<persistence::ReviewSummary> interrupted_;
    std::int64_t recoveryPage_ = 0;
    bool recoveryHasNext_ = false;
    QString recoveryError_;
};
} // namespace chess_review::app
