#pragma once
#include "app/PuzzleController.h"
#include "app/PuzzleGenerationController.h"
#include <QWidget>
#include <vector>

class QLabel;
class QPushButton;
class QTableWidget;
class QStackedWidget;
class QComboBox;
class QSplitter;
namespace chess_review::ui {
class ChessBoardWidget;
class PuzzlesPage final : public QWidget {
    Q_OBJECT
  public:
    PuzzlesPage(app::PuzzleController& controller, app::PuzzleGenerationController& generation,
                QWidget* parent = nullptr);
    void setPreferences(const app::BoardPreferences& preferences);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void refresh();
    void refreshGeneration();
    void removeSelected();
    app::PuzzleController& controller_;
    app::PuzzleGenerationController& generation_;
    QStackedWidget* pages_;
    QTableWidget* library_;
    ChessBoardWidget* board_;
    QLabel* message_;
    QLabel* hints_;
    QLabel* libraryNotice_;
    QLabel* generationNotice_;
    QPushButton* open_;
    QPushButton* remove_;
    QPushButton* previous_;
    QPushButton* next_;
    QPushButton* refresh_;
    QPushButton* retry_;
    QPushButton* hint_;
    QPushButton* reveal_;
    QPushButton* analyze_;
    QPushButton* stepBack_;
    QPushButton* stepNext_;
    QPushButton* cancel_;
    QPushButton* resume_;
    QPushButton* saved_;
    QComboBox* jobs_;
    QSplitter* splitter_;
    std::vector<persistence::PuzzleId> rows_;
    std::vector<persistence::PuzzleJobId> jobIds_;
};
} // namespace chess_review::ui
