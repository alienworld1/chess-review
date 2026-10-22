#pragma once
#include "engine/EngineSupervisor.h"
#include "persistence/Database.h"
#include <QObject>
#include <cstdint>
#include <vector>

namespace chess_review::diagnostics {
class Logger;
}
namespace chess_review::app {
enum class EngineRegistrationState { Idle, Inspecting, Selected, Validating, Saving, Closing };
struct EngineSettingsState {
    EngineRegistrationState registration = EngineRegistrationState::Idle;
    QString selectedPath;
    QString message;
    QString listError;
    std::vector<engine::EngineProfile> profiles;
    std::uint64_t profilesRevision = 0;
    std::int64_t offset = 0;
    bool ready = false;
    bool listing = false;
    bool hasNext = false;
    engine::RuntimeState runtime;
    QString connectedDisplayName;
};
class EngineControllerTestAccess;
class EngineSettingsController final : public QObject {
    Q_OBJECT
  public:
    explicit EngineSettingsController(persistence::Database& database,
                                      engine::EngineSupervisor& supervisor,
                                      diagnostics::Logger* logger = nullptr,
                                      QObject* parent = nullptr);
    const EngineSettingsState& state() const { return state_; }
    void setReady(bool ready);
    void selectExecutable(const QString& path);
    void registerSelected();
    void cancel();
    void refresh(std::int64_t offset = 0);
    void connectEngine(std::int64_t profileId);
    void disconnectEngine();
    void close();
  signals:
    void changed();
    void closed();

  private:
    void finishClose();
    persistence::Database& database_;
    diagnostics::Logger* logger_;          // Outlives closed().
    engine::EngineSupervisor& supervisor_; // Outlives closed().
    EngineSettingsState state_;
    QUuid selection_;
    QUuid registration_;
    QUuid listing_;
    std::int64_t requestedOffset_ = 0;
    bool closing_ = false;
    bool supervisorClosed_ = false;
    bool saving_ = false;
    bool cancelRequested_ = false;
    bool closed_ = false;
    friend class EngineControllerTestAccess;
};
} // namespace chess_review::app
