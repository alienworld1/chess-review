#pragma once
#include "engine/EngineSupervisor.h"
#include "persistence/Database.h"
#include <QObject>
#include <cstdint>
#include <set>
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
    QString profileMessage;
    QString testMessage;
    QString detectMessage;
    bool editing = false;
    bool testing = false;
    bool analyzing = false;
    bool detecting = false;
    std::vector<engine::EngineProfile> profiles;
    std::set<std::int64_t> missingProfiles;
    QList<engine::DetectedEngine> detectedEngines;
    std::optional<std::int64_t> defaultProfileId;
    std::optional<std::int64_t> relocatingProfileId;
    QString defaultDisplayName;
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
    void relocateProfile(std::int64_t profileId, const QString& path);
    void detectInstalledEngines();
    void registerSelected();
    void cancel();
    void refresh(std::int64_t offset = 0);
    void setPoolLimits(engine::EnginePoolLimits limits) { poolLimits_ = limits; }
    bool connectEngine(std::int64_t profileId);
    void disconnectEngine();
    bool canEditProfile() const;
    void editProfile(std::int64_t profileId, persistence::EngineProfileEdit edit);
    void testEngine(std::int64_t profileId);
    // Stops submissions; composition must then close the shared supervisor.
    void close();
  signals:
    void changed();
    void activityChanged();
    void closed();

  private:
    void finishClose();
    persistence::Database& database_;
    diagnostics::Logger* logger_;          // Outlives closed().
    engine::EngineSupervisor& supervisor_; // Outlives closed().
    EngineSettingsState state_;
    engine::EnginePoolLimits poolLimits_;
    QUuid selection_;
    QUuid registration_;
    QUuid listing_;
    QUuid edit_;
    QUuid test_;
    QUuid detection_;
    QUuid executableCheck_;
    std::optional<std::int64_t> pendingRelocationId_;
    std::set<QUuid> analysisAttempts_;
    std::int64_t requestedOffset_ = 0;
    bool closing_ = false;
    bool supervisorClosed_ = false;
    bool saving_ = false;
    bool cancelRequested_ = false;
    bool closed_ = false;
    friend class EngineControllerTestAccess;
};
} // namespace chess_review::app
