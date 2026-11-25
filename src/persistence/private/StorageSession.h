#pragma once

#include "persistence/Database.h"
#include "persistence/GameRepository.h"
#include "persistence/private/AnalysisCodec.h"
#include "persistence/private/Migrations.h"

#include <QSqlDatabase>

#include <memory>
#include <optional>
#include <set>
#include <utility>

class QLockFile;

namespace chess_review::persistence::detail {

struct EncodedGame;

// Construct, initialize, and destroy on one storage thread. Owns the write connection.
class StorageSession final {
  public:
    explicit StorageSession(diagnostics::Logger* logger = nullptr);
    ~StorageSession();
    StorageSession(const StorageSession&) = delete;
    StorageSession& operator=(const StorageSession&) = delete;

    std::optional<DatabaseError> initialize(const QString& path,
                                            std::span<const Migration> registry,
                                            const Progress& progress, const Observe& observe = {});

    StoreAnalysisResult storeAnalysis(const engine::AnalysisResult& result,
                                      qint64 budget = analysisBudgetBytes,
                                      qint64 recordLimit = maxAnalysisRecords);
    FindAnalysisResult findAnalysis(const engine::CacheQuery& query,
                                    qint64 scanLimit = maxAnalysisRecords);

    LoadReviewContextResult loadReviewContext(GameId gameId, ReviewId reviewId);
    SaveReviewContextResult saveReviewContext(ReviewContext context);
    ListLibraryReviewsResult listLibraryReviews(std::vector<GameId> gameIds);
    ListReviewsResult listInterruptedReviews(std::int64_t offset, int limit);

    CreateReviewResult createReview(const CreateReview& request);
    LoadReviewResult loadReview(ReviewId id);
    ListReviewsResult listReviews(GameId gameId, std::int64_t offset, int limit);
    CheckpointReviewResult checkpointReview(const ReviewCheckpoint& request);
    ContinueReviewResult continueReview(const ContinueReview& request);
    LoadReviewMovesResult deriveReviewMoves(ReviewId id, std::int64_t expectedRevision, int offset,
                                            int limit);
    LoadReviewExportResult loadReviewExport(ReviewId id, std::int64_t revision);
    LoadReviewOverviewResult loadReviewOverview(ReviewId id, std::int64_t expectedRevision,
                                                int keyOffset = 0);
    LoadReviewExplorationResult loadReviewExploration(ReviewId id, std::int64_t revision,
                                                      int index);
    LoadReviewEvidenceResult
    loadReviewEvidence(ReviewId id, int positionIndex,
                       review::EvidenceStage stage = review::EvidenceStage::Quick);
    ListReviewTransitionsResult listReviewTransitions(ReviewId id, std::int64_t offset, int limit);
    std::optional<DatabaseError> interruptReviewJobs(const QString& path);

    RegisterEngineResult registerEngine(const engine::ValidatedEngine& engine);
    EditEngineResult editEngine(std::int64_t id, const EngineProfileEdit& edit);
    ListEnginesResult listEngines(std::int64_t offset, int limit);

    InsertGameResult insertGame(const pgn::PgnDocument& document, const GameSource& source);
    std::vector<InsertGameResult>
    insertGameBatch(std::span<const std::shared_ptr<const pgn::PgnDocument>> documents,
                    const GameSource& source);
    LoadGameResult loadGame(GameId id, bool includeOpening = false);
    ListGamesResult listGames(std::int64_t offset, int limit, const GameLibraryQuery& filter = {});
    LibraryStatisticsResult loadLibraryStatistics();
    GamePreviewResult previewGame(GameId id);
    DeleteGameResult deleteGame(GameId id);
    ResolveGameOpeningResult resolveGameOpening(GameId id);
    ListPuzzlesResult listPuzzles(std::int64_t offset, int limit, std::optional<ReviewId> reviewId);
    LoadPuzzleResult loadPuzzle(PuzzleId id);
    DeletePuzzleResult deletePuzzle(PuzzleId id);
    SavePuzzleJobResult preparePuzzleJob(ReviewId id, config::PuzzleConfiguration settings = {});
    SavePuzzleJobResult checkpointPuzzleJob(PuzzleCheckpoint checkpoint);
    SavePuzzleJobResult loadPuzzleJob(PuzzleJobId id);
    ListPuzzleJobsResult listPuzzleJobs(std::int64_t offset, int limit,
                                        std::optional<ReviewId> reviewId);

  private:
    struct RankedPuzzlePosition {
        int index;
        int priority;
        double loss;
    };
    std::variant<std::vector<RankedPuzzlePosition>, ReviewError>
    rankPuzzlePositions(ReviewId id, std::int64_t revision);
    LoadReviewResult loadReviewRecord(ReviewId id, StoredGame* owningGame);
    bool refreshGameValidationCache();
    std::optional<OpeningResolution> recognizeOpening(const pgn::PgnDocument& document,
                                                      const openings::BookConfig* book = nullptr);
    void readOpeningCache(std::span<GameSummary> games);
    bool writeOpeningCache(GameId id, const OpeningResolution& opening);
    void saveOpeningCache(GameId id, const OpeningResolution& opening);
    InsertGameResult insertGameInTransaction(const EncodedGame& game, const GameSummary& metadata,
                                             const std::optional<OpeningResolution>& opening,
                                             const GameSource& source,
                                             std::vector<GameId>& newlyValidated);
    diagnostics::Logger* logger_;
    bool openingIndexLoaded_ = false;
    bool openingCacheUsable_ = true;
    const openings::OpeningIndex* openingIndex_ = nullptr;
    bool usable_ = true;
    int schemaVersion_ = 0;
    std::optional<std::pair<std::int64_t, std::int64_t>> validationStamp_;
    std::set<GameId> validatedGames_; // At most 256 IDs, never snapshots or source payloads.
    QString connectionName_;
    QSqlDatabase database_;
    std::unique_ptr<QLockFile> lock_;
};

} // namespace chess_review::persistence::detail
