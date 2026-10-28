#include "ui/chess/MoveTimeline.h"
#include <QAbstractTableModel>
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
#include <map>
namespace chess_review::ui {
namespace {
constexpr int currentPositionRole = Qt::UserRole;
constexpr int pendingPositionRole = Qt::UserRole + 1;
constexpr int accentRole = Qt::UserRole + 2;
} // namespace
class TimelineModel final : public QAbstractTableModel {
  public:
    explicit TimelineModel(QObject* parent) : QAbstractTableModel(parent) {}
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
        if (!id || !tree_) {
            return {};
        }
        const auto& entry = tree_->nodes.at(*id);
        const QString extra = description_ ? description_(*id) : QString{};
        if (role == Qt::DisplayRole) {
            return (entry.parent ? entry.san : entry.label) +
                   (extra.isEmpty() ? QString{} : QStringLiteral(" · ") + extra);
        }
        if (role == Qt::ToolTipRole || role == Qt::AccessibleTextRole) {
            const auto text = entry.label +
                              (extra.isEmpty() ? QString{} : QStringLiteral(" · ") + extra) +
                              (entry.notes.isEmpty() ? QString{} : QLatin1Char('\n') + entry.notes);
            return role == Qt::ToolTipRole ? text.toHtmlEscaped() : text;
        }
        if (role == Qt::AccessibleDescriptionRole) {
            return current_ == *id   ? tr("Current position")
                   : pending_ == *id ? tr("Pending position")
                                     : tr("Go to position");
        }
        if (role == accentRole) {
            return accent_ ? accent_(*id) : QColor{};
        }
        if (role == currentPositionRole) {
            return current_ == *id;
        }
        if (role == pendingPositionRole) {
            return pending_ == *id;
        }
        return {};
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
    void refresh(std::shared_ptr<const app::NavigationTree> tree,
                 const std::vector<chess::NodeId>& line, std::uint64_t session,
                 chess::NodeId current, std::optional<chess::NodeId> pending) {
        current_ = current;
        pending_ = pending;
        if (tree_ != tree || line_ != line || session_ != session) {
            beginResetModel();
            tree_ = std::move(tree);
            session_ = session;
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
    chess::NodeId current_{0};
    std::optional<chess::NodeId> pending_;

  public:
    std::function<QString(chess::NodeId)> description_;
    std::function<QColor(chess::NodeId)> accent_;

  private:
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
        QStyledItemDelegate::paint(painter, option, index);
        const auto color = index.data(accentRole).value<QColor>();
        if (color.isValid()) {
            painter->fillRect(QRect(option.rect.right() - 3, option.rect.top() + 3, 3,
                                    std::max(1, option.rect.height() - 6)),
                              color);
        }
    }
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        option->state &= ~QStyle::State_Selected;
        if (index.data(currentPositionRole).toBool()) {
            option->backgroundBrush = option->palette.highlight();
            option->palette.setBrush(QPalette::Text, option->palette.highlightedText());
        }
        if (index.data(pendingPositionRole).toBool()) {
            option->font.setUnderline(true);
        }
    }
};
} // namespace
MoveTimeline::MoveTimeline(QWidget* parent) : QTableView(parent), moves_(new TimelineModel(this)) {
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
                        clearHover();
                        emit navigationRequested({moves_->session(), *node});
                        return;
                    }
                }
            });
    const auto activate = [this](const QModelIndex& index) {
        if (const auto node = moves_->node(index)) {
            clearHover();
            emit navigationRequested({moves_->session(), *node});
        }
    };
    connect(this, &QTableView::clicked, this, activate);
    connect(this, &QTableView::activated, this, activate);
}
void MoveTimeline::setSnapshot(std::shared_ptr<const app::NavigationTree> tree,
                               const std::vector<chess::NodeId>& line, std::uint64_t session,
                               chess::NodeId current, std::optional<chess::NodeId> pending,
                               bool enabled) {
    if (refreshing_) {
        return;
    }
    const QScopedValueRollback guard(refreshing_, true);
    const auto focused = moves_->node(currentIndex());
    const auto scroll = verticalScrollBar()->value();
    const bool replaced = moves_->session() != session;
    if (replaced) {
        clearHover();
        displayed_.reset();
    }
    moves_->refresh(std::move(tree), line, session, current, pending);
    setEnabled(enabled);
    if (focused && currentIndex() != moves_->indexFor(*focused)) {
        setCurrentIndex(moves_->indexFor(*focused));
    }
    const auto published = moves_->indexFor(current);
    if (published.isValid()) {
        selectionModel()->select(published, QItemSelectionModel::ClearAndSelect);
    } else {
        selectionModel()->clearSelection();
    }
    if (displayed_ != current) {
        displayed_ = current;
        scrollTo(published, EnsureVisible);
    } else {
        verticalScrollBar()->setValue(scroll);
    }
}
void MoveTimeline::setMoveDescription(std::function<QString(chess::NodeId)> description) {
    moves_->description_ = std::move(description);
}
void MoveTimeline::setMoveAccent(std::function<QColor(chess::NodeId)> accent) {
    moves_->accent_ = std::move(accent);
}
void MoveTimeline::setHoverPreview(bool enabled) {
    hoverPreview_ = enabled;
    setMouseTracking(enabled);
}
std::optional<app::NavigationTarget> MoveTimeline::targetAt(const QPoint& point) const {
    const auto node = moves_->node(indexAt(point));
    return node ? std::optional<app::NavigationTarget>{{moves_->session(), *node}} : std::nullopt;
}
void MoveTimeline::clearHover() {
    if (hovered_) {
        hovered_.reset();
        emit previewCleared();
    }
}
void MoveTimeline::mouseMoveEvent(QMouseEvent* event) {
    QTableView::mouseMoveEvent(event);
    if (!hoverPreview_) {
        return;
    }
    const auto target = targetAt(event->position().toPoint());
    if (!target) {
        clearHover();
        return;
    }
    if (!hovered_ || hovered_->session != target->session || hovered_->node != target->node) {
        hovered_ = target;
        emit previewRequested(*target);
    }
}
void MoveTimeline::leaveEvent(QEvent* event) {
    clearHover();
    QTableView::leaveEvent(event);
}
void MoveTimeline::hideEvent(QHideEvent* event) {
    clearHover();
    QTableView::hideEvent(event);
}
void MoveTimeline::resizeEvent(QResizeEvent* event) {
    clearHover();
    QTableView::resizeEvent(event);
    visibleChanged();
}
void MoveTimeline::scrollContentsBy(int dx, int dy) {
    clearHover();
    QTableView::scrollContentsBy(dx, dy);
    visibleChanged();
}
void MoveTimeline::visibleChanged() {
    if (!model() || model()->rowCount() == 0) {
        return;
    }
    const int firstRow = std::max(0, rowAt(0));
    const int bottom = rowAt(viewport()->height() - 1);
    const int lastRow = bottom < 0 ? model()->rowCount() - 1 : bottom;
    auto first = moves_->node(model()->index(firstRow, 1));
    if (!first) {
        first = moves_->node(model()->index(firstRow, 2));
    }
    auto last = moves_->node(model()->index(lastRow, 2));
    if (!last) {
        last = moves_->node(model()->index(lastRow, 1));
    }
    if (first && last) {
        emit visibleRequested({moves_->session(), *first}, {moves_->session(), *last});
    }
}
void MoveTimeline::keyPressEvent(QKeyEvent* event) {
    if (event->modifiers() == Qt::NoModifier) {
        switch (event->key()) {
        case Qt::Key_Left:
            clearHover();
            emit previousRequested();
            break;
        case Qt::Key_Right:
            clearHover();
            emit nextRequested();
            break;
        case Qt::Key_Home:
            clearHover();
            emit beginningRequested();
            break;
        case Qt::Key_End:
            clearHover();
            emit endRequested();
            break;
        default:
            QTableView::keyPressEvent(event);
            return;
        }
        event->accept();
        return;
    }
    QTableView::keyPressEvent(event);
}
} // namespace chess_review::ui
