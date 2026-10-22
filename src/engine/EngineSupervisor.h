#pragma once
#include "engine/EngineProfile.h"
#include <QObject>
#include <QUuid>
#include <memory>

namespace chess_review::engine {
class ValidatorTestAccess;
enum class RuntimePhase {
    Disconnected,
    CheckingExecutable,
    Starting,
    Handshaking,
    Ready,
    Disconnecting,
    Failed
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
    void cancel();
    // Use a fresh non-null session ID per attempt. Accepted sessions last until disconnect,
    // failure or close; Failed/Disconnected arrive after child cleanup. No searches or overrides.
    void connectEngine(QUuid session, const EngineProfile& profile);
    void disconnectEngine(QUuid session);
    void close();
  signals:
    void inspected(QUuid request, const chess_review::engine::PathResult& result);
    void progress(QUuid request, const QString& message);
    void completed(QUuid request, const chess_review::engine::ValidationResult& result);
    void runtimeChanged(const chess_review::engine::RuntimeState& state);
    void closed();

  private:
    void finishClose();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class ValidatorTestAccess;
    void setTestArguments(QStringList arguments, int startMs, int handshakeMs,
                          qsizetype outputLimit);
};
} // namespace chess_review::engine
Q_DECLARE_METATYPE(chess_review::engine::RuntimeState)
