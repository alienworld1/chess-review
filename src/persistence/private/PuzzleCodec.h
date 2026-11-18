#pragma once
#include "persistence/PuzzleRepository.h"
#include <QByteArray>
#include <optional>

namespace chess_review::persistence::detail {
inline constexpr qsizetype maxPuzzleBytes = 16 * 1024 * 1024;
inline constexpr qsizetype maxPuzzleJobBytes = 32 * 1024 * 1024;
QByteArray encodePuzzle(const PuzzleDocument& document);
std::optional<PuzzleDocument> decodePuzzle(const QByteArray& bytes);
QByteArray encodePuzzleJob(const PuzzleJob& job);
std::optional<PuzzleJob> decodePuzzleJob(const QByteArray& bytes);
QByteArray puzzleSolution(const puzzles::Solution& solution);
} // namespace chess_review::persistence::detail
