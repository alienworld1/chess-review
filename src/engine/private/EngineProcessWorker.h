#pragma once
#include "engine/EngineSupervisor.h"
#include "engine/private/ProcessStage.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTimer>
#include <utility>

namespace chess_review::engine {
enum class AttemptMode { Registration, Runtime };
struct ProcessConfiguration {
    QStringList arguments;
    int startMs = 5000;
    int handshakeMs = 10000;
    qsizetype outputLimit = 1024 * 1024;
};
PathResult inspectExecutable(const QString& path);
// Single attempt; constructed, used and destroyed on the supervisor event-loop thread.
class EngineProcessWorker final : public QObject {
    Q_OBJECT
  public:
    EngineProcessWorker(AttemptMode mode, ProcessConfiguration configuration,
                        QObject* parent = nullptr);
    void connectEngine(QUuid session, const EngineProfile& profile);
    void disconnectEngine(QUuid session);
    void validate(QUuid request, const QString& path);
    void cancel();
    void close();
  signals:
    void progress(QUuid request, const QString& message);
    void completed(QUuid request, const chess_review::engine::ValidationResult& result);
    void runtimeChanged(const chess_review::engine::RuntimeState& state);
    void stateChanged(chess_review::engine::ProcessStage state);
    void finished();

  private:
    using Stage = ProcessStage;
    bool transition(Stage next);
    void beginHash(bool after);
    void hashChunk();
    void send(const QByteArray& bytes);
    void drain(bool errors);
    void completeExit();
    void handleLine(const QByteArray& bytes, Stage receivedStage);
    void protocolFailure();
    void timeout();
    void fail(Error error);
    void finish(ValidationResult result);
    void publish(RuntimePhase phase);
    void finishRuntime();
    QProcess process_;
    QTimer timer_;
    QTimer chunks_;
    QFile file_;
    std::optional<QFileInfo> snapshot_;
    qint64 remaining_ = 0;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    Stage stage_ = Stage::Created;
    Stage lineStage_ = Stage::Created;
    QUuid request_;
    ValidatedEngine profile_;
    std::optional<Error> failure_;
    std::optional<std::pair<int, QProcess::ExitStatus>> pendingExit_;
    std::optional<RuntimeState> runtime_;
    std::optional<ValidatedEngine> expected_;
    bool disconnecting_ = false;
    bool awaitingExit_ = false;
    QByteArray output_;
    qint64 outputBytes_ = 0;
    qint64 preReadyBytes_ = 0;
    const AttemptMode mode_;
    const QStringList arguments_;
    const qsizetype outputLimit_;
    const int startMs_;
    const int handshakeMs_;
};
} // namespace chess_review::engine
