#include "ui/analysis/MoveNavigation.h"
#include "TestEnvironment.h"
#include "app/AnalysisController.h"
#include "app/UiStateStore.h"
#include "ui/analysis/AnalysisPage.h"
#include "ui/chess/ChessBoardWidget.h"
#include <QAbstractItemModelTester>
#include <QAccessible>
#include <QApplication>
#include <QEventLoop>
#include <QKeyEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <algorithm>
#include <array>
#include <memory>

using namespace chess_review;
namespace {
chess::Move move(const char* from, const char* to) {
    return {*chess::Square::fromName(from), *chess::Square::fromName(to), std::nullopt};
}
void key(QWidget& widget, Qt::Key value, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QTest::keyPress(&widget, value, modifiers);
    QTest::keyRelease(&widget, value, modifiers);
}
} // namespace
class MoveNavigationTest final : public QObject {
    Q_OBJECT
  private slots:
    void init();
    void cleanup();
    void coalescedNavigation();
    void historyAnimation_data();
    void historyAnimation();
    void navigationDuringEdit();
    void staleSessionAndFailedReset();
    void resetSupersedesPendingNavigation();
    void workspaceLayout_data();
    void workspaceLayout();
    void numberedTimeline();
    void pgnHitTestingAndAccessibility();
    void branchExpansionAndSnapshotReuse();
    void orientationAndResponsiveSplit();
    void settingsValidation_data();
    void settingsValidation();
    void longAndBroadTree();
    void nestedVariationsRemainReachable();

