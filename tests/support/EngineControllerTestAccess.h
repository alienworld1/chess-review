#pragma once
#include "app/EngineSettingsController.h"
#include <utility>
namespace chess_review::app {
class EngineControllerTestAccess {
  public:
    static void replaceProfiles(EngineSettingsController& controller,
                                std::vector<engine::EngineProfile> profiles) {
        controller.state_.profiles = std::move(profiles);
        ++controller.state_.profilesRevision;
        emit controller.changed();
    }
    static engine::EngineSupervisor& validator(EngineSettingsController& controller) {
        return controller.supervisor_;
    }
};
} // namespace chess_review::app
