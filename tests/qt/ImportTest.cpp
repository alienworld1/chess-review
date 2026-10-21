#include "TestEnvironment.h"
#include "app/ImportController.h"
#include "app/NavigationController.h"
#include "app/StartupCoordinator.h"
#include "import/LocalPgnInput.h"
#include "ui/games/GamesPage.h"
#include "ui/games/PgnTextEdit.h"
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
#include <QScrollBar>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
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
    void hostileFileUrls();
    void progressRequestIdentity();
    void readinessKeepsOneRead();
    void boundedPaste();
    void largePastePreview();
    void pastePreservesUnicode_data();
    void pastePreservesUnicode();
    void progressPreservesReading();
    void visibleReportStress();
    void completionAdmission();
    void completionCloseOrdering();
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
void ImportTest::progressRequestIdentity() {
    Fixture fixture;
    auto& imports = fixture.startup.localImports();
    auto& service = fixture.startup.gameImports();
    QTRY_VERIFY(imports.available());
    bool injected = false;
    QObject observer;
    connect(&service, &app::GameImportService::savingGame, &observer,
            [&](persistence::RequestId, std::size_t ordinal) {
                if (injected) {
                    return;
                }
                injected = true;
                const auto foreign = QUuid::createUuid();
                emit service.progress(foreign, {999, persistence::Inserted{999}});
                emit service.savingGame(foreign, 999);
                QCOMPARE(imports.currentGame(), ordinal);
            });
    QVERIFY(imports.importText(QStringLiteral("1.e4 *")));
    QVERIFY(waitForBatch(imports));
    QVERIFY(injected);
    QCOMPARE(imports.inserted(), std::size_t(1));
    QCOMPARE(imports.files()[0].items.size(), std::size_t(1));
}
void ImportTest::readinessKeepsOneRead() {
    Fixture fixture;
    QTRY_VERIFY(fixture.startup.localImports().available());
    auto& library = fixture.startup.library();
    QSignalSpy results(&fixture.startup.gameRepository(), &persistence::Database::gamesListed);
    library.activate();
    QVERIFY(library.busy());
    for (int i = 0; i < 8; ++i) {
        library.setReady(false);
        library.setReady(true);
    }
    QTRY_VERIFY(!library.busy());
    QCOMPARE(results.count(), 2); // One stale accepted read, then one refresh for the latest state.
}
void ImportTest::boundedPaste() {
    ui::PgnTextEdit editor;
    editor.setPgnText(QStringLiteral("1.e4 *"));
    QSignalSpy rejected(&editor, &ui::PgnTextEdit::inputRejected);
    QApplication::clipboard()->setText(
        QString(static_cast<qsizetype>(app::maxBatchBytes) + 1, QLatin1Char('x')));
    editor.paste();
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(editor.pgnText(), QStringLiteral("1.e4 *"));
    QApplication::clipboard()->setText(QString(QChar(0xd800)));
    editor.paste();
    QCOMPARE(rejected.count(), 2);
    QCOMPARE(editor.pgnText(), QStringLiteral("1.e4 *"));
    QApplication::clipboard()->setText(QStringLiteral("[White \"A\u00a0B\"]\n1.e4 *"));
    editor.selectAll();
    editor.paste();
    QCOMPARE(editor.pgnText(), QApplication::clipboard()->text());
    editor.undo();
    QCOMPARE(editor.pgnText(), QStringLiteral("1.e4 *"));
    QApplication::clipboard()->clear();
}
void ImportTest::largePastePreview() {
    Fixture fixture;
    app::NavigationController navigation;
    ui::MainWindow window(navigation);
    window.installImports(fixture.startup.localImports(), fixture.startup.library());
    window.show();
    QTRY_VERIFY(fixture.startup.localImports().available());
    // A surrogate pair crosses the preview boundary; the retained input stays exact.
    QString source(4095, QLatin1Char(' '));
    source.replace(0, 6, QStringLiteral("1.e4 *"));
    source += QString::fromUcs4(U"\U0001f600");
    source += QString(4 * 1024 * 1024, QLatin1Char(' '));
    source += QStringLiteral("1.e4 *");
    QApplication::clipboard()->setText(source);
    window.findChild<QAction*>(QStringLiteral("pastePgnAction"))->trigger();
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("pastePgnDialog"));
    QVERIFY(dialog);
    auto* editor = dialog->findChild<ui::PgnTextEdit*>();
    QVERIFY(editor->previewOnly());
    QVERIFY(editor->isReadOnly());
    QVERIFY(editor->toPlainText().size() <= 4096);
    QVERIFY(editor->toPlainText().isValidUtf16());
    QCOMPARE(editor->pgnText(), source);
    QVERIFY(dialog->findChild<QLabel*>(QStringLiteral("pastePgnPreviewInfo"))->isVisible());
    auto* confirm = dialog->findChild<QPushButton*>(QStringLiteral("confirmPasteImport"));
    QCOMPARE(confirm->text(), QStringLiteral("Import full text"));
    QVERIFY(!fixture.startup.localImports().busy());
    const auto screenshotDirectory = qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR");
    if (!screenshotDirectory.isEmpty()) {
        QVERIFY(dialog->grab().save(screenshotDirectory + QStringLiteral("/paste-preview.png")));
    }
    // Replace with valid oversized whitespace input and verify explicit full-source submission.
    source = QString(4 * 1024 * 1024, QLatin1Char(' ')) + QStringLiteral("1.e4 *");
    editor->setPgnText(source);
    confirm->click();
    QVERIFY(waitForBatch(fixture.startup.localImports()));
    QCOMPARE(fixture.startup.localImports().inserted(), std::size_t(1));
    ui::PgnTextEdit draft;
    draft.setPgnText(QStringLiteral("1.e4 *\r\nreplace"));
    auto cursor = draft.textCursor();
    cursor.setPosition(7);
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    draft.setTextCursor(cursor);
    QApplication::clipboard()->setText(source);
    draft.paste();
    QVERIFY(draft.previewOnly());
    QVERIFY(draft.document()->characterCount() <= 4097);
    QCOMPARE(draft.pgnText(), QStringLiteral("1.e4 *\n") + source);
    draft.setPgnText(QStringLiteral("1.d4 *"));
    QVERIFY(!draft.previewOnly());
    QVERIFY(!draft.isReadOnly());
    QApplication::clipboard()->clear();
    window.completeShutdown();
}
void ImportTest::hostileFileUrls() {
    Fixture fixture;
    auto& imports = fixture.startup.localImports();
    QTRY_VERIFY(imports.available());
    for (const auto& url : QList<QUrl>{
             QUrl(QStringLiteral("file://untrusted.example/share/game.pgn")),
             QUrl(QStringLiteral("file:relative.pgn")),
             QUrl(QStringLiteral("file:////untrusted.example/share/game.pgn")),
             QUrl::fromEncoded("file:///tmp/%FF.pgn"), QUrl::fromEncoded("file:///tmp/a%00.pgn"),
             QUrl(QStringLiteral("file:///tmp/game.pgn?query")),
             QUrl(QStringLiteral("file:///tmp/game.pgn#fragment"))}) {
        QVERIFY2(!imports.importFiles({url}), qPrintable(url.toString()));
        QVERIFY(imports.files().empty());
        const auto input = input::readLocalPgn(url, 10, {});
        QVERIFY(!input.error.isEmpty());
        QCOMPARE(input.bytesRead, std::size_t(0));
    }
}
void ImportTest::pastePreservesUnicode_data() {
    QTest::addColumn<QString>("event");
    QTest::newRow("no-break-space") << QStringLiteral("ordinary");
    QTest::newRow("unicode-line-separator") << QStringLiteral("one\u2028two");
    QTest::newRow("field-bom") << QStringLiteral("\ufeffEvent");
}
void ImportTest::pastePreservesUnicode() {
    QFETCH(QString, event);
    Fixture fixture;
    app::NavigationController navigation;
    ui::MainWindow window(navigation);
    window.installImports(fixture.startup.localImports(), fixture.startup.library());
    window.show();
    QTRY_VERIFY(fixture.startup.localImports().available());
    const auto original =
        QStringLiteral("[White \"A\u00a0B\"]\r\n[Event \"%1\"]\r\n1.e4 *").arg(event);
    auto ordinary = original;
    ordinary.replace(QChar(0x00a0), QLatin1Char(' '));
    QVERIFY(fixture.startup.localImports().importText(ordinary));
    QVERIFY(waitForBatch(fixture.startup.localImports()));
    QApplication::clipboard()->setText(original);
    window.findChild<QAction*>(QStringLiteral("pastePgnAction"))->trigger();
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("pastePgnDialog"));
    QVERIFY(dialog);
    dialog->findChild<QPushButton*>(QStringLiteral("confirmPasteImport"))->click();
    QTRY_VERIFY(!fixture.startup.localImports().busy());
    QCOMPARE(fixture.startup.localImports().inserted(), std::size_t(1));
    auto& library = fixture.startup.library();
    QTRY_VERIFY(!library.busy());
    QCOMPARE(library.rows().size(), std::size_t(2));
    QCOMPARE(library.rows()[0].white, QStringLiteral("A\u00a0B"));
    QSignalSpy loaded(&fixture.startup.gameRepository(), &persistence::Database::gameLoaded);
    fixture.startup.gameRepository().loadGame(QUuid::createUuid(), library.rows()[0].id);
    QTRY_COMPARE(loaded.count(), 1);
    const auto result = qvariant_cast<persistence::LoadGameResult>(loaded.at(0).at(1));
    QVERIFY(std::holds_alternative<persistence::StoredGame>(result));
    QCOMPARE(std::get<persistence::StoredGame>(result).document->originalText,
             original.toStdString());
    // Ordinary edits must not replace a no-break space elsewhere in the source.
    QApplication::clipboard()->setText(original);
    window.findChild<QAction*>(QStringLiteral("pastePgnAction"))->trigger();
    dialog = window.findChild<QDialog*>(QStringLiteral("pastePgnDialog"));
    auto* editor = dialog->findChild<QPlainTextEdit*>(QStringLiteral("pastePgnEditor"));
    auto cursor = editor->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral(" {edited}"));
    dialog->findChild<QPushButton*>(QStringLiteral("confirmPasteImport"))->click();
    QTRY_VERIFY(!fixture.startup.localImports().busy());
    QCOMPARE(fixture.startup.localImports().duplicates(), std::size_t(1));
    window.completeShutdown();
}
void ImportTest::progressPreservesReading() {
    Fixture fixture;
    QAction fileAction(QStringLiteral("Import"));
    QAction pasteAction(QStringLiteral("Paste"));
    ui::GamesPage page(fixture.startup.localImports(), fixture.startup.library(), fileAction,
                       pasteAction);
    page.resize(1100, 720);
    page.show();
    QTRY_VERIFY(fixture.startup.localImports().available());
    QString text;
    for (int i = 0; i < 40; ++i) {
        text += QStringLiteral("1.e5 *\n");
    }
    QVERIFY(fixture.startup.localImports().importText(text));
    QVERIFY(waitForBatch(fixture.startup.localImports()));
    auto* results = page.findChild<QTableView*>(QStringLiteral("importResults"));
    auto* details = page.findChild<QPlainTextEdit*>(QStringLiteral("importDetails"));
    QTRY_COMPARE(results->model()->rowCount(), 41);
    results->setCurrentIndex(results->model()->index(1, 0));
    auto cursor = details->textCursor();
    cursor.select(QTextCursor::Document);
    details->setTextCursor(cursor);
    details->setFocus();
    const auto selected = cursor.selectedText();
    QVERIFY(!selected.isEmpty());
    results->verticalScrollBar()->setValue(15);
    const auto scroll = results->verticalScrollBar()->value();
    QVERIFY(scroll > 0);
    emit fixture.startup.localImports().changed();
    QTest::qWait(150);
    QVERIFY(details->hasFocus());
    QCOMPARE(details->textCursor().selectedText(), selected);
    QCOMPARE(results->verticalScrollBar()->value(), scroll);
    QVERIFY(page.findChild<QLabel*>(QStringLiteral("importStatus"))
                ->text()
                .startsWith(QStringLiteral("Import completed with errors")));
    QCOMPARE(results->model()->data(results->model()->index(0, 2)).toString(),
             QStringLiteral("Completed with errors"));
    QVERIFY(fixture.startup.localImports().importText(QStringLiteral("1.d4 *")));
    fixture.startup.localImports().cancel();
    QVERIFY(waitForBatch(fixture.startup.localImports()));
    QVERIFY(page.findChild<QLabel*>(QStringLiteral("importStatus"))
                ->text()
                .startsWith(QStringLiteral("Import cancelled")));
}
void ImportTest::visibleReportStress() {
    Fixture fixture;
    QAction fileAction(QStringLiteral("Import"));
    QAction pasteAction(QStringLiteral("Paste"));
    ui::GamesPage page(fixture.startup.localImports(), fixture.startup.library(), fileAction,
                       pasteAction);
    page.resize(1100, 720);
    page.show();
    QTRY_VERIFY(fixture.startup.localImports().available());
    constexpr int games = 2000;
    QString source;
    for (int i = 0; i < games; ++i) {
        source += QStringLiteral("1.e5 *\n");
    }
    int beats = 0;
    qint64 largestGap = 0;
    QElapsedTimer elapsed;
    QElapsedTimer gap;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
        largestGap = std::max(largestGap, gap.restart());
        ++beats;
    });
    elapsed.start();
    gap.start();
    heartbeat.start(5);
    QVERIFY(fixture.startup.localImports().importText(source));
    QVERIFY(waitForBatch(fixture.startup.localImports()));
    largestGap = std::max(largestGap, gap.elapsed());
    heartbeat.stop();
    auto* table = page.findChild<QTableView*>(QStringLiteral("importResults"));
    QCOMPARE(table->model()->rowCount(), games + 1);
    QVERIFY(beats > 0);
    qInfo("visible import report: %d games, %lld ms, largest heartbeat gap %lld ms", games,
          static_cast<long long>(elapsed.elapsed()), static_cast<long long>(largestGap));
}
void ImportTest::completionAdmission() {
    Fixture fixture;
    auto& imports = fixture.startup.localImports();
    QTRY_VERIFY(imports.available());
    bool tried = false;
    bool admitted = false;
    const auto connection = connect(&imports, &app::ImportController::changed, &imports, [&] {
        if (!imports.busy() && !tried) {
            tried = true;
            admitted = imports.importText(QStringLiteral("1.d4 *"));
        }
    });
    QSignalSpy finished(&imports, &app::ImportController::finished);
    QVERIFY(imports.importText(QStringLiteral("1.e4 *")));
    QTRY_VERIFY(tried);
    QVERIFY(
        !admitted); // Terminal notifications must finish before another batch replaces its state.
    disconnect(connection);
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(imports.importText(QStringLiteral("1.d4 *")));
    QVERIFY(waitForBatch(imports));
}
void ImportTest::completionCloseOrdering() {
    Fixture fixture;
    auto& imports = fixture.startup.localImports();
    QTRY_VERIFY(imports.available());
    bool closed = false;
    QObject observer;
    connect(&imports, &app::ImportController::closed, &observer, [&] { closed = true; });
    connect(&imports, &app::ImportController::finished, &observer, [&] {
        QVERIFY(!imports.importText(QStringLiteral("1.d4 *")));
        imports.close();
        // Nested event delivery must not publish closed() while finished() observers still run.
        QTest::qWait(10);
        QVERIFY(!closed);
    });
    QVERIFY(imports.importText(QStringLiteral("1.e4 *")));
    QTRY_VERIFY(closed);
    QCOMPARE(imports.files().size(), std::size_t(1));
    QCOMPARE(imports.inserted(), std::size_t(1));
}
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
    QCOMPARE(controller.status(), app::ImportBatchStatus::Cancelled);
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
    for (std::size_t i = 0; i < app::maxBatchGames; ++i) {
        games += "1.e5 *\n";
    }
    const auto firstHalf = fixture.file(QStringLiteral("part1.pgn"), games.first(games.size() / 2));
    const auto secondHalf =
        fixture.file(QStringLiteral("part2.pgn"), games.sliced(games.size() / 2));
    QVERIFY(controller.importFiles({firstHalf, secondHalf, good}));
    int ticks = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] { ++ticks; });
    heartbeat.start(5);
    QVERIFY(waitForBatch(controller));
    QVERIFY(ticks > 0);
    heartbeat.stop();
    QCOMPARE(controller.status(), app::ImportBatchStatus::GameLimitReached);
    QCOMPARE(controller.files()[0].items.size() + controller.files()[1].items.size(),
             app::maxBatchGames);
    QCOMPARE(controller.files()[0].state, app::ImportFileState::CompletedWithErrors);
    QCOMPARE(controller.files()[2].state, app::ImportFileState::Unprocessed);
    QVERIFY(controller.files()[2].message.contains(QStringLiteral("game limit")));
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
