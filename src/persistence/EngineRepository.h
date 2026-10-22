#pragma once
#include "engine/EngineProfile.h"
#include <QMetaType>
#include <variant>
#include <vector>
namespace chess_review::persistence {
struct RegisteredEngine {
    engine::EngineProfile profile;
    bool duplicate = false;
};
using RegisterEngineResult = std::variant<RegisteredEngine, engine::Error>;
using ListEnginesResult = std::variant<std::vector<engine::EngineProfile>, engine::Error>;
} // namespace chess_review::persistence
Q_DECLARE_METATYPE(chess_review::persistence::RegisterEngineResult)
Q_DECLARE_METATYPE(chess_review::persistence::ListEnginesResult)
