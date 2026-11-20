#pragma once
#include "config/Configuration.h"
#include <QObject>
#include <QThreadPool>
#include <functional>
namespace chess_review::app {
enum class ConfigurationState { Loading, Valid, Invalid };
// Owning thread only. Publishes immutable snapshots and drains accepted work before closing.
class ReviewConfiguration final : public QObject {
    Q_OBJECT
  public:
    explicit ReviewConfiguration(QObject* parent = nullptr);
    ~ReviewConfiguration() override;
    void load(const std::optional<QString>& filename);
    void reload();
    void saveBasic(const config::BasicPreferences& preferences);
    void validateDraft(const QByteArray& contents);
    void saveDraft(const QByteArray& baseline, const QByteArray& contents);
    void close();
    bool busy() const { return busy_; }
    bool canEdit() const { return filename_.has_value() && !busy_ && !closing_; }
    const std::optional<QString>& filename() const { return filename_; }
    const std::optional<QByteArray>& source() const { return source_; }
    const QString& operationReport() const { return operationReport_; }
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
    enum class Operation { Load, Save, Validate };
    void submit(Operation operation, std::function<config::Document()> work);
    void finishClose();
    QThreadPool pool_;
    ConfigurationState state_ = ConfigurationState::Loading;
    std::shared_ptr<const config::ValidatedConfiguration> configuration_;
    config::Diagnostics diagnostics_;
    std::optional<QString> filename_;
    std::optional<QByteArray> source_;
    QString operationReport_;
    bool busy_ = false;
    bool started_ = false;
    bool closing_ = false;
};
} // namespace chess_review::app
