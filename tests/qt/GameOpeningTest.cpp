#include "TestEnvironment.h"
#include "app/StartupCoordinator.h"
#include "ui/analysis/AnalysisPage.h"
#include "ui/shell/MainWindow.h"
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace chess_review;
namespace {
// Morphy–Duke of Brunswick/Count Isouard, Paris 1858 (the Opera Game),
// with test annotations and a legal alternate second move.
const QString games = QStringLiteral(R"pgn([Event "Opera Game"]
[White "Paul Morphy"]
[Black "Duke / Count"]
[Result "1-0"]

{Paris 1858} 1. e4 e5 2. Nf3 (2. Bc4 $5 {Alternative <b>line</b>} Nf6)
2... d6 3. d4 Bg4 4. dxe5 Bxf3 5. Qxf3 dxe5 6. Bc4 Nf6
7. Qb3 Qe7 8. Nc3 c6 9. Bg5 b5 10. Nxb5 cxb5 11. Bxb5+ Nbd7
12. O-O-O Rd8 13. Rxd7 Rxd7 14. Rd1 Qe6 15. Bxd7+ Nxd7
16. Qb8+ Nxb8 17. Rd8# 1-0

[White "En passant"]
[SetUp "1"]
[FEN "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2"]
2. exd6 Kd7 *

[White "Promotion"]
[SetUp "1"]
[FEN "7k/P7/8/8/8/8/8/7K w - - 0 43"]
43. a8=N Kh7 *
)pgn");
struct Fixture {
    app::StartupCoordinator startup;
    app::NavigationController navigation;
    ui::MainWindow window{navigation, startup.analysis()};
    bool closed = false;
    Fixture(const QString& database, const QString& preferences) {
        window.installImports(startup.localImports(), startup.library());
        QObject::connect(&startup, &app::StartupCoordinator::initialUiState, &window,
                         &ui::MainWindow::restoreUiState);
        QObject::connect(&startup, &app::StartupCoordinator::ready, &window,
                         &ui::MainWindow::showReady);
        QObject::connect(&window, &ui::MainWindow::closeRequested, &startup,
                         &app::StartupCoordinator::shutdown);
        QObject::connect(&startup, &app::StartupCoordinator::stopping, &window, [this] {
            startup.setFinalUiState(window.uiState());
            window.showClosing();
        });
        QObject::connect(&startup, &app::StartupCoordinator::finished, &window, [this] {
            closed = true;
            window.completeShutdown();
        });
        startup.start(database, preferences);
        window.show();
    }
    ~Fixture() {
        startup.shutdown();
        QElapsedTimer deadline;
        deadline.start();
        while (!closed && deadline.elapsed() < 10000) {
            QTest::qWait(1);
        }
        if (!closed) {
            qFatal("Game-opening fixture did not close");
        }
    }
};
} // namespace
class GameOpeningTest final : public QObject {
    Q_OBJECT
  private slots:
    void importNavigateAndReopen();
    void productionRestart();
    void resumeFallbacks_data();
    void resumeFallbacks();
    void recoveryPreservesNewWork();
    void failedAndStaleLoads();
    void replacementAndDiscard();
};
void GameOpeningTest::importNavigateAndReopen() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto database = directory.filePath(QStringLiteral("games.sqlite3"));
    const auto preferences = directory.filePath(QStringLiteral("ui.ini"));
    std::optional<persistence::GameId> savedId;
    chess::NodeId branch{0};
    {
        Fixture fixture(database, preferences);
        auto& analysis = fixture.startup.analysis();
        auto& imports = fixture.startup.localImports();
        QTRY_VERIFY(imports.available());
        const auto source = directory.filePath(QStringLiteral("real-games.pgn"));
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(games.toUtf8()), games.toUtf8().size());
        file.close();
        QVERIFY(imports.importFiles({QUrl::fromLocalFile(source)}));
        QTRY_VERIFY_WITH_TIMEOUT(!imports.busy(), 15000);
        QCOMPARE(imports.inserted(), std::size_t{3});
        QCOMPARE(imports.failures(), std::size_t{0});
        fixture.navigation.navigate(app::Destination::Games);
        auto* table = fixture.window.findChild<QTableView*>(QStringLiteral("gamesTable"));
        auto* open = fixture.window.findChild<QPushButton*>(QStringLiteral("openGameButton"));
        QVERIFY(table && open);
        QTRY_COMPARE(table->model()->rowCount(), 3);
        QTRY_VERIFY(!fixture.startup.library().busy());
        QVERIFY(!open->isEnabled());
        // Newest first: open and traverse the two unusual FEN games too.
        for (int row = 0; row < 2; ++row) {
            fixture.navigation.navigate(app::Destination::Games);
            table->setCurrentIndex(table->model()->index(row, 0));
            QVERIFY(open->isEnabled());
            open->click();
            QTRY_VERIFY(!fixture.startup.library().opening() && !analysis.busy());
            QCOMPARE(fixture.navigation.current(), app::Destination::Analyze);
            analysis.end();
            QTRY_VERIFY(!analysis.busy());
            QCOMPARE(analysis.history().size(), std::size_t{3});
            QVERIFY(!analysis.hasContent());
            if (row == 0) {
                QCOMPARE(analysis.history()[1].label, QStringLiteral("43. a8=N"));
                QCOMPARE(
                    analysis.board().pieces[56],
                    (std::optional{chess::Piece{chess::Color::White, chess::PieceType::Knight}}));
            } else {
                QVERIFY(!analysis.board().pieces[35]); // d5 pawn captured en passant.
                QVERIFY(analysis.board().pieces[43]);  // d6 white pawn.
            }
        }
        fixture.navigation.navigate(app::Destination::Games);
        table->setCurrentIndex(table->model()->index(2, 0));
        table->setFocus();
        QTest::keyClick(table, Qt::Key_Return); // Keyboard activation, same as Open.
        QTRY_VERIFY(!fixture.startup.library().opening() && !analysis.busy());
        savedId = analysis.gameId();
        QVERIFY(savedId);
        QCOMPARE(analysis.whitePlayer(), QStringLiteral("Paul Morphy"));
        analysis.next();
        QTRY_VERIFY(!analysis.busy());
        QCOMPARE(analysis.positionNotes(), QStringLiteral("Paris 1858"));
        analysis.end();
        QTRY_VERIFY(!analysis.busy());
        QCOMPARE(analysis.board().status, app::BoardStatus::Checkmate);
        QCOMPARE(analysis.history().size(), std::size_t{34});
        // Resolve every stored node, including the variation, against the visible board.
        const auto tree = analysis.navigationTree();
        for (const auto& [id, entry] : tree->nodes) {
            analysis.navigate(id);
            QTRY_VERIFY(!analysis.busy());
            QCOMPARE(analysis.currentNode(), id);
            QVERIFY(analysis.board().interactive);
            if (entry.san == QStringLiteral("Bc4") && entry.number == 2) {
                branch = id;
                QVERIFY(analysis.positionNotes().contains(QStringLiteral("$5")));
                auto* notes =
                    fixture.window.findChild<QPlainTextEdit*>(QStringLiteral("positionNotes"));
                QVERIFY(notes->toPlainText().contains(QStringLiteral("<b>line</b>")));
            }
        }
        QVERIFY(branch != chess::NodeId{0});
        analysis.navigate(branch);
        QTRY_VERIFY(!analysis.busy());
        QVERIFY(!analysis.hasContent());
        if (const auto path = qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR");
            !path.isEmpty()) {
            for (const auto size : {QSize{1360, 850}, QSize{1100, 720}, QSize{680, 520}}) {
                fixture.window.resize(size);
                QTest::qWait(30);
                QVERIFY(fixture.window.grab().save(path + QStringLiteral("/imported-game-%1x%2.png")
                                                              .arg(size.width())
                                                              .arg(size.height())));
            }
        }
        fixture.window.close();
        QTRY_VERIFY(fixture.closed);
        QVERIFY(!fixture.window.findChild<QMessageBox*>()); // Saved moves need no discard.
        QVERIFY(QFile::remove(source)); // Recovery must be independent of the source file.
    }
    {
        Fixture fixture(database, preferences);
        auto& analysis = fixture.startup.analysis();
        QTRY_COMPARE(analysis.gameId(), savedId);
        // gameOpened queues the remembered-node navigation after the root snapshot.
        QTRY_COMPARE(analysis.currentNode(), branch);
        QTRY_VERIFY(!analysis.busy());
        QCOMPARE(fixture.navigation.current(), app::Destination::Analyze);
        QVERIFY(analysis.positionNotes().contains(QStringLiteral("$5")));
        analysis.end();
        QTRY_VERIFY(!analysis.busy());
        QCOMPARE(analysis.history().back().label, QStringLiteral("2... Nf6"));
        QVERIFY(!analysis.hasContent());
        analysis.attemptMove({*chess::Square::fromName("d2"), *chess::Square::fromName("d4"), {}},
                             analysis.board().revision);
        QTRY_VERIFY(!analysis.busy());
        QVERIFY(analysis.hasContent());
        fixture.window.close();
        auto* dialog =
            fixture.window.findChild<QMessageBox*>(QStringLiteral("discardAnalysisDialog"));
        QVERIFY(dialog);
        dialog->button(QMessageBox::Discard)->click();
        QTRY_VERIFY(fixture.closed);
    }
    {
        Fixture fixture(database, preferences);
        auto& analysis = fixture.startup.analysis();
        QTRY_COMPARE(analysis.gameId(), savedId);
        QTRY_VERIFY(!analysis.busy());
        QCOMPARE(analysis.currentNode(), chess::NodeId{0}); // Temporary branch is absent.
        QVERIFY(!analysis.hasContent());
        fixture.window.findChild<QPushButton*>(QStringLiteral("newAnalysisButton"))->click();
        QTRY_VERIFY(!analysis.busy());
        QVERIFY(!analysis.gameId());
        fixture.window.close();
        QTRY_VERIFY(fixture.closed);
    }
    {
        Fixture fixture(database, preferences);
        QTRY_VERIFY(fixture.startup.library().ready());
        QVERIFY(!fixture.startup.analysis().gameId()); // New Analysis cleared recovery.
    }
}
void GameOpeningTest::failedAndStaleLoads() {
    QTemporaryDir directory;
    Fixture fixture(directory.filePath(QStringLiteral("games.sqlite3")),
                    directory.filePath(QStringLiteral("ui.ini")));
    auto& library = fixture.startup.library();
    auto& analysis = fixture.startup.analysis();
    QTRY_VERIFY(library.ready());
    analysis.start();
    QTRY_VERIFY(!analysis.busy());
    const auto before = analysis.navigationTree();
    library.open(999);
    QVERIFY(library.opening());
    emit fixture.startup.gameRepository().gameLoaded(
        QUuid::createUuid(), persistence::GameError{persistence::GameErrorCode::Missing,
                                                    QStringLiteral("foreign response")});
    QVERIFY(library.opening());
    library.open(1000); // Bounded admission: one load, no queue.
    QTRY_VERIFY(!library.opening());
    QVERIFY(!library.error().isEmpty());
    QCOMPARE(analysis.navigationTree(), before);
    QCOMPARE(analysis.currentNode(), chess::NodeId{0});
    QSignalSpy opened(&library, &app::GameLibraryController::opened);
    library.open(999);
    library.setReady(false);
    QTest::qWait(30);
    QCOMPARE(opened.size(), 0);
    QVERIFY(!library.opening());
}
void GameOpeningTest::replacementAndDiscard() {
    QTemporaryDir directory;
    Fixture fixture(directory.filePath(QStringLiteral("games.sqlite3")),
                    directory.filePath(QStringLiteral("ui.ini")));
    auto& imports = fixture.startup.localImports();
    auto& analysis = fixture.startup.analysis();
    QTRY_VERIFY(imports.available());
    QVERIFY(imports.importText(games));
    QTRY_VERIFY(!imports.busy());
    fixture.navigation.navigate(app::Destination::Games);
    auto* table = fixture.window.findChild<QTableView*>(QStringLiteral("gamesTable"));
    auto* open = fixture.window.findChild<QPushButton*>(QStringLiteral("openGameButton"));
    QTRY_COMPARE(table->model()->rowCount(), 3);
    QTRY_VERIFY(!fixture.startup.library().busy());
    table->setCurrentIndex(table->model()->index(2, 0));
    open->click();
    QTRY_VERIFY(!fixture.startup.library().opening() && !analysis.busy());
    const auto oldTarget = analysis.target(chess::NodeId{1});
    analysis.end();
    // Queue replacement behind an active navigation; retire old node intents.
    QSignalSpy loaded(&fixture.startup.gameRepository(), &persistence::Database::gameLoaded);
    fixture.startup.gameRepository().loadGame(QUuid::createUuid(), *analysis.gameId());
    QTRY_COMPARE(loaded.size(), 1);
    const auto result = qvariant_cast<persistence::LoadGameResult>(loaded[0][1]);
    const auto game = std::get<persistence::StoredGame>(result);
    analysis.beginning();
    analysis.openGame(game);
    analysis.navigate(oldTarget);
    QTRY_VERIFY(!analysis.busy());
    QCOMPARE(analysis.currentNode(), chess::NodeId{0});
    analysis.attemptMove({*chess::Square::fromName("d2"), *chess::Square::fromName("d4"), {}},
                         analysis.board().revision);
    QTRY_VERIFY(!analysis.busy());
    QVERIFY(analysis.hasContent());
    fixture.navigation.navigate(app::Destination::Games);
    open->click();
    auto* dialog = fixture.window.findChild<QMessageBox*>(QStringLiteral("discardAnalysisDialog"));
    QVERIFY(dialog);
    dialog->button(QMessageBox::Cancel)->click();
    QVERIFY(analysis.hasContent());
    QVERIFY(!fixture.startup.library().opening());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    open->click();
    dialog = fixture.window.findChild<QMessageBox*>(QStringLiteral("discardAnalysisDialog"));
    QVERIFY(dialog);
    dialog->button(QMessageBox::Discard)->click();
    QTRY_VERIFY(!fixture.startup.library().opening() && !analysis.busy());
    QVERIFY(!analysis.hasContent());
    QCOMPARE(analysis.navigationTree()->nodes.size(), game.document->tree.size());
}
void GameOpeningTest::productionRestart() {
#ifndef Q_OS_LINUX
    QSKIP("Application profile isolation uses Linux XDG paths");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile source(directory.filePath(QStringLiteral("phase2-games.pgn")));
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(games.toUtf8()), games.toUtf8().size());
    source.close();
    QByteArray identity;
    for (const auto* mode : {"game-import", "game-resume"}) {
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_DATA_HOME"),
                           directory.filePath(QStringLiteral("data")));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"),
                           directory.filePath(QStringLiteral("config")));
        environment.insert(QStringLiteral("XDG_CACHE_HOME"),
                           directory.filePath(QStringLiteral("cache")));
        environment.insert(QStringLiteral("CHESS_REVIEW_EXIT_MODE"), QString::fromLatin1(mode));
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(directory.path());
        process.start(QString::fromUtf8(CHESS_REVIEW_EXIT_PROBE), QStringList{});
        const auto cleanup = qScopeGuard([&] {
            if (process.state() != QProcess::NotRunning) {
                process.kill();
                if (!process.waitForFinished(5000)) {
                    qFatal("Game probe could not be reaped");
                }
            }
        });
        QVERIFY(process.waitForStarted(5000));
        QVERIFY2(process.waitForFinished(10000), process.readAllStandardError().constData());
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        const auto output = process.readAllStandardOutput();
        QVERIFY2(output.contains("restored-game:"), output.constData());
        QVERIFY(output.contains("event-loop-quitting"));
        const auto begin = output.indexOf("restored-game:");
        const auto restored = output.mid(begin).split('\n').front();
        if (identity.isEmpty()) {
            identity = restored;
            QVERIFY(source.remove());
        } else {
            QCOMPARE(restored, identity);
        }
    }
    QFile events(directory.filePath(QStringLiteral("cache/chess-review/logs/events-0.jsonl")));
    QVERIFY(events.open(QIODevice::ReadOnly));
    QVERIFY(events.readAll().contains("session.stopped"));
