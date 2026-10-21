#include "ui/analysis/MoveNavigation.h"
#include <QAbstractTableModel>
#include <QAccessibleWidget>
#include <QApplication>
#include <QFontMetrics>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QVarLengthArray>
#include <algorithm>
#include <map>
#include <memory>

namespace chess_review::ui {
namespace {
bool historyKey(QKeyEvent* event, app::AnalysisController& controller) {
    if (event->modifiers() != Qt::NoModifier) {
        return false;
    }
    switch (event->key()) {
    case Qt::Key_Left:
        controller.previous();
        break;
    case Qt::Key_Right:
        controller.next();
        break;
    case Qt::Key_Home:
        controller.beginning();
        break;
    case Qt::Key_End:
        controller.end();
        break;
    default:
        return false;
    }
    event->accept();
    return true;
}
QVariant moveData(const app::AnalysisController& controller, chess::NodeId node, int role) {
    const auto& entry = controller.navigationTree()->nodes.at(node);
    if (role == Qt::DisplayRole) {
        return entry.parent ? entry.san : entry.label;
    }
    if (role == Qt::ToolTipRole || role == Qt::AccessibleTextRole) {
        return entry.label;
    }
    if (role == Qt::AccessibleDescriptionRole) {
        return controller.currentNode() == node ? QObject::tr("Current position")
               : controller.busy() && controller.requestedNode() == node
                   ? QObject::tr("Pending position")
                   : QObject::tr("Go to position");
    }
    if (role == Qt::BackgroundRole && controller.currentNode() == node) {
        return QApplication::palette().highlight();
    }
    if (role == Qt::ForegroundRole && controller.currentNode() == node) {
        return QApplication::palette().highlightedText();
    }
    if (role == Qt::FontRole && controller.busy() && controller.requestedNode() == node) {
        auto font = QApplication::font();
        font.setUnderline(true);
        return font;
    }
    return {};
}
} // namespace
class TimelineModel final : public QAbstractTableModel {
  public:
    explicit TimelineModel(app::AnalysisController& controller, QObject* parent)
        : QAbstractTableModel(parent), controller_(controller) {}
    struct Row {
        QString number;
        std::optional<chess::NodeId> white;
        std::optional<chess::NodeId> black;
    };
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& = {}) const override { return 3; }
    std::optional<chess::NodeId> node(const QModelIndex& index) const {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
            return std::nullopt;
        }
        const auto& row = rows_[static_cast<std::size_t>(index.row())];
        return index.column() == 1 ? row.white : index.column() == 2 ? row.black : std::nullopt;
    }
    QModelIndex indexFor(chess::NodeId node) const {
        const auto found = locations_.find(node);
        return found == locations_.end() ? QModelIndex{}
                                         : index(found->second.first, found->second.second);
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) {
            return {};
        }
        if (index.column() == 0 && role == Qt::DisplayRole) {
            return rows_[static_cast<std::size_t>(index.row())].number;
        }
        const auto id = node(index);
        return id ? moveData(controller_, *id, role) : QVariant{};
    }
    Qt::ItemFlags flags(const QModelIndex& index) const override {
        return node(index) ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        return section == 0 ? tr("Move") : section == 1 ? tr("White") : tr("Black");
    }
    void refresh() {
        const auto& line = controller_.chosenLine();
        if (tree_ != controller_.navigationTree() || line_ != line ||
            session_ != controller_.target(chess::NodeId{0}).session) {
            beginResetModel();
            tree_ = controller_.navigationTree();
            session_ = controller_.target(chess::NodeId{0}).session;
            line_ = line;
            rows_.clear();
            locations_.clear();
            for (const auto id : line_) {
                const auto& entry = tree_->nodes.at(id);
                if (!entry.parent) {
                    rows_.push_back({{}, id, std::nullopt});
                    locations_.emplace(id, std::pair{0, 1});
                    continue;
                }
                if (entry.side == chess::Color::White || rows_.size() == 1 ||
                    rows_.back().number != QString::number(entry.number)) {
                    rows_.push_back({QString::number(entry.number), std::nullopt, std::nullopt});
                }
                const auto column = entry.side == chess::Color::White ? 1 : 2;
                (column == 1 ? rows_.back().white : rows_.back().black) = id;
                locations_.emplace(id, std::pair{static_cast<int>(rows_.size() - 1), column});
            }
            endResetModel();
        } else if (!rows_.empty()) {
            emit dataChanged(index(0, 1), index(rowCount() - 1, 2));
        }
    }
    std::uint64_t session() const { return session_; }

  private:
    app::AnalysisController& controller_;
    std::shared_ptr<const app::NavigationTree> tree_;
    std::vector<chess::NodeId> line_;
    std::vector<Row> rows_;
    std::map<chess::NodeId, std::pair<int, int>> locations_;
    std::uint64_t session_ = 0;
};

