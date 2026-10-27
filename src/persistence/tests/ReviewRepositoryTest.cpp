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
    void init() { QTest::failOnWarning(); }
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
        broken.back().statements.append(
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
        QCOMPARE(ready[0][0].toInt(), 6);
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
        std::fputs("CHECKPOINT\n", stdout);
        std::fflush(stdout);
        return application.exec();
    }
    ReviewRepositoryTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ReviewRepositoryTest.moc"
