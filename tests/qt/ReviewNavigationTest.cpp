#include "TestEnvironment.h"
#include "app/ReviewNavigationController.h"
#include "ui/chess/MoveTimeline.h"
#include <QAbstractItemModelTester>
#include <QApplication>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <limits>

using namespace chess_review;
namespace {
class Fixture {
  public:
    Fixture() : moves(database, false), nav(database, moves) {
        QSignalSpy ready(&database, &persistence::Database::ready);
        database.initialize(directory.filePath(QStringLiteral("review.sqlite3")));
        if (!ready.wait()) {
            qFatal("Database did not initialize");
        }
    }
    ~Fixture() {
        QSignalSpy closed(&nav, &app::ReviewNavigationController::closed);
        nav.close();
        if (closed.isEmpty() && !closed.wait()) {
            qFatal("Navigation did not close");
        }
        moves.close();
        if (!QTest::qWaitFor([this] { return moves.drained(); })) {
            qFatal("Evidence did not drain");
        }
        QSignalSpy databaseClosed(&database, &persistence::Database::closed);
        database.close();
        if (databaseClosed.isEmpty() && !databaseClosed.wait()) {
            qFatal("Database did not close");
        }
    }
    void open(const std::string& text, persistence::GameId id = 1) {
        pgn::PgnReader reader(text);
        auto result = reader.next();
        if (!std::holds_alternative<pgn::PgnDocument>(result)) {
            qFatal("Invalid fixture PGN");
        }
        persistence::StoredGame game;
        game.summary.id = id;
        game.summary.white = QStringLiteral("White player");
        game.summary.black = QStringLiteral("Black player");
        game.document =
            std::make_shared<const pgn::PgnDocument>(std::get<pgn::PgnDocument>(std::move(result)));
        nav.open(game);
    }
    QTemporaryDir directory;
    persistence::Database database;
    app::ReviewMovesController moves;
    app::ReviewNavigationController nav;
};
} // namespace
class ReviewNavigationTest final : public QObject {
    Q_OBJECT
  private slots:
    void expectedScorePresentationFailsClosed() {
        persistence::ReviewMove move;
        QVERIFY(!app::reviewPositionEvaluation(&move, false).whiteExpected);
        move.classification.emplace();
        auto& result = *move.classification;
        result.before.expected = review::ExpectedScore{0.25,
                                                       chess::Color::Black,
                                                       review::ConversionSource::NativeWdl,
                                                       review::expectedScoreModelVersion,
                                                       {}};
        result.after.expected =
            review::ExpectedScore{0.1,
                                  chess::Color::Black,
                                  review::ConversionSource::ApproximateCentipawns,
                                  review::expectedScoreModelVersion,
                                  {}};
        auto view = app::reviewPositionEvaluation(&move, true);
        QCOMPARE(view.whiteExpected, std::optional{0.75});
        QVERIFY(!view.approximate);
        view = app::reviewPositionEvaluation(&move, false);
        QCOMPARE(view.whiteExpected, std::optional{0.9});
        QVERIFY(view.approximate);
        QVERIFY(view.description.contains(QStringLiteral("approximate")));
        result.after.issue = review::EvidenceIssue::Bounded;
        QVERIFY(!app::reviewPositionEvaluation(&move, false).whiteExpected);
        result.after.issue = review::EvidenceIssue::MateClaim;
        QVERIFY(!app::reviewPositionEvaluation(&move, false).whiteExpected);
        result.after.issue.reset();
        result.after.expected->value = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!app::reviewPositionEvaluation(&move, false).whiteExpected);
        result.after.expected->value = 1.1;
        QVERIFY(!app::reviewPositionEvaluation(&move, false).whiteExpected);
        result.after.expected->value = 1.0;
        result.after.expected->source = review::ConversionSource::Terminal;
        result.after.expected->perspective = chess::Color::White;
        QCOMPARE(app::reviewPositionEvaluation(&move, false).whiteExpected, std::optional{1.0});
        result.after.expected->modelVersion = 999;
        QVERIFY(!app::reviewPositionEvaluation(&move, false).whiteExpected);
    }
    void previewRestoresCommittedAndRejectsStaleTargets() {
        Fixture f;
        f.open("1. e4 (1. d4 d5) e5 2. Nf3 Nc6 *");
        QTRY_VERIFY(f.nav.ready());
        QCOMPARE(f.nav.line().size(), std::size_t{5});
        f.nav.select(f.nav.target(1));
        QTRY_COMPARE(f.nav.displayed(), 1);
        const auto committed = f.nav.board().pieces;
        QVERIFY(!f.nav.board().interactive);
        f.nav.preview(f.nav.target(4));
        QCOMPARE(f.nav.selected(), 1);
        QTRY_COMPARE(f.nav.displayed(), 4);
        QVERIFY(f.nav.previewing());
        f.nav.clearPreview();
        QTRY_COMPARE(f.nav.displayed(), 1);
        QCOMPARE(f.nav.board().pieces, committed);
        f.nav.preview(f.nav.target(2));
        f.nav.select(f.nav.target(3));
        f.nav.preview(f.nav.target(4));
        f.nav.clearPreview();
        QTRY_COMPARE(f.nav.displayed(), 3);
        QVERIFY(!f.nav.previewing());
        const auto stale = f.nav.target(2);
        f.open("1. d4 d5 *", 2);
        QTRY_VERIFY(f.nav.ready());
        f.nav.select(stale);
        f.nav.preview(stale);
        QCOMPARE(f.nav.selected(), 0);
        QVERIFY(!f.nav.previewing());
        f.nav.end();
        QTRY_COMPARE(f.nav.displayed(), 2);
        QVERIFY(!f.nav.evaluation().whiteExpected);
    }
    void keyMomentEntryAndNavigationAcrossPages() {
        Fixture f;
        std::string text;
        for (int i = 0; i < 60; ++i) {
            text += "Nf3 Nf6 Ng1 Ng8 ";
        }
        f.open(text + "*");
        QTRY_VERIFY(f.nav.ready());
        persistence::ReviewSummary run;
        run.id = 42;
        run.revision = 1;
        run.totalPositions = 241;
        run.selectedSide = chess::Color::Black;
        persistence::ReviewOverview overview;
        overview.reviewId = run.id;
        overview.revision = run.revision;
        for (int i = 2; i <= 240; i += 2) {
            overview.keyPositions.push_back(i);
        }
        overview.totalKeyMoments = static_cast<int>(overview.keyPositions.size());
        f.nav.bind(run, overview);
        QTRY_COMPARE(f.nav.displayed(), 2);
        QVERIFY(f.nav.board().blackAtBottom);
        f.nav.select(f.nav.target(199));
        f.nav.nextMoment();
        QTRY_COMPARE(f.nav.displayed(), 200);
        f.nav.previousMoment();
        QTRY_COMPARE(f.nav.displayed(), 198);
        f.nav.flip();
        QVERIFY(!f.nav.board().blackAtBottom);
        run.revision = 2;
        overview.keyPositions.insert(overview.keyPositions.begin(), 1);
        f.nav.bind(run, overview);
        QCOMPARE(f.nav.selected(), 198);
        QVERIFY(!f.nav.board().blackAtBottom);
        QVERIFY(f.nav.keyMoment(200));
        QVERIFY(!f.nav.keyMoment(199));
    }
    void manualNavigationWinsOverLateSummary() {
        Fixture f;
        f.open("1. e4 e5 2. Nf3 Nc6 *");
        QTRY_VERIFY(f.nav.ready());
        persistence::ReviewSummary run;
        run.id = 1;
        run.totalPositions = 5;
        f.nav.bind(run, std::nullopt);
        f.nav.select(f.nav.target(4));
        persistence::ReviewOverview overview;
        overview.reviewId = 1;
        overview.keyPositions = {1, 3};
        f.nav.bind(run, overview);
        QTRY_COMPARE(f.nav.displayed(), 4);
        QCOMPARE(f.nav.selected(), 4);
    }
    void fenNumberingAndEmptyGame() {
        Fixture f;
        f.open("[SetUp \"1\"]\n[FEN \"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 "
               "24\"]\n24... e5 25. e4 *");
        QTRY_VERIFY(f.nav.ready());
        const auto& entry = f.nav.tree()->nodes.at(f.nav.line()[1]);
        QCOMPARE(entry.number, std::uint32_t{24});
        QCOMPARE(entry.side, chess::Color::Black);
        f.nav.end();
        QTRY_COMPARE(f.nav.displayed(), 2);
        f.open("*");
        QTRY_VERIFY(f.nav.ready());
        QCOMPARE(f.nav.line().size(), std::size_t{1});
        f.nav.next();
        f.nav.previous();
        f.nav.end();
        QCOMPARE(f.nav.selected(), 0);
        QVERIFY(!f.nav.canNextMoment());
    }
    void timelineHoverKeyboardAndHide() {
        Fixture f;
        f.open("1. e4 e5 2. Nf3 Nc6 *");
        QTRY_VERIFY(f.nav.ready());
        ui::MoveTimeline timeline;
        timeline.setHoverPreview(true);
        timeline.setSnapshot(f.nav.tree(), f.nav.line(), f.nav.target(0).session, f.nav.line()[0],
                             {}, true);
        QAbstractItemModelTester tester(timeline.model(),
                                        QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy previews(&timeline, &ui::MoveTimeline::previewRequested);
        QSignalSpy cleared(&timeline, &ui::MoveTimeline::previewCleared);
        QSignalSpy next(&timeline, &ui::MoveTimeline::nextRequested);
        timeline.resize(600, 150);
        timeline.show();
        QTest::qWait(10);
        const auto rect = timeline.visualRect(timeline.model()->index(1, 1));
        QVERIFY(!rect.isEmpty());
        QTest::mouseMove(timeline.viewport(), rect.center());
        QTRY_VERIFY(!previews.isEmpty());
        QTest::keyPress(&timeline, Qt::Key_Right);
        QCOMPARE(next.count(), 1);
        QVERIFY(!cleared.isEmpty());
        QTest::mouseMove(timeline.viewport(), rect.center() + QPoint(2, 0));
        const auto before = cleared.count();
        timeline.hide();
        QVERIFY(cleared.count() > before);
    }
    void rapidLongGameNavigationKeepsHeartbeat() {
        Fixture f;
        std::string text;
        for (int i = 0; i < 128; ++i) {
            text += "Nf3 Nf6 Ng1 Ng8 ";
        }
        int beats = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&beats] { ++beats; });
        heartbeat.start(1);
        f.open(text + "*");
        QTRY_VERIFY(f.nav.ready());
        QCOMPARE(f.nav.line().size(), std::size_t{513});
        for (int i = 0; i < 100; ++i) {
            f.nav.preview(f.nav.target(i * 5));
            f.nav.select(f.nav.target(512 - i));
        }
        f.nav.end();
        QTRY_COMPARE(f.nav.displayed(), 512);
        QVERIFY(beats > 0);
    }
};
int main(int argc, char* argv[]) {
    const TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    ReviewNavigationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ReviewNavigationTest.moc"
