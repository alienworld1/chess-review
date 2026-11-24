#pragma once
#include "review/Summary.h"
namespace chess_review::review {
inline constexpr std::uint32_t retryModelVersion = 1;
inline constexpr std::uint32_t explanationModelVersion = 1;
enum class RetryGrade { Best, Strong, Okay, TryAgain };
enum class RetryIdea { General, Checkmate, Fork, HangingPiece, Promotion, Exchange, Material };
enum class RetryHintLevel { None, Idea, Piece, Destination };
struct RetryHint {
    RetryIdea idea = RetryIdea::General;
    chess::Move bestMove;
    chess::PieceType piece = chess::PieceType::Pawn;
};
struct RetryResult {
    std::optional<RetryGrade> grade;
    std::optional<double> gap;
    bool success() const { return grade == RetryGrade::Best || grade == RetryGrade::Strong; }
};
// Both original pairs are revalidated against the original history-bearing witness.
// Absent child evidence permits grading only candidates demonstrated in both passes.
[[nodiscard]] RetryResult gradeRetry(const chess::Position& root, chess::Move attempted,
                                     const MoveEvidence& deep, const MoveEvidence& verified,
                                     const Configuration& configuration,
                                     const PositionEvidence* deepChild = nullptr,
                                     const PositionEvidence* verifiedChild = nullptr);
// Single-pass preliminary grading from the move's best available evidence stage.
[[nodiscard]] RetryResult gradeRetry(const chess::Position& root, chess::Move attempted,
                                     const MoveEvidence& evidence,
                                     const Configuration& configuration,
                                     const PositionEvidence* child = nullptr);
// Session-only teaching projection. Revalidates both passes using current tactical rules;
// it never changes the captured review interpretation or grading policy.
[[nodiscard]] std::optional<RetryHint> retryHint(const chess::Position& root,
                                                 const MoveEvidence& deep,
                                                 const MoveEvidence& verified,
                                                 const Configuration& configuration);
[[nodiscard]] std::optional<RetryHint> retryHint(const chess::Position& root,
                                                 const MoveEvidence& evidence,
                                                 const Configuration& configuration);
} // namespace chess_review::review
