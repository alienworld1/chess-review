#pragma once
#include "app/EngineSettingsController.h"
#include "app/ReviewConfiguration.h"
#include <QThreadPool>
#include <QTimer>
#include <deque>
#include <functional>
#include <memory>
#include <optional>

namespace chess_review::app {
struct PuzzleGenerationState {
    bool active = false;
    bool cancelling = false;
    bool savingPosition = false;
    QString message;
    QString error;
    std::optional<persistence::PuzzleJobSummary> job;
    std::optional<persistence::PuzzleId> lastSaved;
};
// Owning-thread facade. All chess interpretation runs on the serial worker;
// accepted storage writes and scheduled cancellation drain before closed().
class PuzzleGenerationController final : public QObject {
    Q_OBJECT
  public:
    PuzzleGenerationController(persistence::Database& database, EngineSettingsController& settings,
                               engine::EngineSupervisor& supervisor,
                               ReviewConfiguration& configuration, QObject* parent = nullptr);
    ~PuzzleGenerationController() override;
    const PuzzleGenerationState& state() const { return state_; }
    bool canSavePosition() const {
        return ready_ && !closing_ && !state_.active && configuration_.snapshot() &&
               settings_.state().runtime.phase == engine::RuntimePhase::Ready;
    }
    void setReady(bool ready);
    void generate(review::ReviewId reviewId);
    void enqueue(review::ReviewId reviewId);
    void savePosition(std::shared_ptr<const chess::Position> position,
                      std::optional<persistence::GameId> gameId = std::nullopt);
    void resume(persistence::PuzzleJobId id);
    void cancel();
    void puzzleRemoved(persistence::PuzzleId id);
    void close();
  signals:
    void changed();
    void libraryChanged();
    void closed();

  private:
    void loaded(const persistence::SavePuzzleJobResult& result);
    void advance();
    void evaluate();
    void checkpoint(std::optional<persistence::PuzzleDocument> accepted = std::nullopt);
    void schedule(puzzles::SearchNeed need);
    void stopWithError(const QString& message);
    void idle();
    void finishClose();
    void request(std::function<void(QUuid)> operation);
    persistence::Database& database_;
    EngineSettingsController& settings_;
    engine::EngineSupervisor& supervisor_;
    ReviewConfiguration& configuration_;
    PuzzleGenerationState state_;
    std::shared_ptr<persistence::PuzzleJob> job_;
    std::deque<review::ReviewId> queued_;
    QThreadPool worker_;
    QTimer retry_;
    std::function<void(QUuid)> operation_;
    QUuid pending_;
    QUuid search_;
    QUuid session_;
    QUuid engineJob_;
    bool workerBusy_ = false;
    bool ready_ = false;
    bool closing_ = false;
    bool closed_ = false;
    std::size_t durablePasses_ = 0;
};
} // namespace chess_review::app
