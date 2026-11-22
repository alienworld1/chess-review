#pragma once

#include <optional>

namespace chess_review::engine {
inline constexpr int maximumEngineWorkers = 4;
inline constexpr int maximumPoolThreads = 256;

struct EnginePoolLimits {
    int workers = 1;
    std::optional<int> cpuThreads;
    bool operator==(const EnginePoolLimits&) const = default;
};
} // namespace chess_review::engine
