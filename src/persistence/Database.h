#pragma once

#include "persistence/AnalysisRepository.h"
#include "persistence/EngineRepository.h"
#include "persistence/GameRepository.h"
#include "persistence/ReviewRepository.h"
#include <QMetaType>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

namespace chess_review::diagnostics {
class Logger;
}

namespace chess_review::persistence {

// Values are stable diagnostic identifiers; do not renumber.
enum class DatabaseErrorCode {
    Path = 0,
    Locked = 1,
    Open = 2,
    Configuration = 3,
    Schema = 4,
    Backup = 5,
    Migration = 6
};

struct DatabaseError {
    DatabaseErrorCode code{};
    QString operation;
    QString detail;
    QString nativeCode;
    QString databasePath;
    QString backupPath;
    std::optional<int> migrationVersion;
};

// Call on the owning (application) thread. One initialization per instance.
// The owner must call close() and await closed() before destroying this service.
// SQL connections and all filesystem/database work belong to the storage thread.
class Database final : public QObject {
    Q_OBJECT

  public:
    explicit Database(QObject* parent = nullptr);
    // The injected logger must outlive closed(); a default instance has no logging.
    explicit Database(diagnostics::Logger& logger, QObject* parent = nullptr);
    ~Database() override;

    void initialize(const QString& databasePath);
    void close();
    // Repository calls are asynchronous, on this object's owning thread. IDs are caller-owned.
    // At most 16 accepted repository requests may be pending; otherwise returns Busy.
    // Completed attempts only. Cache calls share repository admission and shutdown drainage.
    // Results retain original engine identities; request only correlates the storage operation.
    void storeAnalysis(RequestId request, engine::AnalysisResult result);
    void findAnalysis(RequestId request, engine::CacheQuery query);
    void registerEngine(RequestId request, engine::ValidatedEngine engine);
    void listEngines(RequestId request, std::int64_t offset = 0, int limit = 50);
    void insertGame(RequestId request, std::shared_ptr<const pgn::PgnDocument> document,
                    GameSource source = {});
    void loadGame(RequestId request, GameId id);
    void listGames(RequestId request, std::int64_t offset = 0, int limit = 50);

    // Durable review operations share repository admission and accepted-write drainage.
    void createReview(RequestId requestId, CreateReview request);
    void loadReview(RequestId requestId, ReviewId id);
    void listReviews(RequestId requestId, GameId gameId, std::int64_t offset = 0, int limit = 50);
    void checkpointReview(RequestId requestId, ReviewCheckpoint checkpoint);
    void continueReview(RequestId requestId, ContinueReview continuation);
    void deriveReviewMoves(RequestId requestId, ReviewId id, std::int64_t expectedRevision,
                           int offset, int limit = 50);
    void loadReviewEvidence(RequestId requestId, ReviewId id, int positionIndex);
    void listReviewTransitions(RequestId requestId, ReviewId id, std::int64_t offset = 0,
                               int limit = 50);

  signals:
    void reviewCreated(chess_review::persistence::RequestId requestId,
                       const chess_review::persistence::CreateReviewResult& result);
    void reviewLoaded(chess_review::persistence::RequestId requestId,
                      const chess_review::persistence::LoadReviewResult& result);
    void reviewsListed(chess_review::persistence::RequestId requestId,
                       const chess_review::persistence::ListReviewsResult& result);
    void reviewCheckpointed(chess_review::persistence::RequestId requestId,
                            const chess_review::persistence::CheckpointReviewResult& result);
    void reviewContinued(chess_review::persistence::RequestId requestId,
                         const chess_review::persistence::ContinueReviewResult& result);
    void reviewMovesLoaded(chess_review::persistence::RequestId requestId,
                           const chess_review::persistence::LoadReviewMovesResult& result);
    void reviewEvidenceLoaded(chess_review::persistence::RequestId requestId,
                              const chess_review::persistence::LoadReviewEvidenceResult& result);
    void
    reviewTransitionsListed(chess_review::persistence::RequestId requestId,
                            const chess_review::persistence::ListReviewTransitionsResult& result);

    void analysisStored(chess_review::persistence::RequestId request,
                        const chess_review::persistence::StoreAnalysisResult& result);
    void analysisFound(chess_review::persistence::RequestId request,
                       const chess_review::persistence::FindAnalysisResult& result);
    void engineRegistered(chess_review::persistence::RequestId request,
                          const chess_review::persistence::RegisterEngineResult& result);
    void enginesListed(chess_review::persistence::RequestId request,
                       const chess_review::persistence::ListEnginesResult& result);
    void progress(const QString& message);
    void ready(int schemaVersion);
    void failed(const chess_review::persistence::DatabaseError& error);
    void closed();
    void gameInserted(chess_review::persistence::RequestId request,
                      const chess_review::persistence::InsertGameResult& result);
    void gameLoaded(chess_review::persistence::RequestId request,
                    const chess_review::persistence::LoadGameResult& result);
    void gamesListed(chess_review::persistence::RequestId request,
                     const chess_review::persistence::ListGamesResult& result);

  private:
    void finishClose();
    std::optional<GameError> acceptRequest();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chess_review::persistence

Q_DECLARE_METATYPE(chess_review::persistence::DatabaseError)
