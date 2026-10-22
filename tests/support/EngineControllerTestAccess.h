#pragma once
#include "app/EngineSettingsController.h"
namespace chess_review::app {
class EngineControllerTestAccess {
  public:
    static engine::EngineSupervisor& validator(EngineSettingsController& controller) {
        return controller.supervisor_;
    }
};
} // namespace chess_review::app
