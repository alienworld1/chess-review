#include "app/MoveNavigationState.h"
#include "TestEnvironment.h"
#include <QCoreApplication>
#include <QTest>
#include <array>

using namespace chess_review;
namespace {
chess::Move move(const char* from, const char* to) {
    return {*chess::Square::fromName(from), *chess::Square::fromName(to), std::nullopt};
}
} // namespace
class MoveNavigationStateTest final : public QObject {
    Q_OBJECT
  private slots:
    void maximumDepthSnapshot();
    void boundedNotes();
};
void MoveNavigationStateTest::maximumDepthSnapshot() {
    const std::array moves{move("g1", "f3"), move("g8", "f6"), move("f3", "g1"), move("f6", "g8")};
    constexpr std::size_t depth = 16384;
    std::vector<chess::GameTreeInputNode> input(depth + 1);
    for (std::size_t i = 1; i <= depth; ++i) {
        input[i].parent = i - 1;
        input[i].move = moves[(i - 1) % moves.size()];
    }
    const auto result = chess::GameTree::fromNodes(std::nullopt, input);
    QVERIFY(std::holds_alternative<chess::GameTree>(result));
    const auto navigation = app::makeNavigationTree(std::get<chess::GameTree>(result));
    QCOMPARE(navigation->nodes.size(), depth + 1);
    QCOMPARE(navigation->lines.size(), std::size_t{2049});
    QCOMPARE(navigation->nodes.at(chess::NodeId{depth}).label, QStringLiteral("8192... Ng8"));
    for (const auto& line : navigation->lines) {
        QVERIFY(line.moves.size() <= 8);
        QVERIFY(!line.parent); // A long mainline never becomes a recursively owned hierarchy.
    }
}
void MoveNavigationStateTest::boundedNotes() {
    auto tree = std::get<chess::GameTree>(chess::GameTree::initial());
    chess::NodeAnnotations annotations;
    annotations.commentBefore = std::string(4095, 'a') + "\xf0\x9f\x98\x80";
    annotations.commentAfter = "tail";
    annotations.nags.assign(1'000'000, 255);
    QVERIFY(
        std::holds_alternative<std::monostate>(tree.setAnnotations(tree.rootId(), annotations)));
    const auto navigation = app::makeNavigationTree(tree);
    const auto& notes = navigation->nodes.at(tree.rootId()).notes;
    QVERIFY(notes.size() < 4200);
    QVERIFY(notes.isValidUtf16());
    QVERIFY(notes.contains(QStringLiteral("truncated")));
    QCOMPARE(tree.node(tree.rootId())->annotations, annotations);
    annotations.commentBefore.clear();
    QVERIFY(
        std::holds_alternative<std::monostate>(tree.setAnnotations(tree.rootId(), annotations)));
    const auto nags = app::makeNavigationTree(tree)->nodes.at(tree.rootId()).notes;
    QVERIFY(nags.size() < 4200);
    QVERIFY(nags.startsWith(QStringLiteral("$255")));
    QVERIFY(nags.contains(QStringLiteral("truncated")));
}
int main(int argc, char* argv[]) {
    const TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QCoreApplication application(argc, argv);
    QTest::failOnWarning();
    MoveNavigationStateTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "MoveNavigationStateTest.moc"