namespace {
class TimelineDelegate final : public QStyledItemDelegate {
  public:
    explicit TimelineDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        auto state = option;
        // Selection is keyboard/accessibility focus. Only the published position
        // gets a filled background; pending intent remains separately underlined.
        state.state &= ~QStyle::State_Selected;
        QStyledItemDelegate::paint(painter, state, index);
    }
};
} // namespace
MoveTimelineView::MoveTimelineView(app::AnalysisController& controller, QWidget* parent)
    : QTableView(parent), controller_(controller), moves_(new TimelineModel(controller, this)) {
    setObjectName(QStringLiteral("moveTimeline"));
    setAccessibleName(tr("Chosen line move timeline"));
    setModel(moves_);
    setEditTriggers(NoEditTriggers);
    setSelectionMode(SingleSelection);
    setItemDelegate(new TimelineDelegate(this));
    setSelectionBehavior(SelectItems);
    verticalHeader()->hide();
    horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    setMinimumHeight(84);
    setMaximumHeight(110);
    connect(selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection& selected) {
                if (refreshing_) {
                    return;
                }
                for (const auto& index : selected.indexes()) {
                    if (const auto node = moves_->node(index)) {
                        controller_.navigate(app::NavigationTarget{moves_->session(), *node});
                        return;
                    }
                }
            });
    connect(this, &QTableView::clicked, this, [this](const QModelIndex& index) {
        if (const auto node = moves_->node(index)) {
            controller_.navigate(app::NavigationTarget{moves_->session(), *node});
        }
    });
    connect(this, &QTableView::activated, this, [this](const QModelIndex& index) {
        if (const auto node = moves_->node(index)) {
            controller_.navigate(app::NavigationTarget{moves_->session(), *node});
        }
    });
    connect(&controller_, &app::AnalysisController::changed, this, &MoveTimelineView::refresh);
    refresh();
}
void MoveTimelineView::refresh() {
    const QScopedValueRollback guard(refreshing_, true);
    const auto focused = moves_->node(currentIndex());
    const auto scroll = verticalScrollBar()->value();
    moves_->refresh();
    setEnabled(controller_.canNavigate());
    if (focused && currentIndex() != moves_->indexFor(*focused)) {
        setCurrentIndex(moves_->indexFor(*focused));
    }
    if (displayed_ != controller_.currentNode()) {
        displayed_ = controller_.currentNode();
        scrollTo(moves_->indexFor(*displayed_), EnsureVisible);
    } else {
        verticalScrollBar()->setValue(scroll);
    }
}
void MoveTimelineView::keyPressEvent(QKeyEvent* event) {
    if (!historyKey(event, controller_)) {
        QTableView::keyPressEvent(event);
    }
}

