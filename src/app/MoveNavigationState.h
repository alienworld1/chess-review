#pragma once

#include "chess/GameTree.h"
#include <QString>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace chess_review::app {
struct NavigationTarget {
    std::uint64_t session;
    chess::NodeId node;
};
struct NavigationEntry {
    chess::NodeId id;
    std::optional<chess::NodeId> parent;
    std::vector<chess::NodeId> children;
    QString san;
    QString label;
    std::uint32_t number;
    chess::Color side;
    std::size_t accessibleIndex = 0;
};
struct NavigationLine {
    std::vector<chess::NodeId> moves;
    std::optional<std::size_t> parent;
    std::vector<std::size_t> children;
    std::size_t row = 0;
    std::size_t depth = 0;
    bool mainline = false;
};
struct NavigationTree {
    std::map<chess::NodeId, NavigationEntry> nodes;
    std::vector<NavigationLine> lines;
    std::vector<std::size_t> roots;
    std::map<chess::NodeId, std::size_t> lineForNode;
    std::vector<chess::NodeId> accessibleOrder;
};

// Builds an owned, immutable presentation snapshot. Call off the GUI thread for large trees.
[[nodiscard]] std::shared_ptr<const NavigationTree> makeNavigationTree(const chess::GameTree& tree);
} // namespace chess_review::app
