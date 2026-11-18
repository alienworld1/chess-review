#pragma once
#include "persistence/ReviewRepository.h"
#include "puzzles/Puzzle.h"
#include <QDateTime>
#include <QMetaType>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace chess_review::persistence {
using PuzzleId = std::int64_t;
using PuzzleJobId = std::int64_t;
enum class PuzzleJobStatus { Running, Paused, Interrupted, Complete, Failed };
struct PuzzleDocument {
    puzzles::Definition definition;
    engine::EngineFingerprint fingerprint; // Base MultiPV=1; each pass has its effective width.
    std::int64_t engineProfileId = 0;
    std::optional<GameId> gameId;
    std::optional<ReviewId> reviewId;
    std::optional<int> sourcePosition;
    std::vector<engine::AnalysisResult> passes;
    puzzles::Solution solution;
};
struct StoredPuzzle {
    PuzzleId id = 0;
    QDateTime createdAt;
    PuzzleDocument document;
};
struct PuzzleSummary {
    PuzzleId id = 0;
    puzzles::Origin origin = puzzles::Origin::Manual;
    std::optional<GameId> gameId;
    std::optional<ReviewId> reviewId;
    QDateTime createdAt;
};
struct PuzzlePage {
    std::int64_t offset = 0;
    bool hasNext = false;
    std::vector<PuzzleSummary> puzzles;
};
struct PuzzleJob {
    PuzzleJobId id = 0;
    std::int64_t revision = 0;
    std::optional<ReviewId> reviewId;
    PuzzleJobStatus status = PuzzleJobStatus::Running;
    std::vector<PuzzleDocument> candidates;
    int current = 0;
    std::vector<PuzzleId> accepted;
    bool reusedExisting = false;
    std::vector<PuzzleId> available; // Read projection of still-present accepted puzzles.
};
struct PuzzleCheckpoint {
    PuzzleJob job;
    // The completed current candidate is saved atomically with the advanced checkpoint.
    std::optional<PuzzleDocument> accepted;
};
struct PuzzleJobSummary {
    PuzzleJobId id = 0;
    std::optional<ReviewId> reviewId;
    PuzzleJobStatus status = PuzzleJobStatus::Running;
    int current = 0;
    int total = 0;
    int saved = 0;
};
using LoadPuzzleResult = std::variant<std::shared_ptr<const StoredPuzzle>, ReviewError>;
using ListPuzzlesResult = std::variant<PuzzlePage, ReviewError>;
using SavePuzzleJobResult = std::variant<PuzzleJob, ReviewError>;
using ListPuzzleJobsResult = std::variant<std::vector<PuzzleJobSummary>, ReviewError>;
using DeletePuzzleResult = std::variant<std::monostate, ReviewError>;
// CPU-only reconstruction/interpretation. Used on workers by both storage and application.
[[nodiscard]] std::variant<puzzles::Verification, ReviewError>
verifyPuzzle(const PuzzleDocument& document);
} // namespace chess_review::persistence

Q_DECLARE_METATYPE(chess_review::persistence::LoadPuzzleResult)
Q_DECLARE_METATYPE(chess_review::persistence::ListPuzzlesResult)
Q_DECLARE_METATYPE(chess_review::persistence::SavePuzzleJobResult)
Q_DECLARE_METATYPE(chess_review::persistence::ListPuzzleJobsResult)
