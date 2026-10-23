#pragma once
#include "engine/private/EngineProcessWorker.h"
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
    void validate(QUuid request, const QString& path);
    void connectEngine(QUuid session, const EngineProfile& profile);
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
  signals:
    void inspected(QUuid request, const chess_review::engine::PathResult& result);
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
    void dispatchScheduled();
    void considerPreemption();
    void finishSearch(const AnalysisResult& result);
    void cancelPending(ScheduledOutcome outcome, std::optional<Error> error = std::nullopt);
    void stopScheduling();
    Error unavailable() const;
    std::optional<RuntimeState> runtime_;
    bool schedulingStopped_ = true;
    std::optional<QUuid> directSearch_;
    std::vector<ScheduledRequest> pending_;
    std::optional<ScheduledRequest> active_;
    std::set<QUuid> scheduledIds_;
    std::set<QUuid> cancelledJobs_;
    std::uint64_t submissionOrder_ = 0;
    void createWorker(AttemptMode mode, QUuid request);
    EngineProcessWorker* worker_ = nullptr; // QObject-owned, including while retiring.
    ProcessConfiguration configuration_;
    std::variant<std::monostate, ValidationResult, RuntimeState> result_;
    std::optional<AnalysisResult> failedSearch_;
    bool retiring_ = false;
    bool closing_ = false;
};
} // namespace chess_review::engine
