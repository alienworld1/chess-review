#pragma once

#include "app/MoveNavigationState.h"
#include <QColor>
#include <QTableView>
#include <functional>

namespace chess_review::ui {
class TimelineModel;
// Presentation-only timeline, shared by Analysis and Review. The owner supplies
// immutable topology and identity; all interactions leave through intent signals.
class MoveTimeline : public QTableView {
    Q_OBJECT
  public:
    explicit MoveTimeline(QWidget* parent = nullptr);
    void setSnapshot(std::shared_ptr<const app::NavigationTree> tree,
                     const std::vector<chess::NodeId>& line, std::uint64_t session,
                     chess::NodeId current, std::optional<chess::NodeId> pending, bool enabled);
    void setMoveDescription(std::function<QString(chess::NodeId)> description);
    void setMoveAccent(std::function<QColor(chess::NodeId)> accent);
    void setHoverPreview(bool enabled);
    std::optional<app::NavigationTarget> targetAt(const QPoint& point) const;
  signals:
    void navigationRequested(chess_review::app::NavigationTarget target);
    void previewRequested(chess_review::app::NavigationTarget target);
    void previewCleared();
    void visibleRequested(chess_review::app::NavigationTarget first,
                          chess_review::app::NavigationTarget last);
    void previousRequested();
    void nextRequested();
    void beginningRequested();
    void endRequested();

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

  private:
    void visibleChanged();
    void clearHover();
    TimelineModel* moves_;
    bool refreshing_ = false;
    bool hoverPreview_ = false;
    std::optional<app::NavigationTarget> hovered_;
    std::optional<chess::NodeId> displayed_;
};
} // namespace chess_review::ui
