#pragma once
#include "review/Evidence.h"
#include <array>
#include <cstdint>
namespace chess_review::review {
enum class Quality { Fast, Standard, Thorough };
struct NodeBudgets {
    std::uint64_t quickScanNodes;
    std::uint64_t deepeningNodes;
    std::uint64_t verificationNodes;
    bool operator==(const NodeBudgets&) const = default;
};
struct ClassificationConfig {
    double inaccuracyLoss = 0.05;
    double mistakeLoss = 0.10;
    double blunderLoss = 0.20;
    bool operator==(const ClassificationConfig&) const = default;
};
struct Configuration {
    std::uint32_t version = 1;
    std::array<NodeBudgets, 3> budgets{
        {{50000, 500000, 2000000}, {100000, 1000000, 5000000}, {250000, 3000000, 10000000}}};
    ClassificationConfig classification;
    ConversionConfig conversion;
    Quality quality = Quality::Standard;
    bool operator==(const Configuration&) const = default;
};
} // namespace chess_review::review
