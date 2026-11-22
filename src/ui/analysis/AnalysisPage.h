#pragma once
#include "app/AnalysisController.h"
#include <QWidget>

class QAction;
class QComboBox;
class QLabel;
class QMenu;
class QPushButton;
class QSplitter;
class QPlainTextEdit;
class QVBoxLayout;
namespace chess_review::app {
class EngineAnalysisController;
}
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
    void installReviewNavigation();
    void setReviewContext(const QString& game, const QString& position);
    void requestDiscard(std::function<void()> onDiscard);
    app::AnalysisController& controller() { return controller_; }
    void deactivate();
    void installEngineAnalysis(app::EngineAnalysisController& engine);
    void setAnalysisMultiPv(int count);
  signals:
    void manageEnginesRequested();
    void analysisMultiPvChanged(int count);
    void backToReviewRequested();

  protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void refresh();
    void refreshBoard();
    void applySplit();
    void setPosition();
    void editComment(app::NavigationTarget target);
    void confirmDelete(app::NavigationTarget target);
    void savePgn(std::function<void()> onSaved = {});
    void showVariationMenu(app::NavigationTarget target, const QPoint& globalPosition);
    app::AnalysisController& controller_;
    app::EngineAnalysisController* engine_ = nullptr;
    ChessBoardWidget* board_;
    QLabel* status_;
    QLabel* notice_;
    QLabel* reviewPosition_ = nullptr;
    QLabel* unsavedReviewEdits_ = nullptr;
    QString reviewGame_;
    QPlainTextEdit* notes_;
    QLabel* topPlayer_;
    QLabel* bottomPlayer_;
    QSplitter* splitter_;
    QSplitter* detailSplitter_ = nullptr;
    QWidget* notation_;
    MoveTimelineView* timeline_;
    GameTreeView* tree_;
    QVBoxLayout* boardLayout_;
    QComboBox* input_;
    QComboBox* color_;
    QAction* coordinates_;
    QAction* animation_;
    QAction* undo_;
    QAction* redo_;
    QPushButton* edit_;
    QPushButton* variations_;
    QPushButton* save_;
    QLabel* operationNotice_;
    app::BoardPreferences preferences_;
    bool confirming_ = false;
    bool displayingPreview_ = false;
    std::function<void()> afterExport_;
};
} // namespace chess_review::ui
