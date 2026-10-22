#pragma once
#include "engine/private/EngineProcessWorker.h"
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

  private:
    Error unavailable() const;
    void createWorker(AttemptMode mode, QUuid request);
    EngineProcessWorker* worker_ = nullptr; // QObject-owned, including while retiring.
    ProcessConfiguration configuration_;
    std::variant<std::monostate, ValidationResult, RuntimeState> result_;
    std::optional<AnalysisResult> failedSearch_;
    bool retiring_ = false;
    bool closing_ = false;
};
} // namespace chess_review::engine
