#pragma once
#include "app/EngineSettingsController.h"
#include "app/ReviewConfiguration.h"
#include "app/ReviewMovesController.h"
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace chess_review::app {
enum class ReviewPhase {
    Idle,
    Loading,
    Connecting,
    Preparing,
    LookingUp,
    Scanning,
    Saving,
    Finished
};
struct ReviewGameState {
    persistence::GameId gameId = 0;
    QString game;
    QString engine;
    ReviewPhase phase = ReviewPhase::Idle;
    QString message;
    QString error;
    QString notice;
    std::optional<persistence::ReviewSummary> run;
    std::vector<persistence::ReviewSummary> runs;
    std::int64_t page = 0;
    bool hasNext = false;
    bool listing = false;
    bool active = false;
    bool busy = false;
    bool ready = false;
    bool cancelling = false;
};
QString reviewStatusText(review::JobStatus status);
// GUI-thread facade. Injected services outlive closed(). Only the serial worker owns Cursor.
// closed() is delivered asynchronously outside changed() notifications; then destruction is safe.
class ReviewGameController final : public QObject {
    Q_OBJECT
  public:
    ReviewGameController(persistence::Database& database, EngineSettingsController& settings,
                         engine::EngineSupervisor& supervisor, ReviewConfiguration& configuration,
                         QObject* parent = nullptr);
    ~ReviewGameController() override;
    const ReviewGameState& state() const { return state_; }
    const EngineSettingsState& engines() const { return settings_.state(); }
    ReviewMovesController& moves() { return moves_; }
    review::Quality defaultQuality() const;
    void setReady(bool ready);
    void selectGame(persistence::GameId id);
    void refreshRuns(std::int64_t page = 0);
    void inspect(review::ReviewId id);
    QString startBlocker(std::int64_t profileId) const;
    void start(chess::Color side, std::int64_t profileId, review::Quality quality);
    void resume(review::ReviewId id);
    void cancel();
    void close();
  signals:
    void changed();
    void closed();

  private:
    struct Cursor;
    void publish();
    void request(std::function<void(QUuid)> operation, bool durable = false);
    bool accept(QUuid id);
    void handleError(const persistence::ReviewError& error);
    void runtimeChanged();
    void connectSelected(bool fresh);
    void prepareIdentity();
    void preparePosition();
    void positionReady(std::shared_ptr<const chess::Position> position, bool terminal,
                       engine::CacheQuery query);
    void advance();
    void search();
    void save(std::optional<engine::AnalysisResult> evidence = std::nullopt, bool terminal = false);
    void checkpoint(review::JobStatus status, review::JobFailure failure = review::JobFailure::None,
                    std::optional<engine::AnalysisResult> evidence = std::nullopt,
                    bool terminal = false);
    void finish();
    void fail(const QString& message, review::JobFailure failure);
    void finishClose();
    void deliverClose();
    void loadInspection(review::ReviewId id);
    persistence::Database& database_;
    EngineSettingsController& settings_;
    engine::EngineSupervisor& supervisor_;
    ReviewConfiguration& configuration_;
    ReviewGameState state_;
    ReviewMovesController moves_;
    std::optional<persistence::StoredGame> game_;
    std::optional<persistence::StoredReview> stored_;
    config::ValidatedConfiguration runConfiguration_;
    engine::EngineFingerprint fingerprint_;
    QThreadPool worker_;
    std::unique_ptr<Cursor> cursor_;
    std::stop_source stop_;
    QTimer retry_;
    QTimer listRetry_;
    std::function<void(QUuid)> operation_;
    QUuid pending_;
    std::optional<review::ReviewId> nextInspection_;
    QUuid listing_;
    QUuid cacheWrite_;
    QUuid session_;
    QUuid job_;
    QUuid search_;
    std::optional<QUuid> attempt_;
    std::shared_ptr<const chess::Position> position_;
    std::uint64_t generation_ = 0;
    std::int64_t profileId_ = 0;
    chess::Color side_ = chess::Color::White;
    int index_ = 0;
    int retryMs_ = 25;
    bool workerBusy_ = false;
    bool resuming_ = false;
    bool reconnecting_ = false;
    bool identityReady_ = false;
    bool terminalWrite_ = false;
    bool durableOperation_ = false;
    bool closing_ = false;
    bool closed_ = false;
    bool notifying_ = false;
    bool closeQueued_ = false;
    bool failurePending_ = false;
    review::JobFailure failure_ = review::JobFailure::None;
};
} // namespace chess_review::app
