#pragma once
#include "app/EngineAnalysisController.h"
#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
namespace chess_review::ui {
class ChessBoardWidget;
class EvaluationBarWidget final : public QWidget {
  public:
    explicit EvaluationBarWidget(ChessBoardWidget& board, QWidget* parent = nullptr);
    void setEvaluation(const app::EngineLinePresentation& line);
    void setExpectedScore(std::optional<double> whiteShare, const QString& description);
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
    QListWidget* pv_;
    QSpinBox* width_;
    QPushButton* start_;
    QPushButton* pause_;
    QPushButton* stop_;
    QPushButton* deepen_;
    QWidget* previewControls_;
    QLabel* previewStatus_;
    QPushButton* previous_;
    QPushButton* next_;
    QPushButton* add_;
    std::optional<std::uint64_t> renderedCandidates_;
};
} // namespace chess_review::ui
