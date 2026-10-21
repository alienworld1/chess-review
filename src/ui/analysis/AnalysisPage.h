#pragma once
#include "app/AnalysisController.h"
#include <QWidget>

class QAction;
class QComboBox;
class QLabel;
class QSplitter;
class QPlainTextEdit;
namespace chess_review::ui {
class ChessBoardWidget;
class MoveTimelineView;
class GameTreeView;
class AnalysisPage final : public QWidget {
    Q_OBJECT
  public:
    explicit AnalysisPage(app::AnalysisController& controller, QWidget* parent = nullptr);
    app::BoardPreferences preferences() const;
    void setPreferences(const app::BoardPreferences& preferences);
    void requestDiscard(std::function<void()> onDiscard);
    app::AnalysisController& controller() { return controller_; }
    void deactivate();

  protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void refresh();
    void applySplit();
    app::AnalysisController& controller_;
    ChessBoardWidget* board_;
    QLabel* status_;
    QLabel* notice_;
    QPlainTextEdit* notes_;
    QLabel* topPlayer_;
    QLabel* bottomPlayer_;
    QSplitter* splitter_;
    MoveTimelineView* timeline_;
    GameTreeView* tree_;
    QComboBox* input_;
    QComboBox* color_;
    QAction* coordinates_;
    QAction* animation_;
    QAction* undo_;
    QAction* redo_;
    app::BoardPreferences preferences_;
    bool confirming_ = false;
};
} // namespace chess_review::ui
