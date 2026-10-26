#pragma once
#include "app/EngineAnalysisController.h"
#include <QWidget>

class QLabel;
class QPushButton;
class QPlainTextEdit;
namespace chess_review::ui {
class ChessBoardWidget;
class EvaluationBarWidget final : public QWidget {
  public:
    explicit EvaluationBarWidget(ChessBoardWidget& board, QWidget* parent = nullptr);
    void setEvaluation(const app::EngineLinePresentation& line);
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void alignWithBoard();
    ChessBoardWidget& board_;
    std::optional<double> whiteShare_;
};

class EngineAnalysisPanel final : public QWidget {
    Q_OBJECT
  public:
    explicit EngineAnalysisPanel(app::EngineAnalysisController& controller,
                                 QWidget* parent = nullptr);
  signals:
    void manageEnginesRequested();

  private:
    void refresh();
    app::EngineAnalysisController& controller_;
    QLabel* engine_;
    QLabel* status_;
    QLabel* evaluation_;
    QLabel* depth_;
    QLabel* error_;
    QPlainTextEdit* pv_;
    QPushButton* start_;
    QPushButton* stop_;
};
} // namespace chess_review::ui
