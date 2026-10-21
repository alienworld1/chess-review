#pragma once

#include "app/GameLibraryController.h"
#include "app/ImportController.h"
#include <QWidget>

class QAction;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QTableView;
class QTimer;
namespace chess_review::ui {
class LibraryTableModel;
class ImportTableModel;
class GamesPage final : public QWidget {
    Q_OBJECT
  public:
    GamesPage(app::ImportController& imports, app::GameLibraryController& library,
              QAction& fileAction, QAction& pasteAction, QWidget* parent = nullptr);

  protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    void updateLibrary();
    void updateImport();
    void showResultDetails();
    app::ImportController& imports_;
    app::GameLibraryController& library_;
    LibraryTableModel* libraryModel_;
    ImportTableModel* importModel_;
    QTableView* games_;
    QTableView* results_;
    QLabel* libraryStatus_;
    QLabel* importStatus_;
    QPushButton* previous_;
    QPushButton* next_;
    QPushButton* refresh_;
    QPushButton* cancel_;
    QPlainTextEdit* details_;
    QTimer* updateTimer_;
};
} // namespace chess_review::ui
