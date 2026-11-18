#pragma once
#include "app/EngineAnalysisController.h"
#include "app/PuzzleAlternative.h"
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace chess_review::app {
struct PuzzleState {
    persistence::PuzzlePage library;
    std::vector<persistence::PuzzleJobSummary> interrupted;
    bool loading = false;
    bool solving = false;
    bool revealed = false;
    bool complete = false;
    bool grading = false;
    std::optional<persistence::PuzzleId> selected;
    QString message;
    QString error;
    QStringList hints;
};
struct ReviewPuzzleState {
    review::ReviewId id = 0;
    int count = 0;
    bool more = false;
    std::optional<persistence::PuzzleJobSummary> generation;
};
class PuzzleController final : public QObject {
    Q_OBJECT
  public:
    PuzzleController(persistence::Database& database, EngineSettingsController& settings,
                     engine::EngineSupervisor& supervisor, QObject* parent = nullptr);
    ~PuzzleController() override;
    const PuzzleState& state() const { return state_; }
    const BoardViewState& board() const { return displayed_; }
    bool canHint() const;
    bool canStepBack() const;
    bool canStepForward() const;
    const ReviewPuzzleState& reviewSummary() const { return reviewSummary_; }
    void inspectReview(review::ReviewId id, bool refresh = false);
    void setReady(bool ready);
    void refresh(std::int64_t offset = 0, std::optional<review::ReviewId> review = std::nullopt);
    void refreshCurrent() { refresh(offset_, filter_); }
    void open(persistence::PuzzleId id);
    void remove(persistence::PuzzleId id);
    void showLibrary();
    void retry();
    void hint();
    void reveal();
    void previous();
    void next();
    void selectSquare(chess::Square square);
    void clearSelection();
    void attemptMove(chess::Move move, std::uint64_t revision);
    void flip();
    void openAnalysis();
    bool enterAnalysis(); // False when newer navigation invalidated the discard gate.
    AnalysisController& linkedAnalysis() { return linked_; }
    EngineAnalysisController& linkedEngine() { return linkedEngine_; }
    void close();
  signals:
    void changed();
    void moveFeedback(chess_review::chess::Move move, bool success);
    void analysisRequested();
    void reviewSummaryChanged();
    void puzzleRemoved(persistence::PuzzleId id);
    void closed();

  private:
    void publish();
    void resetBoard();
    void advance();
    void play(puzzles::Edge edge, bool solverMove);
    void cancelAlternative();
    void evaluateAlternative();
    void scheduleAlternative(puzzles::SearchNeed need);
    void alternativeUnavailable();
    void finishClose();
    persistence::Database& database_;
    EngineSettingsController& settings_;
    engine::EngineSupervisor& supervisor_;
    AnalysisController board_;
    AnalysisController linked_;
    EngineAnalysisController linkedEngine_;
    PuzzleState state_;
    ReviewPuzzleState reviewSummary_;
    BoardViewState displayed_;
    std::shared_ptr<const persistence::StoredPuzzle> puzzle_;
    std::shared_ptr<const chess::Position> root_;
    std::shared_ptr<const chess::Position> analysisOrigin_;
    std::uint64_t analysisEpoch_ = 0;
    std::shared_ptr<PuzzleAlternative> alternative_;
    std::vector<chess::Move> revealedLine_;
    std::vector<int> hintLevels_;
    std::size_t node_ = 0;
    std::uint64_t epoch_ = 0;
    QThreadPool worker_;
    QTimer reply_;
    QTimer reads_;
    QUuid list_;
    QUuid reviewList_;
    QUuid reviewJobs_;
    QUuid jobs_;
    QUuid load_;
    QUuid deletion_;
    persistence::PuzzleId deleting_ = 0;
    persistence::PuzzleId opening_ = 0;
    QUuid search_;
    QUuid session_;
    QUuid analysisJob_;
    std::optional<review::ReviewId> filter_;
    std::int64_t offset_ = 0;
    bool ready_ = false;
    bool workerBusy_ = false;
    bool moving_ = false;
    bool closing_ = false;
    bool closed_ = false;
    bool boardClosed_ = false;
    bool linkedClosed_ = false;
    bool engineClosed_ = false;
    bool pendingReset_ = false;
    bool refreshPending_ = false;
};
} // namespace chess_review::app