class VariationModel final : public QAbstractItemModel {
  public:
    explicit VariationModel(app::AnalysisController& controller, QObject* parent)
        : QAbstractItemModel(parent), controller_(controller) {}
    const std::shared_ptr<const app::NavigationTree>& tree() const { return tree_; }
    const app::NavigationLine& line(const QModelIndex& index) const {
        return tree_->lines[index.internalId() - 1];
    }
    QModelIndex lineIndex(std::size_t global) const {
        return createIndex(static_cast<int>(tree_->lines[global].row), 0, global + 1);
    }
    QModelIndex indexFor(chess::NodeId node) const {
        if (!tree_) {
            return {};
        }
        const auto found = tree_->lineForNode.find(node);
        return found == tree_->lineForNode.end() ? QModelIndex{} : lineIndex(found->second);
    }
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override {
        if (!tree_ || row < 0 || column != 0) {
            return {};
        }
        const auto& children = parent.isValid() ? line(parent).children : tree_->roots;
        return static_cast<std::size_t>(row) < children.size()
                   ? lineIndex(children[static_cast<std::size_t>(row)])
                   : QModelIndex{};
    }
    QModelIndex parent(const QModelIndex& index) const override {
        if (!index.isValid()) {
            return {};
        }
        const auto ancestor = line(index).parent;
        return ancestor ? lineIndex(*ancestor) : QModelIndex{};
    }
    int rowCount(const QModelIndex& parent = {}) const override {
        if (!tree_ || parent.column() > 0) {
            return 0;
        }
        return static_cast<int>(parent.isValid() ? line(parent).children.size()
                                                 : tree_->roots.size());
    }
    int columnCount(const QModelIndex& = {}) const override { return 1; }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid()) {
            return {};
        }
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) {
            QStringList labels;
            for (auto node : line(index).moves) {
                labels.push_back(tree_->nodes.at(node).label);
            }
            return labels.join(QLatin1Char(' '));
        }
        return {};
    }
    bool refresh() {
        if (tree_ == controller_.navigationTree()) {
            return false;
        }
        beginResetModel();
        tree_ = controller_.navigationTree();
        endResetModel();
        return true;
    }

  private:
    app::AnalysisController& controller_;
    std::shared_ptr<const app::NavigationTree> tree_;
};

namespace {
struct MoveToken {
    chess::NodeId node;
    QRect rect;
    QString text;
};
QRect disclosureRect(const VariationModel& model, const QModelIndex& index, const QRect& row,
                     const QFont& font) {
    // Preserve hierarchy without letting pathological variation depth consume the viewport.
    const auto indent =
        std::min(static_cast<int>(std::min<std::size_t>(model.line(index).depth, 12)) * 12,
                 std::max(0, row.width() / 4));
    return {row.left() + indent, row.top() + 3, 18, QFontMetrics(font).height() + 10};
}
QVarLengthArray<MoveToken, 8> tokens(const VariationModel& model, const QModelIndex& index,
                                     const QRect& rect, const QFont& font) {
    const QFontMetrics metrics(font);
    const auto height = metrics.height() + 10;
    const auto contentLeft = disclosureRect(model, index, rect, font).right() + 4;
    int x = contentLeft;
    int y = rect.top() + 3;
    QVarLengthArray<MoveToken, 8> result;
    std::optional<std::uint32_t> previousWhite;
    for (const auto node : model.line(index).moves) {
        const auto& entry = model.tree()->nodes.at(node);
        const auto text = entry.side == chess::Color::Black && previousWhite == entry.number
                              ? entry.san
                              : entry.label;
        const int width = metrics.horizontalAdvance(text) + 16;
        if (x > contentLeft && x + width > rect.right()) {
            x = contentLeft;
            y += height;
        }
        result.push_back({node, QRect(x, y, width, height), text});
        x += width + 4;
        previousWhite = entry.parent && entry.side == chess::Color::White
                            ? std::optional{entry.number}
                            : std::nullopt;
    }
    return result;
}
class VariationDelegate final : public QStyledItemDelegate {
  public:
    explicit VariationDelegate(GameTreeView& view, VariationModel& model)
        : QStyledItemDelegate(&view), view_(view), model_(model) {}
    QFont lineFont(const QModelIndex& index) const {
        auto font = view_.font();
        font.setBold(model_.line(index).mainline);
        return font;
    }
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        const int width = std::max(100, view_.viewport()->width());
        const auto items = tokens(model_, index, QRect(0, 0, width, 0), lineFont(index));
        return {width, items.empty() ? 30 : items.back().rect.bottom() + 4};
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setClipRect(option.rect);
        painter->setFont(lineFont(index));
        if (!model_.line(index).children.empty()) {
            QStyleOption branch;
            branch.rect = disclosureRect(model_, index, option.rect, lineFont(index));
            branch.palette = option.palette;
            branch.state = QStyle::State_Enabled | QStyle::State_Children;
            if (view_.isExpanded(index)) {
                branch.state |= QStyle::State_Open;
            }
            view_.style()->drawPrimitive(QStyle::PE_IndicatorBranch, &branch, painter, &view_);
        }
        for (const auto& token : tokens(model_, index, option.rect, lineFont(index))) {
            const auto target = app::NavigationTarget{session, token.node};
            const bool current = view_.isCurrent(target);
            const bool pending = view_.isPending(target);
            const bool focused = view_.isMoveFocused(target);
            if (current) {
                painter->fillRect(token.rect, option.palette.highlight());
            } else if (view_.isHovered(target)) {
                painter->fillRect(token.rect, option.palette.alternateBase());
            }
            painter->setPen(current ? option.palette.highlightedText().color()
                                    : option.palette.text().color());
            painter->drawText(token.rect.adjusted(8, 0, -8, 0), Qt::AlignVCenter, token.text);
            if (pending || focused) {
                QPen pen(option.palette.highlight().color(), pending ? 2 : 1,
                         pending ? Qt::DashLine : Qt::DotLine);
                painter->setPen(pen);
                painter->drawRect(token.rect.adjusted(1, 1, -2, -2));
            }
        }
        painter->restore();
    }
    std::uint64_t session = 0;

