#pragma once

#include "persistence/GameRepository.h"
#include <QString>
#include <cstddef>
#include <string>

namespace chess_review::input {
struct InputResult {
    std::string bytes;
    persistence::GameSource source;
    QString error;
    std::size_t bytesRead = 0;
    bool cancelled = false;
};
} // namespace chess_review::input
