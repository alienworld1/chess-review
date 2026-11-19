#pragma once

#include "app/GameLibraryController.h"
#include "app/ImportController.h"
#include <QString>
#include <QWidget>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QTableView;
class QTimer;
namespace chess_review::ui {
class ChessBoardWidget;
class LibraryTableModel;
class ImportReportDialog;
class GamesPage final : public QWidget {
    Q_OBJECT
  public:
    GamesPage(app::ImportController& imports, app::GameLibraryController& library,
              QAction& fileAction, QAction& pasteAction, QWidget* parent = nullptr);
    void setVerbose(bool verbose);

  signals:
    void reviewRequested(chess_review::persistence::GameId id);
    void openRequested(chess_review::persistence::GameId id);

  protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void updateLibrary();
    void applyQuery();
    void updatePreview();
    void updatePreviewVisibility();
    void confirmDeletion();
    void requestOpen();
    void updateOpenAction();
    void updateImport();
    app::ImportController& imports_;
    app::GameLibraryController& library_;
    LibraryTableModel* libraryModel_;
    QTableView* games_;
    QLabel* libraryStatus_;
    QLabel* importStatus_;
    QPushButton* previous_;
    QPushButton* next_;
    QPushButton* refresh_;
    QPushButton* open_;
    QPushButton* review_;
    QPushButton* remove_;
    QPushButton* toggleFilters_;
    QPushButton* togglePreview_;
    QPushButton* clearFilters_;
    QLineEdit* search_;
    QComboBox* resultFilter_;
    QComboBox* reviewFilter_;
    QComboBox* sort_;
    QWidget* previewPane_;
    ChessBoardWidget* board_;
    QLabel* previewHeading_;
    QLabel* previewStatus_;
    QPlainTextEdit* metadata_;
    QTimer* searchTimer_;
    std::optional<bool> previewPreference_;
    std::optional<persistence::GameId> selectedGame_;
    std::optional<persistence::GameId> displayedPreview_;
    QLabel* deletionStatus_;
    QPushButton* cancel_;
    QPushButton* report_;
    ImportReportDialog* reportDialog_;
    QTimer* updateTimer_;
};
} // namespace chess_review::ui
