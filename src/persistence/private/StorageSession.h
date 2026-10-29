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

// Construct, initialize, and destroy on one storage thread. Owns the write connection.
class StorageSession final {
  public:
    StorageSession();
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
    ListEnginesResult listEngines(std::int64_t offset, int limit);

    InsertGameResult insertGame(const pgn::PgnDocument& document, const GameSource& source);
    LoadGameResult loadGame(GameId id);
    ListGamesResult listGames(std::int64_t offset, int limit);

  private:
    LoadReviewResult loadReviewRecord(ReviewId id, StoredGame* owningGame);
    bool refreshGameValidationCache();
    bool usable_ = true;
    std::optional<std::pair<std::int64_t, std::int64_t>> validationStamp_;
    std::set<GameId> validatedGames_; // At most 256 IDs, never snapshots or source payloads.
    QString connectionName_;
    QSqlDatabase database_;
    std::unique_ptr<QLockFile> lock_;
};

} // namespace chess_review::persistence::detail