  private:
    GameTreeView& view_;
    VariationModel& model_;
};

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
        return move ? view_.moveIndex(move->target()) : -1;
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

GameTreeView::GameTreeView(app::AnalysisController& controller, QWidget* parent)
    : QTreeView(parent), controller_(controller), moves_(new VariationModel(controller, this)) {
    static const bool installed = [] {
        QAccessible::installFactory(navigationFactory);
        return true;
    }();
    static_cast<void>(installed);
    setObjectName(QStringLiteral("gameTree"));
    setAccessibleName(tr("PGN variations"));
    setModel(moves_);
    setItemDelegate(new VariationDelegate(*this, *moves_));
    setHeaderHidden(true);
    setEditTriggers(NoEditTriggers);
    setSelectionMode(NoSelection);
    setExpandsOnDoubleClick(false);
    setIndentation(0);
    setRootIsDecorated(false);
    setItemsExpandable(false);
    setMouseTracking(true);
    setMinimumSize(200, 80);
    setToolTip(tr("Click a move to navigate. Ctrl+Left/Right focuses moves in a line; Enter "
                  "selects. Plus/minus expands/collapses variations."));
    connect(&controller_, &app::AnalysisController::changed, this, &GameTreeView::refresh);
    refresh();
}
int GameTreeView::moveCount() const {
    return moves_->tree() ? static_cast<int>(moves_->tree()->accessibleOrder.size()) : 0;
}
std::optional<app::NavigationTarget> GameTreeView::moveAt(int index) const {
    return index >= 0 && index < moveCount()
               ? std::optional{app::NavigationTarget{
                     modelSession_,
                     moves_->tree()->accessibleOrder[static_cast<std::size_t>(index)]}}
               : std::nullopt;
}
int GameTreeView::moveIndex(app::NavigationTarget target) const {
    return validTarget(target)
               ? static_cast<int>(moves_->tree()->nodes.at(target.node).accessibleIndex)
               : -1;
}
bool GameTreeView::isHovered(app::NavigationTarget target) const {
    return hovered_ && hovered_->session == target.session && hovered_->node == target.node;
}
bool GameTreeView::validTarget(app::NavigationTarget target) const {
    return target.session == modelSession_ && moves_->tree() &&
           moves_->tree()->nodes.contains(target.node);
}
QString GameTreeView::moveDescription(app::NavigationTarget target) const {
    return validTarget(target) ? moves_->tree()->nodes.at(target.node).label : QString{};
}
QRect GameTreeView::moveRect(app::NavigationTarget target) const {
    if (!validTarget(target)) {
        return {};
    }
    const auto index = moves_->indexFor(target.node);
    for (auto ancestor = index.parent(); ancestor.isValid(); ancestor = ancestor.parent()) {
        if (!isExpanded(ancestor)) {
            return {};
        }
    }
    const auto rect = visualRect(index);
    const auto* delegate = static_cast<VariationDelegate*>(itemDelegate());
    for (const auto& token : tokens(*moves_, index, rect, delegate->lineFont(index))) {
        if (token.node == target.node) {
            return token.rect;
        }
    }
    return {};
}
bool GameTreeView::isCurrent(app::NavigationTarget target) const {
    return validTarget(target) && controller_.currentNode() == target.node;
}
bool GameTreeView::isPending(app::NavigationTarget target) const {
    return validTarget(target) && controller_.busy() && controller_.requestedNode() == target.node;
}
bool GameTreeView::isMoveFocused(app::NavigationTarget target) const {
    return hasFocus() && focused_ && focused_->session == target.session &&
           focused_->node == target.node;
}
bool GameTreeView::canExpand(app::NavigationTarget target) const {
    return validTarget(target) && !moves_->line(moves_->indexFor(target.node)).children.empty();
}
bool GameTreeView::expanded(app::NavigationTarget target) const {
    return validTarget(target) && isExpanded(moves_->indexFor(target.node));
}
void GameTreeView::toggleExpanded(app::NavigationTarget target) {
    if (canExpand(target)) {
        const auto index = moves_->indexFor(target.node);
        setExpanded(index, !isExpanded(index));
    }
}
void GameTreeView::activateMove(app::NavigationTarget target) {
    if (isEnabled() && isVisible() && validTarget(target)) {
        focusMove(target);
        controller_.navigate(target);
    }
}
void GameTreeView::focusMove(app::NavigationTarget target) {
    if (!isEnabled() || !isVisible() || !validTarget(target)) {
        return;
    }
    focused_ = target;
    const auto index = moves_->indexFor(target.node);
    for (auto ancestor = index.parent(); ancestor.isValid(); ancestor = ancestor.parent()) {
        expand(ancestor);
    }
    setCurrentIndex(index);
    scrollTo(index, EnsureVisible);
    setFocus(Qt::OtherFocusReason);
    viewport()->update();
    QAccessibleEvent event(this, QAccessible::Focus);
    QAccessible::updateAccessibility(&event);
}
std::optional<app::NavigationTarget> GameTreeView::targetAt(const QPoint& point) const {
    const auto index = indexAt(point);
    if (!index.isValid()) {
        return std::nullopt;
    }
    const auto* delegate = static_cast<VariationDelegate*>(itemDelegate());
    for (const auto& token : tokens(*moves_, index, visualRect(index), delegate->lineFont(index))) {
        if (token.rect.contains(point)) {
            return app::NavigationTarget{modelSession_, token.node};
        }
    }
    return std::nullopt;
}
void GameTreeView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        if (const auto target = targetAt(event->position().toPoint())) {
            activateMove(*target);
            event->accept();
            return;
        }
    }
    const auto index = indexAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && index.isValid()) {
        const auto* delegate = static_cast<VariationDelegate*>(itemDelegate());
        if (!moves_->line(index).children.empty() &&
            disclosureRect(*moves_, index, visualRect(index), delegate->lineFont(index))
                .contains(event->position().toPoint())) {
            setExpanded(index, !isExpanded(index));
            event->accept();
            return;
        }
    }
    const auto before = currentIndex();
    QTreeView::mousePressEvent(event);
    if (currentIndex().isValid() && currentIndex() != before) {
        focused_ = app::NavigationTarget{modelSession_, moves_->line(currentIndex()).moves.front()};
    }
}
void GameTreeView::mouseMoveEvent(QMouseEvent* event) {
    hovered_ = targetAt(event->position().toPoint());
    viewport()->setCursor(hovered_ ? Qt::PointingHandCursor : Qt::ArrowCursor);
    setToolTip(hovered_ ? moveDescription(*hovered_) : tr("Click a move to navigate"));
    viewport()->update();
    QTreeView::mouseMoveEvent(event);
}
void GameTreeView::leaveEvent(QEvent* event) {
    hovered_.reset();
    viewport()->update();
    QTreeView::leaveEvent(event);
}
void GameTreeView::resizeEvent(QResizeEvent* event) {
    QTreeView::resizeEvent(event);
    scheduleDelayedItemsLayout();
}
void GameTreeView::keyPressEvent(QKeyEvent* event) {
    const auto index = currentIndex();
    if (event->modifiers() == Qt::ControlModifier && index.isValid() &&
        (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        const auto& line = moves_->line(index).moves;
        auto found = focused_ ? std::find(line.begin(), line.end(), focused_->node) : line.begin();
        if (found == line.end()) {
            found = line.begin();
        }
        if (event->key() == Qt::Key_Right && found + 1 != line.end()) {
            ++found;
        }
        if (event->key() == Qt::Key_Left && found != line.begin()) {
            --found;
        }
        focusMove({modelSession_, *found});
        return;
    }
    if (historyKey(event, controller_)) {
        return;
    }
    if (index.isValid() && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
                            event->key() == Qt::Key_Space)) {
        activateMove(focused_.value_or(
            app::NavigationTarget{modelSession_, moves_->line(index).moves.front()}));
        return;
    }
    if (index.isValid() && (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Minus)) {
        setExpanded(index, event->key() == Qt::Key_Plus);
        return;
    }
    QTreeView::keyPressEvent(event);
    if (currentIndex().isValid() && currentIndex() != index) {
        focused_ = app::NavigationTarget{modelSession_, moves_->line(currentIndex()).moves.front()};
        viewport()->update();
    }
}
void GameTreeView::refresh() {
    const bool newSession = modelSession_ != controller_.target(chess::NodeId{0}).session;
    std::vector<chess::NodeId> expandedNodes;
    if (!newSession && moves_->tree() != controller_.navigationTree() && moves_->tree()) {
        for (std::size_t i = 0; i < moves_->tree()->lines.size(); ++i) {
            if (isExpanded(moves_->lineIndex(i))) {
                expandedNodes.push_back(moves_->tree()->lines[i].moves.front());
            }
        }
    }
    const auto scroll = verticalScrollBar()->value();
    const bool rebuilt = moves_->refresh();
    modelSession_ = controller_.target(chess::NodeId{0}).session;
    static_cast<VariationDelegate*>(itemDelegate())->session = modelSession_;
    setEnabled(controller_.canNavigate());
    if (newSession) {
        focused_.reset();
        displayed_.reset();
    }
    if (rebuilt) {
        for (auto node : expandedNodes) {
            expand(moves_->indexFor(node));
        }
        if (focused_ && validTarget(*focused_)) {
            setCurrentIndex(moves_->indexFor(focused_->node));
        }
    }
    if (displayed_ != controller_.currentNode()) {
        displayed_ = controller_.currentNode();
        const auto index = moves_->indexFor(*displayed_);
        for (auto ancestor = index.parent(); ancestor.isValid(); ancestor = ancestor.parent()) {
            expand(ancestor);
        }
        scrollTo(index, EnsureVisible);
    } else {
        verticalScrollBar()->setValue(scroll);
    }
    viewport()->update();
}
} // namespace chess_review::ui
