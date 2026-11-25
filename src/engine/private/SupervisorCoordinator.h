#pragma once
#include "engine/private/EngineProcessWorker.h"
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <variant>
#include <vector>
namespace chess_review::engine {
// Lives on the engine event-loop thread. Only this object admits attempts or quits the thread.
class SupervisorCoordinator final : public QObject {
    Q_OBJECT
  public:
    void inspect(QUuid request, const QString& path);
    void detectEngines(QUuid request);
    void checkExecutables(QUuid request, const QList<std::pair<std::int64_t, QString>>& profiles);
    void validate(QUuid request, const QString& path);
    void connectEngine(QUuid session, const EngineProfile& profile, EnginePoolLimits limits);
    void disconnectEngine(QUuid session);
    void analyze(QUuid session, const AnalysisRequest& request);
    void stopAnalysis(QUuid session, QUuid requestId);
    void scheduleAnalysis(QUuid session, const AnalysisRequest& request, AnalysisPriority priority,
                          std::optional<QUuid> job = std::nullopt);
    void cancelScheduledAnalysis(QUuid session, QUuid requestId);
    void cancelAnalysisJob(QUuid session, QUuid jobId);
    void cancel();
    void close();
    void configure(QStringList arguments, int startMs, int handshakeMs, qsizetype outputLimit);
    void configureSearch(int idleMs, int responseMs);
    void configureDetection(QStringList directories, bool includeDefaults);
  signals:
    void inspected(QUuid request, const chess_review::engine::PathResult& result);
    void enginesDetected(QUuid request,
                         const QList<chess_review::engine::DetectedEngine>& candidates);
    void executablesChecked(QUuid request, const QList<std::int64_t>& missingIds);
    void progress(QUuid request, const QString& message);
    void completed(QUuid request, const chess_review::engine::ValidationResult& result);
    void runtimeChanged(const chess_review::engine::RuntimeState& state);

    void searchChanged(const chess_review::engine::SearchState& state);
    void analysisUpdated(const chess_review::engine::AnalysisSnapshot& snapshot);
    void analysisFinished(const chess_review::engine::AnalysisResult& result);
    void scheduledChanged(const chess_review::engine::ScheduledState& state);
    void scheduledUpdated(const chess_review::engine::ScheduledEvidence& evidence);
    void scheduledInterrupted(const chess_review::engine::ScheduledEvidence& evidence);
    void scheduledFinished(const chess_review::engine::ScheduledResult& result);

  private:
    struct ScheduledRequest {
        AnalysisRequest request;
        AnalysisPriority priority;
        ScheduledState state;
        std::uint64_t order = 0;
        bool cancelled = false;
        bool preempted = false;
        std::optional<ScheduledEvidence> evidence;
    };
    struct Worker {
        EngineProcessWorker* process = nullptr; // QObject-owned, including while retiring.
        std::optional<ScheduledRequest> active;
        std::optional<AnalysisResult> failedSearch;
        bool ready = false;
        bool retiring = false;
    };
    void dispatchScheduled();
    void considerPreemption();
    void finishSearch(Worker& worker, const AnalysisResult& result);
    void cancelPending(ScheduledOutcome outcome, std::optional<Error> error = std::nullopt);
    void stopScheduling();
    Error unavailable() const;
    std::optional<RuntimeState> runtime_;
    bool schedulingStopped_ = true;
    std::optional<QUuid> directSearch_;
    std::vector<ScheduledRequest> pending_;
    std::set<QUuid> scheduledIds_;
    std::set<QUuid> cancelledJobs_;
    std::uint64_t submissionOrder_ = 0;
    void createWorker(Worker& worker, AttemptMode mode, QUuid request);
    bool workersRunning() const;
    void startRuntimeWorker();
    void onRuntimeChanged(Worker& worker, const RuntimeState& state);
    void drainRuntime(std::optional<Error> error = std::nullopt);
    void workerDestroyed(Worker& worker, QUuid request);
    std::array<Worker, maximumEngineWorkers> workers_;
    ProcessConfiguration configuration_;
    std::optional<ValidationResult> registrationResult_;
    std::optional<Error> runtimeFailure_;
    std::optional<EngineProfile> connectingProfile_;
    std::size_t startedWorkers_ = 0;
    bool draining_ = false;
    bool closing_ = false;
    QStringList detectionDirectories_;
    bool detectionIncludeDefaults_ = true;
};
} // namespace chess_review::engine
