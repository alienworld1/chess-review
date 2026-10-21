#include "TestEnvironment.h"
#include "app/ImportController.h"
#include "app/NavigationController.h"
#include "app/StartupCoordinator.h"
#include "import/LocalPgnInput.h"
#include "ui/games/GamesPage.h"
#include "ui/shell/MainWindow.h"
#include <QAbstractItemModelTester>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

using namespace chess_review;
namespace {
bool waitForBatch(app::ImportController& controller) {
    if (!controller.busy()) {
        return true;
    }
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&controller, &app::ImportController::finished, &loop, &QEventLoop::quit);
    deadline.start(15'000);
    loop.exec();
    return !controller.busy();
}
struct Fixture {
    QTemporaryDir directory;
    app::StartupCoordinator startup;
    bool closed = false;
    Fixture() {
        if (!directory.isValid()) {
            qFatal("Temporary directory unavailable");
        }
        QObject::connect(&startup, &app::StartupCoordinator::finished, &startup,
                         [this] { closed = true; });
        startup.start(directory.filePath(QStringLiteral("games.sqlite3")));
    }
    ~Fixture() {
        startup.shutdown();
        QElapsedTimer deadline;
        deadline.start();
        while (!closed && deadline.elapsed() < 10'000) {
            QTest::qWait(1);
        }
        if (!closed) {
            qFatal("Import fixture did not shut down");
        }
    }
    QUrl file(const QString& name, const QByteArray& content) {
        const auto path = directory.filePath(name);
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly) || output.write(content) != content.size()) {
            qFatal("Could not write fixture");
        }
        output.close();
        return QUrl::fromLocalFile(path);
    }
};
} // namespace
class ImportTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        QTest::failOnWarning(QRegularExpression(
            QStringLiteral("^(?!This plugin does not support propagateSizeHints\\(\\)$).*")));
    }
    void acquisition();
    void mixedFilesAndProvenance();
    void cancellationAndClosing();
    void sharedBudgets();
    void reducedServiceBudgets();
    void paginationAndStaleResults();
    void desktopPasteAndDrop();
    void startupFailure();
    void metadataPresentation();
    void layouts_data();
    void layouts();
};
void ImportTest::acquisition() {
    Fixture fixture;
    const auto good = fixture.file(QStringLiteral("UPPER.PGN"), "1.e4 *");
    const auto read = input::readLocalPgn(good, 10, {});
    QCOMPARE(read.bytes, std::string("1.e4 *"));
    QCOMPARE(read.bytesRead, std::size_t(6));
    QCOMPARE(read.source.type, QStringLiteral("file"));
    QCOMPARE(QUrl(read.source.uri), good);
    QVERIFY(!input::readLocalPgn(good, 5, {}).error.isEmpty());
    QVERIFY(!input::readLocalPgn(QUrl(QStringLiteral("https://example.com/a.pgn")), 10, {})
                 .error.isEmpty());
    QVERIFY(
        !input::readLocalPgn(fixture.file(QStringLiteral("a.txt"), "*"), 10, {}).error.isEmpty());
    QVERIFY(!input::readLocalPgn(QUrl::fromLocalFile(fixture.directory.path()), 10, {})
                 .error.isEmpty());
    const auto empty = input::readLocalPgn(fixture.file(QStringLiteral("empty.pgn"), {}), 0, {});
    QVERIFY(empty.error.isEmpty());
    QVERIFY(empty.bytes.empty());
    std::stop_source stop;
    stop.request_stop();
    QVERIFY(input::readLocalPgn(good, 10, stop.get_token()).cancelled);
    QCOMPARE(input::encodePgnText(QString::fromUtf8("é"), 2, {}).bytes, std::string("\xc3\xa9"));
    QVERIFY(!input::encodePgnText(QString::fromUtf8("é"), 1, {}).error.isEmpty());
    QVERIFY(!input::encodePgnText(QString(QChar(0xd800)), 10, {}).error.isEmpty());
    const auto bom = QString(QChar(0xfeff)) + QStringLiteral("*");
    QCOMPARE(input::encodePgnText(bom, 10, {}).bytes, std::string("\xef\xbb\xbf*"));
#ifdef Q_OS_UNIX
    const auto link = fixture.directory.filePath(QStringLiteral("link.pgn"));
    QVERIFY(QFile::link(good.toLocalFile(), link));
    QCOMPARE(input::readLocalPgn(QUrl::fromLocalFile(link), 10, {}).bytes, read.bytes);
    const auto fifo = fixture.directory.filePath(QStringLiteral("fifo.pgn"));
    QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
    QVERIFY(!input::readLocalPgn(QUrl::fromLocalFile(fifo), 10, {}).error.isEmpty());
#endif
}
void ImportTest::mixedFilesAndProvenance() {
    Fixture fixture;
    auto& controller = fixture.startup.localImports();
    QTRY_VERIFY(controller.available());
    const auto first = fixture.file(QString::fromUtf8("α.PGN"), "[White \"<b>Alice</b>\"]\n1.e4 *");
    const auto broken = fixture.file(QStringLiteral("broken.pgn"), "1.d4 {unterminated");
    const auto duplicate =
        fixture.file(QStringLiteral("duplicate.pgn"), "[White \"<b>Alice</b>\"]\n1.e4 *");
    const auto last = fixture.file(QStringLiteral("last.pgn"), "1.Nf3 *");
    const auto missing =
        QUrl::fromLocalFile(fixture.directory.filePath(QStringLiteral("missing.pgn")));
    const auto invalid =
        fixture.file(QStringLiteral("encoding.pgn"), QByteArray("[White \"\xff\"] *"));
    QSignalSpy rejected(&controller, &app::ImportController::rejected);
    QVERIFY(controller.importFiles({first, broken, duplicate, missing, invalid, last}));
    QVERIFY(!controller.importText(QStringLiteral("1.c4 *")));
    QCOMPARE(rejected.count(), 1);
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.inserted(), std::size_t(2));
    QCOMPARE(controller.duplicates(), std::size_t(1));
    QCOMPARE(controller.failures(), std::size_t(3));
    QCOMPARE(controller.files().size(), std::size_t(6));
    emit fixture.startup.gameImports().rejected(
        {}, {persistence::GameErrorCode::Busy, QStringLiteral("Unrelated result")});
    QVERIFY(!controller.importFiles(QList<QUrl>(33, first)));
    QCOMPARE(controller.files().size(), std::size_t(6));
    auto& library = fixture.startup.library();
    library.activate();
    QTRY_VERIFY(!library.busy());
    QCOMPARE(library.rows().size(), std::size_t(2));
    const auto id = std::get<persistence::Inserted>(controller.files()[0].items[0].outcome).id;
    QSignalSpy loaded(&fixture.startup.gameRepository(), &persistence::Database::gameLoaded);
    fixture.startup.gameRepository().loadGame(QUuid::createUuid(), id);
    QTRY_COMPARE(loaded.count(), 1);
    const auto result = qvariant_cast<persistence::LoadGameResult>(loaded.at(0).at(1));
    QVERIFY(std::holds_alternative<persistence::StoredGame>(result));
    const auto stored = std::get<persistence::StoredGame>(result);
    QCOMPARE(QUrl(stored.source.uri), first);
    QCOMPARE(stored.summary.white, QStringLiteral("<b>Alice</b>"));
    QVERIFY(QFile::remove(first.toLocalFile()));
    loaded.clear();
    fixture.startup.gameRepository().loadGame(QUuid::createUuid(), id);
    QTRY_COMPARE(loaded.count(), 1);
    QVERIFY(std::holds_alternative<persistence::StoredGame>(
        qvariant_cast<persistence::LoadGameResult>(loaded.at(0).at(1))));
    QVERIFY(controller.importText(QStringLiteral(" \n")));
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.files().size(), std::size_t(1));
    QCOMPARE(controller.files()[0].message, QStringLiteral("No games found."));
}
void ImportTest::cancellationAndClosing() {
    Fixture fixture;
    auto& controller = fixture.startup.localImports();
    QTRY_VERIFY(controller.available());
    const auto file = fixture.file(QStringLiteral("a.pgn"), "1.e4 * 1.d4 *");
    QVERIFY(controller.importFiles({file, file}));
    controller.cancel();
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.inserted(), std::size_t(0));
    QCOMPARE(controller.files()[0].state, app::ImportFileState::Cancelled);
    QCOMPARE(controller.files()[1].state, app::ImportFileState::Unprocessed);
    QSignalSpy commits(&controller, &app::ImportController::committed);
    auto connection = connect(&fixture.startup.gameImports(), &app::GameImportService::savingGame,
                              &controller, [&] { controller.cancel(); });
    QVERIFY(controller.importFiles({file, file}));
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(commits.count(), 1);
    QCOMPARE(controller.inserted(), std::size_t(1));
    QCOMPARE(controller.files()[1].state, app::ImportFileState::Unprocessed);
    disconnect(connection);
    QVERIFY(controller.importFiles({file, file}));
    fixture.startup.shutdown();
    QTRY_VERIFY(fixture.closed);
    QVERIFY(!controller.available());
    QVERIFY(!controller.busy());
}
void ImportTest::sharedBudgets() {
    Fixture fixture;
    auto& controller = fixture.startup.localImports();
    QTRY_VERIFY(controller.available());
    const auto oversized = fixture.file(QStringLiteral("large.pgn"), {});
    QFile sparse(oversized.toLocalFile());
    QVERIFY(sparse.open(QIODevice::WriteOnly));
    QVERIFY(sparse.resize(static_cast<qint64>(app::maxBatchBytes) + 1));
    sparse.close();
    const auto good = fixture.file(QStringLiteral("good.pgn"), "1.e4 *");
    QVERIFY(controller.importFiles({oversized, good}));
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.failures(), std::size_t(1));
    QCOMPARE(controller.inserted(), std::size_t(1));
    QByteArray games;
    for (std::size_t i = 0; i < app::maxBatchGames + 1; ++i) {
        games += "1.e5 *\n";
    }
    const auto tooMany = fixture.file(QStringLiteral("many.pgn"), games);
    QVERIFY(controller.importFiles({tooMany, good}));
    int ticks = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++ticks; });
    heartbeat.start(5);
    QVERIFY(waitForBatch(controller));
    QVERIFY(ticks > 0);
    heartbeat.stop();
    QCOMPARE(controller.files()[0].items.size(), app::maxBatchGames + 1);
    QCOMPARE(controller.files()[1].state, app::ImportFileState::Unprocessed);
    // Each missing move number warning consumes the same shared warning budget across files.
    QByteArray warnings;
    for (int i = 0; i < 101; ++i) {
        warnings += "[White \"a\"]";
    }
    warnings += " *";
    QList<QUrl> warningFiles;
    for (int i = 0; i < 11; ++i) {
        warningFiles.push_back(fixture.file(QStringLiteral("warning%1.pgn").arg(i), warnings));
    }
    QVERIFY(controller.importFiles(warningFiles));
    QTRY_VERIFY(!controller.busy());
    std::size_t retained = 0;
    std::size_t suppressed = 0;
    for (const auto& report : controller.files()) {
        for (const auto& item : report.items) {
            retained += item.warnings.size();
            suppressed += item.suppressedWarnings;
        }
    }
    QCOMPARE(retained, app::maxImportReportWarnings);
    QCOMPARE(suppressed, std::size_t(100));
}
void ImportTest::reducedServiceBudgets() {
    Fixture fixture;
    QTRY_VERIFY(fixture.startup.localImports().available());
    auto& service = fixture.startup.gameImports();
    QSignalSpy report(&service, &app::GameImportService::finished);
    service.importText(QUuid::createUuid(), "1.e4 * 1.d4 *", {}, {1, 0});
    QTRY_COMPARE(report.count(), 1);
    const auto result = qvariant_cast<app::ImportReport>(report.at(0).at(0));
    QCOMPARE(result.attemptedGames, std::size_t(1));
    QVERIFY(result.gameLimitReached);
    QCOMPARE(result.items.size(), std::size_t(2));
    QSignalSpy rejected(&service, &app::GameImportService::rejected);
    service.importText(QUuid::createUuid(), "*", {}, {0, 0});
    QTRY_COMPARE(rejected.count(), 1);
}
void ImportTest::paginationAndStaleResults() {
    Fixture fixture;
    QTRY_VERIFY(fixture.startup.localImports().available());
    auto& library = fixture.startup.library();
    QSignalSpy listed(&fixture.startup.gameRepository(), &persistence::Database::gamesListed);
    QVERIFY(library.rows().empty());
    QCOMPARE(listed.count(), 0);
    QString text;
    for (int i = 0; i < 52; ++i) {
        text += QStringLiteral("[Event \"%1\"]\n[White \"Player %1\"]\n1.e4 *\n").arg(i);
    }
    QVERIFY(fixture.startup.localImports().importText(text));
    QTRY_VERIFY(!fixture.startup.localImports().busy());
    QCOMPARE(listed.count(), 0);
    library.activate();
    QTRY_VERIFY(!library.busy());
    QCOMPARE(library.rows().size(), std::size_t(50));
    QVERIFY(library.hasNext());
    library.next();
    QTRY_VERIFY(!library.busy());
    QCOMPARE(library.page(), std::int64_t(1));
    QCOMPARE(library.rows().size(), std::size_t(2));
    QVERIFY(!library.hasNext());
    library.previous();
    library.refresh(true); // Supersede an accepted list request before delivering its result.
    QTRY_VERIFY(!library.busy());
    QCOMPARE(library.page(), std::int64_t(0));
    QCOMPARE(library.rows().size(), std::size_t(50));
    emit fixture.startup.gameRepository().gamesListed({}, std::vector<persistence::GameSummary>{});
    QCOMPARE(library.rows().size(), std::size_t(50));
    const auto oldRequest = qvariant_cast<persistence::RequestId>(listed.last().at(0));
    library.setReady(false);
    fixture.startup.gameRepository().gamesListed(oldRequest,
                                                 std::vector<persistence::GameSummary>{});
    QVERIFY(library.rows().empty());
    QVERIFY(!library.busy());
}
void ImportTest::desktopPasteAndDrop() {
    Fixture fixture;
    app::NavigationController navigation;
    ui::MainWindow window(navigation);
    window.installImports(fixture.startup.localImports(), fixture.startup.library());
    window.show();
    QTRY_VERIFY(fixture.startup.localImports().available());
    auto* paste = window.findChild<QAction*>(QStringLiteral("pastePgnAction"));
    auto* open = window.findChild<QAction*>(QStringLiteral("importPgnAction"));
    QVERIFY(paste && open);
    QApplication::clipboard()->setText(QStringLiteral("[White \"<b>Player</b>\"]\n1.e4 *"));
    paste->trigger();
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("pastePgnDialog"));
    QVERIFY(dialog);
    auto* editor = dialog->findChild<QPlainTextEdit*>(QStringLiteral("pastePgnEditor"));
    QCOMPARE(editor->toPlainText(), QApplication::clipboard()->text());
    QVERIFY(!fixture.startup.localImports().busy());
    editor->setFocus();
    QTest::keyPress(editor, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::keyRelease(editor, Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(window.findChildren<QDialog*>(QStringLiteral("pastePgnDialog")).size(), 1);
    editor->setPlainText(QStringLiteral("[White \"<b>Player</b>\"]\n1.e4 *"));
    dialog->findChild<QPushButton*>(QStringLiteral("confirmPasteImport"))->click();
    QCOMPARE(navigation.current(), app::Destination::Games);
    QVERIFY(!open->isEnabled());
    QTRY_VERIFY(!fixture.startup.localImports().busy());
    QTRY_VERIFY(open->isEnabled());
    auto* table = window.findChild<QTableView*>(QStringLiteral("gamesTable"));
    QTRY_COMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->data(table->model()->index(0, 0)).toString(),
             QStringLiteral("<b>Player</b>"));
    auto* results = window.findChild<QTableView*>(QStringLiteral("importResults"));
    QTRY_VERIFY(results->model()->rowCount() >= 2);
    QAbstractItemModelTester modelChecks(results->model(),
                                         QAbstractItemModelTester::FailureReportingMode::QtTest);
    results->setCurrentIndex(results->model()->index(1, 0));
    auto* details = window.findChild<QPlainTextEdit*>(QStringLiteral("importDetails"));
    QVERIFY(details->toPlainText().contains(QStringLiteral("Source byte range")));
    navigation.navigate(app::Destination::Analyze);
    navigation.navigate(app::Destination::Games);
    QVERIFY(results->model()->rowCount() >= 2);
    auto* page = window.findChild<ui::GamesPage*>();
    QMimeData mime;
    mime.setUrls({fixture.file(QStringLiteral("drop.pgn"), "1.d4 *")});
    QDragEnterEvent drag(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(page, &drag);
    QVERIFY(drag.isAccepted());
    QDropEvent drop(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(page, &drop);
    QVERIFY(drop.isAccepted());
    QCOMPARE(drop.dropAction(), Qt::CopyAction);
    QTRY_VERIFY(!fixture.startup.localImports().busy());
    QCOMPARE(fixture.startup.localImports().inserted(), std::size_t(1));
    QDragEnterEvent moveOnly(QPointF(5, 5), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(page, &moveOnly);
    QVERIFY(!moveOnly.isAccepted());
    mime.setUrls({QUrl(QStringLiteral("https://example.com/a.pgn"))});
    QDragEnterEvent remote(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(page, &remote);
    QVERIFY(!remote.isAccepted());
    const auto repeat = fixture.file(QStringLiteral("repeat.pgn"), "1.Nf3 *");
    QVERIFY(fixture.startup.localImports().importFiles({repeat, repeat, repeat}));
    QTRY_VERIFY(!fixture.startup.localImports().busy());
    QTRY_COMPARE(results->model()->rowCount(), 6);
    results->setCurrentIndex(results->model()->index(5, 0));
    const auto replacementCheck =
        connect(results->model(), &QAbstractItemModel::modelAboutToBeReset, results, [&] {
            const auto last = results->model()->index(results->model()->rowCount() - 1, 0);
            QVERIFY(!results->model()->data(last).toString().isEmpty());
        });
    QVERIFY(fixture.startup.localImports().importText(QStringLiteral("*")));
    QTRY_VERIFY(!fixture.startup.localImports().busy());
    QTRY_COMPARE(results->model()->rowCount(), 2);
    disconnect(replacementCheck);
    window.completeShutdown();
}
void ImportTest::metadataPresentation() {
    Fixture fixture;
    QTRY_VERIFY(fixture.startup.localImports().available());
    auto& database = fixture.startup.gameRepository();
    bool delivering = false;
    bool returnError = true;
    QObject responses;
    // Inject typed boundary values before this presentation controller receives the real result.
    // Corrupt storage creation and detection are covered by persistence-owned fixtures.
    const auto connection = connect(
        &database, &persistence::Database::gamesListed, &database,
        [&](persistence::RequestId request, const persistence::ListGamesResult&) {
            if (delivering) {
                return;
            }
            persistence::GameSummary damaged;
            damaged.id = 7;
            damaged.metadataError =
                persistence::GameError{persistence::GameErrorCode::InvalidSnapshot,
                                       QStringLiteral("Unreadable metadata.")};
            persistence::GameSummary clipped;
            clipped.id = 8;
            clipped.white = QStringLiteral("<b>literal</b>");
            clipped.metadataTruncated = true;
            delivering = true;
            if (returnError) {
                emit database.gamesListed(
                    request, persistence::GameError{persistence::GameErrorCode::Storage,
                                                    QStringLiteral("Could not list games.")});
            } else {
                emit database.gamesListed(request,
                                          std::vector<persistence::GameSummary>{damaged, clipped});
            }
            delivering = false;
        });
    app::GameLibraryController library(database);
    app::NavigationController navigation;
    ui::MainWindow window(navigation);
    window.installImports(fixture.startup.localImports(), library);
    library.setReady(true);
    navigation.navigate(app::Destination::Games);
    QTRY_VERIFY(!library.busy());
    QVERIFY(!library.error().isEmpty());
    QVERIFY(window.findChild<QPushButton*>(QStringLiteral("refreshGames"))->isEnabled());
    returnError = false;
    library.refresh();
    QTRY_VERIFY(!library.busy());
    QVERIFY(library.error().isEmpty());
    auto* table = window.findChild<QTableView*>(QStringLiteral("gamesTable"));
    QCOMPARE(table->model()->rowCount(), 2);
    const auto damaged = table->model()->index(0, 0);
    QVERIFY(table->model()->data(damaged).toString().contains(QStringLiteral("ID 7")));
    QVERIFY(table->model()
                ->data(damaged, Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("Unreadable metadata")));
    QCOMPARE(table->model()->data(table->model()->index(1, 0)).toString(),
             QStringLiteral("<b>literal</b> [truncated]"));
    disconnect(connection);
    window.completeShutdown();
}
void ImportTest::layouts_data() {
    QTest::addColumn<QSize>("size");
    QTest::newRow("laptop") << QSize(1100, 720);
    QTest::newRow("default") << QSize(1360, 850);
    QTest::newRow("desktop") << QSize(1800, 1000);
    QTest::newRow("constrained") << QSize(640, 480);
}
void ImportTest::layouts() {
    QFETCH(QSize, size);
    Fixture fixture;
    QAction fileAction(QStringLiteral("Import PGN"));
    QAction pasteAction(QStringLiteral("Paste PGN"));
    ui::GamesPage page(fixture.startup.localImports(), fixture.startup.library(), fileAction,
                       pasteAction);
    page.resize(size);
    page.show();
    QTRY_VERIFY(fixture.startup.localImports().available());
    fixture.startup.library().activate();
    QVERIFY(fixture.startup.localImports().importText(
        QStringLiteral("[White \"Alice\"]\n1.e4 * 1.e5 *")));
    QTRY_VERIFY(!fixture.startup.localImports().busy());
    auto* table = page.findChild<QTableView*>(QStringLiteral("gamesTable"));
    QTRY_COMPARE(table->model()->rowCount(), 1);
    QTest::qWait(120);
    auto* results = page.findChild<QTableView*>(QStringLiteral("importResults"));
    QCOMPARE(page.size(), size);
    QVERIFY(table->width() > 200);
    QVERIFY(table->height() > 50);
    QVERIFY(results->height() > 30);
    QVERIFY(page.rect().contains(QRect(table->mapTo(&page, QPoint{}), table->size())));
    QVERIFY(page.rect().contains(QRect(results->mapTo(&page, QPoint{}), results->size())));
    const auto directory = qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR");
    if (!directory.isEmpty()) {
        QVERIFY(page.grab().save(directory + QStringLiteral("/import-%1x%2-%3.png")
                                                 .arg(size.width())
                                                 .arg(size.height())
                                                 .arg(page.devicePixelRatioF())));
    }
}
void ImportTest::startupFailure() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    app::StartupCoordinator startup;
    QSignalSpy failed(&startup, &app::StartupCoordinator::failed);
    QSignalSpy closed(&startup, &app::StartupCoordinator::finished);
    startup.start(directory.path());
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(!startup.localImports().available());
    QVERIFY(!startup.localImports().importText(QStringLiteral("*")));
    QVERIFY(!startup.library().ready());
    startup.shutdown();
    QTRY_COMPARE(closed.count(), 1);
}
int main(int argc, char** argv) {
    TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("chess-review"));
    application.setQuitOnLastWindowClosed(false);
    ImportTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ImportTest.moc"
