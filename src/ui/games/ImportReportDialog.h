#pragma once

#include "app/ImportController.h"
#include <QDialog>
#include <set>

class QPlainTextEdit;
class QPushButton;
class QTableView;
namespace chess_review::ui {
class ImportTableModel;
class ImportReportDialog final : public QDialog {
    Q_OBJECT
  public:
    explicit ImportReportDialog(app::ImportController& imports, QWidget* parent = nullptr);
    void resetBatch();
    void sync();
    void setVerbose(bool verbose);
    void setActionsAvailable(bool available);
    void gameRemoved(persistence::GameId id);
  signals:
    void openRequested(persistence::GameId id);
    void reviewRequested(persistence::GameId id);

  private:
    void activateGame(bool review);
    void showResultDetails();
    app::ImportController& imports_;
    ImportTableModel* model_;
    QTableView* results_;
    QPlainTextEdit* details_;
    QPushButton* open_;
    QPushButton* review_;
    std::set<persistence::GameId> removed_;
    QString detailText_;
    bool actionsAvailable_ = false;
    bool verbose_ = false;
};
} // namespace chess_review::ui
