#pragma once
#include "app/EngineSettingsController.h"
#include "app/ReviewConfiguration.h"
#include "app/ReviewExplorationController.h"
#include "app/ReviewGameState.h"
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
class ReviewWorkspaceController;
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
    const ReviewGameState& state() const;
    const ReviewGameState& taskState() const { return state_; }
    QUuid taskToken() const { return job_; }
    ReviewWorkspaceController& workspace();
    const EngineSettingsState& engines() const { return settings_.state(); }
    ReviewMovesController& moves();
    ReviewExplorationController& exploration();
    ReviewNavigationController& navigation();
    review::Quality defaultQuality() const;
    void setReady(bool ready);
    void selectGame(persistence::GameId id);
    void refreshRuns(std::int64_t page = 0);
    void inspect(review::ReviewId id);
    QString startBlocker(std::int64_t profileId) const;
    void start(chess::Color side, std::int64_t profileId, review::Quality quality);
    void resume(review::ReviewId id);
    void refreshOverview(int keyOffset = 0);
    void selectMoment(int positionIndex);
    void cancel();
    void close();
  signals:
    void changed();
    void taskChanged();
    void closed();

  private:
    friend class ReviewWorkspaceController;
    void startJob(chess::Color side, std::int64_t profileId, review::Quality quality);
    void resumeJob(review::ReviewId id);
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
    void advancePosition();
    std::uint64_t nodeBudget() const;
    void search();
    void save(std::optional<engine::AnalysisResult> evidence = std::nullopt, bool terminal = false);
    void checkpoint(review::JobStatus status, review::JobFailure failure = review::JobFailure::None,
                    std::optional<engine::AnalysisResult> evidence = std::nullopt,
                    bool terminal = false);
    void finish();
    void fail(const QString& message, review::JobFailure failure);
    void finishClose();
    void deliverClose();

    persistence::Database& database_;
    EngineSettingsController& settings_;
    engine::EngineSupervisor& supervisor_;
    ReviewConfiguration& configuration_;
    ReviewGameState state_;
    std::unique_ptr<ReviewWorkspaceController> workspace_;
    std::optional<persistence::StoredGame> game_;
    std::optional<persistence::StoredReview> stored_;
    config::ValidatedConfiguration runConfiguration_;
    engine::EngineFingerprint fingerprint_;
    engine::EngineFingerprint searchFingerprint_;
    review::EvidenceStage stage_ = review::EvidenceStage::Quick;
    std::vector<int> refinementPositions_;
    std::size_t refinementIndex_ = 0;
    bool refinementEnabled_ = true;
    bool planned_ = false;
    bool statusWrite_ = false;
    bool planning_ = false;
    int searchWidth_ = 1;
    QThreadPool worker_;
    std::unique_ptr<Cursor> cursor_;
    std::stop_source stop_;
    QTimer retry_;
    std::function<void(QUuid)> operation_;
    QUuid pending_;
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
