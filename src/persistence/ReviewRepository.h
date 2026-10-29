#pragma once
#include "analysis/ReviewEvidenceAdapter.h"
#include "config/Configuration.h"
#include "persistence/GameRepository.h"
#include "review/Classification.h"
#include "review/Review.h"
#include "review/Summary.h"
#include <QByteArray>
#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>
namespace chess_review::persistence {
using review::ReviewId;
using review::ReviewJobId;
enum class ReviewErrorCode {
    Missing,
    InvalidInput,
    InvalidRecord,
    UnsupportedVersion,
    Conflict,
    Storage,
    Busy,
    Closing,
    Unavailable
};
struct ReviewError {
    ReviewErrorCode code = ReviewErrorCode::Storage;
    QString message; // Safe static diagnostic, never SQL, engine text or paths.
};
struct CreateReview {
    GameId gameId = 0;
    std::int64_t engineProfileId = 0;
    engine::EngineFingerprint fingerprint;
    chess::Color selectedSide = chess::Color::White;
    config::ValidatedConfiguration configuration;
    bool refinement = false;
};
struct ReviewSummary {
    ReviewId id = 0;
    ReviewJobId jobId = 0;
    GameId gameId = 0;
    chess::Color selectedSide = chess::Color::White;
    review::Quality quality = review::Quality::Standard;
    review::JobStatus status = review::JobStatus::Queued;
    std::int64_t revision = 0;
    int completedPositions = 0;
    int totalPositions = 0; // Root plus every played mainline position, including the final one.
    QDateTime createdAt;
    QDateTime updatedAt;
    std::optional<ReviewErrorCode> metadataError = std::nullopt;
};
struct StoredReview {
    ReviewSummary summary;
    CreateReview settings;
    QByteArray mainlineIdentity;
    std::uint32_t evidenceModelVersion = review::evidenceModelVersion;
    std::uint32_t expectedScoreModelVersion = review::expectedScoreModelVersion;
    // Classification metadata is stored separately; legacy settings remain unchanged.
};
struct ReviewCheckpoint {
    ReviewId reviewId = 0;
    std::int64_t expectedRevision = 0;
    review::JobStatus status = review::JobStatus::QuickScanning;
    review::JobFailure failure = review::JobFailure::None;
    // Absent evidence permits status-only checkpoints, including cancellation/failure.
    std::optional<int> positionIndex;
    std::optional<engine::AnalysisResult> analysis;
    review::EvidenceStage stage = review::EvidenceStage::Quick;
    bool terminal = false; // Request local proof at positionIndex; never a caller-supplied outcome.
};
struct ContinueReview {
    ReviewId reviewId = 0;
    std::int64_t expectedRevision = 0;
    engine::EngineFingerprint fingerprint;
    config::ValidatedConfiguration configuration;
};
struct StoredReviewEvidence {
    int positionIndex = 0;
    std::int64_t evidenceId = 0;
    std::optional<engine::AnalysisResult> analysis;
    review::PositionEvidence evidence;
    // Raw score conversion only; sufficiency and terminal proof are interpreted by pairing.
    review::EvaluationResult conversion;
    // Pair for the move ending at positionIndex, absent until both positions exist.
    std::optional<review::MoveEvidence> precedingMove;
};
// Compact page values: no owning position histories or full PVs cross to the GUI.
struct ReviewMove {
    int positionIndex = 0;
    std::uint32_t moveNumber = 0;
    chess::Color mover = chess::Color::White;
    QString san;
    std::optional<review::ClassificationResult> classification;
    std::optional<double> accuracyLoss;
    std::array<std::int64_t, 4> stabilityEvidenceIds{};
    std::array<review::AnalysisStrength, 4> stabilityStrength;
    review::StabilityAssessment stability;
    bool refinementTarget = false;
    review::EvidenceStage stage = review::EvidenceStage::Quick;
    std::optional<int> contextPosition;
    QString bestSan;
    std::vector<QString> alternativeSans;
    review::RawEvaluation rawBefore;
    review::RawEvaluation rawAfter;
    review::AnalysisStrength beforeStrength;
    review::AnalysisStrength afterStrength;
    QString beforeAttempt;
    QString afterAttempt;
    QString beforeSession;
    QString afterSession;
    QString engineIdentity;
    std::int64_t beforeEvidenceId = 0;
    std::int64_t afterEvidenceId = 0;
};
struct ReviewMovePage {
    ReviewId reviewId = 0;
    std::int64_t revision = 0;
    int offset = 0;
    int totalMoves = 0;
    std::vector<ReviewMove> moves;
};
struct ReviewOverview {
    ReviewId reviewId = 0;
    std::int64_t revision = 0;
    QString result;
    std::array<review::SideSummary, 2> sides;
    std::vector<int> refinementPositions;
    int deepenedPositions = 0;
    int verifiedPositions = 0;
    std::vector<ReviewMove> keyMoments; // At most 50 compact entries per page.
    int keyOffset = 0;
    int totalKeyMoments = 0;
    std::vector<int> keyPositions;     // Complete lightweight chronological navigation index.
    std::vector<int> opponentContexts; // Key positions with an attached preceding opponent error.
    std::optional<int> largestLossPosition;
    std::optional<double> largestLoss;
};
// One selected moment only. Shared immutable worker-created values never enter timeline pages.
struct ReviewExploration {
    StoredReview run;
    ReviewMove move;
    int rootPly = 0;
    std::shared_ptr<const chess::Position> before;
    review::PositionEvidence evidence;
    std::optional<review::MoveEvidence> deep;
    std::optional<review::MoveEvidence> verified;
    std::vector<QString> candidateSans;
};
using LoadReviewExplorationResult =
    std::variant<std::shared_ptr<const ReviewExploration>, ReviewError>;
using LoadReviewOverviewResult = std::variant<ReviewOverview, ReviewError>;
using LoadReviewMovesResult = std::variant<ReviewMovePage, ReviewError>;
// Navigation references only; saved review metadata remains authoritative.
struct ReviewContext {
    std::optional<GameId> gameId;
    std::optional<ReviewId> reviewId;
    std::optional<int> position;
    bool reopen = false;
    bool invalidPosition = false; // Recoverable navigation metadata, never evidence.
};
struct LibraryReview {
    GameId gameId = 0;
    bool hasInterrupted = false;
    std::optional<ReviewSummary> run;
    std::optional<ReviewError> error;
};
using LoadReviewContextResult = std::variant<ReviewContext, ReviewError>;
using SaveReviewContextResult = std::variant<std::monostate, ReviewError>;
using ListLibraryReviewsResult = std::variant<std::vector<LibraryReview>, ReviewError>;
struct ReviewTransition {
    std::int64_t revision = 0;
    std::optional<review::JobStatus> previous;
    review::JobStatus status = review::JobStatus::Queued;
    review::JobFailure failure = review::JobFailure::None;
    QDateTime createdAt;
};
using CreateReviewResult = std::variant<ReviewSummary, ReviewError>;
using LoadReviewResult = std::variant<StoredReview, ReviewError>;
using ListReviewsResult = std::variant<std::vector<ReviewSummary>, ReviewError>;
using CheckpointReviewResult = std::variant<ReviewSummary, ReviewError>;
using ContinueReviewResult = std::variant<ReviewSummary, ReviewError>;
using LoadReviewEvidenceResult = std::variant<StoredReviewEvidence, ReviewError>;
using ListReviewTransitionsResult = std::variant<std::vector<ReviewTransition>, ReviewError>;
} // namespace chess_review::persistence
Q_DECLARE_METATYPE(chess_review::persistence::CreateReviewResult)
Q_DECLARE_METATYPE(chess_review::persistence::LoadReviewResult)
Q_DECLARE_METATYPE(chess_review::persistence::ListReviewsResult)
Q_DECLARE_METATYPE(chess_review::persistence::LoadReviewEvidenceResult)
Q_DECLARE_METATYPE(chess_review::persistence::ListReviewTransitionsResult)

Q_DECLARE_METATYPE(chess_review::persistence::LoadReviewMovesResult)

Q_DECLARE_METATYPE(chess_review::persistence::LoadReviewOverviewResult)

Q_DECLARE_METATYPE(chess_review::persistence::LoadReviewContextResult)
Q_DECLARE_METATYPE(chess_review::persistence::SaveReviewContextResult)
Q_DECLARE_METATYPE(chess_review::persistence::ListLibraryReviewsResult)
