#include "ui/analysis/MoveNavigation.h"
#include <QAccessibleWidget>
#include <map>
#include <memory>

namespace chess_review::ui {
namespace {
class AccessibleMove final : public QAccessibleInterface, public QAccessibleActionInterface {
  public:
    AccessibleMove(GameTreeView& view, app::NavigationTarget target)
        : view_(view), target_(target) {}
    app::NavigationTarget target() const { return target_; }
    bool isValid() const override { return view_.validTarget(target_); }
    QObject* object() const override { return nullptr; }
    QWindow* window() const override { return parent()->window(); }
    QAccessibleInterface* childAt(int, int) const override { return nullptr; }
    QAccessibleInterface* parent() const override {
        return QAccessible::queryAccessibleInterface(&view_);
    }
    QAccessibleInterface* child(int) const override { return nullptr; }
    int childCount() const override { return 0; }
    int indexOfChild(const QAccessibleInterface*) const override { return -1; }
    QString text(QAccessible::Text type) const override {
        if (type == QAccessible::Name) {
            return view_.moveDescription(target_);
        }
        if (type == QAccessible::Description) {
            return view_.isPending(target_)   ? QObject::tr("Pending position")
                   : view_.isCurrent(target_) ? QObject::tr("Current position")
                                              : QObject::tr("Go to position");
        }
        return {};
    }
    void setText(QAccessible::Text, const QString&) override {}
    QRect rect() const override {
        const auto local = view_.moveRect(target_);
        return local.isEmpty()
                   ? QRect{}
                   : QRect(view_.viewport()->mapToGlobal(local.topLeft()), local.size());
    }
    QAccessible::Role role() const override { return QAccessible::ListItem; }
    QAccessible::State state() const override {
        QAccessible::State state;
        state.invalid = !isValid();
        state.focusable = true;
        state.selectable = true;
        state.selected = view_.isCurrent(target_);
        state.focused = view_.isMoveFocused(target_);
        state.busy = view_.isPending(target_);
        state.disabled = !view_.isEnabled();
        state.invisible = !view_.isVisible() || rect().isEmpty();
        state.offscreen = !view_.viewport()->rect().intersects(view_.moveRect(target_));
        state.expandable = view_.canExpand(target_);
        state.expanded = state.expandable && view_.expanded(target_);
        state.collapsed = state.expandable && !state.expanded;
        return state;
    }
    void* interface_cast(QAccessible::InterfaceType type) override {
        return type == QAccessible::ActionInterface ? static_cast<QAccessibleActionInterface*>(this)
                                                    : nullptr;
    }
    QStringList actionNames() const override {
        auto actions = QStringList{pressAction(), setFocusAction()};
        if (view_.canExpand(target_)) {
            actions.push_back(toggleAction());
        }
        return actions;
    }
    void doAction(const QString& action) override {
        if (!view_.isVisible() || !view_.isEnabled() || !isValid()) {
            return;
        }
        if (action == pressAction()) {
            view_.activateMove(target_);
        }
        if (action == setFocusAction()) {
            view_.focusMove(target_);
        }
        if (action == toggleAction()) {
            view_.toggleExpanded(target_);
        }
    }
    QStringList keyBindingsForAction(const QString& action) const override {
        return action == pressAction() ? QStringList{QStringLiteral("Enter")} : QStringList{};
    }

  private:
    GameTreeView& view_;
    app::NavigationTarget target_;
};
class AccessibleGameTree final : public QAccessibleWidget {
  public:
    explicit AccessibleGameTree(GameTreeView& view)
        : QAccessibleWidget(&view, QAccessible::List), view_(view) {}
    ~AccessibleGameTree() override {
        for (const auto& [key, id] : children_) {
            static_cast<void>(key);
            QAccessible::deleteAccessibleInterface(id);
        }
    }
    int childCount() const override {
        synchronize();
        return view_.moveCount();
    }
    QAccessibleInterface* child(int index) const override {
        synchronize();
        const auto target = view_.moveAt(index);
        if (!target) {
            return nullptr;
        }
        const auto key = std::pair{target->session, target->node};
        // Allocate the registry slot first: a throwing map insertion must not strand
        // an interface whose ownership has already transferred to Qt.
        const auto found = children_.try_emplace(key, 0).first;
        if (found->second == 0) {
            auto move = std::make_unique<AccessibleMove>(view_, *target);
            found->second = QAccessible::registerAccessibleInterface(move.get());
            if (found->second != 0) {
                return move.release(); // Qt's accessibility registry now owns the interface.
            }
            return nullptr;
        }
        return QAccessible::accessibleInterface(found->second);
    }
    int indexOfChild(const QAccessibleInterface* candidate) const override {
        const auto* move = dynamic_cast<const AccessibleMove*>(candidate);
        return move && move->parent() == this ? view_.moveIndex(move->target()) : -1;
    }
    QAccessibleInterface* childAt(int x, int y) const override {
        const auto target = view_.targetAt(view_.viewport()->mapFromGlobal(QPoint(x, y)));
        return target ? child(view_.moveIndex(*target)) : nullptr;
    }
    QAccessibleInterface* focusChild() const override {
        const auto target = view_.focusedTarget();
        return view_.hasFocus() && target ? child(view_.moveIndex(*target)) : nullptr;
    }

  private:
    void synchronize() const {
        const auto root = view_.moveAt(0);
        if (root && session_ == root->session) {
            return;
        }
        for (const auto& [key, id] : children_) {
            static_cast<void>(key);
            QAccessible::deleteAccessibleInterface(id);
        }
        children_.clear();
        session_ = root ? std::optional{root->session} : std::nullopt;
    }
    GameTreeView& view_;
    mutable std::optional<std::uint64_t> session_;
    mutable std::map<std::pair<std::uint64_t, chess::NodeId>, QAccessible::Id> children_;
};
QAccessibleInterface* navigationFactory(const QString&, QObject* object) {
    if (auto* view = qobject_cast<GameTreeView*>(object)) {
        return new AccessibleGameTree(*view);
    }
    return nullptr;
}
} // namespace
void installMoveNavigationAccessibility() {
    static const bool installed = [] {
        QAccessible::installFactory(navigationFactory);
        return true;
    }();
    static_cast<void>(installed);
}
} // namespace chess_review::ui
