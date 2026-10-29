#pragma once

#include "app/AnalysisController.h"
#include "app/ReviewMovePresentation.h"
#include "app/ReviewMovesController.h"
#include <deque>

namespace chess_review::app {
struct ReviewTarget {
    std::uint64_t session = 0;
    int position = 0;
    bool operator==(const ReviewTarget&) const = default;
};
// Read-only original-game navigation. Private replay and evidence facades are
// drained asynchronously before closed(); injected services must outlive it.
class ReviewNavigationController final : public QObject {
    Q_OBJECT
  public:
    ReviewNavigationController(persistence::Database& database, ReviewMovesController& moves);
    void open(const persistence::StoredGame& game);
    void clear();
    void bind(const std::optional<persistence::ReviewSummary>& run,
              const std::optional<persistence::ReviewOverview>& overview);
    const std::shared_ptr<const chess::Position>& position() const { return replay_.position(); }
    const BoardViewState& board() const { return board_; }
    const std::shared_ptr<const NavigationTree>& tree() const { return replay_.navigationTree(); }
    const std::vector<chess::NodeId>& line() const { return line_; }
    const QString& whitePlayer() const { return replay_.whitePlayer(); }
    const QString& blackPlayer() const { return replay_.blackPlayer(); }
    const QString& error() const { return error_; }
    ReviewTarget target(int position) const { return {session_, position}; }
    int selected() const { return selected_; }
    int displayed() const { return displayed_; }
    int requested() const { return preview_.value_or(selected_); }
    bool previewing() const { return preview_.has_value(); }
    bool ready() const { return ready_; }
    bool pending() const { return !ready_ || displayed_ != requested(); }
    const persistence::ReviewMove* move(int position) const;
    std::optional<int> positionFor(chess::NodeId node) const;
    bool keyMoment(int position) const;
    std::optional<int> opponentContext(int position) const;
    ReviewEvaluation evaluation() const;
    QString evidenceError() const;
    void retryEvidence();
    void select(ReviewTarget target);
    void preview(ReviewTarget target);
    void clearPreview();
    void visible(ReviewTarget first, ReviewTarget last);
    void previous();
    void next();
    void beginning();
    void end();
    void flip();
    void previousMoment();
    void nextMoment();
    bool canPreviousMoment() const;
    bool canNextMoment() const;
    void close();
    bool drained() const { return replayClosed_ && previewEvidence_.drained(); }
  signals:
    void changed();
    void selectionCommitted(int position);
    void closed();

  private:
    bool valid(ReviewTarget target) const;
    void navigate(int position, bool user);
    void dispatch();
    void refreshBoard();
    void cachePage(const ReviewMovesState& state);
    void requestEvidence(int position, bool committed);
    void requestVisibleEvidence();
    void moment(int direction);
    void finishClose();
    AnalysisController replay_;
    ReviewMovesController& moves_;
    ReviewMovesController previewEvidence_;
    BoardViewState board_;
    std::vector<chess::NodeId> line_;
    std::map<chess::NodeId, int> positions_;
    std::deque<persistence::ReviewMovePage> pages_;
    std::optional<persistence::ReviewSummary> run_;
    std::optional<persistence::ReviewOverview> overview_;
    std::optional<int> preview_;
    std::optional<std::pair<int, int>> visible_;
    int selected_ = 0;
    int displayed_ = 0;
    std::uint64_t session_ = 0;
    bool ready_ = false;
    bool opening_ = false;
    bool userNavigated_ = false;
    bool entryChosen_ = false;
    bool closing_ = false;
    bool replayClosed_ = false;
    QString error_;
};
} // namespace chess_review::app
