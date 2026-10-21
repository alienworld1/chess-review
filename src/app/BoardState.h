#pragma once

#include "chess/GameTree.h"
#include <QMetaType>
#include <QString>
#include <array>
#include <optional>
#include <vector>

namespace chess_review::app {
enum class BoardInputMode { Drag, Click };
enum class BoardStatus { Playing, Check, Checkmate, Stalemate };
struct BoardPreferences {
    BoardInputMode inputMode = BoardInputMode::Drag;
    bool coordinates = true;
    bool animation = true;
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
    std::vector<chess::Square> captureTargets;
    std::vector<BoardAnnotation> annotations;
    std::uint64_t revision = 0;
};
struct HistoryEntry {
    chess::NodeId id;
    QString label;
};
} // namespace chess_review::app

Q_DECLARE_METATYPE(chess_review::chess::Square)
Q_DECLARE_METATYPE(chess_review::chess::Move)
Q_DECLARE_METATYPE(chess_review::app::BoardAnnotation)

Q_DECLARE_METATYPE(chess_review::app::BoardStatus)
