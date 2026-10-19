#pragma once
#include "app/Diagnostic.h"
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
  signals:
    void initialUiState(const chess_review::app::UiState& state);
    void progress(const QString& message);
    void ready();
    void failed(const chess_review::app::Diagnostic& diagnostic);
    void uiStateFailed();
    void stopping();
    void finished();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void connectServices();
    void finishServices();
    diagnostics::Logger* logger_ = nullptr;
    persistence::Database database_;
    UiStateStore uiState_;
    std::optional<UiState> finalUiState_;
    Lifecycle state_ = Lifecycle::Loading;
    bool started_ = false;
    bool storageClosed_ = false;
    bool preferencesClosed_ = false;
    bool closingLogger_ = false;
};
} // namespace chess_review::app
