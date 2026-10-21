#include "TestEnvironment.h"
#include "app/AnalysisController.h"
#include "app/UiStateStore.h"
#include "ui/analysis/AnalysisPage.h"
#include "ui/chess/ChessBoardWidget.h"
#include "ui/shell/MainWindow.h"
#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <memory>

using namespace chess_review;
namespace {
// These fixtures retain the board for both events. Keeping press/release explicit
// avoids a clang-analyzer false positive in QtTest's inline QPointer click guard.
void boardKey(QWidget* board, Qt::Key key) {
    QTest::keyPress(board, key);
    QTest::keyRelease(board, key);
}
chess::Square square(const char* name) { return *chess::Square::fromName(name); }
chess::Move move(const char* from, const char* to,
                 std::optional<chess::PieceType> promotion = std::nullopt) {
    return {square(from), square(to), promotion};
}
} // namespace
class AnalysisTest final : public QObject {
    Q_OBJECT
  private slots:
    void init();
    void cleanup();
    void legalMovesAndStaleInput();
    void branchesAndAnnotations();
    void specialMoves_data();
    void specialMoves();
    void failedResetPreservesSession();
    void geometryAndAccessibility();
    void clickDragAndKeyboard();
    void annotationsAndCancellation();
    void promotionChoiceAndCancellation();
    void positionChangeCancelsPromotion();
    void preferenceRoundTrip();
    void invalidPreferences();
    void discardGate();
    void closeDuringWork();
    void reentrantCloseSuppressesMove_data();
    void reentrantCloseSuppressesMove();
    void layout_data();
    void layout();

