#pragma once
#include "app/EngineAnalysisController.h"
#include "app/ReviewNavigationController.h"
#include "review/Retry.h"
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <cstdint>
#include <memory>
#include <optional>

namespace chess_review::app {
enum class ExplorationMode { Game, Line, Retry, Grading, Success };
struct ReviewReturnOrigin {
    persistence::GameId game = 0;
    review::ReviewId run = 0;
    ReviewTarget target;
    int candidate = -1;
    int step = 0;
    bool blackAtBottom = false;
    bool operator==(const ReviewReturnOrigin&) const = default;
};
struct ExplorationState {
    ExplorationMode mode = ExplorationMode::Game;
    bool loading = false;
    bool pendingBoard = false;
    bool canRetry = false;
    bool canAnalyze = false;
    int candidate = 0;
    int step = 0;
    int length = 0;
    QString message;
    QStringList candidates;
};
// All injected services outlive closed(). Owns isolated replay, retry and linked Analysis sessions.
class ReviewExplorationController final : public QObject {
    Q_OBJECT
  public:
    ReviewExplorationController(persistence::Database& database,
                                ReviewNavigationController& navigation,
                                EngineSettingsController& settings,
                                engine::EngineSupervisor& supervisor);
    ~ReviewExplorationController() override;
    const ExplorationState& state() const { return state_; }
    bool active() const { return state_.mode != ExplorationMode::Game; }
    bool hidesSolution() const;
    const BoardViewState& board() const;
    void bind(const std::optional<persistence::ReviewSummary>& run);
    void reload();
    void showLine(int candidate = 0);
    void step(int position);
    void previous();
    void next();
    void beginning();
    void end();
    void returnToGame();
    void tryAgain();
    void hint();
    void selectSquare(chess::Square square);
    void clearSelection();
    void attemptMove(chess::Move move, std::uint64_t revision);
    void flip();
    std::optional<ReviewReturnOrigin> analysisOrigin() const;
    bool sameAnalysisOrigin() const;
    void restoreReviewOrigin();
    void openAnalysis(); // Caller resolves discard confirmation for a different edited origin.
    AnalysisController& linkedAnalysis() { return linked_; }
    EngineAnalysisController& linkedEngine() { return linkedEngine_; }
    void close();
    bool drained() const { return closed_; }
  signals:
    void changed();
    void closed();
    void moveFeedback(chess_review::chess::Move move, bool success);

  private:
    void synchronize();
    void request();
    void grade(chess::Move move);
    void scheduleChild();
    void evaluateChild(engine::AnalysisResult result);
    void showGrade(review::RetryResult result);
    void finishClose();
    persistence::Database& database_;
    ReviewNavigationController& navigation_;
    EngineSettingsController& settings_;
    engine::EngineSupervisor& supervisor_;
    AnalysisController sandbox_;
    AnalysisController linked_;
    EngineAnalysisController linkedEngine_;
    ExplorationState state_;
    BoardViewState displayedBoard_;
    std::optional<persistence::ReviewSummary> run_;
    std::shared_ptr<const persistence::ReviewExploration> evidence_;
    std::optional<ReviewReturnOrigin> linkedOrigin_;
    std::shared_ptr<const persistence::ReviewExploration> linkedEvidence_;
    ExplorationState linkedState_;
    std::optional<engine::AnalysisRequest> prepared_;
    std::optional<review::PositionEvidence> deepChild_;
    std::optional<chess::Move> attempted_;
    QThreadPool worker_;
    QTimer success_;
    QTimer retryRead_;
    QUuid cacheRead_;
    std::uint64_t cacheGeneration_ = 0;
    QUuid pending_;
    QUuid search_;
    QUuid session_;
    ReviewTarget target_;
    std::uint64_t generation_ = 0;
    std::uint64_t pendingGeneration_ = 0;
    int hint_ = 0;
    bool verifying_ = false;
    bool workerBusy_ = false;
    bool evidenceReadFailed_ = false;
    bool closing_ = false;
    bool sandboxClosed_ = false;
    bool linkedClosed_ = false;
    bool engineClosed_ = false;
    bool closed_ = false;
};
} // namespace chess_review::app
