#include "ui/analysis/MoveNavigation.h"
#include <QAbstractTableModel>
#include <QAccessible>
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
void installMoveNavigationAccessibility();
namespace {

QString navigationHelp() {
    return GameTreeView::tr("Click a move to navigate. Ctrl+Left/Right focuses moves in a line; "
                            "Enter selects. Plus/minus expands/collapses variations.");
}
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
} // namespace
MoveTimelineView::MoveTimelineView(app::AnalysisController& controller, QWidget* parent)
    : MoveTimeline(parent), controller_(controller) {
    connect(this, &MoveTimeline::navigationRequested, &controller_,
            qOverload<app::NavigationTarget>(&app::AnalysisController::navigate));
    connect(this, &MoveTimeline::previousRequested, &controller_,
            &app::AnalysisController::previous);
    connect(this, &MoveTimeline::nextRequested, &controller_, &app::AnalysisController::next);
    connect(this, &MoveTimeline::beginningRequested, &controller_,
            &app::AnalysisController::beginning);
    connect(this, &MoveTimeline::endRequested, &controller_, &app::AnalysisController::end);
    connect(&controller_, &app::AnalysisController::changed, this, &MoveTimelineView::refresh);
    refresh();
}
void MoveTimelineView::refresh() {
    setSnapshot(controller_.navigationTree(), controller_.chosenLine(),
                controller_.target(chess::NodeId{0}).session, controller_.currentNode(),
                controller_.busy() ? std::optional{controller_.requestedNode()} : std::nullopt,
                controller_.canNavigate());
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
                QPen pen(current ? option.palette.highlightedText().color()
                                 : option.palette.highlight().color(),
                         pending ? 2 : 1, pending ? Qt::DashLine : Qt::DotLine);
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

} // namespace

GameTreeView::GameTreeView(app::AnalysisController& controller, QWidget* parent)
    : QTreeView(parent), controller_(controller), moves_(new VariationModel(controller, this)) {
    installMoveNavigationAccessibility();
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
    setVerticalScrollMode(ScrollPerPixel);
    setMouseTracking(true);
    setMinimumSize(200, 80);
    setToolTip(navigationHelp());
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { clearHover(); });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { clearHover(); });
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
    if (!validTarget(target)) {
        return {};
    }
    const auto& entry = moves_->tree()->nodes.at(target.node);
    return entry.notes.isEmpty() ? entry.label : entry.label + QLatin1Char('\n') + entry.notes;
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
    setCurrentIndex(index);
    setFocus(Qt::OtherFocusReason);
    revealMove(target);
    viewport()->update();
    QAccessibleEvent event(this, QAccessible::Focus);
    QAccessible::updateAccessibility(&event);
}
void GameTreeView::revealMove(app::NavigationTarget target) {
    if (!validTarget(target)) {
        return;
    }
    const auto index = moves_->indexFor(target.node);
    for (auto ancestor = index.parent(); ancestor.isValid(); ancestor = ancestor.parent()) {
        expand(ancestor);
    }
    scrollTo(index, EnsureVisible);
    const auto rect = moveRect(target);
    const auto visible = viewport()->rect();
    if (rect.bottom() > visible.bottom()) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() + rect.bottom() -
                                      visible.bottom());
    } else if (rect.top() < visible.top()) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() + rect.top() - visible.top());
    }
}
std::optional<app::NavigationTarget> GameTreeView::targetAt(const QPoint& point) const {
    if (!viewport()->rect().contains(point)) {
        return std::nullopt;
    }
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
    const auto target = targetAt(event->position().toPoint());
    if (target) {
        hovered_ = target;
        viewport()->setCursor(Qt::PointingHandCursor);
        setToolTip(moveDescription(*hovered_).toHtmlEscaped());
    } else {
        clearHover();
    }
    viewport()->update();
    QTreeView::mouseMoveEvent(event);
}
void GameTreeView::leaveEvent(QEvent* event) {
    clearHover();
    QTreeView::leaveEvent(event);
}
void GameTreeView::clearHover() {
    if (!hovered_) {
        return;
    }
    hovered_.reset();
    viewport()->unsetCursor();
    setToolTip(navigationHelp());
    viewport()->update();
}
bool GameTreeView::viewportEvent(QEvent* event) {
    if (event->type() == QEvent::Leave || event->type() == QEvent::Resize ||
        event->type() == QEvent::Hide) {
        clearHover();
    }
    if (event->type() == QEvent::Resize) {
        scheduleDelayedItemsLayout();
    }
    return QTreeView::viewportEvent(event);
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
        clearHover();
        focused_.reset();
        displayed_.reset();
    }
    if (rebuilt) {
        clearHover();
        for (auto node : expandedNodes) {
            expand(moves_->indexFor(node));
        }
        if (focused_ && validTarget(*focused_)) {
            setCurrentIndex(moves_->indexFor(focused_->node));
        }
    }
    if (displayed_ != controller_.currentNode()) {
        displayed_ = controller_.currentNode();
        revealMove({modelSession_, *displayed_});
    } else {
        verticalScrollBar()->setValue(scroll);
    }
    viewport()->update();
}
} // namespace chess_review::ui