  private:
    void play(chess::Move value);
    std::unique_ptr<app::AnalysisController> controller_;
};
void MoveNavigationTest::init() {
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
void MoveNavigationTest::cleanup() {
    if (!controller_) {
        return;
    }
    QSignalSpy closed(controller_.get(), &app::AnalysisController::closed);
    controller_->close();
    QTRY_COMPARE(closed.count(), 1);
    controller_.reset();
}
void MoveNavigationTest::play(chess::Move value) {
    controller_->attemptMove(value, controller_->board().revision);
    QTRY_VERIFY(!controller_->busy());
}
void MoveNavigationTest::historyAnimation_data() {
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<bool>("mouse");
    QTest::newRow("mouse-enabled") << true << true;
    QTest::newRow("keyboard-enabled") << true << false;
    QTest::newRow("mouse-disabled") << false << true;
    QTest::newRow("keyboard-disabled") << false << false;
}
void MoveNavigationTest::historyAnimation() {
    QFETCH(bool, enabled);
    QFETCH(bool, mouse);
    play(move("e2", "e4"));
    play(move("e7", "e5"));
    controller_->beginning();
    QTRY_VERIFY(!controller_->busy());
    ui::AnalysisPage page(*controller_);
    page.resize(1100, 800);
    auto preferences = page.preferences();
    preferences.animation = enabled;
    page.setPreferences(preferences);
    page.show();
    QTest::qWait(1);
    auto* board = page.findChild<ui::ChessBoardWidget*>();
    QVERIFY(board);
    QPushButton* next = nullptr;
    for (auto* button : page.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Next")) {
            next = button;
        }
    }
    QVERIFY(next);
    QSignalSpy accepted(controller_.get(), &app::AnalysisController::moveAccepted);
    QSignalSpy animations(controller_.get(),
                          &app::AnalysisController::navigationAnimationRequested);
    QImage firstFrame;
    connect(controller_.get(), &app::AnalysisController::navigationAnimationRequested, &page,
            [&](chess::Move) { firstFrame = board->grab().toImage(); });
    if (mouse) {
        QTest::mouseClick(next, Qt::LeftButton);
    } else {
        QTest::keyClick(board, Qt::Key_Right);
    }
    QTRY_COMPARE(animations.count(), 1);
    QCOMPARE(qvariant_cast<chess::Move>(animations[0][0]), move("e2", "e4"));
    QTest::qWait(220);
    QCOMPARE(firstFrame != board->grab().toImage(), enabled);
    QTest::keyClick(board, Qt::Key_Left);
    QTRY_COMPARE(animations.count(), 2);
    QCOMPARE(qvariant_cast<chess::Move>(animations[1][0]), move("e4", "e2"));
    QTest::qWait(220);
    QCOMPARE(firstFrame != board->grab().toImage(), enabled);
    QCOMPARE(accepted.count(), 0); // Browsing is not entered-move acceptance.
    animations.clear();
    controller_->next();
    controller_->next(); // Coalesced two-ply jump must not animate a bypassed move.
    QTRY_VERIFY(!controller_->busy());
    QTest::qWait(1);
    QCOMPARE(animations.count(), 0);
    controller_->beginning();
    QTRY_VERIFY(!controller_->busy());
    QTest::qWait(1);
    QCOMPARE(animations.count(), 0);
}
void MoveNavigationTest::coalescedNavigation() {
    play(move("e2", "e4"));
    const auto e4 = controller_->currentNode();
    play(move("e7", "e5"));
    play(move("g1", "f3"));
    const auto nf3 = controller_->currentNode();
    play(move("b8", "c6"));
    const auto last = controller_->currentNode();
    controller_->beginning();
    QTRY_VERIFY(!controller_->busy());
    ui::MoveTimelineView timeline(*controller_);
    QSignalSpy resets(timeline.model(), &QAbstractItemModel::modelReset);
    const auto structure = controller_->navigationTree();
    std::vector<chess::NodeId> published;
    connect(controller_.get(), &app::AnalysisController::changed, &timeline, [&] {
        if (!controller_->busy()) {
            published.push_back(controller_->currentNode());
        }
    });
    controller_->next();
    controller_->next();
    controller_->end();
    controller_->previous();
    controller_->next();
    controller_->previous();
    QVERIFY(controller_->busy());
    QCOMPARE(controller_->requestedNode(), nf3);
    QCOMPARE(controller_->currentNode(), chess::NodeId{0});
    QVERIFY(!controller_->board().interactive);
    controller_->attemptMove(move("d2", "d4"), controller_->board().revision);
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), nf3);
    QCOMPARE(published, std::vector<chess::NodeId>{nf3});
    QCOMPARE(controller_->navigationTree(), structure);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(controller_->chosenLine().back(), last);
    QCOMPARE(timeline.model()->rowCount(), 3); // Root and both full moves remain visible.
    controller_->beginning();
    controller_->end();
    controller_->navigate(e4);
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), e4);
    controller_->beginning();
    QTRY_VERIFY(!controller_->busy());
    const auto revision = controller_->board().revision;
    controller_->previous();
    controller_->beginning();
    controller_->navigate(chess::NodeId{999999});
    QVERIFY(!controller_->busy());
    QCOMPARE(controller_->board().revision, revision);
}
void MoveNavigationTest::navigationDuringEdit() {
    play(move("e2", "e4"));
    const auto e4 = controller_->currentNode();
    play(move("e7", "e5"));
    controller_->previous();
    QTRY_VERIFY(!controller_->busy());
    QSignalSpy accepted(controller_.get(), &app::AnalysisController::moveAccepted);
    controller_->attemptMove(move("c7", "c5"), controller_->board().revision);
    controller_->beginning();
    controller_->next();
    controller_->flip();
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), e4);
    QVERIFY(controller_->board().blackAtBottom);
    QCOMPARE(accepted.count(), 0); // The bypassed move never animates on a later position.
    QCOMPARE(controller_->navigationTree()->nodes.at(e4).children.size(), std::size_t{2});
    QCOMPARE(controller_->navigationTree()->nodes.size(), std::size_t{4});
    controller_->end();
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->history().back().label, QStringLiteral("1... c5"));
    controller_->previous();
    QTRY_VERIFY(!controller_->busy());
    const auto existing = controller_->navigationTree()->nodes.at(e4).children.front();
    controller_->attemptMove(move("e7", "e5"), controller_->board().revision);
    controller_->navigate(existing);
    QTRY_VERIFY(!controller_->busy());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCOMPARE(accepted.count(), 0);
    QCOMPARE(controller_->currentNode(), existing);
}
void MoveNavigationTest::resetSupersedesPendingNavigation() {
    play(move("e2", "e4"));
    play(move("e7", "e5"));
    const auto stale = controller_->target(controller_->currentNode());
    controller_->beginning();
    controller_->end();
    controller_->reset(std::string("invalid"));
    controller_->reset(); // Only the most recent reset survives.
    QVERIFY(!controller_->canNavigate());
    controller_->navigate(stale);
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), chess::NodeId{0});
    QCOMPARE(controller_->navigationTree()->nodes.size(), std::size_t{1});
    QCOMPARE(controller_->chosenLine().size(), std::size_t{1});
    QVERIFY(controller_->board().interactive);
    QVERIFY(!controller_->hasContent());
}
void MoveNavigationTest::workspaceLayout_data() {
    QTest::addColumn<QSize>("size");
    QTest::newRow("workspace-desktop") << QSize(1360, 850);
    QTest::newRow("workspace-laptop") << QSize(1100, 720);
    QTest::newRow("workspace-large") << QSize(1800, 1000);
}
void MoveNavigationTest::workspaceLayout() {
    QFETCH(QSize, size);
    play(move("e2", "e4"));
    play(move("e7", "e5"));
    controller_->previous();
    QTRY_VERIFY(!controller_->busy());
    play(move("c7", "c5"));
    play(move("g1", "f3"));
    ui::AnalysisPage page(*controller_);
    page.resize(size);
    page.show();
    QTest::qWait(1);
    auto* timeline = page.findChild<ui::MoveTimelineView*>();
    auto* tree = page.findChild<ui::GameTreeView*>();
    auto* splitter = page.findChild<QSplitter*>();
    QVERIFY(timeline && tree && splitter);
    QCOMPARE(page.size(), size);
    QCOMPARE(splitter->orientation(), Qt::Horizontal);
    QVERIFY(page.rect().contains(QRect(timeline->mapTo(&page, QPoint{}), timeline->size())));
    QVERIFY(page.rect().contains(QRect(tree->mapTo(&page, QPoint{}), tree->size())));
    if (qEnvironmentVariableIsSet("CHESS_REVIEW_SCREENSHOT_DIR")) {
        QVERIFY(page.grab().save(qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR") +
                                 QLatin1Char('/') + QString::fromLatin1(QTest::currentDataTag()) +
                                 QStringLiteral(".png")));
    }
}
void MoveNavigationTest::staleSessionAndFailedReset() {
    play(move("e2", "e4"));
    const auto old = controller_->target(controller_->currentNode());
    controller_->flip();
    controller_->reset();
    QTRY_VERIFY(!controller_->busy());
    play(move("d2", "d4"));
    controller_->beginning();
    QTRY_VERIFY(!controller_->busy());
    controller_->navigate(old);
    QVERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), chess::NodeId{0});
    QVERIFY(controller_->board().blackAtBottom);
    const auto structure = controller_->navigationTree();
    QSignalSpy failed(controller_.get(), &app::AnalysisController::failed);
    controller_->reset(std::string("invalid"));
    QTRY_VERIFY(!controller_->busy());
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(controller_->navigationTree(), structure);
    QVERIFY(controller_->board().interactive);
    controller_->end();
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->history().back().label, QStringLiteral("1. d4"));
    controller_->beginning();
    controller_->end();
    QSignalSpy closed(controller_.get(), &app::AnalysisController::closed);
    controller_->close();
    QTRY_COMPARE(closed.count(), 1);
    QVERIFY(!controller_->canNavigate());
    QVERIFY(!controller_->board().interactive);
    controller_.reset();
}
void MoveNavigationTest::numberedTimeline() {
    controller_->reset(std::string("4k3/8/8/8/8/8/8/4K3 b - - 0 42"));
    QTRY_VERIFY(!controller_->busy());
    play(move("e8", "d8"));
    play(move("e1", "d1"));
    ui::MoveTimelineView timeline(*controller_);
    QAbstractItemModelTester tester(timeline.model(),
                                    QAbstractItemModelTester::FailureReportingMode::QtTest);
    auto* model = timeline.model();
    QCOMPARE(model->rowCount(), 3);
    QCOMPARE(model->index(0, 1).data().toString(), QStringLiteral("Starting position"));
    QCOMPARE(model->index(1, 0).data().toString(), QStringLiteral("42"));
    QVERIFY(!model->index(1, 1).data().isValid());
    QCOMPARE(model->index(1, 2).data(Qt::AccessibleTextRole).toString(),
             QStringLiteral("42... Kd8"));
    QCOMPARE(model->index(2, 1).data(Qt::AccessibleTextRole).toString(), QStringLiteral("43. Kd1"));
    timeline.resize(600, 110);
    timeline.show();
    QTest::mouseClick(timeline.viewport(), Qt::LeftButton, Qt::NoModifier,
                      timeline.visualRect(model->index(0, 1)).center());
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), chess::NodeId{0});
    QCOMPARE(model->rowCount(), 3);
    auto* table = QAccessible::queryAccessibleInterface(&timeline)->tableInterface();
    QVERIFY(table);
    auto* cell = table->cellAt(1, 2);
    QVERIFY(cell);
    QVERIFY(cell->actionInterface());
    cell->actionInterface()->doAction(QAccessibleActionInterface::toggleAction());
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->history().back().label, QStringLiteral("42... Kd8"));
    key(timeline, Qt::Key_End);
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->history().back().label, QStringLiteral("43. Kd1"));
}
void MoveNavigationTest::pgnHitTestingAndAccessibility() {
    play(move("e2", "e4"));
    const auto e4 = controller_->target(controller_->currentNode());
    play(move("e7", "e5"));
    const auto e5 = controller_->target(controller_->currentNode());
    play(move("g1", "f3"));
    const auto nf3 = controller_->target(controller_->currentNode());
    ui::GameTreeView tree(*controller_);
    QAbstractItemModelTester tester(tree.model(),
                                    QAbstractItemModelTester::FailureReportingMode::QtTest);
    tree.resize(480, 350);
    tree.show();
    QTest::qWait(1);
    QVERIFY(!tree.moveRect(e4).isEmpty());
    QVERIFY(!tree.moveRect(e4).intersects(tree.moveRect(e5)));
    auto* accessible = QAccessible::queryAccessibleInterface(&tree);
    QVERIFY(accessible);
    QCOMPARE(accessible->childCount(), 4);
    auto* item = accessible->child(tree.moveIndex(e5));
    QCOMPARE(item->text(QAccessible::Name), QStringLiteral("1... e5"));
    QCOMPARE(accessible->indexOfChild(item), tree.moveIndex(e5));
    QCOMPARE(accessible->childAt(item->rect().center().x(), item->rect().center().y()), item);
    QTest::mouseClick(tree.viewport(), Qt::LeftButton, Qt::NoModifier, tree.moveRect(e5).center());
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), e5.node);
    QVERIFY(item->state().selected);
    const auto revision = controller_->board().revision;
    QTest::mouseMove(tree.viewport(), tree.moveRect(nf3).center());
    QTest::qWait(1);
    QCOMPARE(controller_->board().revision, revision);
    QVERIFY(!controller_->busy());
    tree.focusMove(e4);
    key(tree, Qt::Key_Right, Qt::ControlModifier);
    QCOMPARE(accessible->focusChild()->text(QAccessible::Name), QStringLiteral("1... e5"));
    key(tree, Qt::Key_Right, Qt::ControlModifier);
    key(tree, Qt::Key_Return);
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), nf3.node);
    key(tree, Qt::Key_Left);
    key(tree, Qt::Key_Left);
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), e4.node);
    item->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), e5.node);
    tree.hide();
    item->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
    QVERIFY(!controller_->busy());
}
void MoveNavigationTest::branchExpansionAndSnapshotReuse() {
    play(move("e2", "e4"));
    const auto e4 = controller_->currentNode();
    play(move("e7", "e5"));
    const auto e5 = controller_->currentNode();
    controller_->previous();
    QTRY_VERIFY(!controller_->busy());
    play(move("c7", "c5"));
    const auto c5 = controller_->target(controller_->currentNode());
    play(move("g1", "f3"));
    ui::GameTreeView tree(*controller_);
    tree.resize(450, 300);
    tree.show();
    QTest::qWait(1);
    auto* accessible = QAccessible::queryAccessibleInterface(&tree);
    auto* item = accessible->child(tree.moveIndex(c5));
    tree.collapseAll();
    QVERIFY(tree.moveRect(c5).isEmpty());
    QVERIFY(item->state().invisible);
    item->actionInterface()->doAction(QAccessibleActionInterface::pressAction());
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), c5.node);
    QVERIFY(!tree.moveRect(c5).isEmpty());
    const auto structure = controller_->navigationTree();
    QSignalSpy resets(tree.model(), &QAbstractItemModel::modelReset);
    controller_->flip();
    controller_->toggleAnnotation(
        {*chess::Square::fromName("a1"), *chess::Square::fromName("a1"), 0});
    controller_->navigate(e4);
    QTRY_VERIFY(!controller_->busy());
    play(move("c7", "c5")); // Reusing a child does not rebuild topology or lose expansion.
    QCOMPARE(controller_->navigationTree(), structure);
    QCOMPARE(resets.count(), 0);
    QVERIFY(!tree.moveRect(c5).isEmpty());
    controller_->navigate(e5);
    QTRY_VERIFY(!controller_->busy());
    controller_->beginning();
    controller_->end();
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->currentNode(), e5);
    const auto stale = c5;
    controller_->reset();
    QTRY_VERIFY(!controller_->busy());
    QVERIFY(!tree.validTarget(stale));
    tree.activateMove(stale);
    QVERIFY(!controller_->busy());
    QCOMPARE(accessible->childCount(), 1);
}
void MoveNavigationTest::orientationAndResponsiveSplit() {
    controller_->flip();
    ui::AnalysisPage page(*controller_);
    QVERIFY(controller_->board().blackAtBottom);
    auto preferences = page.preferences();
    preferences.blackAtBottom = true;
    preferences.analysisSplit = 65;
    page.setPreferences(preferences);
    page.resize(1100, 850);
    page.show();
    QTest::qWait(1);
    auto* splitter = page.findChild<QSplitter*>();
    QVERIFY(splitter);
    QCOMPARE(splitter->orientation(), Qt::Horizontal);
    QCOMPARE(page.preferences().analysisSplit, 65);
    const auto sizes = splitter->sizes();
    QVERIFY(std::abs(sizes[0] * 100 / (sizes[0] + sizes[1]) - 65) <= 1);
    QVERIFY(controller_->board().blackAtBottom);
    controller_->reset();
    QTRY_VERIFY(!controller_->busy());
    QVERIFY(controller_->board().blackAtBottom);
    page.resize(700, 850);
    QTest::qWait(1);
    QCOMPARE(splitter->orientation(), Qt::Vertical);
    QCOMPARE(page.preferences().analysisSplit, 65);
    controller_->flip();
    QVERIFY(!page.preferences().blackAtBottom);
    page.resize(1100, 850);
    QTest::qWait(1);
    QCOMPARE(splitter->orientation(), Qt::Horizontal);
    QCOMPARE(page.preferences().analysisSplit, 65);
}
void MoveNavigationTest::settingsValidation_data() {
    QTest::addColumn<QString>("setting");
    QTest::addColumn<QVariant>("value");
    QTest::addColumn<bool>("invalid");
    QTest::newRow("legacy") << QString{} << QVariant{} << false;
    QTest::newRow("black") << QStringLiteral("board/v1/blackAtBottom") << QVariant(true) << false;
    QTest::newRow("bad-orientation")
        << QStringLiteral("board/v1/blackAtBottom") << QVariant(QStringLiteral("sideways")) << true;
    QTest::newRow("bad-split") << QStringLiteral("analysis/v1/split") << QVariant(100) << true;
    QTest::newRow("nonnumeric-split")
        << QStringLiteral("analysis/v1/split") << QVariant(QStringLiteral("wide")) << true;
}
void MoveNavigationTest::settingsValidation() {
    QFETCH(QString, setting);
    QFETCH(QVariant, value);
    QFETCH(bool, invalid);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto filename = directory.filePath(QStringLiteral("ui.ini"));
    if (!setting.isEmpty()) {
        QSettings settings(filename, QSettings::IniFormat);
        settings.setValue(setting, value);
    }
    app::UiStateStore store;
    QSignalSpy loaded(&store, &app::UiStateStore::loaded);
    QSignalSpy failed(&store, &app::UiStateStore::failed);
    QSignalSpy closed(&store, &app::UiStateStore::closed);
    store.load(filename);
    QTRY_COMPARE(loaded.count(), 1);
    QCOMPARE(failed.count(), invalid ? 1 : 0);
    const auto state = qvariant_cast<app::UiState>(loaded.front().front());
    QCOMPARE(state.board.blackAtBottom, !invalid && value == QVariant(true));
    QCOMPARE(state.board.analysisSplit, 60);
    store.saveAndClose(state);
    QTRY_COMPARE(closed.count(), 1);
}
void MoveNavigationTest::longAndBroadTree() {
    constexpr int plies = 512;
    const std::array moves{move("g1", "f3"), move("g8", "f6"), move("f3", "g1"), move("f6", "g8")};
    int submitted = 0;
    QTimer heartbeat;
    int beats = 0;
    connect(&heartbeat, &QTimer::timeout, this, [&] { ++beats; });
    heartbeat.start(1);
    QObject driver;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    const auto chain = connect(
        controller_.get(), &app::AnalysisController::changed, &driver,
        [&] {
            if (!controller_->busy() && submitted < plies) {
                const auto value = moves[static_cast<std::size_t>(submitted % 4)];
                ++submitted;
                controller_->attemptMove(value, controller_->board().revision);
            } else if (submitted == plies && !controller_->busy()) {
                loop.quit();
            }
        },
        Qt::QueuedConnection);
    deadline.start(30000);
    controller_->clearSelection(); // Starts the queued driver without a production-only batch API.
    // Let worker deliveries drive the chain directly instead of QTRY polling sleeps.
    loop.exec();
    QVERIFY(submitted == plies && !controller_->busy());
    disconnect(chain);
    QVERIFY(beats > 0);
    QCOMPARE(controller_->navigationTree()->nodes.size(), std::size_t{plies + 1});
    QCOMPARE(controller_->navigationTree()->lines.size(), std::size_t{65});
    ui::GameTreeView tree(*controller_);
    tree.resize(450, 300);
    tree.show();
    const auto structure = controller_->navigationTree();
    controller_->beginning();
    QTRY_VERIFY(!controller_->busy());
    const auto initialMoves = controller_->board().legalMoves;
    for (const auto value : initialMoves) {
        play(value);
        controller_->beginning();
        QTRY_VERIFY(!controller_->busy());
    }
    QCOMPARE(controller_->navigationTree()->nodes.at(chess::NodeId{0}).children.size(),
             std::size_t{20});
    QAbstractItemModelTester tester(tree.model(),
                                    QAbstractItemModelTester::FailureReportingMode::QtTest);
    controller_->navigate(structure->accessibleOrder.back());
    QTRY_VERIFY(!controller_->busy());
    QCOMPARE(controller_->history().size(), std::size_t{plies + 1});
    QCOMPARE(tree.moveCount(), plies + 20);
    if (qEnvironmentVariableIsSet("CHESS_REVIEW_SCREENSHOT_DIR")) {
        QVERIFY(tree.grab().save(qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR") +
                                 QStringLiteral("/long-tree.png")));
    }
}
void MoveNavigationTest::nestedVariationsRemainReachable() {
    ui::GameTreeView tree(*controller_);
    tree.resize(360, 220);
    tree.show();
    constexpr int depth = 96;
    const std::array moves{move("g1", "f3"), move("g8", "f6"), move("f3", "g1"), move("f6", "g8")};
    int level = 0;
    int phase = 0;
    chess::NodeId parent{0};
    QObject driver;
    connect(
        controller_.get(), &app::AnalysisController::changed, &driver,
        [&] {
            if (controller_->busy() || level == depth) {
                return;
            }
            const auto continuation = moves[static_cast<std::size_t>(level % 4)];
            if (phase == 0) {
                parent = controller_->currentNode();
                const auto& legal = controller_->board().legalMoves;
                const auto main = std::find_if(legal.begin(), legal.end(), [&](const auto& value) {
                    return value != continuation;
                });
                if (main == legal.end()) {
                    return;
                }
                phase = 1;
                controller_->attemptMove(*main, controller_->board().revision);
            } else if (phase == 1) {
                phase = 2;
                controller_->navigate(parent);
            } else {
                phase = 0;
                ++level;
                controller_->attemptMove(continuation, controller_->board().revision);
            }
        },
        Qt::QueuedConnection);
    controller_->clearSelection();
    QTRY_VERIFY_WITH_TIMEOUT(level == depth && !controller_->busy(), 15000);
    const auto target = controller_->target(controller_->currentNode());
    QTest::qWait(1);
    QCOMPARE(controller_->navigationTree()->nodes.size(), std::size_t{193});
    const auto line = controller_->navigationTree()->lineForNode.at(target.node);
    QCOMPARE(controller_->navigationTree()->lines[line].depth, std::size_t{96});
    QVERIFY(tree.viewport()->rect().intersects(tree.moveRect(target)));
    tree.focusMove(target);
    QCOMPARE(QAccessible::queryAccessibleInterface(&tree)->focusChild()->text(QAccessible::Name),
             tree.moveDescription(target));
    if (qEnvironmentVariableIsSet("CHESS_REVIEW_SCREENSHOT_DIR")) {
        QVERIFY(tree.grab().save(qEnvironmentVariable("CHESS_REVIEW_SCREENSHOT_DIR") +
                                 QStringLiteral("/nested-tree.png")));
    }
}
int main(int argc, char* argv[]) {
    const TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("chess-review"));
    MoveNavigationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "MoveNavigationTest.moc"
