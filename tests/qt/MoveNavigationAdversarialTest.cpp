#include "TestEnvironment.h"
#include "app/AnalysisController.h"
#include "ui/analysis/AnalysisPage.h"
#include "ui/analysis/MoveNavigation.h"
#include <QAccessible>
#include <QApplication>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>
#include <array>
#include <memory>

using namespace chess_review;
namespace {
chess::Move move(const char* from, const char* to) {
    return {*chess::Square::fromName(from), *chess::Square::fromName(to), std::nullopt};
}
} // namespace
class MoveNavigationAdversarialTest final : public QObject {
    Q_OBJECT
  private slots:
    void init();
    void cleanup();
    void explicitChoiceSurvivesAncestorIntent_data();
    void explicitChoiceSurvivesAncestorIntent();
    void timelineSelectionTracksPublishedPosition();
    void wrappedMoveRemainsVisible();
    void accessibilityRejectsAnotherViewsChild();
    void selectedMoveShowsKeyboardFocus();
    void scrollingClearsHover();
    void initialButtonsRespectBoundaries();
    void timelineUsesViewPaletteAndFont();

  private:
    void play(chess::Move value);
    void line(int plies = 8);
    std::unique_ptr<app::AnalysisController> controller_;
};
void MoveNavigationAdversarialTest::init() {
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
}
void MoveNavigationAdversarialTest::cleanup() {
    QSignalSpy closed(controller_.get(), &app::AnalysisController::closed);
    controller_->close();
    QTRY_COMPARE(closed.count(), 1);
    controller_.reset();
}
void MoveNavigationAdversarialTest::play(chess::Move value) {
    controller_->attemptMove(value, controller_->board().revision);
    QTRY_VERIFY(!controller_->busy());
}
void MoveNavigationAdversarialTest::line(int plies) {
    const std::array moves{move("g1", "f3"), move("g8", "f6"), move("f3", "g1"), move("f6", "g8")};
    for (int i = 0; i < plies; ++i) {
        play(moves[static_cast<std::size_t>(i % 4)]);
    }
}
void MoveNavigationAdversarialTest::explicitChoiceSurvivesAncestorIntent_data() {
    QTest::addColumn<bool>("otherRootBranch");
    QTest::newRow("same-fork") << false;
    QTest::newRow("ancestor-fork") << true;
}
void MoveNavigationAdversarialTest::explicitChoiceSurvivesAncestorIntent() {
    QFETCH(bool, otherRootBranch);
    play(move("e2", "e4"));
    const auto e4 = controller_->currentNode();
    play(move("e7", "e5"));
    auto explicitChoice = controller_->currentNode();
    if (otherRootBranch) {
        controller_->beginning();
        QTRY_VERIFY(!controller_->busy());
        play(move("d2", "d4"));
        play(move("d7", "d5"));
        explicitChoice = controller_->currentNode();
    }
    controller_->navigate(e4);
    QTRY_VERIFY(!controller_->busy());
    controller_->attemptMove(move("c7", "c5"), controller_->board().revision);
    controller_->navigate(explicitChoice);
    controller_->beginning(); // An ancestor intent must preserve earlier explicit fork choices.
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), chess::NodeId{0});
    controller_->end();
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), explicitChoice);
    if (otherRootBranch) {
        controller_->navigate(e4);
        controller_->end();
        QTRY_VERIFY(!controller_->busy());
        QCOMPARE(controller_->history().back().label, QStringLiteral("1... c5"));
    }
}
void MoveNavigationAdversarialTest::timelineSelectionTracksPublishedPosition() {
    line();
    const auto last = controller_->currentNode();
    ui::MoveTimelineView timeline(*controller_);
    timeline.resize(350, 110);
    timeline.show();
    QTest::qWait(1);
    auto* accessible = QAccessible::queryAccessibleInterface(&timeline)->tableInterface();
    QVERIFY(accessible);
    auto* first = accessible->cellAt(1, 1);
    first->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), chess::NodeId{1});
    controller_->end();
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), last);
    QVERIFY(accessible->cellAt(4, 2)->state().selected);
    QVERIFY(!first->state().selected);
    first->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), chess::NodeId{1});
}
void MoveNavigationAdversarialTest::timelineUsesViewPaletteAndFont() {
    line();
    controller_->beginning();
    QTRY_VERIFY(!controller_->busy());
    ui::MoveTimelineView timeline(*controller_);
    auto palette = timeline.palette();
    palette.setColor(QPalette::Highlight, QColor("#59184c"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    timeline.setPalette(palette);
    auto font = timeline.font();
    font.setPointSizeF(font.pointSizeF() * 1.5);
    timeline.setFont(font);
    timeline.resize(500, 110);
    timeline.show();
    QTest::qWait(1);
    const auto selected = timeline.model()->index(0, 1);
    const auto rect = timeline.visualRect(selected);
    const auto image = timeline.viewport()->grab().toImage();
    const auto sample = QPoint(rect.right() - 12, rect.center().y()) * image.devicePixelRatio();
    QCOMPARE(image.pixelColor(sample), palette.color(QPalette::Highlight));
    if (qEnvironmentVariableIsSet("CHESS_REVIEW_SCREENSHOT_DIR")) {
        QVERIFY(timeline.grab().save(qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR") +
                                     QStringLiteral("/view-palette.png")));
    }
    controller_->next();
    const auto pendingFont = timeline.model()->index(1, 1).data(Qt::FontRole);
    QVERIFY(!pendingFont.isValid() || pendingFont.value<QFont>().pointSizeF() == font.pointSizeF());
    QTRY_VERIFY(!controller_->busy());
}
void MoveNavigationAdversarialTest::wrappedMoveRemainsVisible() {
    line();
    ui::GameTreeView tree(*controller_);
    tree.resize(220, 80);
    tree.show();
    QTest::qWait(1);
    const auto target = controller_->target(controller_->currentNode());
    tree.focusMove(target);
    QTest::qWait(1);
    QVERIFY2(tree.viewport()->rect().contains(tree.moveRect(target)),
             qPrintable(QStringLiteral("Token top %1 bottom %2, viewport height %3")
                            .arg(tree.moveRect(target).top())
                            .arg(tree.moveRect(target).bottom())
                            .arg(tree.viewport()->height())));
    if (qEnvironmentVariableIsSet("CHESS_REVIEW_SCREENSHOT_DIR")) {
        QVERIFY(tree.grab().save(qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR") +
                                 QStringLiteral("/wrapped-focus.png")));
    }
}
void MoveNavigationAdversarialTest::accessibilityRejectsAnotherViewsChild() {
    ui::GameTreeView first(*controller_);
    ui::GameTreeView second(*controller_);
    auto* left = QAccessible::queryAccessibleInterface(&first);
    auto* right = QAccessible::queryAccessibleInterface(&second);
    QCOMPARE(left->indexOfChild(right->child(0)), -1);
}
void MoveNavigationAdversarialTest::selectedMoveShowsKeyboardFocus() {
    line();
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* tree = new ui::GameTreeView(*controller_, &host);
    auto* editor = new QLineEdit(&host);
    layout->addWidget(tree);
    layout->addWidget(editor);
    host.resize(400, 240);
    host.show();
    editor->setFocus();
    QTRY_VERIFY(editor->hasFocus());
    const auto target = controller_->target(controller_->currentNode());
    const auto before = tree->viewport()->grab(tree->moveRect(target)).toImage();
    tree->focusMove(target);
    QTRY_VERIFY(tree->hasFocus());
    const auto after = tree->viewport()->grab(tree->moveRect(target)).toImage();
    QVERIFY(before != after);
    if (qEnvironmentVariableIsSet("CHESS_REVIEW_SCREENSHOT_DIR")) {
        QVERIFY(host.grab().save(qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR") +
                                 QStringLiteral("/focus-contrast.png")));
    }
}
void MoveNavigationAdversarialTest::scrollingClearsHover() {
    line(24);
    ui::GameTreeView tree(*controller_);
    tree.resize(300, 100);
    tree.show();
    QTest::qWait(1);
    const auto target = controller_->target(chess::NodeId{1});
    tree.focusMove(target);
    QTest::mouseMove(tree.viewport(), tree.moveRect(target).center());
    QVERIFY(tree.isHovered(target));
    QVERIFY(tree.verticalScrollBar()->maximum() > 0);
    tree.verticalScrollBar()->setValue(tree.verticalScrollBar()->maximum());
    QVERIFY(!tree.isHovered(target));
}
void MoveNavigationAdversarialTest::initialButtonsRespectBoundaries() {
    ui::AnalysisPage page(*controller_);
    const auto buttons = page.findChildren<QPushButton*>();
    int found = 0;
    for (const auto* button : buttons) {
        if (button->text() == QStringLiteral("Beginning") ||
            button->text() == QStringLiteral("Previous") ||
            button->text() == QStringLiteral("Next") || button->text() == QStringLiteral("End")) {
            ++found;
            QVERIFY(!button->isEnabled());
        }
    }
    QCOMPARE(found, 4);
}
int main(int argc, char* argv[]) {
    const TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QApplication application(argc, argv);
    MoveNavigationAdversarialTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "MoveNavigationAdversarialTest.moc"
