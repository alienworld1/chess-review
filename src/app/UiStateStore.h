#pragma once
#include "app/BoardState.h"
#include "app/NavigationController.h"
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <optional>

namespace chess_review::app {
// Stable privacy-safe diagnostic identifiers.
enum class UiStateError { Read = 0, Invalid = 1, Write = 2, Busy = 3 };

struct UiState {
    QByteArray geometry;
    bool maximized = false;
    Destination destination = Destination::Home;
    BoardPreferences board{};
};

// Owning thread only. One load, then one saveAndClose; await closed() before destruction.
// The private single-worker pool serializes all settings I/O and is joined asynchronously.
// Contended settings saves fail with Busy; they never wait for another writer to exit.
class UiStateStore final : public QObject {
    Q_OBJECT
  public:
    explicit UiStateStore(QObject* parent = nullptr);
    ~UiStateStore() override;
    void load(const QString& filename);
    void saveAndClose(const std::optional<UiState>& state = std::nullopt);
  signals:
    void loaded(const chess_review::app::UiState& state);
    void failed(chess_review::app::UiStateError error);
    void closed();

  private:
    void finishClose();
    QThreadPool pool_;
    QString filename_;
    bool started_ = false;
    bool closing_ = false;
};
} // namespace chess_review::app
Q_DECLARE_METATYPE(chess_review::app::UiState)

Q_DECLARE_METATYPE(chess_review::app::UiStateError)
