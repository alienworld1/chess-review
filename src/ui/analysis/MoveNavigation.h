#pragma once

#include "app/AnalysisController.h"
#include "ui/chess/MoveTimeline.h"
#include <QTreeView>

namespace chess_review::ui {
class VariationModel;
class MoveTimelineView final : public MoveTimeline {
    Q_OBJECT
  public:
    explicit MoveTimelineView(app::AnalysisController& controller, QWidget* parent = nullptr);

  private:
    void refresh();
    app::AnalysisController& controller_;
};

class GameTreeView final : public QTreeView {
    Q_OBJECT
  public:
    explicit GameTreeView(app::AnalysisController& controller, QWidget* parent = nullptr);
    int moveCount() const;
    int moveIndex(app::NavigationTarget target) const;
    std::optional<app::NavigationTarget> targetAt(const QPoint& point) const;
    std::optional<app::NavigationTarget> focusedTarget() const { return focused_; }
    bool isHovered(app::NavigationTarget target) const;
    std::optional<app::NavigationTarget> moveAt(int index) const;
    QString moveDescription(app::NavigationTarget target) const;
    QRect moveRect(app::NavigationTarget target) const;
    void activateMove(app::NavigationTarget target);
    void focusMove(app::NavigationTarget target);
    bool validTarget(app::NavigationTarget target) const;
    bool isCurrent(app::NavigationTarget target) const;
    bool isPending(app::NavigationTarget target) const;
    bool isMoveFocused(app::NavigationTarget target) const;
    bool canExpand(app::NavigationTarget target) const;
    bool expanded(app::NavigationTarget target) const;
    void toggleExpanded(app::NavigationTarget target);

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool viewportEvent(QEvent* event) override;

  private:
    void refresh();
    void revealMove(app::NavigationTarget target);
    void clearHover();
    app::AnalysisController& controller_;
    VariationModel* moves_;
    std::optional<chess::NodeId> displayed_;
    std::optional<app::NavigationTarget> focused_;
    std::optional<app::NavigationTarget> hovered_;
    std::uint64_t modelSession_ = 0;
};
} // namespace chess_review::ui
