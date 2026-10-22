#pragma once
#include "engine/EngineProfile.h"
#include <QObject>
#include <QUuid>
#include <memory>

namespace chess_review::engine {
class ValidatorTestAccess;
// Owning-thread API. Await closed() before destruction. No executable starts on construction.
class ExecutableValidator final : public QObject {
    Q_OBJECT
  public:
    explicit ExecutableValidator(QObject* parent = nullptr);
    ~ExecutableValidator() override;
    void inspect(QUuid request, const QString& path);
    void validate(QUuid request, const QString& resolvedPath);
    void cancel();
    void close();
  signals:
    void inspected(QUuid request, const chess_review::engine::PathResult& result);
    void progress(QUuid request, const QString& message);
    void completed(QUuid request, const chess_review::engine::ValidationResult& result);
    void closed();

  private:
    void finishClose();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class ValidatorTestAccess;
    void setTestArguments(QStringList arguments, int startMs, int handshakeMs,
                          qsizetype outputLimit);
};
} // namespace chess_review::engine
