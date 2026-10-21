#pragma once
#include "app/AnalysisController.h"
#include <QWidget>

class QAction;
class QComboBox;
class QLabel;
namespace chess_review::ui {
class ChessBoardWidget;
class AnalysisPage final : public QWidget {
    Q_OBJECT
  public:
    explicit AnalysisPage(app::AnalysisController& controller, QWidget* parent = nullptr);
    app::BoardPreferences preferences() const { return preferences_; }
    void setPreferences(const app::BoardPreferences& preferences);
    void requestDiscard(std::function<void()> onDiscard);
    app::AnalysisController& controller() { return controller_; }
    void deactivate();

  private:
    void refresh();
    app::AnalysisController& controller_;
    ChessBoardWidget* board_;
    QLabel* status_;
    QLabel* topPlayer_;
    QLabel* bottomPlayer_;
    QComboBox* history_;
    QComboBox* continuations_;
    QComboBox* input_;
    QComboBox* color_;
    QAction* coordinates_;
    QAction* animation_;
    QAction* undo_;
    QAction* redo_;
    app::BoardPreferences preferences_;
    bool confirming_ = false;
    std::optional<std::uint64_t> displayedRevision_;
};
} // namespace chess_review::ui
