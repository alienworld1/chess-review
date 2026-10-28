#include "TestEnvironment.h"
#include "app/AnalysisController.h"
#include "app/UiStateStore.h"
#include "ui/analysis/AnalysisPage.h"
#include "ui/analysis/MoveNavigation.h"
#include "ui/chess/ChessBoardWidget.h"
#include "ui/shell/MainWindow.h"
#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
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
    void dropSettlement_data();
    void dropSettlement();
    void terminalStates_data();
    void terminalStates();
    void controlsReference();
    void branchesAndAnnotations();
    void directBranchNavigation();
    void releaseWithoutMoveEvent();
    void cancelledPromotionCannotSubmit_data();
    void cancelledPromotionCannotSubmit();
    void unavailableBoardActions();
    void numberedHistory();
    void transposedAnnotationsAndReuse();
    void specialMoves_data();
    void specialMoves();
    void failedResetPreservesSession();
    void geometryAndAccessibility();
    void accessibleMoveDestinations_data();
    void accessibleMoveDestinations();
    void annotationBounds();
    void clickDragAndKeyboard();
    void annotationsAndCancellation();
    void promotionChoiceAndCancellation();
    void positionChangeCancelsPromotion();
    void preferenceRoundTrip();
    void customColorPresentation();
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
void AnalysisTest::dropSettlement_data() {
    QTest::addColumn<bool>("drag");
    QTest::newRow("drag-settles") << true;
    QTest::newRow("click-animates") << false;
}
void AnalysisTest::dropSettlement() {
    const auto initial = controller_->board();
    play(move("e2", "e4"));
    const auto accepted = controller_->board();
    ui::ChessBoardWidget board;
    board.resize(500, 500);
    board.show();
    QTest::qWait(1); // Deliver initial show/resize/focus events before starting a gesture.
    board.setState(initial);
    QFETCH(bool, drag);
    app::BoardPreferences preferences;
    preferences.inputMode = drag ? app::BoardInputMode::Drag : app::BoardInputMode::Click;
    board.setPreferences(preferences);
    // Model the immutable snapshot consumer without relying on animation timing
    // across worker scheduling: accepted input is delivered explicitly below.
    connect(&board, &ui::ChessBoardWidget::squareSelected, &board, [&](chess::Square selected) {
        auto state = initial;
        state.selection = selected;
        board.setState(state);
    });
    QSignalSpy attempted(&board, &ui::ChessBoardWidget::moveAttempted);
    const auto from = board.squareRect(square("e2")).center().toPoint();
    const auto to = board.squareRect(square("e4")).center().toPoint();
    if (drag) {
        QTest::mousePress(&board, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(&board, to);
        QTest::mouseRelease(&board, Qt::LeftButton, Qt::NoModifier, to);
    } else {
        QTest::mouseClick(&board, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseClick(&board, Qt::LeftButton, Qt::NoModifier, to);
    }
    QCOMPARE(attempted.count(), 1);
    auto validating = initial;
    validating.interactive = false;
    board.setState(validating);
    const auto pendingImage = board.grab().toImage();
    board.setState(accepted);
    board.animateMove(move("e2", "e4"));
    const auto firstFrame = board.grab().toImage();
    QTest::qWait(220);
    const auto finalFrame = board.grab().toImage();
    if (drag) {
        QCOMPARE(firstFrame, finalFrame);
        // The pending drop must also already show the piece at the destination.
        QCOMPARE(pendingImage.pixelColor(to), finalFrame.pixelColor(to));
        QVERIFY(pendingImage.pixelColor(to) != QColor(QStringLiteral("#e9e1d0")));
    } else {
        QVERIFY(firstFrame != finalFrame);
    }
    // A rejected drag clears the preview and restores the actual position.
    board.setState(initial);
    QTest::mousePress(&board, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(&board, to);
    QTest::mouseRelease(&board, Qt::LeftButton, Qt::NoModifier, to);
    board.setState(validating);
    auto rejected = initial;
    rejected.revision = accepted.revision + 1;
    board.setState(rejected);
    const auto rejectedImage = board.grab().toImage();
    board.setState(initial);
    QCOMPARE(rejectedImage, board.grab().toImage());
}
void AnalysisTest::terminalStates_data() {
    QTest::addColumn<QString>("fen");
    QTest::addColumn<app::BoardStatus>("status");
    QTest::addColumn<QString>("description");
    QTest::newRow("check") << QStringLiteral("4r1k1/8/8/8/8/8/8/4K3 w - - 0 1")
                           << app::BoardStatus::Check << QStringLiteral("White to move — check");
    QTest::newRow("white-mated") << QStringLiteral("8/8/8/8/8/5k2/6q1/7K w - - 0 1")
                                 << app::BoardStatus::Checkmate
                                 << QStringLiteral("Checkmate — Black wins");
    QTest::newRow("black-mated") << QStringLiteral("7k/6Q1/5K2/8/8/8/8/8 b - - 0 1")
                                 << app::BoardStatus::Checkmate
                                 << QStringLiteral("Checkmate — White wins");
    QTest::newRow("stalemate") << QStringLiteral("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1")
                               << app::BoardStatus::Stalemate << QStringLiteral("Stalemate — draw");
}
void AnalysisTest::terminalStates() {
    QFETCH(QString, fen);
    QFETCH(app::BoardStatus, status);
    QFETCH(QString, description);
    controller_->reset(fen.toStdString());
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->board().status, status);
    ui::AnalysisPage page(*controller_);
    page.resize(900, 850);
    page.show();
    auto* board = page.findChild<ui::ChessBoardWidget*>();
    QCOMPARE(page.findChild<QLabel*>(QStringLiteral("analysisStatus"))->text(), description);
    QVERIFY(board->accessibleDescription().contains(description));
    if (status == app::BoardStatus::Checkmate) {
        QVERIFY(controller_->board().legalMoves.empty());
        QVERIFY(controller_->board().checkedKing);
        const auto king = *controller_->board().checkedKing;
        QVERIFY(board->squareDescription(king).contains(QStringLiteral("checkmated")));
        const auto mateImage = board->grab().toImage();
        auto checkState = controller_->board();
        checkState.status = app::BoardStatus::Check;
        board->setState(checkState);
        QVERIFY(board->grab().toImage() != mateImage); // Mate adds a visible # badge.
        board->setState(controller_->board());
        controller_->selectSquare(king);
        QVERIFY(!controller_->board().selection);
        controller_->toggleAnnotation({king, king, 0});
        QCOMPARE(controller_->board().annotations.size(), std::size_t{1});
    } else if (status == app::BoardStatus::Check) {
        QVERIFY(!controller_->board().legalMoves.empty());
    } else {
        QVERIFY(controller_->board().legalMoves.empty());
        QVERIFY(!controller_->board().checkedKing);
    }
    if (qEnvironmentVariableIsSet("CHESS_REVIEW_SCREENSHOT_DIR")) {
        QVERIFY(page.grab().save(qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR") +
                                 QLatin1Char('/') + QString::fromLatin1(QTest::currentDataTag()) +
                                 QStringLiteral(".png")));
    }
}
void AnalysisTest::controlsReference() {
    ui::AnalysisPage page(*controller_);
    page.resize(900, 850);
    page.show();
    auto* options = page.findChild<QToolButton*>(QStringLiteral("boardOptionsButton"));
    QVERIFY(options);
    options->menu()->actions().back()->trigger();
    auto* dialog = page.findChild<QDialog*>(QStringLiteral("boardControlsDialog"));
    QVERIFY(dialog && dialog->isVisible());
    auto* reference = dialog->findChild<QTextBrowser*>();
    QVERIFY(reference);
    const auto text = reference->toPlainText();
    for (const auto& section :
         {QStringLiteral("Moving pieces"), QStringLiteral("Move history"),
          QStringLiteral("Arrows and highlights"), QStringLiteral("Colors and board options")}) {
        QVERIFY(text.contains(section));
    }
    QVERIFY(reference->document()->blockCount() > 15);
    QVERIFY(reference->isReadOnly());
    if (qEnvironmentVariableIsSet("CHESS_REVIEW_SCREENSHOT_DIR")) {
        QVERIFY(dialog->grab().save(qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR") +
                                    QStringLiteral("/board-controls.png")));
    }
    dialog->close();
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
    QCOMPARE(controller_->continuations()[0].label, QStringLiteral("1... e5"));
    QCOMPARE(controller_->continuations()[1].label, QStringLiteral("1... c5"));
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
void AnalysisTest::directBranchNavigation() {
    play(move("e2", "e4"));
    play(move("e7", "e5"));
    play(move("g1", "f3"));
    const auto firstLine = controller_->currentNode();
    controller_->beginning();
    QTRY_VERIFY(!controller_->busy());
    play(move("d2", "d4"));
    play(move("d7", "d5"));
    controller_->navigate(firstLine);
    QTRY_VERIFY(!controller_->busy());
    controller_->beginning();
    QTRY_VERIFY(!controller_->busy());
    controller_->end();
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), firstLine);
}
void AnalysisTest::releaseWithoutMoveEvent() {
    ui::AnalysisPage page(*controller_);
    page.resize(900, 850);
    page.show();
    auto* board = page.findChild<ui::ChessBoardWidget*>();
    const auto from = board->squareRect(square("e2")).center().toPoint();
    const auto to = board->squareRect(square("e4")).center().toPoint();
    // Platforms can coalesce pointer movement; the release still proves displacement.
    QTest::mousePress(board, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseRelease(board, Qt::LeftButton, Qt::NoModifier, to);
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->history().size(), std::size_t{2});
    QTest::mousePress(board, Qt::RightButton, Qt::NoModifier, from);
    QTest::mouseRelease(board, Qt::RightButton, Qt::NoModifier, to);
    QCOMPARE(controller_->board().annotations.size(), std::size_t{1});
    QCOMPARE(controller_->board().annotations.front().to, square("e4"));
}
void AnalysisTest::cancelledPromotionCannotSubmit_data() {
    QTest::addColumn<int>("cancellation");
    QTest::newRow("flip") << 0;
    QTest::newRow("hide") << 1;
    QTest::newRow("disable") << 2;
    QTest::newRow("preferences") << 3;
}
void AnalysisTest::cancelledPromotionCannotSubmit() {
    controller_->reset(std::string("4k3/P7/8/8/8/8/8/4K3 w - - 0 1"));
    QTRY_VERIFY(!controller_->busy());
    ui::AnalysisPage page(*controller_);
    page.resize(900, 850);
    page.show();
    auto* board = page.findChild<ui::ChessBoardWidget*>();
    board->activateSquare(square("a7"));
    board->activateSquare(square("a8"));
    auto* popup = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    QVERIFY(popup);
    auto* choice = popup->actions().front();
    QFETCH(int, cancellation);
    switch (cancellation) {
    case 0:
        controller_->flip();
        break;
    case 1:
        board->hide();
        board->show();
        break;
    case 2:
        board->setEnabled(false);
        board->setEnabled(true);
        break;
    default:
        board->setPreferences(page.preferences());
        break;
    }
    QVERIFY(!QApplication::activePopupWidget());
    choice->trigger(); // A late action must not resurrect the cancelled gesture.
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->history().size(), std::size_t{1});
}
void AnalysisTest::unavailableBoardActions() {
    ui::ChessBoardWidget board;
    board.setState(controller_->board());
    board.resize(500, 500);
    auto* accessible = QAccessible::queryAccessibleInterface(&board);
    QVERIFY(accessible);
    auto* action = accessible->child(square("e2").index())->actionInterface();
    QSignalSpy selections(&board, &ui::ChessBoardWidget::squareSelected);
    action->doAction(QAccessibleActionInterface::pressAction());
    QCOMPARE(selections.count(), 0); // Hidden squares are unavailable.
    board.show();
    board.setEnabled(false);
    action->doAction(QAccessibleActionInterface::pressAction());
    QCOMPARE(selections.count(), 0);
    QVERIFY(accessible->child(square("e2").index())->state().disabled);
}
void AnalysisTest::numberedHistory() {
    controller_->reset(std::string("4k3/8/8/8/8/8/8/4K3 b - - 0 42"));
    QTRY_VERIFY(!controller_->busy());
    play(move("e8", "d8"));
    QCOMPARE(controller_->history().back().label, QStringLiteral("42... Kd8"));
    play(move("e1", "d1"));
    QCOMPARE(controller_->history().back().label, QStringLiteral("43. Kd1"));
    controller_->previous();
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->continuations().front().label, QStringLiteral("43. Kd1"));
}
void AnalysisTest::transposedAnnotationsAndReuse() {
    play(move("g1", "f3"));
    play(move("g8", "f6"));
    play(move("g2", "g3"));
    play(move("g7", "g6"));
    const auto first = controller_->currentNode();
    const auto position = controller_->board().pieces;
    controller_->toggleAnnotation({square("f3"), square("g5"), 0});
    controller_->beginning();
    QTRY_VERIFY(!controller_->busy());
    play(move("g2", "g3"));
    play(move("g7", "g6"));
    play(move("g1", "f3"));
    play(move("g8", "f6"));
    const auto second = controller_->currentNode();
    QVERIFY(first != second);
    QCOMPARE(controller_->board().pieces, position);
    QVERIFY(controller_->board().annotations.empty());
    QVERIFY(!controller_->canUndoAnnotation());
    controller_->toggleAnnotation({square("f6"), square("g4"), 1});
    controller_->previous();
    QTRY_VERIFY(!controller_->busy());
    play(move("g8", "f6"));
    QCOMPARE(controller_->currentNode(), second); // Reuses the existing continuation.
    QCOMPARE(controller_->board().annotations.front().color, 1);
    controller_->navigate(first);
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->board().annotations.size(), std::size_t{1});
    QCOMPARE(controller_->board().annotations.front().color, 0);
    controller_->undoAnnotation();
    QVERIFY(controller_->board().annotations.empty());
    controller_->navigate(second);
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->board().annotations.size(), std::size_t{1});
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
void AnalysisTest::accessibleMoveDestinations_data() {
    QTest::addColumn<QString>("fen");
    QTest::addColumn<QString>("source");
    QTest::addColumn<QString>("destination");
    QTest::addColumn<QString>("description");
    QTest::newRow("quiet") << QStringLiteral("4k3/8/8/8/8/8/4P3/4K3 w - - 0 1")
                           << QStringLiteral("e2") << QStringLiteral("e4")
                           << QStringLiteral("legal move destination");
    QTest::newRow("capture") << QStringLiteral("4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 2")
                             << QStringLiteral("e4") << QStringLiteral("d5")
                             << QStringLiteral("legal capture");
    QTest::newRow("quiet-to-en-passant-square")
        << QStringLiteral("4k3/8/8/3pPN2/8/8/8/4K3 w - d6 0 2") << QStringLiteral("f5")
        << QStringLiteral("d6") << QStringLiteral("legal move destination");
    QTest::newRow("en-passant") << QStringLiteral("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2")
                                << QStringLiteral("e5") << QStringLiteral("d6")
                                << QStringLiteral("legal capture");
}
void AnalysisTest::accessibleMoveDestinations() {
    QFETCH(QString, fen);
    QFETCH(QString, source);
    QFETCH(QString, destination);
    QFETCH(QString, description);
    controller_->reset(fen.toStdString());
    QTRY_VERIFY(!controller_->busy());
    controller_->selectSquare(*chess::Square::fromName(source.toStdString()));
    ui::ChessBoardWidget board;
    board.setState(controller_->board());
    auto* accessible = QAccessible::queryAccessibleInterface(&board);
    const auto target = *chess::Square::fromName(destination.toStdString());
    const auto selected = *controller_->board().selection;
    const bool capture = std::any_of(
        controller_->board().captureMoves.begin(), controller_->board().captureMoves.end(),
        [&](const auto& move) { return move.from == selected && move.to == target; });
    QCOMPARE(capture, description == QStringLiteral("legal capture"));
    QVERIFY(accessible->child(target.index())->text(QAccessible::Name).contains(description));
    controller_->clearSelection();
    board.setState(controller_->board());
    QVERIFY(!accessible->child(target.index())->text(QAccessible::Name).contains(description));
}
void AnalysisTest::annotationBounds() {
    QSignalSpy failures(controller_.get(), &app::AnalysisController::failed);
    for (int i = 0; i < 260; ++i) {
        controller_->toggleAnnotation({*chess::Square::fromCoordinates(i % 8, i / 8 % 8),
                                       *chess::Square::fromCoordinates(i / 64, 7), 0});
    }
    QCOMPARE(controller_->board().annotations.size(), std::size_t{256});
    QCOMPARE(failures.count(), 4);
    int undos = 0;
    while (controller_->canUndoAnnotation()) {
        controller_->undoAnnotation();
        ++undos;
    }
    QCOMPARE(undos, 64);
    QCOMPARE(controller_->board().annotations.size(), std::size_t{192});
    int redos = 0;
    while (controller_->canRedoAnnotation()) {
        controller_->redoAnnotation();
        ++redos;
    }
    QCOMPARE(redos, 64);
    QCOMPARE(controller_->board().annotations.size(), std::size_t{256});
    controller_->clearAnnotations();
    QVERIFY(controller_->board().annotations.empty());
    controller_->undoAnnotation();
    QCOMPARE(controller_->board().annotations.size(), std::size_t{256});
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
    state.board.blackAtBottom = true;
    state.board.analysisSplit = 88;
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
void AnalysisTest::customColorPresentation() {
    ui::AnalysisPage page(*controller_);
    auto preferences = page.preferences();
    preferences.annotationColors[0] = QStringLiteral("#aa22cc");
    page.setPreferences(preferences);
    const auto* colors = page.findChild<QComboBox*>(QStringLiteral("annotationColor"));
    QVERIFY(colors);
    QVERIFY(!colors->itemIcon(0).isNull());
    QCOMPARE(colors->itemIcon(0).pixmap(16, 16).toImage().pixelColor(8, 8), QColor("#aa22cc"));
    QCOMPARE(colors->itemData(0, Qt::ToolTipRole).toString(), QStringLiteral("#aa22cc"));
    QCOMPARE(colors->itemText(0), QStringLiteral("Color 1"));
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
    auto* board = window.findChild<ui::ChessBoardWidget*>(QStringLiteral("chessBoard"));
    QVERIFY(board);
    QTest::qWait(180);
    if (qEnvironmentVariableIsSet("CHESS_REVIEW_SCREENSHOT_DIR")) {
        const auto filename = qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR") +
                              QLatin1Char('/') + QString::fromLatin1(QTest::currentDataTag()) +
                              QStringLiteral(".png");
        QVERIFY(window.grab().save(filename));
    }
    QVERIFY(window.width() <= size.width());
    QVERIFY(window.height() <= size.height());
    QCOMPARE(board->boardRect().width(), board->boardRect().height());
    QVERIFY2(board->boardRect().width() >= 240,
             qPrintable(QStringLiteral("Board widget %1 x %2, board side %3")
                            .arg(board->width())
                            .arg(board->height())
                            .arg(board->boardRect().width())));
    // At supported desktop sizes essential commands remain on screen, not merely
    // reachable by scrolling past a board that retained its preferred height.
    const auto* history = window.findChild<ui::MoveTimelineView*>();
    const auto* options = window.findChild<QToolButton*>(QStringLiteral("boardOptionsButton"));
    QVERIFY(history && options);
    QVERIFY(window.rect().contains(QRect(history->mapTo(&window, QPoint{}), history->size())));
    QVERIFY(window.rect().contains(QRect(options->mapTo(&window, QPoint{}), options->size())));
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
