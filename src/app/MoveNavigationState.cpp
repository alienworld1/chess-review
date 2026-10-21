#include "app/MoveNavigationState.h"
#include <QCoreApplication>
#include <string>

namespace chess_review::app {
namespace {
QString moveLabel(std::uint32_t number, chess::Color side, const std::string& san) {
    return QStringLiteral("%1%2 %3").arg(number).arg(
        side == chess::Color::White ? QStringLiteral(".") : QStringLiteral("..."),
        QString::fromStdString(san));
}
} // namespace
std::shared_ptr<const NavigationTree> makeNavigationTree(const chess::GameTree& tree) {
    auto navigation = std::make_shared<NavigationTree>();
    const auto rootResult = tree.position(tree.rootId());
    const auto& root = std::get<chess::Position>(rootResult);
    struct Visit {
        chess::NodeId id;
        std::uint32_t number;
        chess::Color side;
    };
    std::vector<Visit> remaining{{tree.rootId(), root.fullmoveNumber(), root.sideToMove()}};
    while (!remaining.empty()) {
        const auto visit = remaining.back();
        remaining.pop_back();
        const auto item = *tree.node(visit.id);
        navigation->nodes.emplace(
            item.id, NavigationEntry{item.id, item.parent, item.children,
                                     item.san ? QString::fromStdString(*item.san) : QString{},
                                     item.san ? moveLabel(visit.number, visit.side, *item.san)
                                              : QCoreApplication::translate("AnalysisController",
                                                                            "Starting position"),
                                     visit.number, visit.side});
        const auto nextSide =
            item.parent
                ? (visit.side == chess::Color::White ? chess::Color::Black : chess::Color::White)
                : visit.side;
        const auto nextNumber =
            visit.number + (item.parent && visit.side == chess::Color::Black ? 1U : 0U);
        for (auto child = item.children.rbegin(); child != item.children.rend(); ++child) {
            remaining.push_back({*child, nextNumber, nextSide});
        }
    }
    struct LineStart {
        chess::NodeId node;
        std::optional<std::size_t> parent;
        bool mainline;
    };
    std::vector<LineStart> starts{{tree.rootId(), std::nullopt, true}};
    for (std::size_t task = 0; task < starts.size(); ++task) {
        auto cursor = std::optional{starts[task].node};
        const auto parent = starts[task].parent;
        const auto mainline = starts[task].mainline;
        while (cursor) {
            const auto lineIndex = navigation->lines.size();
            auto& siblings = parent ? navigation->lines[*parent].children : navigation->roots;
            const auto row = siblings.size();
            siblings.push_back(lineIndex);
            NavigationLine line;
            line.parent = parent;
            line.row = row;
            line.depth = parent ? navigation->lines[*parent].depth + 1 : 0;
            line.mainline = mainline;
            // Bounded line segments keep hit testing/layout work local.
            const auto capacity = *cursor == tree.rootId() ? 1U : 8U;
            while (cursor && line.moves.size() < capacity) {
                const auto& entry = navigation->nodes.at(*cursor);
                line.moves.push_back(*cursor);
                navigation->nodes.at(*cursor).accessibleIndex = navigation->accessibleOrder.size();
                navigation->accessibleOrder.push_back(*cursor);
                navigation->lineForNode.emplace(*cursor, lineIndex);
                for (std::size_t i = 1; i < entry.children.size(); ++i) {
                    starts.push_back({entry.children[i], lineIndex, false});
                }
                if (entry.children.empty()) {
                    cursor.reset();
                } else {
                    cursor = entry.children.front();
                }
            }
            navigation->lines.push_back(std::move(line));
        }
    }
    return navigation;
}
} // namespace chess_review::app
