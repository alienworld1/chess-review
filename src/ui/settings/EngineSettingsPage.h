#pragma once
#include "app/EngineSettingsController.h"
#include <QWidget>
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QFileDialog;
class QTableWidget;
class QComboBox;
class QTabWidget;
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
    void updateRuntime();
    void updateOption();
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
    QPushButton* connect_;
    QPushButton* disconnect_;
    QLabel* runtimeStatus_;
    QComboBox* options_;
    QPlainTextEdit* optionDetails_;
    QTabWidget* detailsTabs_;
    std::optional<engine::ValidatedEngine> discovery_;
};
} // namespace chess_review::ui
