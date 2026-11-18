#include "PuzzleTestSupport.h"
#include "TestEnvironment.h"
#include "persistence/private/PuzzleCodec.h"
#include "persistence/private/ReviewCodec.h"
#include "persistence/private/StorageSession.h"
#include <QJsonArray>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

using namespace chess_review;
using namespace chess_review::persistence;
using namespace chess_review::persistence::detail;
namespace {
class Fixture {
  public:
    QTemporaryDir dir;
    QString path = dir.filePath(QStringLiteral("puzzles.sqlite3"));
    StorageSession session;
    Fixture() {
        if (session.initialize(path, migrations(), [](const QString&) {})) {
            qFatal("Cannot initialize puzzle fixture");
        }
    }
    PuzzleJob start() {
        PuzzleJob job;
        job.candidates = {puzzle_test::seed()};
        return std::get<PuzzleJob>(session.checkpointPuzzleJob({job, {}}));
    }
    PuzzleJob accept(PuzzleJob job) {
        job.candidates[0] = puzzle_test::complete();
        job.current = 1;
        return std::get<PuzzleJob>(session.checkpointPuzzleJob({job, job.candidates[0]}));
    }
};
} // namespace
class PuzzleRepositoryTest final : public QObject {
    Q_OBJECT
  private slots:
    void reviewCandidatesAreSelectedVerifiedAndSuppressedAfterDeletion() {
        Fixture f;
        pgn::PgnReader reader(
            "[SetUp \"1\"]\n[FEN \"6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1\"]\n\n1. h3 *");
        const auto game = std::get<pgn::PgnDocument>(reader.next());
        const auto gameId = std::get<Inserted>(f.session.insertGame(game, {})).id;
        const auto profileId =
            std::get<RegisteredEngine>(f.session.registerEngine(puzzle_test::engine())).profile.id;
        CreateReview request;
        request.gameId = gameId;
        request.engineProfileId = profileId;
        request.fingerprint = puzzle_test::fingerprint();
        request.refinement = true;
        request.pipelineVersion = 4;
        auto values = review::Configuration{};
        values.refinement.multiPv = 4;
        values.budgets[1] = {1, 5, 10};
        values.openings = {false, 10, 4};
        request.configuration = *std::get<std::shared_ptr<const config::ValidatedConfiguration>>(
            config::validate(values));
        const auto completedReview = [&]() {
            auto run = std::get<ReviewSummary>(f.session.createReview(request));
            for (int stage = 0; stage < 3; ++stage) {
                for (int index = 0; index < 2; ++index) {
                    const int width = stage == 0 ? 1 : 4;
                    const auto budget = *review::stageBudget(
                        values.budgets[1], static_cast<review::EvidenceStage>(stage));
                    auto raw = puzzle_test::pass(puzzle_test::seed(profileId), budget);
                    auto position = puzzle_test::root();
                    if (index == 1) {
                        const auto played = std::get<chess::Move>(position.parseUci("h2h3"));
                        if (std::holds_alternative<chess::Error>(position.play(played))) {
                            qFatal("Invalid review fixture move");
                        }
                        const auto legal = position.legalMoves();
                        for (std::size_t i = 0; i < raw.evidence.lines.size(); ++i) {
                            auto& line = raw.evidence.lines[i];
                            line.pv = {legal[i]};
                            line.score = engine::AnalysisScore{engine::ScoreType::Centipawns, 0,
                                                               engine::ScoreBound::Exact};
                        }
                    }
                    auto& snapshot = raw.evidence;
                    snapshot.lines.resize(static_cast<std::size_t>(width));
                    snapshot.requestedMultiPv = snapshot.effectiveMultiPv = snapshot.achievedWidth =
                        width;
                    snapshot.bestMove = snapshot.lines.front().pv.front();
                    snapshot.provenance = std::make_shared<const engine::AnalysisProvenance>(
                        engine::AnalysisProvenance{
                            position.context(),
                            engine::reviewWidthFingerprint(request.fingerprint, width),
                            {},
                            QDateTime::currentDateTimeUtc()});
                    ReviewCheckpoint cp;
                    cp.reviewId = run.id;
                    cp.expectedRevision = run.revision;
                    cp.positionIndex = index;
                    cp.stage = static_cast<review::EvidenceStage>(stage);
                    cp.status = stage == 0 ? review::JobStatus::QuickScanning
                                           : review::JobStatus::Deepening;
                    cp.analysis = raw;
                    const auto result = f.session.checkpointReview(cp);
                    if (!std::holds_alternative<ReviewSummary>(result)) {
                        qFatal("Cannot checkpoint review puzzle fixture");
                    }
                    run = std::get<ReviewSummary>(result);
                }
            }
            ReviewCheckpoint cp;
            cp.reviewId = run.id;
            cp.expectedRevision = run.revision;
            cp.status = review::JobStatus::Complete;
            const auto result = f.session.checkpointReview(cp);
            if (!std::holds_alternative<ReviewSummary>(result)) {
                qFatal("Cannot complete review puzzle fixture");
            }
            return std::get<ReviewSummary>(result).id;
        };
        const auto firstReview = completedReview();
        auto result = f.session.preparePuzzleJob(firstReview);
        QVERIFY2(std::holds_alternative<PuzzleJob>(result),
                 "Candidate selection must accept a completed review");
        auto job = std::get<PuzzleJob>(result);
        QCOMPARE(job.candidates.size(), std::size_t{1});
        QCOMPARE(job.candidates[0].definition.origin, puzzles::Origin::Generated);
        auto d = job.candidates[0];
        d.passes = {puzzle_test::pass(d, 20), puzzle_test::pass(d, 40)};
        const auto verified = std::get<puzzles::Verification>(verifyPuzzle(d));
        QCOMPARE(verified.outcome, puzzles::Outcome::Accepted);
        d.solution = verified.solution;
        job.candidates[0] = d;
        job.current = 1;
        result = f.session.checkpointPuzzleJob({job, d});
        QVERIFY(std::holds_alternative<PuzzleJob>(result));
        const auto saved = std::get<PuzzleJob>(result);
        QCOMPARE(saved.accepted.size(), std::size_t{1});
        auto manual = d;
        manual.definition.origin = puzzles::Origin::Manual;
        manual.definition.policy.rootMinimumNodes = 0;
        manual.reviewId.reset();
        manual.sourcePosition.reset();
        manual.solution = std::get<puzzles::Verification>(verifyPuzzle(manual)).solution;
        auto manualSeed = manual;
        manualSeed.passes.clear();
        manualSeed.solution = {};
        PuzzleJob manualJob;
        manualJob.candidates = {manualSeed};
        manualJob = std::get<PuzzleJob>(f.session.checkpointPuzzleJob({manualJob, {}}));
        manualJob.candidates[0] = manual;
        manualJob.current = 1;
        manualJob = std::get<PuzzleJob>(f.session.checkpointPuzzleJob({manualJob, manual}));
        QCOMPARE(manualJob.accepted.front(), saved.accepted.front());
        const auto existing = std::get<std::shared_ptr<const StoredPuzzle>>(
            f.session.loadPuzzle(saved.accepted.front()));
        QCOMPARE(existing->document.definition.origin, puzzles::Origin::Generated);

        QVERIFY(
            std::holds_alternative<std::monostate>(f.session.deletePuzzle(saved.accepted.front())));
        const auto secondReview = completedReview();
        job = std::get<PuzzleJob>(f.session.preparePuzzleJob(secondReview));
        QCOMPARE(job.candidates.size(), std::size_t{1});
        d = job.candidates[0];
        d.passes = {puzzle_test::pass(d, 20), puzzle_test::pass(d, 40)};
        d.solution = std::get<puzzles::Verification>(verifyPuzzle(d)).solution;
        job.candidates[0] = d;
        job.current = 1;
        const auto suppressed = std::get<PuzzleJob>(f.session.checkpointPuzzleJob({job, d}));
        QVERIFY(suppressed.accepted.empty());
        QVERIFY(std::get<PuzzlePage>(f.session.listPuzzles(0, 50, {})).puzzles.empty());
    }
    void codecRequiresOwnedCompleteEvidence() {
        auto d = puzzle_test::complete();
        const auto bytes = encodePuzzle(d);
        QVERIFY(!bytes.isEmpty());
        const auto decoded = decodePuzzle(bytes);
        QVERIFY(decoded);
        QCOMPARE(decoded->solution, d.solution);
        QCOMPARE(encodePuzzle(*decoded), bytes);
        auto changed = bytes;
        changed[50] = static_cast<char>(changed[50] ^ 1);
        QVERIFY(!decodePuzzle(changed));
        d.solution.nodes.back().complete = false;
        QVERIFY(encodePuzzle(d).isEmpty());
        d = puzzle_test::complete();
        d.passes[1].evidence.request = d.passes[0].evidence.request;
        QVERIFY(encodePuzzle(d).isEmpty());
        auto object = *readReviewDocument(bytes, maxPuzzleBytes);
        object["modelVersion"] = 99;
        QVERIFY(!decodePuzzle(reviewDocument(object)));
        object = *readReviewDocument(bytes, maxPuzzleBytes);
        object["solution"] = QJsonObject{};
        QVERIFY(!decodePuzzle(reviewDocument(object)));
    }
    void saveLoadDeduplicateAndRestoreDeletedPuzzle() {
        Fixture f;
        auto saved = f.accept(f.start());
        QCOMPARE(saved.accepted.size(), std::size_t{1});
        const auto id = saved.accepted.front();
        for (const auto& pass : saved.candidates[0].passes) {
            QVERIFY(std::holds_alternative<AnalysisStored>(f.session.storeAnalysis(pass)));
        }
        QSqlQuery evict(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
        QVERIFY(evict.exec(QStringLiteral("DELETE FROM analysis_results")));

        auto page = std::get<PuzzlePage>(f.session.listPuzzles(0, 50, {}));
        QCOMPARE(page.puzzles.size(), std::size_t{1});
        auto loaded = f.session.loadPuzzle(id);
        QVERIFY(std::holds_alternative<std::shared_ptr<const StoredPuzzle>>(loaded));
        QCOMPARE(std::get<std::shared_ptr<const StoredPuzzle>>(loaded)->document.solution,
                 saved.candidates[0].solution);
        const auto again = f.accept(f.start());
        QCOMPARE(again.accepted.front(), id);
        QVERIFY(std::holds_alternative<std::monostate>(f.session.deletePuzzle(id)));
        QCOMPARE(std::get<PuzzlePage>(f.session.listPuzzles(0, 50, {})).puzzles.size(),
                 std::size_t{0});
        const auto restored = f.accept(f.start());
        QCOMPARE(restored.accepted.size(), std::size_t{1});
        QVERIFY(restored.accepted.front() > id);
        QVERIFY(std::holds_alternative<ReviewError>(f.session.loadPuzzle(id)));
        const auto previous = std::get<PuzzleJob>(f.session.loadPuzzleJob(saved.id));
        QCOMPARE(previous.accepted, saved.accepted);
        QVERIFY(previous.available.empty());
        QCOMPARE(std::get<PuzzlePage>(f.session.listPuzzles(0, 50, {})).puzzles.size(),
                 std::size_t{1});
    }
    void checkpointsConflictAndSurviveReopen() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("resume.sqlite3"));
        PuzzleJob saved;
        {
            StorageSession s;
            QVERIFY(!s.initialize(path, migrations(), [](const QString&) {}));
            PuzzleJob job;
            job.candidates = {puzzle_test::seed()};
            saved = std::get<PuzzleJob>(s.checkpointPuzzleJob({job, {}}));
            auto progressing = saved;
            progressing.candidates[0].passes.push_back(
                puzzle_test::pass(progressing.candidates[0], 20));
            progressing = std::get<PuzzleJob>(s.checkpointPuzzleJob({progressing, {}}));
            QVERIFY(std::holds_alternative<ReviewError>(s.checkpointPuzzleJob({saved, {}})));
            saved = progressing;
        }
        StorageSession s;
        QVERIFY(!s.initialize(path, migrations(), [](const QString&) {}));
        const auto reopened = std::get<PuzzleJob>(s.loadPuzzleJob(saved.id));
        QCOMPARE(reopened.status, PuzzleJobStatus::Interrupted);
        QCOMPARE(reopened.revision, saved.revision + 1);
        QCOMPARE(reopened.candidates[0].passes.size(), std::size_t{1});
        const auto result = std::get<puzzles::Verification>(verifyPuzzle(reopened.candidates[0]));
        QVERIFY(result.next);
        QCOMPARE(result.next->nodes, std::uint64_t{40});
    }
    void upgradePreservesPuzzleAndJobIdentity() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("upgrade.sqlite3"));
        PuzzleId id = 0;
        PuzzleJobId jobId = 0;
        const auto document = puzzle_test::complete();
        {
            StorageSession storage;
            const auto legacy = std::span(migrations()).first(12);
            QVERIFY(!storage.initialize(path, legacy, [](const QString&) {}));
            PuzzleJob job;
            job.candidates = {puzzle_test::seed()};
            job = std::get<PuzzleJob>(storage.checkpointPuzzleJob({job, {}}));
            job.current = 1;
            job.candidates[0] = document;
            job = std::get<PuzzleJob>(storage.checkpointPuzzleJob({job, document}));
            QCOMPARE(job.accepted.size(), std::size_t{1});
            id = job.accepted.front();
            jobId = job.id;
        }
        StorageSession storage;
        QVERIFY(!storage.initialize(path, migrations(), [](const QString&) {}));
        const auto saved = std::get<std::shared_ptr<const StoredPuzzle>>(storage.loadPuzzle(id));
        QCOMPARE(saved->document.solution, document.solution);
        const auto job = std::get<PuzzleJob>(storage.loadPuzzleJob(jobId));
        QCOMPARE(job.available, std::vector{id});
    }
    void failedPuzzleInsertRollsBackCheckpoint() {
        Fixture f;
        auto job = f.start();
        auto db = QSqlDatabase::database(QSqlDatabase::connectionNames().front());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("CREATE TRIGGER reject_puzzle BEFORE INSERT ON puzzles BEGIN "
                                      "SELECT RAISE(ABORT,'fixture'); END")));
        auto next = job;
        next.candidates[0] = puzzle_test::complete();
        next.current = 1;
        QVERIFY(std::holds_alternative<ReviewError>(
            f.session.checkpointPuzzleJob({next, next.candidates[0]})));
        const auto unchanged = std::get<PuzzleJob>(f.session.loadPuzzleJob(job.id));
        QCOMPARE(unchanged.revision, job.revision);
        QCOMPARE(unchanged.current, 0);
        QVERIFY(unchanged.candidates[0].passes.empty());
    }
    void damagedPuzzleDoesNotPreventDeletion() {
        Fixture f;
        const auto id = f.accept(f.start()).accepted.front();
        QSqlQuery q(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
        QVERIFY(q.exec(QStringLiteral("UPDATE puzzles SET document=zeroblob(16777217)")));
        QVERIFY(std::holds_alternative<ReviewError>(f.session.loadPuzzle(id)));
        QVERIFY(std::holds_alternative<std::monostate>(f.session.deletePuzzle(id)));
    }
    void manualPuzzleSurvivesRemovalOfItsSourceGame() {
        Fixture f;
        pgn::PgnReader reader(
            "[SetUp \"1\"]\n[FEN \"6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1\"]\n\n*");
        const auto game = std::get<pgn::PgnDocument>(reader.next());
        const auto gameId = std::get<Inserted>(f.session.insertGame(game, {})).id;
        auto document = puzzle_test::complete();
        document.gameId = gameId;
        auto seed = document;
        seed.passes.clear();
        seed.solution = {};
        PuzzleJob job;
        job.candidates = {seed};
        job = std::get<PuzzleJob>(f.session.checkpointPuzzleJob({job, {}}));
        job.candidates[0] = document;
        job.current = 1;
        job = std::get<PuzzleJob>(f.session.checkpointPuzzleJob({job, document}));
        QSqlQuery q(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
        QVERIFY(q.prepare(QStringLiteral("DELETE FROM games WHERE id=?")));
        q.addBindValue(static_cast<qlonglong>(gameId));
        QVERIFY(q.exec());
        QVERIFY(std::holds_alternative<std::shared_ptr<const StoredPuzzle>>(
            f.session.loadPuzzle(job.accepted.front())));
    }
    void invalidMetadataDoesNotCoerceToAValidCheckpoint() {
        Fixture f;
        const auto job = f.start();
        QSqlQuery q(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
        QVERIFY(q.exec(QStringLiteral("UPDATE puzzle_jobs SET revision='damaged'")));
        const auto loaded = f.session.loadPuzzleJob(job.id);
        QVERIFY(std::holds_alternative<ReviewError>(loaded));
        QCOMPARE(std::get<ReviewError>(loaded).code, ReviewErrorCode::InvalidRecord);
    }
};
int main(int argc, char** argv) {
    TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QCoreApplication application(argc, argv);
    PuzzleRepositoryTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "PuzzleRepositoryTest.moc"
