#pragma once
#include "config/Configuration.h"
#include <QObject>
#include <QThreadPool>
namespace chess_review::app {
enum class ConfigurationState { Loading, Valid, Invalid };
// Owning thread only; one load, immutable publication, await closed() before destruction.
class ReviewConfiguration final : public QObject {
    Q_OBJECT
  public:
    explicit ReviewConfiguration(QObject* parent = nullptr);
    ~ReviewConfiguration() override;
    void load(const std::optional<QString>& filename);
    void close();
    ConfigurationState state() const { return state_; }
    std::shared_ptr<const config::ValidatedConfiguration> snapshot() const {
        return configuration_;
    }
    const config::Diagnostics& diagnostics() const { return diagnostics_; }
    QString safeReport() const;
  signals:
    void changed();
    void closed();

  private:
    void finishClose();
    QThreadPool pool_;
    ConfigurationState state_ = ConfigurationState::Loading;
    std::shared_ptr<const config::ValidatedConfiguration> configuration_;
    config::Diagnostics diagnostics_;
    bool started_ = false;
    bool closing_ = false;
};
} // namespace chess_review::app
