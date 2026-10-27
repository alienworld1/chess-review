#pragma once
#include "app/ReviewGameController.h"
#include <QWidget>
class QComboBox;
class QLabel;
class QPushButton;
class QProgressBar;
class QTableView;
namespace chess_review::ui {
class ReviewRunsModel;
class ReviewPage final : public QWidget {
    Q_OBJECT
  public:
    explicit ReviewPage(app::ReviewGameController& controller, QWidget* parent = nullptr);
  signals:
    void backRequested();
    void settingsRequested();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void update();
    app::ReviewGameController& controller_;
    QComboBox* side_;
    QComboBox* engine_;
    QComboBox* quality_;
    QLabel* game_;
    QLabel* status_;
    QLabel* error_;
    QLabel* reason_;
    QProgressBar* progress_;
    QPushButton* start_;
    QPushButton* cancel_;
    QPushButton* resume_;
    QPushButton* previous_;
    QPushButton* next_;
    QTableView* runs_;
    ReviewRunsModel* model_;
    std::uint64_t profilesRevision_ = 0;
    persistence::GameId gameId_ = 0;
};
} // namespace chess_review::ui
