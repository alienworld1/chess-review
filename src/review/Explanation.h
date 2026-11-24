#pragma once
#include "review/Classification.h"
#include "review/Intelligence.h"
#include <span>

namespace chess_review::review {

enum class ExplanationKind {
    Generic,
    // Positive / neutral move themes
    DeliversCheckmate,
    LeadsToMate,
    PromotesPawn,
    ForksPieces,
    CapturesFreePiece,
    Recaptures,
    WinsExchangeImmediate,
    WinsMaterialImmediate,
    WinsExchangeInLine,
    WinsMaterialInLine,
    CreatesPin,
    SavesPiece,
    DefendsPiece,
    CreatesThreat,
    CreatesDiscoveredAttack,
    Castles,
    TradesPieces,
    EscapesCheck,
    GivesCheck,
    DevelopsPiece,
    // Error / miss themes
    AllowsMate,
    MissesMate,
    HangsPiece,
    FailsToSavePiece,
    LosesPieceImmediate,
    AllowsExchangeImmediate,
    AllowsFork,
    AllowsPromotion,
    MissesFreePiece,
    MissesFork,
    MissesPromotion,
    LosesExchangeInLine,
    LosesMaterialInLine,
    MissesExchangeInLine,
    MissesMaterialInLine
};

struct MoveExplanation {
    ExplanationKind kind = ExplanationKind::Generic;
    chess::Color beneficiary = chess::Color::White;
    chess::PieceType actor = chess::PieceType::Pawn;
    std::optional<chess::PieceType> targetPiece;
    std::optional<chess::PieceType> secondaryPiece;
    std::optional<chess::Square> square;
    std::optional<chess::Square> secondarySquare;
    int material = 0;
    bool kingside = false;
    bool central = false;
    bool undefended = false;
    bool checksKing = false;
    bool blocksCheck = false;
    bool operator==(const MoveExplanation&) const = default;
};

[[nodiscard]] MoveExplanation explainMove(const chess::Position& before, const MoveEvidence& pair,
                                          const ClassificationResult& classification,
                                          std::span<const TacticalFact> tacticalFacts);

} // namespace chess_review::review
