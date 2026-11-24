#include "engine/AnalysisCache.h"
#include "persistence/private/PuzzleCodec.h"
#include "persistence/private/ReviewCodec.h"
#include "persistence/private/StorageSession.h"
#include <QCryptographicHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <limits>

namespace chess_review::persistence::detail {
namespace {
ReviewError error(ReviewErrorCode code = ReviewErrorCode::Storage) {
    return {code, code == ReviewErrorCode::InvalidRecord
                      ? QStringLiteral("Saved puzzle data is damaged or unsupported.")
                  : code == ReviewErrorCode::Conflict
                      ? QStringLiteral("Puzzle generation changed; reload before continuing.")
                      : QStringLiteral("Puzzle storage could not complete the operation.")};
}
bool exec(QSqlQuery& query, const QString& sql, const QVariantList& args = {}) {
    if (!query.prepare(sql)) {
        return false;
    }
    for (const auto& arg : args) {
        query.addBindValue(arg);
    }
    return query.exec();
}
QVariant optionalId(std::optional<std::int64_t> id) {
    return id ? QVariant::fromValue(static_cast<qlonglong>(*id))
              : QVariant(QMetaType::fromType<qlonglong>());
}
class Transaction final {
  public:
    Transaction(QSqlDatabase& db, bool& usable) : db_(db), usable_(usable) {}
    ~Transaction() {
        if (active_ && !db_.rollback()) {
            usable_ = false;
        }
    }
    bool begin() {
        QSqlQuery q(db_);
        active_ = q.exec(QStringLiteral("BEGIN IMMEDIATE"));
        return active_;
    }
    bool commit() {
        if (!db_.commit()) {
            return false;
        }
        active_ = false;
        return true;
    }

