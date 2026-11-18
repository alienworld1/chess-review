#pragma once
#include "persistence/PuzzleRepository.h"
#include <memory>
#include <optional>
#include <vector>

namespace chess_review::app {
struct PuzzleAlternative {
    std::shared_ptr<const persistence::StoredPuzzle> puzzle;
    std::shared_ptr<const chess::Position> position;
    chess::Move move;
    int playerMoves = 0;
    std::vector<engine::AnalysisResult> passes;
};
struct PuzzleAlternativeResult {
    puzzles::AlternativeGrade grade = puzzles::AlternativeGrade::Unavailable;
    std::optional<puzzles::SearchNeed> next;
    std::optional<puzzles::Solution> continuation;
};
// Worker only. Reuses immutable original decision evidence and independently verifies
// the attempted child and the entire remaining lesson before accepting the move.
[[nodiscard]] PuzzleAlternativeResult evaluatePuzzleAlternative(const PuzzleAlternative& attempt);
} // namespace chess_review::app