  private:
    void play(chess::Move move);
    std::unique_ptr<app::AnalysisController> controller_;
};
void AnalysisTest::init() {
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        QTest::failOnWarning(QRegularExpression(
            QStringLiteral("^(?!This plugin does not support "
                           "(propagateSizeHints\\(\\)|raise\\(\\)|grabbing the keyboard)$)")));
    } else {
        QTest::failOnWarning();
    }
    controller_ = std::make_unique<app::AnalysisController>();
    controller_->start();
    QTRY_VERIFY(!controller_->busy());
    QVERIFY(controller_->board().interactive);
}
void AnalysisTest::cleanup() {
    if (!controller_) {
        return;
    }
    QSignalSpy closed(controller_.get(), &app::AnalysisController::closed);
    controller_->close();
    QTRY_COMPARE(closed.count(), 1);
    controller_.reset();
}
void AnalysisTest::play(chess::Move attempted) {
    controller_->attemptMove(attempted, controller_->board().revision);
    QTRY_VERIFY(!controller_->busy());
}
void AnalysisTest::legalMovesAndStaleInput() {
    QCOMPARE(controller_->board().legalMoves.size(), std::size_t{20});
    QSignalSpy failures(controller_.get(), &app::AnalysisController::failed);
    const auto revision = controller_->board().revision;
    play(move("e2", "e5"));
    QCOMPARE(failures.count(), 1);
    QCOMPARE(controller_->history().size(), std::size_t{1});
    QVERIFY(!controller_->hasContent());
    controller_->attemptMove(move("e2", "e4"), revision);
    QVERIFY(!controller_->busy()); // Input from the older snapshot is ignored.
    play(move("e2", "e4"));
    QCOMPARE(controller_->board().sideToMove, chess::Color::Black);
    QCOMPARE(controller_->history().size(), std::size_t{2});
    QVERIFY(controller_->hasContent());
}
void AnalysisTest::branchesAndAnnotations() {
    play(move("e2", "e4"));
    const auto e4 = controller_->currentNode();
    controller_->toggleAnnotation({square("e4"), square("e4"), 0});
    controller_->toggleAnnotation({square("e4"), square("e5"), 1});
    QCOMPARE(controller_->board().annotations.size(), std::size_t{2});
    controller_->undoAnnotation();
    QCOMPARE(controller_->board().annotations.size(), std::size_t{1});
    controller_->redoAnnotation();
    play(move("e7", "e5"));
    QVERIFY(controller_->board().annotations.empty());
    controller_->previous();
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), e4);
    QCOMPARE(controller_->board().annotations.size(), std::size_t{2});
    play(move("c7", "c5"));
    const auto c5 = controller_->currentNode();
    controller_->previous();
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->continuations().size(), std::size_t{2});
    QCOMPARE(controller_->continuations()[0].label, QStringLiteral("e5"));
    QCOMPARE(controller_->continuations()[1].label, QStringLiteral("c5"));
    controller_->next();
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), c5);
    controller_->beginning();
    QTRY_VERIFY(!controller_->busy());
    controller_->end();
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), c5);
    controller_->navigate(e4);
    QTRY_VERIFY(!controller_->busy());
    controller_->clearAnnotations();
    QVERIFY(controller_->board().annotations.empty());
    controller_->undoAnnotation();
    QCOMPARE(controller_->board().annotations.size(), std::size_t{2});
    controller_->reset();
    QTRY_VERIFY(!controller_->busy());
    QVERIFY(!controller_->hasContent());
    QVERIFY(!controller_->canUndoAnnotation());
}
void AnalysisTest::specialMoves_data() {
    QTest::addColumn<QString>("fen");
    QTest::addColumn<QString>("uci");
    QTest::addColumn<QString>("expectedFen");
    QTest::newRow("castle") << QStringLiteral("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1")
                            << QStringLiteral("e1g1")
                            << QStringLiteral("r3k2r/8/8/8/8/8/8/R4RK1 b kq - 1 1");
    QTest::newRow("en-passant") << QStringLiteral("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2")
                                << QStringLiteral("e5d6")
                                << QStringLiteral("4k3/8/3P4/8/8/8/8/4K3 b - - 0 2");
    QTest::newRow("underpromotion")
        << QStringLiteral("4k3/P7/8/8/8/8/8/4K3 w - - 0 1") << QStringLiteral("a7a8n")
        << QStringLiteral("N3k3/8/8/8/8/8/8/4K3 b - - 0 1");
}
void AnalysisTest::specialMoves() {
    QFETCH(QString, fen);
    QFETCH(QString, uci);
    QFETCH(QString, expectedFen);
    controller_->reset(fen.toStdString());
    QTRY_VERIFY(!controller_->busy());
    auto position = std::get<chess::Position>(chess::Position::fromFen(fen.toStdString()));
    const auto attempted = std::get<chess::Move>(position.parseUci(uci.toStdString()));
    play(attempted);
    const auto expected =
        std::get<chess::Position>(chess::Position::fromFen(expectedFen.toStdString()));
    for (int i = 0; i < 64; ++i) {
        QCOMPARE(controller_->board().pieces[static_cast<std::size_t>(i)],
                 expected.pieceAt(*chess::Square::fromCoordinates(i % 8, i / 8)));
    }
    QCOMPARE(controller_->board().sideToMove, expected.sideToMove());
}
void AnalysisTest::failedResetPreservesSession() {
    play(move("d2", "d4"));
    controller_->toggleAnnotation({square("d4"), square("d5"), 2});
    const auto node = controller_->currentNode();
    QSignalSpy failed(controller_.get(), &app::AnalysisController::failed);
    controller_->reset(std::string("invalid FEN"));
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(failed.count(), 1);
    QCOMPARE(controller_->currentNode(), node);
    QCOMPARE(controller_->board().annotations.size(), std::size_t{1});
    QVERIFY(controller_->hasContent());
}
void AnalysisTest::geometryAndAccessibility() {
    ui::ChessBoardWidget board;
    board.setState(controller_->board());
    board.resize(617, 503);
    board.show();
    QTest::qWait(1);
    const auto rect = board.boardRect();
    QCOMPARE(rect.width(), rect.height());
    QVERIFY(!board.squareAt(rect.topLeft() - QPointF(1, 1)));
    QVERIFY(!board.squareAt(QPointF(rect.right(), rect.bottom())));
    for (const bool flipped : {false, true}) {
        auto state = controller_->board();
        state.blackAtBottom = flipped;
        board.setState(state);
        for (int i = 0; i < 64; ++i) {
            const auto location = *chess::Square::fromCoordinates(i % 8, i / 8);
            QCOMPARE(board.squareAt(board.squareRect(location).center()), std::optional{location});
        }
    }
    auto* accessible = QAccessible::queryAccessibleInterface(&board);
    QVERIFY(accessible);
    QCOMPARE(accessible->childCount(), 64);
    auto* a1 = accessible->child(0);
    QVERIFY(a1->text(QAccessible::Name).contains(QStringLiteral("a1, White rook")));
    QVERIFY(a1->actionInterface());
    a1->actionInterface()->doAction(QAccessibleActionInterface::setFocusAction());
    QCOMPARE(board.focusedSquare(), square("a1"));
    QVERIFY(board.entryMode());
}
void AnalysisTest::clickDragAndKeyboard() {
    ui::AnalysisPage page(*controller_);
    page.resize(900, 850);
    page.show();
    auto* board = page.findChild<ui::ChessBoardWidget*>();
    auto preferences = page.preferences();
    preferences.animation = false;
    preferences.inputMode = app::BoardInputMode::Click;
    page.setPreferences(preferences);
    QTest::mouseClick(board, Qt::LeftButton, Qt::NoModifier,
                      board->squareRect(square("e2")).center().toPoint());
    QCOMPARE(controller_->board().selection, std::optional{square("e2")});
    QTest::mouseClick(board, Qt::LeftButton, Qt::NoModifier,
                      board->squareRect(square("e4")).center().toPoint());
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->history().size(), std::size_t{2});
    preferences.inputMode = app::BoardInputMode::Drag;
    page.setPreferences(preferences);
    const auto from = board->squareRect(square("e7")).center().toPoint();
    const auto to = board->squareRect(square("e5")).center().toPoint();
    QTest::mousePress(board, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(board, to);
    QTest::mouseRelease(board, Qt::LeftButton, Qt::NoModifier, to);
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->history().size(), std::size_t{3});
    boardKey(board, Qt::Key_Left);
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->history().size(), std::size_t{2});
    boardKey(board, Qt::Key_Return);
    const auto current = controller_->currentNode();
    boardKey(board, Qt::Key_Right);
    QCOMPARE(controller_->currentNode(), current); // Square navigation cannot advance history.
    boardKey(board, Qt::Key_Escape);
    if (board->entryMode()) {
        boardKey(board, Qt::Key_Escape);
    }
    QVERIFY(!board->entryMode());
}
void AnalysisTest::annotationsAndCancellation() {
    ui::AnalysisPage page(*controller_);
    page.resize(900, 850);
    page.show();
    auto* board = page.findChild<ui::ChessBoardWidget*>();
    const auto from = board->squareRect(square("e2")).center().toPoint();
    const auto to = board->squareRect(square("e4")).center().toPoint();
    QTest::mouseClick(board, Qt::RightButton, Qt::ShiftModifier, from);
    QCOMPARE(controller_->board().annotations.size(), std::size_t{1});
    QCOMPARE(controller_->board().annotations.front().color, 1);
    QTest::mousePress(board, Qt::RightButton, Qt::ControlModifier, from);
    QTest::mouseMove(board, to);
    QTest::mouseRelease(board, Qt::RightButton, Qt::ControlModifier, to);
    QCOMPARE(controller_->board().annotations.size(), std::size_t{2});
    QCOMPARE(controller_->board().annotations.back().to, square("e4"));
    QTest::mousePress(board, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(board, to);
    board->hide();
    board->show();
    QTest::mouseRelease(board, Qt::LeftButton, Qt::NoModifier, to);
    QVERIFY(!controller_->busy());
    QCOMPARE(controller_->history().size(), std::size_t{1});
    QTest::mousePress(board, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(board, to);
    controller_->flip();
    QTest::mouseRelease(board, Qt::LeftButton, Qt::NoModifier, to);
    QCOMPARE(controller_->history().size(), std::size_t{1});
    controller_->flip();
    board->focusSquare(square("d4"));
    boardKey(board, Qt::Key_H);
    QCOMPARE(controller_->board().annotations.size(), std::size_t{3});
}
void AnalysisTest::promotionChoiceAndCancellation() {
    controller_->reset(std::string("4k3/P7/8/8/8/8/8/4K3 w - - 0 1"));
    QTRY_VERIFY(!controller_->busy());
    ui::AnalysisPage page(*controller_);
    page.resize(900, 850);
    page.show();
    auto* board = page.findChild<ui::ChessBoardWidget*>();
    board->activateSquare(square("a7"));
    board->activateSquare(square("a8"));
    auto* popup = board->findChild<QMenu*>(QStringLiteral("promotionMenu"));
    QVERIFY(popup && popup->isVisible());
    // Escape dismisses the popup on press; it schedules its own deletion.
    QTest::keyPress(popup, Qt::Key_Escape);
    QCOMPARE(controller_->history().size(), std::size_t{1});
    board->activateSquare(square("a8"));
    popup = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    QVERIFY(popup);
    popup->actions().back()->trigger();
    popup->close();
    QTRY_VERIFY(!controller_->busy());
    const auto piece = controller_->board().pieces[static_cast<std::size_t>(square("a8").index())];
    QVERIFY(piece);
    QCOMPARE(piece->type, chess::PieceType::Knight);
}
void AnalysisTest::positionChangeCancelsPromotion() {
    controller_->reset(std::string("4k3/P7/8/8/8/8/8/4K3 w - - 0 1"));
    QTRY_VERIFY(!controller_->busy());
    ui::AnalysisPage page(*controller_);
    page.resize(900, 850);
    page.show();
    auto* board = page.findChild<ui::ChessBoardWidget*>();
    board->activateSquare(square("a7"));
    board->activateSquare(square("a8"));
    QVERIFY(QApplication::activePopupWidget());
    controller_->reset();
    QTRY_VERIFY(!controller_->busy());
    QVERIFY(!QApplication::activePopupWidget());
    QVERIFY(!controller_->hasContent());
    QCOMPARE(controller_->board().pieces[static_cast<std::size_t>(square("a8").index())],
             (std::optional{chess::Piece{chess::Color::Black, chess::PieceType::Rook}}));
}
void AnalysisTest::preferenceRoundTrip() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto filename = directory.filePath(QStringLiteral("ui.ini"));
    app::UiState state;
    state.board.inputMode = app::BoardInputMode::Click;
    state.board.coordinates = false;
    state.board.animation = false;
    state.board.annotationColors[0] = QStringLiteral("#123abc");
    app::UiStateStore writer;
    QSignalSpy loaded(&writer, &app::UiStateStore::loaded);
    QSignalSpy closed(&writer, &app::UiStateStore::closed);
    writer.load(filename);
    QTRY_COMPARE(loaded.count(), 1);
    writer.saveAndClose(state);
    QTRY_COMPARE(closed.count(), 1);
    app::UiStateStore reader;
    QSignalSpy restored(&reader, &app::UiStateStore::loaded);
    QSignalSpy readerClosed(&reader, &app::UiStateStore::closed);
    reader.load(filename);
    QTRY_COMPARE(restored.count(), 1);
    QCOMPARE(qvariant_cast<app::UiState>(restored.front().front()).board, state.board);
    reader.saveAndClose();
    QTRY_COMPARE(readerClosed.count(), 1);
}
void AnalysisTest::invalidPreferences() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto filename = directory.filePath(QStringLiteral("ui.ini"));
    {
        QSettings settings(filename, QSettings::IniFormat);
        settings.setValue(QStringLiteral("board/v1/color0"), QStringLiteral("#oops"));
    }
    app::UiStateStore store;
    QSignalSpy loaded(&store, &app::UiStateStore::loaded);
    QSignalSpy failed(&store, &app::UiStateStore::failed);
    QSignalSpy closed(&store, &app::UiStateStore::closed);
    store.load(filename);
    QTRY_COMPARE(loaded.count(), 1);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(qvariant_cast<app::UiState>(loaded.front().front()).board, app::BoardPreferences{});
    store.saveAndClose();
    QTRY_COMPARE(closed.count(), 1);
}
void AnalysisTest::discardGate() {
    app::NavigationController navigation;
    ui::MainWindow window(navigation, *controller_);
    window.show();
    play(move("e2", "e4"));
    QSignalSpy requested(&window, &ui::MainWindow::closeRequested);
    const auto respond = [](QMessageBox::StandardButton button) {
        QTimer::singleShot(0, qApp, [button] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->button(button)->click();
        });
    };
    respond(QMessageBox::Cancel);
    window.close();
    QTRY_VERIFY(!QApplication::activeModalWidget());
    QCOMPARE(requested.count(), 0);
    QVERIFY(controller_->hasContent());
    respond(QMessageBox::Discard);
    window.close();
    QTRY_COMPARE(requested.count(), 1);
    window.completeShutdown();
}
void AnalysisTest::closeDuringWork() {
    QSignalSpy accepted(controller_.get(), &app::AnalysisController::moveAccepted);
    QSignalSpy closed(controller_.get(), &app::AnalysisController::closed);
    controller_->attemptMove(move("e2", "e4"), controller_->board().revision);
    controller_->close();
    QTRY_COMPARE(closed.count(), 1);
    QCOMPARE(accepted.count(), 0);
    QVERIFY(!controller_->board().interactive);
    controller_.reset();
}
void AnalysisTest::reentrantCloseSuppressesMove_data() {
    QTest::addColumn<bool>("closeWhenBusy");
    QTest::newRow("admission") << true;
    QTest::newRow("delivery") << false;
}
void AnalysisTest::reentrantCloseSuppressesMove() {
    QFETCH(bool, closeWhenBusy);
    QSignalSpy accepted(controller_.get(), &app::AnalysisController::moveAccepted);
    QSignalSpy closed(controller_.get(), &app::AnalysisController::closed);
    connect(controller_.get(), &app::AnalysisController::changed, this, [this, closeWhenBusy] {
        if (controller_->busy() == closeWhenBusy && (closeWhenBusy || controller_->hasContent())) {
            controller_->close();
        }
    });
    controller_->attemptMove(move("e2", "e4"), controller_->board().revision);
    QTRY_COMPARE(closed.count(), 1);
    QCOMPARE(accepted.count(), 0);
    controller_.reset();
}
void AnalysisTest::layout_data() {
    QTest::addColumn<QSize>("size");
    QTest::addColumn<bool>("flipped");
    QTest::addColumn<bool>("enlarged");
    QTest::newRow("desktop") << QSize(1360, 850) << false << false;
    QTest::newRow("laptop-flipped") << QSize(1100, 720) << true << false;
    QTest::newRow("constrained") << QSize(800, 600) << false << false;
    QTest::newRow("large-font") << QSize(1100, 720) << false << true;
}
void AnalysisTest::layout() {
    QFETCH(QSize, size);
    QFETCH(bool, flipped);
    QFETCH(bool, enlarged);
    app::NavigationController navigation;
    ui::MainWindow window(navigation, *controller_);
    navigation.navigate(app::Destination::Analyze);
    if (enlarged) {
        auto font = window.font();
        font.setPointSizeF(font.pointSizeF() * 1.5);
        window.setFont(font);
    }
    window.resize(size);
    window.show();
    window.showReady();
    play(move("e2", "e4"));
    controller_->toggleAnnotation({square("g8"), square("f6"), 3});
    controller_->toggleAnnotation({square("e4"), square("e4"), 1});
    controller_->selectSquare(square("d7"));
    if (flipped) {
        controller_->flip();
    }
    auto* board = window.findChild<ui::ChessBoardWidget*>();
    QVERIFY(board);
    QTest::qWait(180);
    QVERIFY(window.width() <= size.width());
    QVERIFY(window.height() <= size.height());
    QCOMPARE(board->boardRect().width(), board->boardRect().height());
    QVERIFY(board->boardRect().width() >= 240);
    // At supported desktop sizes essential commands remain on screen, not merely
    // reachable by scrolling past a board that retained its preferred height.
    const auto* history = window.findChild<QComboBox*>(QStringLiteral("analysisHistory"));
    const auto* options = window.findChild<QToolButton*>(QStringLiteral("boardOptionsButton"));
    QVERIFY(history && options);
    QVERIFY(window.rect().contains(QRect(history->mapTo(&window, QPoint{}), history->size())));
    QVERIFY(window.rect().contains(QRect(options->mapTo(&window, QPoint{}), options->size())));
    if (qEnvironmentVariableIsSet("CHESS_REVIEW_SCREENSHOT_DIR")) {
        const auto filename = qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR") +
                              QLatin1Char('/') + QString::fromLatin1(QTest::currentDataTag()) +
                              QStringLiteral(".png");
        QVERIFY(window.grab().save(filename));
    }
    window.completeShutdown();
}
int main(int argc, char* argv[]) {
    const TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("chess-review"));
    application.setQuitOnLastWindowClosed(false);
    AnalysisTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "AnalysisTest.moc"
