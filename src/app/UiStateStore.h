#pragma once
#include "app/NavigationController.h"
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <optional>

namespace chess_review::app {
struct UiState {
    QByteArray geometry;
    bool maximized = false;
    Destination destination = Destination::Home;
};

// Owning thread only. One load, then one saveAndClose; await closed() before destruction.
// The private single-worker pool serializes all settings I/O and is joined asynchronously.
class UiStateStore final : public QObject {
    Q_OBJECT
  public:
    explicit UiStateStore(QObject* parent = nullptr);
    ~UiStateStore() override;
    void load(const QString& filename);
    void saveAndClose(const std::optional<UiState>& state = std::nullopt);
  signals:
    void loaded(const chess_review::app::UiState& state);
    void failed(); // Safe settings failure; no paths/native error strings.
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
