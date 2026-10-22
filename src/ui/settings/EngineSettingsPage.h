#pragma once
#include "app/EngineSettingsController.h"
#include <QWidget>
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QFileDialog;
class QTableWidget;
namespace chess_review::ui {
class EnginePageTestAccess;
class EngineSettingsPage final : public QWidget {
    Q_OBJECT
  public:
    explicit EngineSettingsPage(app::EngineSettingsController& controller,
                                QWidget* parent = nullptr);

  private:
    friend class EnginePageTestAccess;
    void updateState();
    void updateDetails();
    app::EngineSettingsController& controller_;
    QTableWidget* profiles_;
    QPlainTextEdit* details_;
    QFileDialog* picker_ = nullptr; // Owned by this widget, reused between selections.
    bool picking_ = false;
    std::uint64_t profilesRevision_ = 0;
    QLabel* path_;
    QLabel* message_;
    QLabel* listError_;
    QPushButton* browse_;
    QPushButton* add_;
    QPushButton* cancel_;
    QPushButton* previous_;
    QPushButton* next_;
    QPushButton* refresh_;
};
} // namespace chess_review::ui
