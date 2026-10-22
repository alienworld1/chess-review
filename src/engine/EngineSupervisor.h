#pragma once
#include "engine/Analysis.h"
#include <QObject>
#include <QUuid>
#include <cstdint>
#include <memory>
#include <optional>

namespace chess_review::engine {
class ValidatorTestAccess;
// Stable numeric phase codes in runtime diagnostics; do not renumber.
enum class RuntimePhase {
    Disconnected = 0,
    CheckingExecutable = 1,
    Starting = 2,
    Handshaking = 3,
    Ready = 4,
    Disconnecting = 5,
    Failed = 6
};
constexpr bool runtimeActive(RuntimePhase phase) {
    return phase != RuntimePhase::Disconnected && phase != RuntimePhase::Failed;
}
struct RuntimeState {
    QUuid session;
    std::int64_t profileId = 0;
    RuntimePhase phase = RuntimePhase::Disconnected;
    std::optional<ValidatedEngine> discovery;
    std::optional<Error> error;
};
// Owning-thread API. Await closed() before destruction. No executable starts on construction.
class EngineSupervisor final : public QObject {
    Q_OBJECT
  public:
    explicit EngineSupervisor(QObject* parent = nullptr);
    ~EngineSupervisor() override;
    void inspect(QUuid request, const QString& path);
    void validate(QUuid request, const QString& resolvedPath);
    void cancel(); // Validation only; disconnectEngine() cancels runtime startup.
    // Use a fresh non-null session ID per attempt. Accepted sessions last until disconnect,
    // failure or close; Failed/Disconnected arrive after child cleanup. No profile overrides.
    void connectEngine(QUuid session, const EngineProfile& profile);
    void disconnectEngine(QUuid session);
    // One search at a time. Fresh non-null request IDs; rejections do not change the session.
    // Completed means natural protocol completion, not proof of requested strength/width.
    // Use the evidence statistics/achievedWidth/commonDepth to assess actual coverage.
    void analyze(QUuid session, const AnalysisRequest& request);
    void stopAnalysis(QUuid session, QUuid requestId);
    void close();
  signals:
    void inspected(QUuid request, const chess_review::engine::PathResult& result);
    void progress(QUuid request, const QString& message);
    void completed(QUuid request, const chess_review::engine::ValidationResult& result);
    void runtimeChanged(const chess_review::engine::RuntimeState& state);
    void searchChanged(const chess_review::engine::SearchState& state);
    void analysisUpdated(const chess_review::engine::AnalysisSnapshot& snapshot);
    void analysisFinished(const chess_review::engine::AnalysisResult& result);
    void closed();

  private:
    void finishClose();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class ValidatorTestAccess;
    void setTestSearchDeadlines(int idleMs, int responseMs);
    void setTestArguments(QStringList arguments, int startMs, int handshakeMs,
                          qsizetype outputLimit);
};
} // namespace chess_review::engine
Q_DECLARE_METATYPE(chess_review::engine::RuntimeState)
