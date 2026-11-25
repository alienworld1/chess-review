#pragma once
#include "app/EngineSettingsController.h"
#include <QWidget>
#include <cstdint>
#include <optional>
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QFileDialog;
class QTableWidget;
class QComboBox;
class QTabWidget;
namespace chess_review::app {
class ReviewConfiguration;
}
namespace chess_review::ui {
class EnginePageTestAccess;
class EngineSettingsPage final : public QWidget {
    Q_OBJECT
  public:
    explicit EngineSettingsPage(app::EngineSettingsController& controller,
                                QWidget* parent = nullptr);
    void setVerbose(bool verbose);
    void installConfiguration(app::ReviewConfiguration& configuration);
    void showEngines();
    void setColorScheme(Qt::ColorScheme scheme);

  signals:
    void colorSchemeChanged(Qt::ColorScheme scheme);

  private:
    friend class EnginePageTestAccess;
    void updateState();
    void updateMissingIndicators();
    void updateDetails();
    void updateRuntime();
    void updateOption();
    void showOptionEditor();
    QTabWidget* sections_;
    QComboBox* appearance_;
    QPushButton* configure_;
    QPushButton* rename_;
    QPushButton* makeDefault_;
    QPushButton* locate_;
    QPushButton* remove_;
    QPushButton* test_;
    QLabel* profileMessage_;
    QLabel* testMessage_;
    const engine::EngineProfile* selectedProfile() const;
    app::EngineSettingsController& controller_;
    QTableWidget* profiles_;
    QPlainTextEdit* details_;
    QFileDialog* picker_ = nullptr;       // Owned by this widget, reused between selections.
    QFileDialog* locatePicker_ = nullptr; // Owned by this widget, reused for relocations.
    bool picking_ = false;
    bool locating_ = false;
    std::int64_t locatingProfileId_ = 0;
    std::uint64_t profilesRevision_ = 0;
    std::set<std::int64_t> shownMissing_;
    QList<engine::DetectedEngine> shownDetected_;
    QWidget* detectBox_;
    QLabel* detectMessage_;
    QComboBox* detected_;
    QPushButton* selectDetected_;
    QPushButton* detect_;
    QPlainTextEdit* path_;
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
    QPushButton* inspector_;
    bool verbose_ = false;
    std::optional<engine::ValidatedEngine> discovery_;
};
} // namespace chess_review::ui
