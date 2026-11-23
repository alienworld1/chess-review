#pragma once
#include "openings/Book.h"
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
    double excellentLoss = 0.02;
    bool operator==(const ClassificationConfig&) const = default;
};
struct RefinementConfig {
    double boundaryMargin = 0.01;
    double maximumDrift = 0.02;
    int multiPv = 3;
    bool operator==(const RefinementConfig&) const = default;
};
struct SpecialLabelConfig {
    double maximumLoss = 0.02;
    double maximumLossDrift = 0.02;
    double alternativeGap = 0.10;
    double missLoss = 0.10;
    double viableScore = 0.45;
    double alreadyWinningScore = 0.90;
    double margin = 0.01;
    int sacrificeValue = 2;
    bool operator==(const SpecialLabelConfig&) const = default;
};
struct Configuration {
    std::uint32_t version = 1;
    std::array<NodeBudgets, 3> budgets{
        {{50000, 150000, 300000}, {50000, 200000, 400000}, {250000, 3000000, 10000000}}};
    ClassificationConfig classification;
    ConversionConfig conversion;
    RefinementConfig refinement;
    SpecialLabelConfig special;
    Quality quality = Quality::Standard;
    openings::BookConfig openings;
    bool operator==(const Configuration&) const = default;
};
} // namespace chess_review::review
