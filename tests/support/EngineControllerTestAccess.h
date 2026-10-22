#pragma once
#include "app/EngineSettingsController.h"
namespace chess_review::app {
class EngineControllerTestAccess {
  public:
    static engine::ExecutableValidator& validator(EngineSettingsController& controller) {
        return controller.validator_;
    }
};
} // namespace chess_review::app
