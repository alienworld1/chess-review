#pragma once

#include "app/MoveNavigationState.h"
#include <QMetaType>
#include <QString>
#include <array>
#include <optional>
#include <vector>

namespace chess_review::app {
enum class BoardInputMode { Drag, Click };
enum class BoardStatus { Playing, Check, Checkmate, Stalemate };
struct BoardPreferences {
    static constexpr int defaultAnalysisMultiPv = 3;
    static constexpr int maxAnalysisMultiPv = 256;
    BoardInputMode inputMode = BoardInputMode::Drag;
    bool coordinates = true;
    bool animation = true;
    bool blackAtBottom = false;
    int analysisSplit = 60; // Board percentage, independent of splitter orientation.
    int analysisMultiPv = defaultAnalysisMultiPv;
    int analysisDetailSplit = 48; // Engine pane percentage within the detail splitter.
    std::array<QString, 4> annotationColors{QStringLiteral("#27834b"), QStringLiteral("#c53d49"),
                                            QStringLiteral("#d2a620"), QStringLiteral("#2476ca")};
    bool operator==(const BoardPreferences&) const = default;
};
struct BoardAnnotation {
    chess::Square from;
    chess::Square to; // Equal endpoints denote a square highlight.
    int color = 0;
    bool operator==(const BoardAnnotation&) const = default;
};
struct BoardViewState {
    std::array<std::optional<chess::Piece>, 64> pieces{};
    chess::Color sideToMove = chess::Color::White;
    BoardStatus status = BoardStatus::Playing;
    QString positionDescription;
    bool blackAtBottom = false;
    bool interactive = false;
    std::optional<chess::Square> selection;
    std::optional<chess::Square> checkedKing;
    std::optional<chess::Move> lastMove;
    std::vector<chess::Move> legalMoves;
    std::vector<chess::Move> captureMoves;
    std::vector<BoardAnnotation> annotations;
    std::uint64_t revision = 0;
};
struct HistoryEntry {
    chess::NodeId id;
    QString label;
};
// Build on a worker. Interaction state and user annotations are owned by the controller.
BoardViewState makeAnalysisBoard(const chess::Position& position,
                                 std::optional<chess::Move> lastMove, bool allowMoves);
} // namespace chess_review::app

Q_DECLARE_METATYPE(chess_review::chess::Square)
Q_DECLARE_METATYPE(chess_review::chess::Move)
Q_DECLARE_METATYPE(chess_review::app::BoardAnnotation)

Q_DECLARE_METATYPE(chess_review::app::BoardStatus)
