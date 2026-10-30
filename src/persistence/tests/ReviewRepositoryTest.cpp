#include "TestEnvironment.h"
#include "engine/private/Fingerprint.h"
#include "persistence/private/ReviewCodec.h"
#include "persistence/private/StorageSession.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <cstdio>
using namespace chess_review;
using namespace chess_review::persistence;
using namespace chess_review::persistence::detail;
using review::JobStatus;
namespace {
const Progress ignore = [](const QString&) {};
std::shared_ptr<const pgn::PgnDocument> document(const std::string& text = "1. e4 e5 *") {
    pgn::PgnReader reader(text);
    return std::make_shared<const pgn::PgnDocument>(std::get<pgn::PgnDocument>(reader.next()));
}
engine::ValidatedEngine engineValue() {
    engine::ValidatedEngine value;
    value.executablePath = QStringLiteral("/missing/synthetic-engine");
    value.executableSha256 = QByteArray(32, 'x');
    value.reportedName = QStringLiteral("Synthetic engine");
    value.lastValidatedAt = QDateTime::currentDateTimeUtc();
    return value;
}
CreateReview seed(StorageSession& session,
                  const std::shared_ptr<const pgn::PgnDocument>& doc = document()) {
    CreateReview request;
    request.gameId = std::get<Inserted>(session.insertGame(*doc, {})).id;
    request.engineProfileId =
        std::get<RegisteredEngine>(session.registerEngine(engineValue())).profile.id;
    request.fingerprint = engine::makeFingerprint(engineValue(), {});
    request.configuration =
        *std::get<std::shared_ptr<const config::ValidatedConfiguration>>(config::parse({}));
    return request;
}
chess::Position positionAt(const pgn::PgnDocument& doc, int ply) {
    auto position = std::get<chess::Position>(doc.tree.position(doc.tree.rootId()));
    for (int i = 0; i < ply; ++i) {
        if (std::holds_alternative<chess::Error>(
                position.play(doc.originalMainline()[static_cast<std::size_t>(i)]))) {
            qFatal("Invalid fixture");
        }
    }
    return position;
}
engine::AnalysisResult resultAt(const CreateReview& request, const pgn::PgnDocument& doc,
                                int index) {
    const auto position = positionAt(doc, index);
    engine::AnalysisResult result;
    result.outcome = engine::AnalysisOutcome::Completed;
    auto& s = result.evidence;
    s.session = QUuid::createUuid();
    s.request = QUuid::createUuid();
    s.limit = engine::NodeLimit{100000};
    s.searchStarted = true;
    s.achievedWidth = 1;
    s.commonDepth = 12;
    s.statistics.depth = 12;
    s.statistics.nodes = 100000;
    s.bestMove = position.legalMoves().front();
    engine::AnalysisLine line;
    line.score = engine::AnalysisScore{engine::ScoreType::Centipawns, 0, engine::ScoreBound::Exact};
    line.pv = {*s.bestMove};
    line.statistics = s.statistics;
    s.lines = {line};
    s.provenance = std::make_shared<const engine::AnalysisProvenance>(engine::AnalysisProvenance{
        position.context(), request.fingerprint, std::nullopt, QDateTime::currentDateTimeUtc()});
    return result;
}
ReviewCheckpoint checkpoint(ReviewId id, std::int64_t revision, int index,
                            const engine::AnalysisResult& analysis) {
    ReviewCheckpoint cp;
    cp.reviewId = id;
    cp.expectedRevision = revision;
    cp.positionIndex = index;
    cp.analysis = analysis;
    return cp;
}
class Fixture {
  public:
    QTemporaryDir directory;
    QString path = directory.filePath(QStringLiteral("reviews.sqlite3"));
    StorageSession session;
    Fixture() {
        if (session.initialize(path, migrations(), ignore)) {
            qFatal("Cannot initialize fixture");
        }
    }
    bool exec(const QString& sql) {
        QSqlQuery q(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
        return q.exec(sql);
    }
    QVariant scalar(const QString& sql) {
        QSqlQuery q(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
        if (!q.exec(sql) || !q.next()) {
            return {};
        }
        return q.value(0);
    }
};
class AsyncDatabase {
  public:
    Database value;
    bool closed = false;
    AsyncDatabase() {
        QObject::connect(&value, &Database::closed, &value, [this] { closed = true; });
    }
    ~AsyncDatabase() {
        value.close();
        while (!closed) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
    }
};
} // namespace
class ReviewRepositoryTest final : public QObject {
    Q_OBJECT
  private slots:
    void compactReviewListsRejectUnsupportedHeaders() {
        Fixture f;
        const auto request = seed(f.session);
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        QVERIFY(!f.session.interruptReviewJobs(f.path));
        run = std::get<StoredReview>(f.session.loadReview(run.id)).summary;
        QVERIFY(std::holds_alternative<std::monostate>(
            f.session.saveReviewContext({request.gameId, run.id, 0, false})));
        QVERIFY(f.exec(QStringLiteral("UPDATE reviews SET format_version=99")));
        const auto listed =
            std::get<std::vector<LibraryReview>>(f.session.listLibraryReviews({request.gameId}));
        QCOMPARE(listed.size(), std::size_t{1});
        QVERIFY(listed.front().error);
        QCOMPARE(listed.front().error->code, ReviewErrorCode::UnsupportedVersion);
        QVERIFY(!listed.front().run);
        const auto history =
            std::get<std::vector<ReviewSummary>>(f.session.listReviews(request.gameId, 0, 51));
        QCOMPARE(history.size(), std::size_t{1});
        QCOMPARE(history.front().metadataError,
                 std::optional<ReviewErrorCode>{ReviewErrorCode::UnsupportedVersion});
        const auto recovery =
            std::get<std::vector<ReviewSummary>>(f.session.listInterruptedReviews(0, 51));
        QCOMPARE(recovery.size(), std::size_t{1});
        QCOMPARE(recovery.front().metadataError,
                 std::optional<ReviewErrorCode>{ReviewErrorCode::UnsupportedVersion});
    }
    void workspaceContextValidatesAndRollsBack() {
        Fixture f;
        const auto request = seed(f.session);
        const auto run = std::get<ReviewSummary>(f.session.createReview(request));
        ReviewContext context{request.gameId, run.id, 2, true};
        QVERIFY(std::holds_alternative<std::monostate>(f.session.saveReviewContext(context)));
        const auto loaded = std::get<ReviewContext>(f.session.loadReviewContext(0, 0));
        QCOMPARE(loaded.gameId, context.gameId);
        QCOMPARE(loaded.reviewId, context.reviewId);
        QCOMPARE(loaded.position, context.position);
        QVERIFY(loaded.reopen);
        const auto selection =
            std::get<ReviewContext>(f.session.loadReviewContext(request.gameId, 0));
        QCOMPARE(selection.reviewId, context.reviewId);
        QCOMPARE(selection.position, context.position);
        auto wrong = context;
        wrong.position = 3;
        QCOMPARE(std::get<ReviewError>(f.session.saveReviewContext(wrong)).code,
                 ReviewErrorCode::InvalidInput);
        const auto other = std::get<Inserted>(f.session.insertGame(*document("1. d4 *"), {})).id;
        wrong = context;
        wrong.gameId = other;
        QCOMPARE(std::get<ReviewError>(f.session.saveReviewContext(wrong)).code,
                 ReviewErrorCode::InvalidInput);
        QVERIFY(f.exec(QStringLiteral("CREATE TRIGGER reject_moment BEFORE INSERT ON "
                                      "review_moments BEGIN SELECT RAISE(ABORT,'fixture'); END")));
        wrong = context;
        wrong.position = 1;
        wrong.reopen = false;
        QVERIFY(std::holds_alternative<ReviewError>(f.session.saveReviewContext(wrong)));
        const auto preserved = std::get<ReviewContext>(f.session.loadReviewContext(0, 0));
        QCOMPARE(preserved.position, context.position);
        QVERIFY(preserved.reopen);
        const auto rows = std::get<std::vector<LibraryReview>>(
            f.session.listLibraryReviews({request.gameId, other}));
        QCOMPARE(rows.size(), std::size_t{2});
        QVERIFY(rows[0].run);
        QCOMPARE(rows[0].run->id, run.id);
        QVERIFY(!rows[1].run);
    }
    void workspaceContextRejectsDamagedMetadata() {
        Fixture f;
        const auto request = seed(f.session);
        const auto run = std::get<ReviewSummary>(f.session.createReview(request));
        QVERIFY(std::holds_alternative<std::monostate>(
            f.session.saveReviewContext({request.gameId, run.id, 1, true})));
        QVERIFY(f.exec(QStringLiteral("UPDATE review_moments SET position_index='bad'")));
        QVERIFY(std::get<ReviewContext>(f.session.loadReviewContext(0, 0)).invalidPosition);
        QVERIFY(f.exec(QStringLiteral("PRAGMA ignore_check_constraints=ON")));
        QVERIFY(f.exec(QStringLiteral("UPDATE review_workspace SET format_version=99")));
        QCOMPARE(std::get<ReviewError>(f.session.loadReviewContext(0, 0)).code,
                 ReviewErrorCode::UnsupportedVersion);
    }
    void workspaceMigrationPreservesVersionEightData() {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("migration.sqlite"));
        GameId game;
        ReviewId run;
        {
            StorageSession old;
            QVERIFY(!old.initialize(path, std::span(migrations()).first(8), ignore));
            const auto request = seed(old);
            game = request.gameId;
            run = std::get<ReviewSummary>(old.createReview(request)).id;
        }
        StorageSession current;
        QVERIFY(!current.initialize(path, migrations(), ignore));
        QCOMPARE(std::get<StoredReview>(current.loadReview(run)).summary.gameId, game);
        QCOMPARE(std::get<StoredReview>(current.loadReview(run)).summary.status,
                 JobStatus::Interrupted);
        QVERIFY(!std::get<ReviewContext>(current.loadReviewContext(0, 0)).gameId);
        const auto interrupted =
            std::get<std::vector<ReviewSummary>>(current.listInterruptedReviews(0, 51));
        QCOMPARE(interrupted.size(), std::size_t{1});
        QCOMPARE(interrupted.front().id, run);
    }
    void init() { QTest::failOnWarning(); }
    void damagedStageMetadataCannotInventProgress_data() {
        QTest::addColumn<int>("damage");
        QTest::newRow("stage on legacy run") << 0;
        QTest::newRow("stage outside mainline") << 1;
        QTest::newRow("orphaned stage evidence") << 2;
        QTest::newRow("stage without quick position") << 3;
    }
    void damagedStageMetadataCannotInventProgress() {
        QFETCH(int, damage);
        Fixture f;
        const auto doc = document("*");
        auto request = seed(f.session, doc);
        request.refinement = damage != 0;
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        run = std::get<ReviewSummary>(f.session.checkpointReview(
            checkpoint(run.id, run.revision, 0, resultAt(request, *doc, 0))));
        QVERIFY(f.exec(QStringLiteral(
            "INSERT INTO review_stage_positions(review_id,stage,position_index,evidence_id) SELECT "
            "review_id,1,position_index,id FROM review_evidence")));
        if (damage == 1) {
            QVERIFY(f.exec(QStringLiteral("UPDATE review_stage_positions SET position_index=99")));
        } else if (damage == 2) {
            QVERIFY(f.exec(QStringLiteral("PRAGMA foreign_keys=OFF")));
            QVERIFY(f.exec(QStringLiteral("UPDATE review_stage_positions SET evidence_id=999999")));
        } else if (damage == 3) {
            QVERIFY(f.exec(QStringLiteral("DELETE FROM review_positions")));
            QVERIFY(f.exec(QStringLiteral("UPDATE review_jobs SET completed_positions=0")));
        }
        const auto result = f.session.loadReviewOverview(run.id, run.revision);
        QVERIFY(std::holds_alternative<ReviewError>(result));
        QCOMPARE(std::get<ReviewError>(result).code, ReviewErrorCode::InvalidRecord);
        QCOMPARE(std::get<ReviewError>(f.session.loadReview(run.id)).code,
                 ReviewErrorCode::InvalidRecord);
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_stage_positions")).toInt(),
                 1);
    }
    void refinementSurvivesReopenWithIndependentAccuracy() {
        Fixture f;
        const auto doc = document();
        auto request = seed(f.session, doc);
        request.refinement = true;
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        for (int index = 0; index < 3; ++index) {
            auto result = resultAt(request, *doc, index);
            run = std::get<ReviewSummary>(
                f.session.checkpointReview(checkpoint(run.id, run.revision, index, result)));
        }
        auto overview =
            std::get<ReviewOverview>(f.session.loadReviewOverview(run.id, run.revision));
        QCOMPARE(overview.sides[0].evaluated, 1);
        QCOMPARE(overview.sides[1].evaluated, 1);
        QCOMPARE(overview.sides[0].accuracy, std::optional<double>(100));
        QCOMPARE(overview.refinementPositions.size(), std::size_t(3));
        for (const auto stage :
             {review::EvidenceStage::Deepening, review::EvidenceStage::Verification}) {
            for (int index = 0; index < 3; ++index) {
                auto result = resultAt(request, *doc, index);
                const std::uint64_t nodes =
                    stage == review::EvidenceStage::Deepening ? 1000000 : 5000000;
                result.evidence.limit = engine::NodeLimit{nodes};
                result.evidence.statistics.nodes = nodes;
                result.evidence.lines.front().statistics.nodes = nodes;
                auto cp = checkpoint(run.id, run.revision, index, result);
                cp.stage = stage;
                cp.status = JobStatus::Deepening;
                const auto saved = f.session.checkpointReview(cp);
                QVERIFY2(std::holds_alternative<ReviewSummary>(saved), "Stage checkpoint rejected");
                run = std::get<ReviewSummary>(saved);
            }
        }
        const auto page =
            std::get<ReviewMovePage>(f.session.deriveReviewMoves(run.id, run.revision, 0, 50));
        QVERIFY(page.moves.front().classification->stable);
        QCOMPARE(page.moves.front().stage, review::EvidenceStage::Verification);
        overview = std::get<ReviewOverview>(f.session.loadReviewOverview(run.id, run.revision));
        QCOMPARE(overview.sides[0].stable, 1);
        QCOMPARE(overview.sides[1].stable, 1);
        QCOMPARE(overview.verifiedPositions, 3);
        QVERIFY(f.exec(QStringLiteral("DELETE FROM analysis_results")));
        const auto stageEvidence =
            f.session.loadReviewEvidence(run.id, 1, review::EvidenceStage::Verification);
        QVERIFY(std::holds_alternative<StoredReviewEvidence>(stageEvidence));
        auto cp = checkpoint(run.id, run.revision - 1, 0, resultAt(request, *doc, 0));
        QCOMPARE(std::get<ReviewError>(f.session.checkpointReview(cp)).code,
                 ReviewErrorCode::Conflict);
        QVERIFY(f.exec(
            QStringLiteral("UPDATE review_evidence SET adapted=x'01' WHERE id=(SELECT evidence_id "
                           "FROM review_stage_positions WHERE stage=2 AND position_index=1)")));
        QCOMPARE(std::get<ReviewError>(f.session.loadReviewOverview(run.id, run.revision)).code,
                 ReviewErrorCode::InvalidRecord);
    }
    void accuracyIsIndependentOfLabelsForTheSameOwnedEvidence() {
        Fixture f;
        const auto doc = document("1. e4 *");
        const auto original = seed(f.session, doc);
        std::array<engine::AnalysisResult, 2> analyses{resultAt(original, *doc, 0),
                                                       resultAt(original, *doc, 1)};
        analyses[0].evidence.lines.front().wdl = engine::Wdl{500, 0, 500};
        analyses[1].evidence.lines.front().wdl = engine::Wdl{600, 0, 400};
        std::array<ReviewOverview, 2> summaries;
        std::array<std::optional<review::Classification>, 2> labels;
        for (std::size_t n = 0; n < 2; ++n) {
            auto request = original;
            if (n == 1) {
                auto values = request.configuration.values;
                values.classification.inaccuracyLoss = .15;
                values.classification.mistakeLoss = .3;
                values.classification.blunderLoss = .4;
                request.configuration =
                    *std::get<std::shared_ptr<const config::ValidatedConfiguration>>(
                        config::validate(values));
            }
            auto run = std::get<ReviewSummary>(f.session.createReview(request));
            for (int index = 0; index < 2; ++index) {
                run = std::get<ReviewSummary>(f.session.checkpointReview(checkpoint(
                    run.id, run.revision, index, analyses[static_cast<std::size_t>(index)])));
            }
            summaries[n] =
                std::get<ReviewOverview>(f.session.loadReviewOverview(run.id, run.revision));
            labels[n] =
                std::get<ReviewMovePage>(f.session.deriveReviewMoves(run.id, run.revision, 0, 50))
                    .moves.front()
                    .classification->label;
        }
        QVERIFY(labels[0] != labels[1]);
        QCOMPARE(summaries[0].sides[0].accuracy, summaries[1].sides[0].accuracy);
        QCOMPARE(summaries[0].sides[0].evaluated, 1);
        QCOMPARE(summaries[1].sides[0].evaluated, 1);
    }
    void refinedRunReopensWithoutAnEngine() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("reopen.sqlite3"));
        ReviewId id = 0;
        std::int64_t revision = 0;
        {
            StorageSession session;
            QVERIFY(!session.initialize(path, migrations(), ignore));
            const auto doc = document();
            auto request = seed(session, doc);
            request.refinement = true;
            auto run = std::get<ReviewSummary>(session.createReview(request));
            for (const auto stage : {review::EvidenceStage::Quick, review::EvidenceStage::Deepening,
                                     review::EvidenceStage::Verification}) {
                for (int index = 0; index < 3; ++index) {
                    auto result = resultAt(request, *doc, index);
                    const std::uint64_t nodes = stage == review::EvidenceStage::Quick ? 100000
                                                : stage == review::EvidenceStage::Deepening
                                                    ? 1000000
                                                    : 5000000;
                    result.evidence.limit = engine::NodeLimit{nodes};
                    result.evidence.statistics.nodes =
                        result.evidence.lines.front().statistics.nodes = nodes;
                    auto cp = checkpoint(run.id, run.revision, index, result);
                    cp.stage = stage;
                    cp.status = stage == review::EvidenceStage::Quick ? JobStatus::QuickScanning
                                                                      : JobStatus::Deepening;
                    run = std::get<ReviewSummary>(session.checkpointReview(cp));
                }
            }
            ReviewCheckpoint complete;
            complete.reviewId = run.id;
            complete.expectedRevision = run.revision;
            complete.status = JobStatus::Complete;
            const auto result = session.checkpointReview(complete);
            QVERIFY(std::holds_alternative<ReviewSummary>(result));
            id = run.id;
            revision = std::get<ReviewSummary>(result).revision;
        }
        StorageSession reopened;
        QVERIFY(!reopened.initialize(path, migrations(), ignore));
        const auto loaded = std::get<StoredReview>(reopened.loadReview(id));
        QCOMPARE(loaded.summary.status, JobStatus::Complete);
        QCOMPARE(loaded.summary.revision, revision);
        const auto overview = std::get<ReviewOverview>(reopened.loadReviewOverview(id, revision));
        QCOMPARE(overview.sides[0].stable, 1);
        QCOMPARE(overview.sides[1].accuracy, std::optional<double>(100));
        QCOMPARE(overview.verifiedPositions, 3);
    }
    void refinedEvidenceRequiresStageBudgetAndCapturedEngine() {
        Fixture f;
        const auto doc = document();
        auto request = seed(f.session, doc);
        request.refinement = true;
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        for (int index = 0; index < 3; ++index) {
            run = std::get<ReviewSummary>(f.session.checkpointReview(
                checkpoint(run.id, run.revision, index, resultAt(request, *doc, index))));
        }
        auto cp = checkpoint(run.id, run.revision, 0, resultAt(request, *doc, 0));
        cp.stage = review::EvidenceStage::Deepening;
        cp.status = JobStatus::Deepening;
        QCOMPARE(std::get<ReviewError>(f.session.checkpointReview(cp)).code,
                 ReviewErrorCode::InvalidInput);
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_stage_positions")).toInt(),
                 0);
        cp.analysis->evidence.limit = engine::NodeLimit{1000000};
        auto provenance =
            std::make_shared<engine::AnalysisProvenance>(*cp.analysis->evidence.provenance);
        provenance->fingerprint.reportedName = QStringLiteral("Different engine");
        cp.analysis->evidence.provenance = provenance;
        QCOMPARE(std::get<ReviewError>(f.session.checkpointReview(cp)).code,
                 ReviewErrorCode::InvalidInput);
    }
    void emptyMovePageDoesNotHideDamagedEvidence_data() {
        QTest::addColumn<bool>("zeroMoves");
        QTest::newRow("empty game root") << true;
        QTest::newRow("end page") << false;
    }
    void emptyMovePageDoesNotHideDamagedEvidence() {
        QFETCH(bool, zeroMoves);
        Fixture f;
        const auto doc = document(zeroMoves ? "*" : "1. e4 e5 *");
        const auto request = seed(f.session, doc);
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        const int index = zeroMoves ? 0 : 2;
        run = std::get<ReviewSummary>(f.session.checkpointReview(
            checkpoint(run.id, 0, index, resultAt(request, *doc, index))));
        QVERIFY(f.exec(QStringLiteral("UPDATE review_evidence SET adapted=zeroblob(128)")));
        const auto page = f.session.deriveReviewMoves(run.id, run.revision, index, 50);
        QVERIFY(std::holds_alternative<ReviewError>(page));
        QCOMPARE(std::get<ReviewError>(page).code, ReviewErrorCode::InvalidRecord);
    }
    void savedModelOneRemainsReadableAndNewEvidenceUsesCurrentModel() {
        Fixture f;
        const auto doc = document();
        const auto request = seed(f.session, doc);
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        for (int index = 0; index < 2; ++index) {
            auto analysis = resultAt(request, *doc, index);
            analysis.evidence.lines.front().wdl =
                engine::Wdl{500 + 50 * index, 0, 500 - 50 * index};
            run = std::get<ReviewSummary>(
                f.session.checkpointReview(checkpoint(run.id, run.revision, index, analysis)));
        }
        auto evidence = std::get<StoredReviewEvidence>(f.session.loadReviewEvidence(run.id, 1));
        const auto position = positionAt(*doc, 0);
        const auto legacy = review::classify(position, *evidence.precedingMove,
                                             request.configuration.values.classification,
                                             request.configuration.values.conversion, 1);
        QCOMPARE(legacy.label, review::Classification::Good);
        const auto beforeId =
            f.scalar(QStringLiteral("SELECT before_id FROM review_classifications")).toLongLong();
        const auto afterId =
            f.scalar(QStringLiteral("SELECT after_id FROM review_classifications")).toLongLong();
        const auto bytes = classificationDocument(legacy, beforeId, afterId,
                                                  request.configuration.identities.classification);
        QSqlQuery q(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
        QVERIFY(q.prepare(QStringLiteral("UPDATE review_classifications SET document=?")));
        q.addBindValue(bytes);
        QVERIFY(q.exec());
        q.finish();
        const auto loaded =
            std::get<ReviewMovePage>(f.session.deriveReviewMoves(run.id, run.revision, 0, 50));
        QCOMPARE(loaded.moves.front().classification->modelVersion, 1U);
        QCOMPARE(loaded.moves.front().classification->label, review::Classification::Good);
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT document FROM review_classifications")).toByteArray(),
            bytes);
        auto fresh = resultAt(request, *doc, 1);
        fresh.evidence.lines.front().wdl = engine::Wdl{550, 0, 450};
        run = std::get<ReviewSummary>(
            f.session.checkpointReview(checkpoint(run.id, run.revision, 1, fresh)));
        const auto changed =
            std::get<ReviewMovePage>(f.session.deriveReviewMoves(run.id, run.revision, 0, 50));
        QCOMPARE(changed.moves.front().classification->modelVersion,
                 review::classificationModelVersion);
        QCOMPARE(changed.moves.front().classification->label, review::Classification::Inaccuracy);
    }
    void classificationsCommitWithEvidenceAndBackfillWithoutChangingProgress() {
        Fixture f;
        const auto doc = document();
        const auto request = seed(f.session, doc);
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        run = std::get<ReviewSummary>(
            f.session.checkpointReview(checkpoint(run.id, 0, 0, resultAt(request, *doc, 0))));
        auto rootOnly =
            std::get<ReviewMovePage>(f.session.deriveReviewMoves(run.id, run.revision, 0, 50));
        QCOMPARE(rootOnly.moves.size(), std::size_t(2));
        QVERIFY(!rootOnly.moves[0].classification);
        run = std::get<ReviewSummary>(
            f.session.checkpointReview(checkpoint(run.id, 1, 1, resultAt(request, *doc, 1))));
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_classifications")).toInt(),
                 1);
        const auto page =
            std::get<ReviewMovePage>(f.session.deriveReviewMoves(run.id, run.revision, 0, 50));
        QVERIFY(page.moves[0].classification);
        QCOMPARE(page.moves[0].classification->label, review::Classification::Excellent);
        QVERIFY(!page.moves[0].classification->playedMoveRank);
        QVERIFY(page.moves[0].classification->alternatives.empty());
        QCOMPARE(page.moves[0].san, QStringLiteral("e4"));
        QVERIFY(!page.moves[1].classification);
        const auto saved =
            f.scalar(QStringLiteral("SELECT document FROM review_classifications")).toByteArray();
        QVERIFY(f.exec(QStringLiteral("DELETE FROM review_classifications")));
        const auto backfilled = f.session.deriveReviewMoves(run.id, run.revision, 0, 50);
        QVERIFY(std::holds_alternative<ReviewMovePage>(backfilled));
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT document FROM review_classifications")).toByteArray(),
            saved);
        QVERIFY(std::holds_alternative<ReviewMovePage>(
            f.session.deriveReviewMoves(run.id, run.revision, 0, 50)));
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_classifications")).toInt(),
                 1);
        const auto loaded = std::get<StoredReview>(f.session.loadReview(run.id));
        QCOMPARE(loaded.summary.revision, run.revision);
        QCOMPARE(loaded.summary.completedPositions, 2);
        QCOMPARE(loaded.summary.status, run.status);
        QCOMPARE(std::get<ReviewError>(f.session.deriveReviewMoves(run.id, 0, 0, 50)).code,
                 ReviewErrorCode::Conflict);
        QCOMPARE(
            std::get<ReviewError>(f.session.deriveReviewMoves(run.id, run.revision, 0, 51)).code,
            ReviewErrorCode::InvalidInput);
    }
    void classificationFailureRollsBackCheckpoint() {
        Fixture f;
        const auto doc = document();
        const auto request = seed(f.session, doc);
        const auto run = std::get<ReviewSummary>(f.session.createReview(request));
        QVERIFY(std::holds_alternative<ReviewSummary>(
            f.session.checkpointReview(checkpoint(run.id, 0, 0, resultAt(request, *doc, 0)))));
        QVERIFY(f.exec(
            QStringLiteral("CREATE TRIGGER prevent_classification BEFORE INSERT ON "
                           "review_classifications BEGIN SELECT RAISE(ABORT,'fixture'); END")));
        QVERIFY(std::holds_alternative<ReviewError>(
            f.session.checkpointReview(checkpoint(run.id, 1, 1, resultAt(request, *doc, 1)))));
        const auto loaded = std::get<StoredReview>(f.session.loadReview(run.id));
        QCOMPARE(loaded.summary.revision, 1);
        QCOMPARE(loaded.summary.completedPositions, 1);
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_pairs")).toInt(), 0);
    }
    void corruptClassificationsAreNotRegenerated_data() {
        QTest::addColumn<bool>("unsupported");
        QTest::newRow("damaged") << false;
        QTest::newRow("future model") << true;
    }
    void corruptClassificationsAreNotRegenerated() {
        QFETCH(bool, unsupported);
        Fixture f;
        const auto doc = document();
        const auto request = seed(f.session, doc);
        const auto run = std::get<ReviewSummary>(f.session.createReview(request));
        QVERIFY(std::holds_alternative<ReviewSummary>(
            f.session.checkpointReview(checkpoint(run.id, 0, 0, resultAt(request, *doc, 0)))));
        QVERIFY(std::holds_alternative<ReviewSummary>(
            f.session.checkpointReview(checkpoint(run.id, 1, 1, resultAt(request, *doc, 1)))));
        if (unsupported) {
            auto document = *readReviewDocument(
                f.scalar(QStringLiteral("SELECT document FROM review_classifications"))
                    .toByteArray(),
                65536);
            document["modelVersion"] = 999;
            QSqlQuery q(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
            QVERIFY(q.prepare(QStringLiteral("UPDATE review_classifications SET document=?")));
            q.addBindValue(reviewDocument(document));
            QVERIFY(q.exec());
        } else {
            QVERIFY(
                f.exec(QStringLiteral("UPDATE review_classifications SET document=zeroblob(128)")));
        }
        const auto saved =
            f.scalar(QStringLiteral("SELECT document FROM review_classifications")).toByteArray();
        const auto result = f.session.deriveReviewMoves(run.id, 2, 0, 50);
        QVERIFY(std::holds_alternative<ReviewError>(result));
        QCOMPARE(std::get<ReviewError>(result).code, unsupported
                                                         ? ReviewErrorCode::UnsupportedVersion
                                                         : ReviewErrorCode::InvalidRecord);
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT document FROM review_classifications")).toByteArray(),
            saved);
    }
    void replacementCannotHideDamagedInterpretation() {
        Fixture f;
        const auto doc = document();
        const auto request = seed(f.session, doc);
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        for (int index = 0; index < 3; ++index) {
            run = std::get<ReviewSummary>(f.session.checkpointReview(
                checkpoint(run.id, run.revision, index, resultAt(request, *doc, index))));
        }
        auto document = *readReviewDocument(
            f.scalar(QStringLiteral(
                         "SELECT document FROM review_classifications WHERE position_index=2"))
                .toByteArray(),
            65536);
        document["ruleId"] = QStringLiteral("forged.rule");
        const auto bytes = reviewDocument(document);
        QSqlQuery q(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
        QVERIFY(q.prepare(
            QStringLiteral("UPDATE review_classifications SET document=? WHERE position_index=2")));
        q.addBindValue(bytes);
        QVERIFY(q.exec());
        q.finish();
        const auto result = f.session.checkpointReview(
            checkpoint(run.id, run.revision, 1, resultAt(request, *doc, 1)));
        QVERIFY(std::holds_alternative<ReviewError>(result));
        QCOMPARE(std::get<ReviewError>(result).code, ReviewErrorCode::InvalidRecord);
        QCOMPARE(std::get<StoredReview>(f.session.loadReview(run.id)).summary.revision,
                 run.revision);
        QCOMPARE(f.scalar(QStringLiteral(
                              "SELECT document FROM review_classifications WHERE position_index=2"))
                     .toByteArray(),
                 bytes);
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_evidence")).toInt(), 3);
    }
    void legacySettingsAndMigrationPreserveEvidence() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("legacy.sqlite3"));
        ReviewId id = 0;
        QByteArray settings;
        {
            StorageSession session;
            QVERIFY(!session.initialize(path, migrations(), ignore));
            const auto doc = document();
            auto request = seed(session, doc);
            request.configuration =
                *std::get<std::shared_ptr<const config::ValidatedConfiguration>>(
                    config::validate(request.configuration.values, 1));
            settings = encodeReviewSettings(request);
            QVERIFY(!settings.isEmpty());
            const auto decoded = decodeReviewSettings(settings);
            QVERIFY(decoded);
            QCOMPARE(decoded->configuration.classificationIdentityVersion, 1U);
            QCOMPARE(encodeReviewSettings(*decoded), settings);
            auto run = std::get<ReviewSummary>(session.createReview(request));
            id = run.id;
            for (int index = 0; index < 3; ++index) {
                run = std::get<ReviewSummary>(session.checkpointReview(
                    checkpoint(id, run.revision, index, resultAt(request, *doc, index))));
            }
            ReviewCheckpoint complete;
            complete.reviewId = id;
            complete.expectedRevision = run.revision;
            complete.status = JobStatus::Complete;
            QVERIFY(std::holds_alternative<ReviewSummary>(session.checkpointReview(complete)));
            QSqlQuery q(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
            QVERIFY(q.exec(QStringLiteral("DROP TABLE review_classifications")));
            QVERIFY(q.exec(QStringLiteral("DROP TABLE review_stage_positions")));
            QVERIFY(q.exec(QStringLiteral("DROP TABLE review_workspace")));
            QVERIFY(q.exec(QStringLiteral("DROP TABLE review_selection")));
            QVERIFY(q.exec(QStringLiteral("DROP TABLE review_moments")));
            QVERIFY(q.exec(QStringLiteral("DELETE FROM schema_migrations WHERE version>=7")));
        }
        StorageSession reopened;
        QVERIFY(!reopened.initialize(path, migrations(), ignore));
        const auto loaded = std::get<StoredReview>(reopened.loadReview(id));
        QCOMPARE(encodeReviewSettings(loaded.settings), settings);
        QCOMPARE(loaded.summary.status, JobStatus::Complete);
        const auto page = std::get<ReviewMovePage>(
            reopened.deriveReviewMoves(id, loaded.summary.revision, 0, 50));
        QCOMPARE(page.moves.size(), std::size_t(2));
        for (const auto& move : page.moves) {
            QVERIFY(move.classification);
            QCOMPARE(move.classification->thresholds.excellentLoss, .01);
        }
        QVERIFY(std::holds_alternative<StoredReviewEvidence>(reopened.loadReviewEvidence(id, 2)));
        QCOMPARE(QDir(directory.filePath(QStringLiteral("backups")))
                     .entryList({QStringLiteral("*.sqlite3")}, QDir::Files)
                     .size(),
                 1);
    }

    void orphanedJobRemainsVisibleAsDamaged() {
        Fixture f;
        const auto request = seed(f.session);
        const auto run = std::get<ReviewSummary>(f.session.createReview(request));
        QVERIFY(f.exec(QStringLiteral("DELETE FROM review_jobs")));
        const auto loaded = f.session.loadReview(run.id);
        QVERIFY(std::holds_alternative<ReviewError>(loaded));
        QCOMPARE(std::get<ReviewError>(loaded).code, ReviewErrorCode::InvalidRecord);
        const auto rows =
            std::get<std::vector<ReviewSummary>>(f.session.listReviews(request.gameId, 0, 50));
        QCOMPARE(rows.size(), std::size_t(1));
        QCOMPARE(rows[0].id, run.id);
        QVERIFY(rows[0].metadataError.has_value());
    }
    void orphanedEvidenceIsNotOrdinaryAbsence() {
        Fixture f;
        const auto doc = document();
        const auto request = seed(f.session, doc);
        const auto run = std::get<ReviewSummary>(f.session.createReview(request));
        QVERIFY(std::holds_alternative<ReviewSummary>(
            f.session.checkpointReview(checkpoint(run.id, 0, 0, resultAt(request, *doc, 0)))));
        QVERIFY(f.exec(QStringLiteral("PRAGMA foreign_keys=OFF")));
        QVERIFY(f.exec(QStringLiteral("UPDATE review_positions SET evidence_id=999999")));
        const auto loaded = f.session.loadReviewEvidence(run.id, 0);
        QVERIFY(std::holds_alternative<ReviewError>(loaded));
        QCOMPARE(std::get<ReviewError>(loaded).code, ReviewErrorCode::InvalidRecord);
    }
    void malformedCurrentTransitionCannotBeHidden_data() {
        QTest::addColumn<QString>("sql");
        QTest::newRow("cause without failure")
            << QStringLiteral("UPDATE review_transitions SET failure=1");
        QTest::newRow("initial predecessor")
            << QStringLiteral("UPDATE review_transitions SET previous=3");
        QTest::newRow("extra future transition")
            << QStringLiteral("INSERT INTO review_transitions SELECT review_id,1,0,1,0,created_at "
                              "FROM review_transitions WHERE revision=0");
    }
    void malformedCurrentTransitionCannotBeHidden() {
        QFETCH(QString, sql);
        Fixture f;
        const auto request = seed(f.session);
        const auto run = std::get<ReviewSummary>(f.session.createReview(request));
        QVERIFY(f.exec(sql));
        const auto loaded = f.session.loadReview(run.id);
        QVERIFY(std::holds_alternative<ReviewError>(loaded));
        QCOMPARE(std::get<ReviewError>(loaded).code, ReviewErrorCode::InvalidRecord);
    }
    void recoveryPreservesInvalidMetadata_data() {
        QTest::addColumn<QString>("sql");
        QTest::newRow("unknown review format")
            << QStringLiteral("UPDATE reviews SET format_version=99");
        QTest::newRow("damaged settings")
            << QStringLiteral("UPDATE reviews SET settings=zeroblob(128)");
        QTest::newRow("inconsistent transition")
            << QStringLiteral("UPDATE review_transitions SET status=9,failure=1");
    }
    void recoveryPreservesInvalidMetadata() {
        QFETCH(QString, sql);
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("db"));
        {
            StorageSession session;
            QVERIFY(!session.initialize(path, migrations(), ignore));
            const auto request = seed(session);
            QVERIFY(std::holds_alternative<ReviewSummary>(session.createReview(request)));
            QSqlQuery query(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
            QVERIFY(query.exec(sql));
        }
        StorageSession reopened;
        const auto failure = reopened.initialize(path, migrations(), ignore);
        QVERIFY(failure.has_value());
        QSqlQuery query(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
        QVERIFY(query.exec(QStringLiteral("SELECT status,revision FROM review_jobs")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 0);
        QCOMPARE(query.value(1).toLongLong(), qint64(0));
    }
    void historyPageChecksTransitionSemantics() {
        Fixture f;
        const auto request = seed(f.session);
        const auto run = std::get<ReviewSummary>(f.session.createReview(request));
        QVERIFY(f.exec(QStringLiteral("UPDATE review_transitions SET failure=1")));
        const auto history = f.session.listReviewTransitions(run.id, 0, 50);
        QVERIFY(std::holds_alternative<ReviewError>(history));
    }
    void historyChecksPredecessorsAcrossPageBoundaries() {
        Fixture f;
        const auto request = seed(f.session);
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        for (auto status : {JobStatus::QuickScanning, JobStatus::Reviewable, JobStatus::Failed}) {
            ReviewCheckpoint cp;
            cp.reviewId = run.id;
            cp.expectedRevision = run.revision;
            cp.status = status;
            if (status == JobStatus::Failed) {
                cp.failure = review::JobFailure::Engine;
            }
            run = std::get<ReviewSummary>(f.session.checkpointReview(cp));
        }
        run = std::get<ReviewSummary>(f.session.continueReview(
            {run.id, run.revision, request.fingerprint, request.configuration}));
        ReviewCheckpoint cp;
        cp.reviewId = run.id;
        cp.expectedRevision = run.revision;
        run = std::get<ReviewSummary>(f.session.checkpointReview(cp));
        const auto page = f.session.listReviewTransitions(run.id, 3, 2);
        QVERIFY(std::holds_alternative<std::vector<ReviewTransition>>(page));
        QCOMPARE(std::get<std::vector<ReviewTransition>>(page).size(), std::size_t(2));
        QVERIFY(f.exec(QStringLiteral("DELETE FROM review_transitions WHERE revision=2")));
        QCOMPARE(std::get<ReviewError>(f.session.listReviewTransitions(run.id, 2, 2)).code,
                 ReviewErrorCode::InvalidRecord);
        QVERIFY(f.exec(QStringLiteral("DELETE FROM review_transitions WHERE revision=4")));
        QCOMPARE(std::get<ReviewError>(f.session.loadReview(run.id)).code,
                 ReviewErrorCode::InvalidRecord);
    }
    void reviewStorageWorkloadMeasurement() {
        if (!qEnvironmentVariableIsSet("CHESS_REVIEW_REVIEW_STORAGE_BENCHMARK")) {
            QSKIP("Optional review storage workload measurement.");
        }
        std::string pgn;
        for (int turn = 1; turn <= 1000; turn += 2) {
            pgn += std::to_string(turn) + ". Nf3 Nf6 " + std::to_string(turn + 1) + ". Ng1 Ng8 ";
        }
        pgn += "*";
        Fixture f;
        const auto doc = document(pgn);
        const auto request = seed(f.session, doc);
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        const int finalIndex = static_cast<int>(doc->originalMainline().size());
        const auto result = resultAt(request, *doc, finalIndex);
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 20; ++i) {
            const auto saved =
                f.session.checkpointReview(checkpoint(run.id, run.revision, finalIndex, result));
            QVERIFY(std::holds_alternative<ReviewSummary>(saved));
            run = std::get<ReviewSummary>(saved);
        }
        qInfo("Twenty checkpoints at ply %d: %lld ms", finalIndex,
              static_cast<long long>(timer.elapsed()));
    }
    void runsKeepSettingsAndSides() {
        Fixture f;
        auto request = seed(f.session);
        auto a = f.session.createReview(request);
        QVERIFY(std::holds_alternative<ReviewSummary>(a));
        request.selectedSide = chess::Color::Black;
        auto b = f.session.createReview(request);
        QVERIFY(std::holds_alternative<ReviewSummary>(b));
        const auto id = std::get<ReviewSummary>(a).id;
        QVERIFY(id != std::get<ReviewSummary>(b).id);
        auto loaded = f.session.loadReview(id);
        QVERIFY(std::holds_alternative<StoredReview>(loaded));
        const auto& run = std::get<StoredReview>(loaded);
        QCOMPARE(run.summary.selectedSide, chess::Color::White);
        QCOMPARE(run.summary.totalPositions, 3);
        QVERIFY(sameConfiguration(run.settings.configuration, request.configuration));
        QCOMPARE(run.settings.fingerprint, request.fingerprint);
        QCOMPARE(std::get<std::vector<ReviewSummary>>(f.session.listReviews(request.gameId, 0, 1))
                     .size(),
                 std::size_t(1));
        auto invalidRequest = request;
        invalidRequest.configuration.identities.analysis[0] ^= 1;
        QCOMPARE(std::get<ReviewError>(f.session.createReview(invalidRequest)).code,
                 ReviewErrorCode::InvalidInput);
        invalidRequest = request;
        invalidRequest.configuration.values.classification.mistakeLoss = 0.001;
        QCOMPARE(std::get<ReviewError>(f.session.createReview(invalidRequest)).code,
                 ReviewErrorCode::InvalidInput);
    }
    void checkpointsPairAndSurviveCacheEviction() {
        Fixture f;
        const auto doc = document();
        const auto request = seed(f.session, doc);
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        const auto first = resultAt(request, *doc, 0);
        QVERIFY(std::holds_alternative<AnalysisStored>(f.session.storeAnalysis(first)));
        // Out-of-order completion pairs correctly when the missing neighbour arrives.
        for (const int index : {2, 0, 1}) {
            auto cp = checkpoint(run.id, run.revision, index,
                                 index == 0 ? first : resultAt(request, *doc, index));
            if (index == 1) {
                cp.status = JobStatus::Complete;
            }
            const auto saved = f.session.checkpointReview(cp);
            QVERIFY(std::holds_alternative<ReviewSummary>(saved));
            run = std::get<ReviewSummary>(saved);
        }
        QCOMPARE(run.completedPositions, 3);
        QCOMPARE(run.status, JobStatus::Complete);
        QVERIFY(f.exec(QStringLiteral("DELETE FROM analysis_results")));
        const auto loaded = f.session.loadReviewEvidence(run.id, 1);
        QVERIFY(std::holds_alternative<StoredReviewEvidence>(loaded));
        const auto& e = std::get<StoredReviewEvidence>(loaded);
        QVERIFY(e.precedingMove.has_value());
        QVERIFY(e.precedingMove->loss.has_value());
        QCOMPARE(*e.precedingMove->loss, 0.0);
        const auto root = std::get<StoredReviewEvidence>(f.session.loadReviewEvidence(run.id, 0));
        QCOMPARE(root.analysis->evidence.request, first.evidence.request);
        QCOMPARE(root.evidence.attemptIdentity,
                 first.evidence.request.toString(QUuid::WithoutBraces).toStdString());
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_evidence")).toInt(), 3);
    }
    void staleConflictingAndInsufficientCheckpoints() {
        Fixture f;
        const auto doc = document();
        const auto request = seed(f.session, doc);
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        const auto result = resultAt(request, *doc, 0);
        auto cp = checkpoint(run.id, 0, 0, result);
        run = std::get<ReviewSummary>(f.session.checkpointReview(cp));
        QCOMPARE(std::get<ReviewError>(f.session.checkpointReview(cp)).code,
                 ReviewErrorCode::Conflict);
        cp.expectedRevision = run.revision;
        run = std::get<ReviewSummary>(f.session.checkpointReview(cp));
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_evidence")).toInt(), 1);
        cp.expectedRevision = run.revision;
        cp.analysis->evidence.lines[0].score->value = 100;
        QCOMPARE(std::get<ReviewError>(f.session.checkpointReview(cp)).code,
                 ReviewErrorCode::Conflict);
        cp.analysis = result;
        cp.positionIndex = 1;
        QCOMPARE(std::get<ReviewError>(f.session.checkpointReview(cp)).code,
                 ReviewErrorCode::InvalidInput);
        cp.positionIndex = 0;
        cp.analysis->outcome = engine::AnalysisOutcome::Stopped;
        QCOMPARE(std::get<ReviewError>(f.session.checkpointReview(cp)).code,
                 ReviewErrorCode::InvalidInput);
        cp.analysis = result;
        cp.status = JobStatus::Complete;
        QCOMPARE(std::get<ReviewError>(f.session.checkpointReview(cp)).code,
                 ReviewErrorCode::InvalidInput);
        auto weak = resultAt(request, *doc, 1);
        weak.evidence.statistics.nodes = 1;
        cp = checkpoint(run.id, run.revision, 1, weak);
        QVERIFY(std::holds_alternative<ReviewSummary>(f.session.checkpointReview(cp)));
        const auto stored = std::get<StoredReviewEvidence>(f.session.loadReviewEvidence(run.id, 1));
        QVERIFY(!stored.evidence.complete);
        QVERIFY(!stored.precedingMove->loss);
    }
    void terminalRequiresLocalProof() {
        Fixture f;
        const auto doc = document("1. f3 e5 2. g4 Qh4# 0-1");
        const auto request = seed(f.session, doc);
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        ReviewCheckpoint cp;
        cp.reviewId = run.id;
        cp.positionIndex = 0;
        cp.terminal = true;
        QCOMPARE(std::get<ReviewError>(f.session.checkpointReview(cp)).code,
                 ReviewErrorCode::InvalidInput);
        cp.positionIndex = 4;
        run = std::get<ReviewSummary>(f.session.checkpointReview(cp));
        const auto stored = std::get<StoredReviewEvidence>(f.session.loadReviewEvidence(run.id, 4));
        QVERIFY(!stored.analysis);
        QCOMPARE(stored.evidence.terminal, review::TerminalOutcome::Checkmate);
        auto before = checkpoint(run.id, run.revision, 3, resultAt(request, *doc, 3));
        QVERIFY(std::holds_alternative<ReviewSummary>(f.session.checkpointReview(before)));
        QVERIFY(std::get<StoredReviewEvidence>(f.session.loadReviewEvidence(run.id, 4))
                    .precedingMove.has_value());
    }
    void terminalRootSupportsZeroMoveGame() {
        Fixture f;
        const auto doc =
            document("[SetUp \"1\"]\n[FEN \"rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w "
                     "KQkq - 1 3\"]\n\n0-1");
        const auto request = seed(f.session, doc);
        const auto run = std::get<ReviewSummary>(f.session.createReview(request));
        QCOMPARE(run.totalPositions, 1);
        ReviewCheckpoint cp;
        cp.reviewId = run.id;
        cp.positionIndex = 0;
        cp.terminal = true;
        cp.status = JobStatus::Complete;
        QVERIFY(std::holds_alternative<ReviewSummary>(f.session.checkpointReview(cp)));
        const auto stored = std::get<StoredReviewEvidence>(f.session.loadReviewEvidence(run.id, 0));
        QCOMPARE(stored.evidence.terminal, review::TerminalOutcome::Checkmate);
        QVERIFY(!stored.precedingMove);
    }
    void cancelledFailedAndPartialCanContinue_data() {
        QTest::addColumn<int>("status");
        for (auto status : {JobStatus::Cancelled, JobStatus::Failed, JobStatus::Partial}) {
            QTest::newRow(qPrintable(QString::number(static_cast<int>(status))))
                << static_cast<int>(status);
        }
    }
    void cancelledFailedAndPartialCanContinue() {
        QFETCH(int, status);
        Fixture f;
        const auto request = seed(f.session);
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        ReviewCheckpoint cp;
        cp.reviewId = run.id;
        cp.status = static_cast<JobStatus>(status);
        if (cp.status == JobStatus::Failed) {
            cp.failure = review::JobFailure::Engine;
        }
        run = std::get<ReviewSummary>(f.session.checkpointReview(cp));
        ContinueReview resume{run.id, run.revision, request.fingerprint, request.configuration};
        auto changed = resume;
        changed.configuration.values.conversion.preferWdl = false;
        QCOMPARE(std::get<ReviewError>(f.session.continueReview(changed)).code,
                 ReviewErrorCode::Conflict);
        changed = resume;
        changed.fingerprint.digest[0] ^= 1;
        QCOMPARE(std::get<ReviewError>(f.session.continueReview(changed)).code,
                 ReviewErrorCode::Conflict);
        auto continued = f.session.continueReview(resume);
        QVERIFY(std::holds_alternative<ReviewSummary>(continued));
        QCOMPARE(std::get<ReviewSummary>(continued).status, JobStatus::Queued);
        QCOMPARE(std::get<ReviewError>(f.session.continueReview(resume)).code,
                 ReviewErrorCode::Conflict);
        const auto history =
            std::get<std::vector<ReviewTransition>>(f.session.listReviewTransitions(run.id, 0, 50));
        QCOMPARE(history.size(), std::size_t(3));
        QCOMPARE(history[1].failure, cp.failure);
        QCOMPARE(history[2].previous, cp.status);
    }
    void restartInterruptsAllActivePhases() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("db"));
        CreateReview request;
        {
            StorageSession session;
            QVERIFY(!session.initialize(path, migrations(), ignore));
            request = seed(session);
            for (int status = 0; status <= 4; ++status) {
                auto run = std::get<ReviewSummary>(session.createReview(request));
                if (status > 0) {
                    ReviewCheckpoint cp;
                    cp.reviewId = run.id;
                    cp.status = static_cast<JobStatus>(status);
                    QVERIFY(std::holds_alternative<ReviewSummary>(session.checkpointReview(cp)));
                }
            }
            auto run = std::get<ReviewSummary>(session.createReview(request));
            ReviewCheckpoint cp;
            cp.reviewId = run.id;
            cp.status = JobStatus::Cancelled;
            QVERIFY(std::holds_alternative<ReviewSummary>(session.checkpointReview(cp)));
        }
        {
            StorageSession session;
            QVERIFY(!session.initialize(path, migrations(), ignore));
            const auto runs =
                std::get<std::vector<ReviewSummary>>(session.listReviews(request.gameId, 0, 50));
            QCOMPARE(runs.size(), std::size_t(6));
            QCOMPARE(runs[0].status, JobStatus::Cancelled);
            for (std::size_t i = 1; i < runs.size(); ++i) {
                QCOMPARE(runs[i].status, JobStatus::Interrupted);
            }
            const auto history = std::get<std::vector<ReviewTransition>>(
                session.listReviewTransitions(runs[1].id, 0, 50));
            QCOMPARE(history.back().previous, JobStatus::Deriving);
            auto resumed = session.continueReview(
                {runs[1].id, runs[1].revision, request.fingerprint, request.configuration});
            QVERIFY(std::holds_alternative<ReviewSummary>(resumed));
        }
        StorageSession again;
        QVERIFY(!again.initialize(path, migrations(), ignore));
        QCOMPARE(std::get<StoredReview>(again.loadReview(5)).summary.status,
                 JobStatus::Interrupted);
        QCOMPARE(std::get<StoredReview>(again.loadReview(1)).summary.revision, std::int64_t(1));
    }
    void failuresRollbackEvidenceProgressAndHistory() {
        Fixture f;
        const auto doc = document();
        const auto request = seed(f.session, doc);
        const auto run = std::get<ReviewSummary>(f.session.createReview(request));
        const auto cp = checkpoint(run.id, 0, 0, resultAt(request, *doc, 0));
        QVERIFY(f.exec(QStringLiteral("CREATE TRIGGER fail_evidence BEFORE INSERT ON "
                                      "review_evidence BEGIN SELECT RAISE(ABORT,'fixture'); END")));
        QCOMPARE(std::get<ReviewError>(f.session.checkpointReview(cp)).code,
                 ReviewErrorCode::Storage);
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_positions")).toInt(), 0);
        QVERIFY(f.exec(QStringLiteral("DROP TRIGGER fail_evidence")));
        QVERIFY(f.exec(QStringLiteral("CREATE TRIGGER fail_progress BEFORE UPDATE ON review_jobs "
                                      "BEGIN SELECT RAISE(ABORT,'fixture'); END")));
        QCOMPARE(std::get<ReviewError>(f.session.checkpointReview(cp)).code,
                 ReviewErrorCode::Storage);
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_evidence")).toInt(), 0);
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_transitions")).toInt(), 1);
        QVERIFY(f.exec(QStringLiteral("DROP TRIGGER fail_progress")));
        // Deferred foreign key failure occurs at COMMIT, after all writes have succeeded.
        QVERIFY(f.exec(QStringLiteral("CREATE TABLE commit_failure(value INTEGER REFERENCES "
                                      "games(id) DEFERRABLE INITIALLY DEFERRED)")));
        QVERIFY(f.exec(QStringLiteral("CREATE TRIGGER fail_commit AFTER UPDATE ON review_jobs "
                                      "BEGIN INSERT INTO commit_failure VALUES(999999); END")));
        QCOMPARE(std::get<ReviewError>(f.session.checkpointReview(cp)).code,
                 ReviewErrorCode::Storage);
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_positions")).toInt(), 0);
        QCOMPARE(std::get<StoredReview>(f.session.loadReview(run.id)).summary.revision,
                 std::int64_t(0));
        QVERIFY(f.exec(QStringLiteral("DROP TRIGGER fail_commit")));
        QVERIFY(std::holds_alternative<ReviewSummary>(f.session.checkpointReview(cp)));
    }
    void malformedRecordsArePreserved_data() {
        QTest::addColumn<QString>("sql");
        QTest::addColumn<int>("code");
        QTest::newRow("raw corruption")
            << QStringLiteral("UPDATE review_evidence SET analysis=zeroblob(128)")
            << static_cast<int>(ReviewErrorCode::InvalidRecord);
        QTest::newRow("raw oversized")
            << QStringLiteral("UPDATE review_evidence SET analysis=zeroblob(16777217)")
            << static_cast<int>(ReviewErrorCode::InvalidRecord);
        QTest::newRow("adapted corruption")
            << QStringLiteral("UPDATE review_evidence SET adapted=zeroblob(128)")
            << static_cast<int>(ReviewErrorCode::InvalidRecord);
        QTest::newRow("future evidence")
            << QStringLiteral("UPDATE review_evidence SET format_version=99")
            << static_cast<int>(ReviewErrorCode::UnsupportedVersion);
        QTest::newRow("settings corruption")
            << QStringLiteral("UPDATE reviews SET settings=zeroblob(128)")
            << static_cast<int>(ReviewErrorCode::InvalidRecord);
        QTest::newRow("future review") << QStringLiteral("UPDATE reviews SET format_version=99")
                                       << static_cast<int>(ReviewErrorCode::UnsupportedVersion);
        QTest::newRow("mainline corruption")
            << QStringLiteral("UPDATE reviews SET mainline_identity=x'01'")
            << static_cast<int>(ReviewErrorCode::InvalidRecord);
    }
    void malformedRecordsArePreserved() {
        QFETCH(QString, sql);
        QFETCH(int, code);
        Fixture f;
        const auto doc = document();
        const auto request = seed(f.session, doc);
        const auto run = std::get<ReviewSummary>(f.session.createReview(request));
        QVERIFY(std::holds_alternative<ReviewSummary>(
            f.session.checkpointReview(checkpoint(run.id, 0, 0, resultAt(request, *doc, 0)))));
        QVERIFY(f.exec(sql));
        const auto loaded = f.session.loadReviewEvidence(run.id, 0);
        QVERIFY(std::holds_alternative<ReviewError>(loaded));
        QCOMPARE(static_cast<int>(std::get<ReviewError>(loaded).code), code);
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_evidence")).toInt(), 1);
    }
    void completedRunReopensWithoutEngineOrCache() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("db"));
        const auto doc = document();
        ReviewId id = 0;
        {
            StorageSession session;
            QVERIFY(!session.initialize(path, migrations(), ignore));
            const auto request = seed(session, doc);
            auto run = std::get<ReviewSummary>(session.createReview(request));
            id = run.id;
            for (int index = 0; index < 3; ++index) {
                auto cp = checkpoint(run.id, run.revision, index, resultAt(request, *doc, index));
                if (index == 2) {
                    cp.status = JobStatus::Complete;
                }
                run = std::get<ReviewSummary>(session.checkpointReview(cp));
            }
        }
        StorageSession reopened;
        QVERIFY(!reopened.initialize(path, migrations(), ignore));
        QCOMPARE(std::get<StoredReview>(reopened.loadReview(id)).summary.status,
                 JobStatus::Complete);
        const auto evidence = reopened.loadReviewEvidence(id, 2);
        QVERIFY(std::holds_alternative<StoredReviewEvidence>(evidence));
        QVERIFY(std::get<StoredReviewEvidence>(evidence).precedingMove.has_value());
    }
    void malformedScalarColumnsAreRejectedBeforeMaterialization() {
        Fixture f;
        const auto request = seed(f.session);
        const auto run = std::get<ReviewSummary>(f.session.createReview(request));
        QVERIFY(f.exec(QStringLiteral("PRAGMA ignore_check_constraints=ON")));
        QVERIFY(f.exec(QStringLiteral("UPDATE review_jobs SET revision=zeroblob(16777217)")));
        QCOMPARE(std::get<ReviewError>(f.session.loadReview(run.id)).code,
                 ReviewErrorCode::InvalidRecord);
        const auto listed =
            std::get<std::vector<ReviewSummary>>(f.session.listReviews(request.gameId, 0, 50));
        QCOMPARE(listed.size(), std::size_t(1));
        QCOMPARE(listed[0].id, run.id);
        QCOMPARE(listed[0].metadataError, ReviewErrorCode::InvalidRecord);
        QVERIFY(f.exec(QStringLiteral("UPDATE review_jobs SET revision=0")));
        QVERIFY(f.exec(QStringLiteral("UPDATE review_transitions SET failure=zeroblob(16777217)")));
        QCOMPARE(std::get<ReviewError>(f.session.listReviewTransitions(run.id, 0, 50)).code,
                 ReviewErrorCode::InvalidRecord);
    }
    void replacementRefreshesBothPairsAndPreservesAttempts() {
        Fixture f;
        const auto doc = document();
        const auto request = seed(f.session, doc);
        auto run = std::get<ReviewSummary>(f.session.createReview(request));
        for (int index = 0; index < 3; ++index) {
            run = std::get<ReviewSummary>(f.session.checkpointReview(
                checkpoint(run.id, run.revision, index, resultAt(request, *doc, index))));
        }
        auto stronger = resultAt(request, *doc, 1);
        stronger.evidence.limit = engine::NodeLimit{200000};
        stronger.evidence.statistics.nodes = 200000;
        stronger.evidence.lines[0].statistics.nodes = 200000;
        stronger.evidence.lines[0].score->value = 100;
        const auto cp = checkpoint(run.id, run.revision, 1, stronger);
        QVERIFY(std::holds_alternative<ReviewSummary>(f.session.checkpointReview(cp)));
        const auto middle = std::get<StoredReviewEvidence>(f.session.loadReviewEvidence(run.id, 1));
        const auto final = std::get<StoredReviewEvidence>(f.session.loadReviewEvidence(run.id, 2));
        QCOMPARE(middle.analysis->evidence.request, stronger.evidence.request);
        QVERIFY(middle.precedingMove.has_value());
        QVERIFY(final.precedingMove.has_value());
        QCOMPARE(final.precedingMove->before.attemptIdentity, middle.evidence.attemptIdentity);
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_evidence")).toInt(), 4);
        QCOMPARE(f.scalar(QStringLiteral("SELECT count(*) FROM review_positions")).toInt(), 3);
        QVERIFY(f.exec(
            QStringLiteral("UPDATE review_pairs SET paired=zeroblob(64) WHERE position_index=2")));
        QCOMPARE(std::get<ReviewError>(f.session.loadReviewEvidence(run.id, 2)).code,
                 ReviewErrorCode::InvalidRecord);
    }
    void unsupportedModelsAndForgedSettingsAreRejected() {
        Fixture f;
        const auto request = seed(f.session);
        const auto run = std::get<ReviewSummary>(f.session.createReview(request));
        auto object = *readReviewDocument(encodeReviewSettings(request), maxReviewSettingsBytes);
        object["evidenceModelVersion"] = 99;
        const auto update = [&](const QByteArray& bytes) {
            QSqlQuery query(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
            if (!query.prepare(QStringLiteral("UPDATE reviews SET settings=? WHERE id=1"))) {
                return false;
            }
            query.addBindValue(bytes);
            return query.exec();
        };
        QVERIFY(update(reviewDocument(object)));
        QCOMPARE(std::get<ReviewError>(f.session.loadReview(run.id)).code,
                 ReviewErrorCode::UnsupportedVersion);
        object["evidenceModelVersion"] = 1;
        object["analysisIdentity"] = QString(64, QChar('0'));
        QVERIFY(update(reviewDocument(object)));
        QCOMPARE(std::get<ReviewError>(f.session.loadReview(run.id)).code,
                 ReviewErrorCode::InvalidRecord);
        QVERIFY(update(encodeReviewSettings(request)));
        QVERIFY(std::holds_alternative<StoredReview>(f.session.loadReview(run.id)));
    }
    void failedReviewMigrationRollsBack() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("db"));
        CreateReview request;
        const auto old = std::span<const Migration>(migrations()).first(5);
        {
            StorageSession session;
            QVERIFY(!session.initialize(path, old, ignore));
            request = seed(session);
        }
        auto broken = migrations();
        broken[5].statements.append(
            QStringLiteral("INSERT INTO nonexistent_review_table VALUES(1)"));
        {
            StorageSession session;
            const auto failure = session.initialize(path, broken, ignore);
            QVERIFY(failure);
            QCOMPARE(failure->migrationVersion, std::optional<int>(6));
            QVERIFY(QFileInfo::exists(failure->backupPath));
        }
        StorageSession original;
        QVERIFY(!original.initialize(path, old, ignore));
        QVERIFY(std::holds_alternative<StoredGame>(original.loadGame(request.gameId)));
        QSqlQuery query(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
        QVERIFY(
            query.exec(QStringLiteral("SELECT count(*) FROM sqlite_schema WHERE name='reviews'")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 0);
    }
    void recoveryFailureRollsBackEveryJob() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("db"));
        {
            StorageSession session;
            QVERIFY(!session.initialize(path, migrations(), ignore));
            const auto request = seed(session);
            QVERIFY(std::holds_alternative<ReviewSummary>(session.createReview(request)));
            QVERIFY(std::holds_alternative<ReviewSummary>(session.createReview(request)));
            QSqlQuery query(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
            QVERIFY(query.exec(QStringLiteral(
                "CREATE TRIGGER fail_recovery BEFORE INSERT ON review_transitions WHEN "
                "NEW.review_id=2 AND NEW.status=8 BEGIN SELECT RAISE(ABORT,'fixture'); END")));
        }
        {
            StorageSession session;
            QVERIFY(session.initialize(path, migrations(), ignore));
            QSqlQuery query(QSqlDatabase::database(QSqlDatabase::connectionNames().front()));
            QVERIFY(query.exec(
                QStringLiteral("SELECT count(*) FROM review_jobs WHERE status=0 AND revision=0")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 2);
            query.finish();
            QVERIFY(query.exec(QStringLiteral("DROP TRIGGER fail_recovery")));
        }
        StorageSession recovered;
        QVERIFY(!recovered.initialize(path, migrations(), ignore));
        QCOMPARE(std::get<StoredReview>(recovered.loadReview(1)).summary.status,
                 JobStatus::Interrupted);
    }
    void upgradePreservesExistingDataAndRejectsOlderRegistry() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("db"));
        CreateReview request;
        const auto old = std::span<const Migration>(migrations()).first(5);
        {
            StorageSession session;
            QVERIFY(!session.initialize(path, old, ignore));
            request = seed(session);
            QVERIFY(std::holds_alternative<AnalysisStored>(
                session.storeAnalysis(resultAt(request, *document(), 0))));
        }
        {
            StorageSession session;
            QVERIFY(!session.initialize(path, migrations(), ignore));
            QVERIFY(std::holds_alternative<StoredGame>(session.loadGame(request.gameId)));
            QCOMPARE(
                std::get<std::vector<engine::EngineProfile>>(session.listEngines(0, 50)).size(),
                std::size_t(1));
            QVERIFY(std::holds_alternative<ReviewSummary>(session.createReview(request)));
        }
        QCOMPARE(QDir(directory.filePath(QStringLiteral("backups")))
                     .entryList({QStringLiteral("*.sqlite3")}, QDir::Files)
                     .size(),
                 1);
        StorageSession oldBinary;
        const auto failure = oldBinary.initialize(path, old, ignore);
        QVERIFY(failure.has_value());
        QCOMPARE(failure->code, DatabaseErrorCode::Schema);
    }
    void crashRetainsCommittedCheckpoint() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("db"));
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--crash-writer"), path});
        QVERIFY(child.waitForStarted(3000));
        QByteArray output;
        QElapsedTimer elapsed;
        elapsed.start();
        while (!output.contains("CHECKPOINT") && elapsed.elapsed() < 10000 &&
               child.state() != QProcess::NotRunning) {
            child.waitForReadyRead(100);
            output += child.readAllStandardOutput();
        }
        if (!output.contains("CHECKPOINT")) {
            child.kill();
            child.waitForFinished();
            QFAIL("Crash writer did not commit checkpoint");
        }
        child.kill();
        QVERIFY(child.waitForFinished(3000));
        QCOMPARE(child.exitStatus(), QProcess::CrashExit);
        StorageSession session;
        QVERIFY(!session.initialize(path, migrations(), ignore));
        const auto loaded = session.loadReview(1);
        QVERIFY(std::holds_alternative<StoredReview>(loaded));
        const auto run = std::get<StoredReview>(loaded);
        QCOMPARE(run.summary.status, JobStatus::Interrupted);
        QCOMPARE(run.summary.completedPositions, 1);
        const auto context = std::get<ReviewContext>(session.loadReviewContext(0, 0));
        QCOMPARE(context.reviewId, std::optional<ReviewId>{run.summary.id});
        QCOMPARE(context.position, std::optional<int>{1});
        QVERIFY(context.reopen);
        QVERIFY(std::holds_alternative<StoredReviewEvidence>(session.loadReviewEvidence(1, 0)));
        QVERIFY(std::holds_alternative<ReviewError>(session.loadReviewEvidence(1, 1)));
    }
    void asynchronousAdmissionCorrelationAndDrainage() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("db"));
        CreateReview request;
        {
            StorageSession session;
            QVERIFY(!session.initialize(path, migrations(), ignore));
            request = seed(session);
        }
        AsyncDatabase db;
        QSignalSpy ready(&db.value, &Database::ready);
        QSignalSpy created(&db.value, &Database::reviewCreated);
        QSignalSpy loaded(&db.value, &Database::reviewLoaded);
        const auto unavailable = QUuid::createUuid();
        db.value.loadReview(unavailable, 1);
        QTRY_COMPARE(loaded.size(), 1);
        QCOMPARE(loaded[0][0].toUuid(), unavailable);
        QCOMPARE(std::get<ReviewError>(qvariant_cast<LoadReviewResult>(loaded[0][1])).code,
                 ReviewErrorCode::Unavailable);
        db.value.initialize(path);
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(ready[0][0].toInt(), 9);
        std::vector<QUuid> ids;
        for (int i = 0; i < 17; ++i) {
            ids.push_back(QUuid::createUuid());
            db.value.createReview(ids.back(), request);
        }
        int ticks = 0;
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, &db.value, [&] { ++ticks; });
        heartbeat.start(1);
        db.value.close();
        db.value.loadReview(QUuid::createUuid(), 1);
        QTRY_VERIFY(db.closed);
        QCOMPARE(created.size(), 17);
        QVERIFY(ticks > 0);
        int saved = 0, busy = 0;
        for (const auto& row : created) {
            QVERIFY(std::find(ids.begin(), ids.end(), row[0].toUuid()) != ids.end());
            const auto result = qvariant_cast<CreateReviewResult>(row[1]);
            if (std::holds_alternative<ReviewSummary>(result)) {
                ++saved;
            } else {
                QCOMPARE(std::get<ReviewError>(result).code, ReviewErrorCode::Busy);
                ++busy;
            }
        }
        QCOMPARE(saved, 16);
        QCOMPARE(busy, 1);
        QTRY_COMPARE(loaded.size(), 2);
        QCOMPARE(std::get<ReviewError>(qvariant_cast<LoadReviewResult>(loaded[1][1])).code,
                 ReviewErrorCode::Closing);
    }
};
int main(int argc, char** argv) {
    TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QCoreApplication application(argc, argv);
    if (application.arguments().size() == 3 &&
        application.arguments()[1] == QStringLiteral("--crash-writer")) {
        StorageSession session;
        if (session.initialize(application.arguments()[2], migrations(), ignore)) {
            return 2;
        }
        const auto doc = document();
        const auto request = seed(session, doc);
        const auto run = std::get<ReviewSummary>(session.createReview(request));
        if (!std::holds_alternative<ReviewSummary>(
                session.checkpointReview(checkpoint(run.id, 0, 0, resultAt(request, *doc, 0))))) {
            return 3;
        }
        if (!std::holds_alternative<std::monostate>(
                session.saveReviewContext({request.gameId, run.id, 1, true}))) {
            return 4;
        }
        std::fputs("CHECKPOINT\n", stdout);
        std::fflush(stdout);
        return application.exec();
    }
    ReviewRepositoryTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ReviewRepositoryTest.moc"