  private:
    QSqlDatabase& db_;
    bool& usable_;
    bool active_ = false;
};
const QString puzzleProjection = QStringLiteral(
    "SELECT id,substr(CAST(position_key AS BLOB),1,33),substr(CAST(position_identity AS "
    "BLOB),1,1048577),"
    "source_game_id,source_review_id,origin,substr(CAST(document AS BLOB),1,16777217),"
    "substr(CAST(created_at AS TEXT),1,65),typeof(document)='blob' AND typeof(position_key)='blob' "
    "AND typeof(position_identity)='blob' AND typeof(id)='integer' AND typeof(origin)='integer' "
    "AND typeof(created_at)='text' AND (source_game_id IS NULL OR "
    "typeof(source_game_id)='integer') "
    "AND (source_review_id IS NULL OR typeof(source_review_id)='integer') FROM puzzles ");
std::optional<StoredPuzzle> readPuzzle(const QSqlQuery& q) {
    auto document = decodePuzzle(q.value(6).toByteArray());
    const auto created = QDateTime::fromString(q.value(7).toString(), Qt::ISODateWithMs);
    if (!document || document->solution.nodes.empty() || !created.isValid() ||
        !q.value(8).toBool() || q.value(0).toLongLong() <= 0 ||
        (!q.value(3).isNull() &&
         (!document->gameId || q.value(3).toLongLong() != *document->gameId)) ||
        (!q.value(4).isNull() &&
         (!document->reviewId || q.value(4).toLongLong() != *document->reviewId)) ||
        q.value(5).toInt() != static_cast<int>(document->definition.origin)) {
        return {};
    }
    const auto identity = positionIdentity(document->definition.position);
    if (q.value(2).toByteArray() != identity ||
        q.value(1).toByteArray() !=
            QCryptographicHash::hash(identity, QCryptographicHash::Sha256)) {
        return {};
    }
    return StoredPuzzle{q.value(0).toLongLong(), created, std::move(*document)};
}
const QString jobProjection = QStringLiteral(
    "SELECT id,source_review_id,revision,status,current_position,candidate_count,saved_count,"
    "substr(CAST(document AS BLOB),1,33554433),typeof(document)='blob' AND typeof(id)='integer' "
    "AND typeof(revision)='integer' AND typeof(status)='integer' AND "
    "typeof(current_position)='integer' "
    "AND typeof(candidate_count)='integer' AND typeof(saved_count)='integer' "
    "AND (source_review_id IS NULL OR typeof(source_review_id)='integer') FROM puzzle_jobs ");
std::optional<PuzzleJob> readJob(const QSqlQuery& q) {
    auto job = decodePuzzleJob(q.value(7).toByteArray());
    const auto id = q.value(0).toLongLong();
    const auto revision = q.value(2).toLongLong();
    const int status = q.value(3).toInt();
    if (!job || !q.value(8).toBool() || id <= 0 || revision < 0 || status < 0 || status > 4 ||
        job->current != q.value(4).toInt() ||
        static_cast<int>(job->candidates.size()) != q.value(5).toInt() ||
        static_cast<int>(job->accepted.size()) != q.value(6).toInt()) {
        return {};
    }
    const std::optional<ReviewId> source =
        q.value(1).isNull() ? std::nullopt : std::optional<ReviewId>(q.value(1).toLongLong());
    if (!job->candidates.empty() && source != job->reviewId) {
        return {};
    }
    job->id = id;
    job->revision = revision;
    job->reviewId = source;
    job->status = static_cast<PuzzleJobStatus>(status);
    return job;
}
bool findAvailablePuzzles(QSqlDatabase& db, PuzzleJob& job) {
    job.available.clear();
    if (job.accepted.empty()) {
        return true;
    }
    QSqlQuery query(db);
    for (const auto& candidate : job.candidates) {
        if (candidate.solution.nodes.empty()) {
            continue;
        }
        const auto identity = positionIdentity(candidate.definition.position);
        if (!exec(query, QStringLiteral("SELECT id FROM puzzles WHERE position_identity=?"),
                  {identity})) {
            return false;
        }
        if (query.next()) {
            const auto id = query.value(0).toLongLong();
            if (std::ranges::find(job.accepted, id) != job.accepted.end()) {
                job.available.push_back(id);
            }
        } else if (query.lastError().isValid()) {
            return false;
        }
        query.finish();
    }
    return true;
}
struct PuzzleSource {
    chess::Position before;
    ReviewMove move;
    std::optional<review::MoveEvidence> older;
    review::MoveEvidence latest;
};
std::variant<review::MoveEvidence, ReviewError>
loadStagePair(StorageSession& storage, const StoredReview& run, int index,
              review::EvidenceStage stage, std::optional<chess::Position>& root) {
    const auto b = storage.loadReviewEvidence(run.summary.id, index - 1, stage);
    const auto a = storage.loadReviewEvidence(run.summary.id, index, stage);
    const auto* before = std::get_if<StoredReviewEvidence>(&b);
    const auto* after = std::get_if<StoredReviewEvidence>(&a);
    if (!before || !after) {
        return error(ReviewErrorCode::Unavailable);
    }
    auto restored = engine::restorePosition(before->evidence.position);
    const auto* position = std::get_if<chess::Position>(&restored);
    if (!position || after->evidence.position.moves.empty()) {
        return error(ReviewErrorCode::InvalidRecord);
    }
    root = *position;
    auto old = before->evidence;
    auto next = after->evidence;
    // Storage has qualified each exact effective fingerprint; normalize only
    // MultiPV width for interpreting a before/after pair, as review itself does.
    if (!old.terminal) {
        old.engineIdentity = run.settings.fingerprint.digest.toHex().toStdString();
    }
    if (!next.terminal) {
        next.engineIdentity = run.settings.fingerprint.digest.toHex().toStdString();
    }
    auto paired =
        review::pairEvidence(*position, after->evidence.position.moves.back(), std::move(old),
                             std::move(next), run.settings.configuration.values.conversion);
    if (paired.issue && paired.issue != review::EvidenceIssue::MateClaim) {
        return error(ReviewErrorCode::Unavailable);
    }
    return paired;
}
std::variant<PuzzleSource, ReviewError> readSource(StorageSession& storage, const StoredReview& run,
                                                   int index) {
    const auto pageResult =
        storage.deriveReviewMoves(run.summary.id, run.summary.revision, index - 1, 1);
    const auto* page = std::get_if<ReviewMovePage>(&pageResult);
    if (!page || page->moves.size() != 1) {
        return page ? error(ReviewErrorCode::InvalidRecord) : std::get<ReviewError>(pageResult);
    }
    const auto& move = page->moves.front();
    std::optional<chess::Position> root;
    if (move.stage >= review::EvidenceStage::Verification) {
        auto olderResult = loadStagePair(storage, run, index, move.comparisonStages[0], root);
        auto latestResult = loadStagePair(storage, run, index, move.comparisonStages[1], root);
        const auto* olderPair = std::get_if<review::MoveEvidence>(&olderResult);
        const auto* latestPair = std::get_if<review::MoveEvidence>(&latestResult);
        if (olderPair && latestPair && root) {
            return PuzzleSource{std::move(*root), move, *olderPair, *latestPair};
        }
        if (const auto* err = std::get_if<ReviewError>(&olderResult);
            err && err->code != ReviewErrorCode::Unavailable) {
            return *err;
        }
        if (const auto* err = std::get_if<ReviewError>(&latestResult);
            err && err->code != ReviewErrorCode::Unavailable) {
            return *err;
        }
    }
    auto singleResult = loadStagePair(storage, run, index, move.stage, root);
    if (const auto* err = std::get_if<ReviewError>(&singleResult)) {
        return *err;
    }
    if (!root) {
        return error(ReviewErrorCode::InvalidRecord);
    }
    return PuzzleSource{std::move(*root), move, std::nullopt,
                        std::get<review::MoveEvidence>(std::move(singleResult))};
}
bool missedMate(const PuzzleSource& e) {
    const auto missed = [](const review::MoveEvidence& p) {
        const auto& before = p.before.evaluation.score;
        const auto& after = p.after.evaluation.score;
        return before && before->bound == review::Bound::Exact &&
               before->kind == review::ScoreKind::Mate && before->value > 0 &&
               (p.after.terminal == review::TerminalOutcome::Stalemate ||
                (after && after->bound == review::Bound::Exact &&
                 !(after->kind == review::ScoreKind::Mate && after->value < 0)));
    };
    if (e.older) {
        return e.older->before.attemptIdentity != e.latest.before.attemptIdentity &&
               missed(*e.older) && missed(e.latest);
    }
    return missed(e.latest);
}
puzzles::CandidateContext candidateContextFor(const PuzzleSource& e,
                                              const review::Configuration& reviewConfig) {
    const auto label = e.move.classification ? e.move.classification->label : std::nullopt;
    const bool isError = label && review::isError(*label) && e.move.classification->loss &&
                         *e.move.classification->loss >= reviewConfig.classification.inaccuracyLoss;
    const bool isSpecial = label && (*label == review::Classification::Brilliant ||
                                     *label == review::Classification::Great);
    const bool opponentBlundered =
        e.move.quickScan.opportunityLoss &&
        *e.move.quickScan.opportunityLoss >= reviewConfig.classification.inaccuracyLoss;
    return {isError, isSpecial, opponentBlundered};
}
} // namespace
ListPuzzlesResult StorageSession::listPuzzles(std::int64_t offset, int limit,
                                              std::optional<ReviewId> reviewId) {
    if (!usable_) {
        return error();
    }
    if (offset < 0 || limit < 1 || limit > 50 || (reviewId && *reviewId <= 0)) {
        return error(ReviewErrorCode::InvalidInput);
    }
    QSqlQuery q(database_);
    QVariantList args;
    QString sql = QStringLiteral(
        "SELECT id,origin,source_game_id,source_review_id,substr(CAST(created_at AS TEXT),1,65), "
        "typeof(id)='integer' AND typeof(origin)='integer' AND typeof(created_at)='text' "
        "AND (source_game_id IS NULL OR typeof(source_game_id)='integer') "
        "AND (source_review_id IS NULL OR typeof(source_review_id)='integer') FROM puzzles ");
    if (reviewId) {
        sql += QStringLiteral("WHERE source_review_id=? ");
        args.append(static_cast<qlonglong>(*reviewId));
    }
    sql += QStringLiteral("ORDER BY id DESC LIMIT ? OFFSET ?");
    args.append(limit + 1);
    args.append(static_cast<qlonglong>(offset));
    if (!exec(q, sql, args)) {
        return error();
    }
    PuzzlePage page;
    page.offset = offset;
    while (q.next()) {
        if (page.puzzles.size() == static_cast<std::size_t>(limit)) {
            page.hasNext = true;
            break;
        }
        const auto created = QDateTime::fromString(q.value(4).toString(), Qt::ISODateWithMs);
        const int origin = q.value(1).toInt();
        if (!q.value(5).toBool() || !created.isValid() || origin < 0 || origin > 1 ||
            q.value(0).toLongLong() <= 0) {
            return error(ReviewErrorCode::InvalidRecord);
        }
        page.puzzles.push_back(
            {q.value(0).toLongLong(), static_cast<puzzles::Origin>(origin),
             q.value(2).isNull() ? std::nullopt : std::optional<GameId>(q.value(2).toLongLong()),
             q.value(3).isNull() ? std::nullopt : std::optional<ReviewId>(q.value(3).toLongLong()),
             created});
    }
    return q.lastError().isValid() ? ListPuzzlesResult(error()) : ListPuzzlesResult(page);
}
LoadPuzzleResult StorageSession::loadPuzzle(PuzzleId id) {
    if (!usable_) {
        return error();
    }
    if (id <= 0) {
        return error(ReviewErrorCode::InvalidInput);
    }
    QSqlQuery q(database_);
    if (!exec(q, puzzleProjection + QStringLiteral("WHERE id=?"), {static_cast<qlonglong>(id)})) {
        return error();
    }
    if (!q.next()) {
        return error(q.lastError().isValid() ? ReviewErrorCode::Storage : ReviewErrorCode::Missing);
    }
    auto puzzle = readPuzzle(q);
    return puzzle ? LoadPuzzleResult(std::make_shared<const StoredPuzzle>(std::move(*puzzle)))
                  : LoadPuzzleResult(error(ReviewErrorCode::InvalidRecord));
}
DeletePuzzleResult StorageSession::deletePuzzle(PuzzleId id) {
    if (!usable_) {
        return error();
    }
    if (id <= 0) {
        return error(ReviewErrorCode::InvalidInput);
    }
    Transaction tx(database_, usable_);
    if (!tx.begin()) {
        return error();
    }
    // Delete also works for a damaged payload; the bounded stored identity remains
    // a suppression, never evidence authorizing a chess claim.
    QSqlQuery q(database_);
    if (!exec(q,
              QStringLiteral(
                  "INSERT OR IGNORE INTO puzzle_suppressions(position_key,position_identity) "
                  "SELECT position_key,position_identity FROM puzzles WHERE id=? "
                  "AND typeof(position_key)='blob' AND typeof(position_identity)='blob' "
                  "AND length(position_key)=32 AND length(position_identity)<=1048576"),
              {static_cast<qlonglong>(id)}) ||
        !exec(q, QStringLiteral("DELETE FROM puzzles WHERE id=?"), {static_cast<qlonglong>(id)})) {
        return error();
    }
    if (!tx.commit()) {
        return error();
    }
    return std::monostate{};
}
SavePuzzleJobResult StorageSession::loadPuzzleJob(PuzzleJobId id) {
    if (!usable_) {
        return error();
    }
    if (id <= 0) {
        return error(ReviewErrorCode::InvalidInput);
    }
    QSqlQuery q(database_);
    if (!exec(q, jobProjection + QStringLiteral("WHERE id=?"), {static_cast<qlonglong>(id)})) {
        return error();
    }
    if (!q.next()) {
        return error(q.lastError().isValid() ? ReviewErrorCode::Storage : ReviewErrorCode::Missing);
    }
    auto job = readJob(q);
    q.finish();
    if (!job) {
        return error(ReviewErrorCode::InvalidRecord);
    }
    if (!findAvailablePuzzles(database_, *job)) {
        return error();
    }
    return std::move(*job);
}
ListPuzzleJobsResult StorageSession::listPuzzleJobs(std::int64_t offset, int limit,
                                                    std::optional<ReviewId> reviewId) {
    if (!usable_) {
        return error();
    }
    if (offset < 0 || limit < 1 || limit > 50 || (reviewId && *reviewId <= 0)) {
        return error(ReviewErrorCode::InvalidInput);
    }
    QSqlQuery q(database_);
    QString sql = QStringLiteral(
        "SELECT id,source_review_id,status,current_position,candidate_count,saved_count,"
        "substr(CAST(document AS BLOB),1,33554433),"
        "typeof(id)='integer' AND typeof(status)='integer' AND typeof(current_position)='integer' "
        "AND typeof(candidate_count)='integer' AND typeof(saved_count)='integer' "
        "AND (source_review_id IS NULL OR typeof(source_review_id)='integer') FROM puzzle_jobs ");
    QVariantList args;
    if (reviewId) {
        sql += QStringLiteral("WHERE source_review_id=? ");
        args.append(static_cast<qlonglong>(*reviewId));
    } else {
        sql += QStringLiteral("WHERE status IN (1,2,4) ");
    }
    sql += QStringLiteral("ORDER BY id DESC LIMIT ? OFFSET ?");
    args.append(limit);
    args.append(static_cast<qlonglong>(offset));
    if (!exec(q, sql, args)) {
        return error();
    }
    std::vector<PuzzleJobSummary> jobs;
    while (q.next()) {
        const int status = q.value(2).toInt();
        if (!q.value(7).toBool() || q.value(0).toLongLong() <= 0 || status < 0 || status > 4 ||
            q.value(3).toInt() < 0 || q.value(4).toInt() < q.value(3).toInt() ||
            q.value(4).toInt() > 6 || q.value(5).toInt() < 0 || q.value(5).toInt() > 2) {
            return error(ReviewErrorCode::InvalidRecord);
        }
        std::uint32_t modelVersion = 1;
        if (q.value(6).typeId() == QMetaType::QByteArray) {
            if (const auto doc = readReviewDocument(q.value(6).toByteArray(), maxPuzzleJobBytes)) {
                const int parsed = (*doc)["modelVersion"].toInt(1);
                if (parsed >= 1 &&
                    puzzles::supportsModelVersion(static_cast<std::uint32_t>(parsed))) {
                    modelVersion = static_cast<std::uint32_t>(parsed);
                }
            }
        }
        jobs.push_back(
            {q.value(0).toLongLong(),
             q.value(1).isNull() ? std::nullopt : std::optional<ReviewId>(q.value(1).toLongLong()),
             static_cast<PuzzleJobStatus>(status), q.value(3).toInt(), q.value(4).toInt(),
             q.value(5).toInt(), modelVersion});
    }
    return q.lastError().isValid() ? ListPuzzleJobsResult(error()) : ListPuzzleJobsResult(jobs);
}
SavePuzzleJobResult StorageSession::preparePuzzleJob(ReviewId id,
                                                     config::PuzzleConfiguration settings) {
    if (!usable_) {
        return error();
    }
    QSqlQuery existing(database_);
    if (!exec(existing, QStringLiteral("SELECT id FROM puzzle_jobs WHERE source_review_id=?"),
              {static_cast<qlonglong>(id)})) {
        return error();
    }
    if (existing.next()) {
        const auto jobId = existing.value(0).toLongLong();
        existing.finish();
        auto loadedJob = loadPuzzleJob(jobId);
        const auto* existingJob = std::get_if<PuzzleJob>(&loadedJob);
        if (existingJob && existingJob->status == PuzzleJobStatus::Complete &&
            existingJob->accepted.empty() && existingJob->modelVersion < puzzles::modelVersion) {
            Transaction tx(database_, usable_);
            if (!tx.begin()) {
                return error();
            }
            QSqlQuery remove(database_);
            if (!exec(remove, QStringLiteral("DELETE FROM puzzle_jobs WHERE id=?"),
                      {static_cast<qlonglong>(jobId)}) ||
                !tx.commit()) {
                return error();
            }
        } else {
            return loadedJob;
        }
    }
    if (existing.lastError().isValid()) {
        return error();
    }
    const auto loaded = loadReview(id);
    const auto* run = std::get_if<StoredReview>(&loaded);
    if (!run) {
        return std::get<ReviewError>(loaded);
    }
    if (run->summary.status != review::JobStatus::Complete || !run->settings.refinement) {
        return ReviewError{
            ReviewErrorCode::Unavailable,
            QStringLiteral(
                "Finish a review with verified deeper analysis before generating puzzles.")};
    }
    auto ranked = rankPuzzlePositions(id, run->summary.revision);
    if (const auto* failure = std::get_if<ReviewError>(&ranked)) {
        return *failure;
    }
    PuzzleJob job;
    job.reviewId = id;
    job.modelVersion = puzzles::modelVersion;
    for (const auto& candidate : std::get<std::vector<RankedPuzzlePosition>>(ranked)) {
        if (job.candidates.size() == 6) {
            break;
        }
        const auto explored = readSource(*this, *run, candidate.index);
        const auto* e = std::get_if<PuzzleSource>(&explored);
        if (!e) {
            if (std::get<ReviewError>(explored).code == ReviewErrorCode::Unavailable) {
                continue;
            }
            return std::get<ReviewError>(explored);
        }
        PuzzleDocument d;
        d.definition.position = e->before.context();
        d.definition.origin = puzzles::Origin::Generated;
        d.definition.modelVersion = puzzles::modelVersion;
        d.definition.policy.review = run->settings.configuration.values;
        d.definition.policy.review.openings = {};
        config::applyPuzzleLimits(d.definition.policy, settings);
        const auto& budgets = d.definition.policy.review.budgets;
        d.definition.policy.baselineNodes =
            budgets[static_cast<std::size_t>(run->summary.quality)].verificationNodes;
        const auto a = e->older ? e->older->before.strength.nodes.value_or(0) : 0;
        const auto b = e->latest.before.strength.nodes.value_or(0);
        const auto maxNodes = std::max(a, b);
        d.definition.policy.rootMinimumNodes =
            std::min(maxNodes, d.definition.policy.baselineNodes * 2);
        const auto context = candidateContextFor(*e, run->settings.configuration.values);
        if (!puzzles::candidateGoal(e->before, e->older ? &*e->older : nullptr, e->latest,
                                    d.definition.policy, context)) {
            continue;
        }
        d.fingerprint = run->settings.fingerprint;
        d.engineProfileId = run->settings.engineProfileId;
        d.gameId = run->summary.gameId;
        d.reviewId = id;
        d.sourcePosition = candidate.index;
        if (!puzzles::validPolicy(d.definition.policy)) {
            continue;
        }
        job.candidates.push_back(std::move(d));
    }
    if (job.candidates.empty()) {
        job.status = PuzzleJobStatus::Complete;
    }
    return checkpointPuzzleJob({std::move(job), {}});
}
SavePuzzleJobResult StorageSession::checkpointPuzzleJob(PuzzleCheckpoint checkpoint) {
    if (!usable_) {
        return error();
    }
    auto& job = checkpoint.job;
    if (job.revision == std::numeric_limits<std::int64_t>::max() || job.id < 0) {
        return error(ReviewErrorCode::InvalidInput);
    }
    bool confirmedLesson = true;
    if (checkpoint.accepted &&
        checkpoint.accepted->definition.origin == puzzles::Origin::Generated) {
        const auto& d = *checkpoint.accepted;
        if (!d.reviewId || !d.sourcePosition) {
            return error(ReviewErrorCode::InvalidInput);
        }
        const auto loaded = loadReview(*d.reviewId);
        const auto* run = std::get_if<StoredReview>(&loaded);
        auto sourcePolicy = d.definition.policy.review;
        if (run) {
            // Puzzle policy stores only the review settings used for chess evaluation.
            sourcePolicy.openings = run->settings.configuration.values.openings;
        }
        if (!run || run->summary.status != review::JobStatus::Complete ||
            run->summary.gameId != d.gameId || run->settings.fingerprint != d.fingerprint ||
            run->settings.engineProfileId != d.engineProfileId ||
            run->settings.configuration.values != sourcePolicy) {
            return error(ReviewErrorCode::InvalidInput);
        }
        const auto explored = readSource(*this, *run, *d.sourcePosition);
        const auto* e = std::get_if<PuzzleSource>(&explored);
        const bool allowedSide = run->settings.configuration.ui.reviewBothSides ||
                                 (e && e->move.mover == run->summary.selectedSide);
        if (!e || e->before.context() != d.definition.position || !allowedSide) {
            return error(ReviewErrorCode::InvalidInput);
        }
        const auto context = candidateContextFor(*e, d.definition.policy.review);
        const bool isMissedMate = missedMate(*e);
        const auto goal = puzzles::candidateGoal(e->before, e->older ? &*e->older : nullptr,
                                                 e->latest, d.definition.policy, context);
        if (!goal) {
            return error(ReviewErrorCode::InvalidInput);
        }
        const bool playedAccepted =
            !d.solution.nodes.empty() &&
            std::ranges::any_of(d.solution.nodes.front().moves, [&](const auto& edge) {
                return edge.move == e->latest.playedMove;
            });
        // A stronger search can change the lesson or find that a blunder actually works, or
        // reveal that a played tactic was unsound. Publish only a confirmed lesson.
        const bool errorSource = context.isError || isMissedMate;
        confirmedLesson = !d.solution.nodes.empty() && d.solution.goal != puzzles::Goal::Practice &&
                          (errorSource ? !playedAccepted : playedAccepted);
    }
    Transaction tx(database_, usable_);
    if (!tx.begin()) {
        return error();
    }
    if (job.id > 0) {
        const auto loaded = loadPuzzleJob(job.id);
        const auto* previous = std::get_if<PuzzleJob>(&loaded);
        if (!previous) {
            return std::get<ReviewError>(loaded);
        }
        job.reusedExisting = previous->reusedExisting;
        if (previous->revision != job.revision || previous->reviewId != job.reviewId ||
            previous->accepted != job.accepted ||
            previous->candidates.size() != job.candidates.size() ||
            job.current < previous->current || job.current > previous->current + 1) {
            return error(ReviewErrorCode::Conflict);
        }
        for (std::size_t i = 0; i < job.candidates.size(); ++i) {
            const auto& before = previous->candidates[i];
            const auto& after = job.candidates[i];
            if (static_cast<int>(i) != previous->current) {
                if (encodePuzzle(before) != encodePuzzle(after)) {
                    return error(ReviewErrorCode::Conflict);
                }
                continue;
            }
            if (before.definition != after.definition || before.fingerprint != after.fingerprint ||
                before.engineProfileId != after.engineProfileId || before.gameId != after.gameId ||
                before.reviewId != after.reviewId ||
                before.sourcePosition != after.sourcePosition ||
                before.passes.size() > after.passes.size()) {
                return error(ReviewErrorCode::Conflict);
            }
            for (std::size_t p = 0; p < before.passes.size(); ++p) {
                const auto a = encodeAnalysis(before.passes[p]);
                const auto b = encodeAnalysis(after.passes[p]);
                if (!std::holds_alternative<QByteArray>(a) ||
                    !std::holds_alternative<QByteArray>(b) ||
                    std::get<QByteArray>(a) != std::get<QByteArray>(b)) {
                    return error(ReviewErrorCode::Conflict);
                }
            }
        }
        ++job.revision;
    } else if (job.current != 0 || !job.accepted.empty() || job.revision != 0 ||
               checkpoint.accepted) {
        return error(ReviewErrorCode::InvalidInput);
    }
    if (job.id == 0) {
        job.reusedExisting = false;
    }
    if (checkpoint.accepted) {
        const auto& d = *checkpoint.accepted;
        if (job.current < 1 || job.current > static_cast<int>(job.candidates.size()) ||
            job.accepted.size() >= 2 || encodePuzzle(d).isEmpty() ||
            encodePuzzle(d) !=
                encodePuzzle(job.candidates[static_cast<std::size_t>(job.current - 1)]) ||
            d.solution.nodes.empty()) {
            return error(ReviewErrorCode::InvalidInput);
        }
        const auto identity = positionIdentity(d.definition.position);
        const auto key = QCryptographicHash::hash(identity, QCryptographicHash::Sha256);
        QSqlQuery q(database_);
        if (!exec(q,
                  QStringLiteral("SELECT EXISTS(SELECT 1 FROM puzzle_suppressions WHERE "
                                 "position_key=? AND position_identity=?)"),
                  {key, identity}) ||
            !q.next()) {
            return error();
        }
        const bool suppressed = q.value(0).toBool();
        q.finish();
        if (confirmedLesson && (!suppressed || d.definition.origin == puzzles::Origin::Manual)) {
            if (!exec(q,
                      puzzleProjection +
                          QStringLiteral("WHERE position_key=? AND position_identity=?"),
                      {key, identity})) {
                return error();
            }
            PuzzleId puzzleId = 0;
            if (q.next()) {
                const auto previous = readPuzzle(q);
                if (!previous) {
                    return error(ReviewErrorCode::InvalidRecord);
                }
                puzzleId = previous->id;
                if (d.definition.origin == puzzles::Origin::Manual) {
                    job.reusedExisting = true;
                }
            } else if (q.lastError().isValid()) {
                return error();
            }
            q.finish();
            bool keep = true;
            if (puzzleId == 0 && d.definition.origin == puzzles::Origin::Generated) {
                if (!exec(q,
                          QStringLiteral(
                              "SELECT count(*) FROM puzzles WHERE source_game_id=? AND origin=0"),
                          {optionalId(d.gameId)}) ||
                    !q.next()) {
                    return error();
                }
                keep = q.value(0).toLongLong() < 2;
                q.finish();
                const auto intervalEnd = [](const PuzzleDocument& document) {
                    std::size_t length = 0;
                    for (const auto& node : document.solution.nodes) {
                        length = std::max(length, node.path.size());
                    }
                    return static_cast<std::int64_t>(*document.sourcePosition) +
                           static_cast<std::int64_t>(length);
                };
                for (const auto acceptedId : job.accepted) {
                    const auto loaded = loadPuzzle(acceptedId);
                    const auto* previous =
                        std::get_if<std::shared_ptr<const StoredPuzzle>>(&loaded);
                    if (!previous) {
                        if (std::get<ReviewError>(loaded).code == ReviewErrorCode::Missing) {
                            continue;
                        }
                        return std::get<ReviewError>(loaded);
                    }
                    const auto& earlier = (**previous).document;
                    if (earlier.gameId == d.gameId && earlier.sourcePosition && d.sourcePosition &&
                        *earlier.sourcePosition < intervalEnd(d) &&
                        *d.sourcePosition < intervalEnd(earlier)) {
                        keep = false;
                    }
                }
            }
            if (puzzleId == 0 && keep) {
                if (!exec(q,
                          QStringLiteral(
                              "INSERT INTO "
                              "puzzles(position_key,position_identity,source_game_id,source_review_"
                              "id,origin,document,created_at) VALUES(?,?,?,?,?,?,?)"),
                          {key, identity, optionalId(d.gameId), optionalId(d.reviewId),
                           static_cast<int>(d.definition.origin), encodePuzzle(d),
                           QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)})) {
                    return error();
                }
                puzzleId = q.lastInsertId().toLongLong();
            }
            if (d.definition.origin == puzzles::Origin::Manual &&
                !exec(q,
                      QStringLiteral("DELETE FROM puzzle_suppressions WHERE position_key=? AND "
                                     "position_identity=?"),
                      {key, identity})) {
                return error();
            }
            if (puzzleId > 0 && std::find(job.accepted.begin(), job.accepted.end(), puzzleId) ==
                                    job.accepted.end()) {
                job.accepted.push_back(puzzleId);
            }
        }
    }
    if (job.current == static_cast<int>(job.candidates.size()) || job.accepted.size() >= 2) {
        job.status = PuzzleJobStatus::Complete;
    }
    const auto bytes = encodePuzzleJob(job);
    if (bytes.isEmpty()) {
        return error(ReviewErrorCode::InvalidInput);
    }
    QSqlQuery q(database_);
    const QVariantList values{optionalId(job.reviewId),
                              static_cast<qlonglong>(job.revision),
                              static_cast<int>(job.status),
                              job.current,
                              static_cast<int>(job.candidates.size()),
                              static_cast<int>(job.accepted.size()),
                              bytes};
    if (job.id == 0) {
        if (!exec(q,
                  QStringLiteral("INSERT INTO "
                                 "puzzle_jobs(source_review_id,revision,status,current_position,"
                                 "candidate_count,saved_count,document) VALUES(?,?,?,?,?,?,?)"),
                  values)) {
            return error();
        }
        job.id = q.lastInsertId().toLongLong();
    } else {
        auto update = values;
        update.append(static_cast<qlonglong>(job.id));
        if (!exec(q,
                  QStringLiteral("UPDATE puzzle_jobs SET "
                                 "source_review_id=?,revision=?,status=?,current_position=?,"
                                 "candidate_count=?,saved_count=?,document=? WHERE id=?"),
                  update) ||
            q.numRowsAffected() != 1) {
            return error();
        }
    }
    if (!findAvailablePuzzles(database_, job) || !tx.commit()) {
        return error();
    }
    return job;
}
} // namespace chess_review::persistence::detail
