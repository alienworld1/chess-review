#include "ui/chess/ChessBoardWidget.h"
#include <QAccessibleWidget>
#include <array>

namespace chess_review::ui {
namespace {
class AccessibleSquare final : public QAccessibleInterface, public QAccessibleActionInterface {
  public:
    AccessibleSquare(ChessBoardWidget& board, chess::Square square, QAccessibleInterface& parent)
        : board_(board), square_(square), parent_(parent) {}
    bool isValid() const override { return true; }
    QObject* object() const override { return nullptr; }
    QWindow* window() const override { return parent_.window(); }
    QAccessibleInterface* childAt(int, int) const override { return nullptr; }
    QAccessibleInterface* parent() const override { return &parent_; }
    QAccessibleInterface* child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface*) const override { return -1; }
    QString text(QAccessible::Text type) const override {
        return type == QAccessible::Name || type == QAccessible::Description
                   ? board_.squareDescription(square_)
                   : QString{};
    }
    void setText(QAccessible::Text, const QString&) override {}
    QRect rect() const override {
        const auto local = board_.squareRect(square_).toAlignedRect();
        return {board_.mapToGlobal(local.topLeft()), local.size()};
    }
    QAccessible::Role role() const override { return QAccessible::Cell; }
    QAccessible::State state() const override {
        QAccessible::State state;
        state.focusable = true;
        state.selectable = true;
        state.selected = board_.state().selection == square_;
        state.focused =
            board_.hasFocus() && board_.entryMode() && board_.focusedSquare() == square_;
        state.disabled = !board_.state().interactive;
        state.invisible = !board_.isVisible();
        return state;
    }
    void* interface_cast(QAccessible::InterfaceType type) override {
        return type == QAccessible::ActionInterface ? static_cast<QAccessibleActionInterface*>(this)
                                                    : nullptr;
    }
    QStringList actionNames() const override { return {pressAction(), setFocusAction()}; }
    void doAction(const QString& action) override {
        if (action == setFocusAction()) {
            board_.focusSquare(square_);
        }
        if (action == pressAction()) {
            board_.focusSquare(square_);
            board_.activateSquare(square_);
        }
    }
    QStringList keyBindingsForAction(const QString& action) const override {
        return action == pressAction() ? QStringList{QStringLiteral("Enter")} : QStringList{};
    }

  private:
    ChessBoardWidget& board_;
    chess::Square square_;
    QAccessibleInterface& parent_;
};
class AccessibleBoard final : public QAccessibleWidget {
  public:
    explicit AccessibleBoard(ChessBoardWidget& board)
        : QAccessibleWidget(&board, QAccessible::Pane), board_(board) {
        for (int i = 0; i < 64; ++i) {
            children_[static_cast<std::size_t>(i)] = QAccessible::registerAccessibleInterface(
                new AccessibleSquare(board, *chess::Square::fromCoordinates(i % 8, i / 8), *this));
        }
    }
    ~AccessibleBoard() override {
        for (const auto id : children_) {
            QAccessible::deleteAccessibleInterface(id);
        }
    }
    int childCount() const override { return 64; }
    QAccessibleInterface* child(int index) const override {
        return index >= 0 && index < 64
                   ? QAccessible::accessibleInterface(children_[static_cast<std::size_t>(index)])
                   : nullptr;
    }
    int indexOfChild(const QAccessibleInterface* childInterface) const override {
        for (int i = 0; i < 64; ++i) {
            if (child(i) == childInterface) {
                return i;
            }
        }
        return -1;
    }
    QAccessibleInterface* childAt(int x, int y) const override {
        const auto square = board_.squareAt(board_.mapFromGlobal(QPoint{x, y}));
        return square ? child(square->index()) : nullptr;
    }
    QAccessibleInterface* focusChild() const override {
        return board_.hasFocus() && board_.entryMode() ? child(board_.focusedSquare().index())
                                                       : nullptr;
    }

  private:
    ChessBoardWidget& board_;
    std::array<QAccessible::Id, 64> children_{};
};
QAccessibleInterface* boardFactory(const QString&, QObject* object) {
    if (auto* board = qobject_cast<ChessBoardWidget*>(object)) {
        return new AccessibleBoard(*board);
    }
    return nullptr;
}
} // namespace
void installBoardAccessibility() {
    static const bool installed = [] {
        QAccessible::installFactory(boardFactory);
        return true;
    }();
    static_cast<void>(installed);
}
} // namespace chess_review::ui
