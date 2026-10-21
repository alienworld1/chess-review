#pragma once
#include "app/AnalysisController.h"
#include "app/Diagnostic.h"
#include "app/GameImportService.h"
#include "app/GameLibraryController.h"
#include "app/ImportController.h"
#include "app/UiStateStore.h"
#include "persistence/Database.h"
#include <QObject>
#include <QString>

namespace chess_review::diagnostics {
class Logger;
}
namespace chess_review::app {
enum class Lifecycle { Loading, Ready, Failed, Stopping, Stopped };

class StartupCoordinator final : public QObject {
    Q_OBJECT
  public:
    explicit StartupCoordinator(QObject* parent = nullptr);
    // Logger must outlive finished(). Omitting the UI-state filename disables preferences.
    explicit StartupCoordinator(diagnostics::Logger& logger, QObject* parent = nullptr);
    void start(const QString& databasePath,
               const std::optional<QString>& uiStateFilename = std::nullopt);
    void shutdown();
    // Called synchronously by the composition root on stopping(), before saving begins.
    void setFinalUiState(const UiState& state);
    Lifecycle state() const;
    AnalysisController& analysis() { return analysis_; }
    // Production composition routes application Quit through the UI's discard gate.
    void requireQuitConfirmation() { confirmQuit_ = true; }
    GameImportService& gameImports() { return imports_; }
    ImportController& localImports() { return localImports_; }
    GameLibraryController& library() { return library_; }
    persistence::Database& gameRepository() { return database_; }
  signals:
    void initialUiState(const chess_review::app::UiState& state);
    void progress(const QString& message);
    void ready();
    void failed(const chess_review::app::Diagnostic& diagnostic);
    void uiStateFailed(chess_review::app::UiStateError error);
    void stopping();
    void finished();
    void quitRequested();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void connectServices();
    void finishServices();
    diagnostics::Logger* logger_ = nullptr;
    AnalysisController analysis_;
    persistence::Database database_;
    GameImportService imports_{database_};
    ImportController localImports_{imports_};
    GameLibraryController library_{database_};
    UiStateStore uiState_;
    std::optional<UiState> finalUiState_;
    Lifecycle state_ = Lifecycle::Loading;
    bool confirmQuit_ = false;
    bool analysisClosed_ = false;
    bool started_ = false;
    bool storageClosed_ = false;
    bool preferencesClosed_ = false;
    bool closingLogger_ = false;
};
} // namespace chess_review::app