#endif
}
void GameOpeningTest::resumeFallbacks_data() {
    QTest::addColumn<QString>("id");
    QTest::addColumn<QString>("node");
    QTest::addColumn<bool>("invalid");
    QTest::newRow("invalid-id") << QStringLiteral("-1") << QStringLiteral("0") << true;
    QTest::newRow("invalid-node") << QStringLiteral("1") << QStringLiteral("-1") << true;
    QTest::newRow("missing-game") << QStringLiteral("123") << QStringLiteral("1") << false;
}
void GameOpeningTest::resumeFallbacks() {
    QFETCH(QString, id);
    QFETCH(QString, node);
    QFETCH(bool, invalid);
    QTemporaryDir directory;
    const auto preferences = directory.filePath(QStringLiteral("ui.ini"));
    QFile file(preferences);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const auto settings =
        QStringLiteral("[workspace]\nv1/gameId=%1\nv1/node=%2\n").arg(id, node).toUtf8();
    QCOMPARE(file.write(settings), settings.size());
    file.close();
    Fixture fixture(directory.filePath(QStringLiteral("games.sqlite3")), preferences);
    QSignalSpy errors(&fixture.startup, &app::StartupCoordinator::uiStateFailed);
    QTRY_VERIFY(fixture.startup.library().ready());
    if (invalid) {
        QCOMPARE(errors.size(), 1);
    } else {
        QTRY_VERIFY(!fixture.startup.library().opening());
        QVERIFY(!fixture.startup.library().error().isEmpty());
        QCOMPARE(fixture.navigation.current(), app::Destination::Games);
    }
    QVERIFY(!fixture.startup.analysis().gameId());
}
void GameOpeningTest::recoveryPreservesNewWork() {
    QTemporaryDir directory;
    const auto preferences = directory.filePath(QStringLiteral("ui.ini"));
    QFile file(preferences);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("[workspace]\nv1/gameId=123\nv1/node=1\n") > 0);
    file.close();
    Fixture fixture(directory.filePath(QStringLiteral("games.sqlite3")), preferences);
    auto& analysis = fixture.startup.analysis();
    bool prepared = false;
    connect(&fixture.startup, &app::StartupCoordinator::initialUiState, this, [&] {
        // Deliver the initial board and a user move before allowing storage startup.
        analysis.start();
        QTRY_VERIFY(!analysis.busy());
        analysis.attemptMove({*chess::Square::fromName("d2"), *chess::Square::fromName("d4"), {}},
                             analysis.board().revision);
        QTRY_VERIFY(!analysis.busy());
        prepared = true;
    });
    QTRY_VERIFY(fixture.startup.library().ready());
    QVERIFY(prepared);
    QVERIFY(analysis.hasContent());
    QVERIFY(!fixture.startup.library().opening());
    QVERIFY(fixture.startup.library().error().isEmpty());
    QCOMPARE(analysis.board().sideToMove, chess::Color::Black);
}
int main(int argc, char* argv[]) {
    const TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QApplication application(argc, argv);
    QTest::failOnWarning(QRegularExpression(
        QStringLiteral("^(?!This plugin does not support propagateSizeHints\\(\\)$).*")));
    GameOpeningTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "GameOpeningTest.moc"
