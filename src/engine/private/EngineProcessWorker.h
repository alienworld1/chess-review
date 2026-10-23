#pragma once
#include "engine/EngineSupervisor.h"
#include "engine/private/OptionCollector.h"
#include "engine/private/ProcessStage.h"
#include "engine/private/SearchProtocol.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTimer>
#include <set>
#include <utility>

namespace chess_review::engine {
enum class AttemptMode { Registration, Runtime };
struct ProcessConfiguration {
    QStringList arguments;
    int startMs = 5000;
    int handshakeMs = 10000;
    qsizetype outputLimit = 1024 * 1024;
    int searchIdleMs = 10000;
    int searchResponseMs = 5000;
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
    void analyze(QUuid session, const AnalysisRequest& request);
    void stopAnalysis(QUuid session, QUuid requestId);
    void validate(QUuid request, const QString& path);
    void cancel();
    void close();
  signals:
    void progress(QUuid request, const QString& message);
    void completed(QUuid request, const chess_review::engine::ValidationResult& result);
    void runtimeChanged(const chess_review::engine::RuntimeState& state);
    void stateChanged(chess_review::engine::ProcessStage state);
    void searchChanged(const chess_review::engine::SearchState& state);
    void analysisUpdated(const chess_review::engine::AnalysisSnapshot& snapshot);
    void analysisFinished(const chess_review::engine::AnalysisResult& result);
    void finished();

  private:
    using Stage = ProcessStage;
    bool transition(Stage next);
    void beginHash(bool after);
    void hashChunk();
    void send(const QByteArray& bytes);
    void drain(bool errors);
    void completeExit();
    void handleLine(const QByteArray& bytes, std::uint64_t receivedEpoch);
    void advanceEpoch();
    void startSearch();
    void searchReady(std::uint64_t receivedEpoch);
    void searchLine(const QStringList& tokens, std::uint64_t receivedEpoch);
    void watchSearch();
    void publishSearch(SearchPhase phase);
    void finishAnalysis(AnalysisOutcome outcome, std::optional<Error> error = std::nullopt);
    void quitRuntime();
    void protocolFailure();
    void timeout();
    FailureStage failureStage() const;
    void recordFailure(Error error);
    void fail(Error error, bool stopFirst = false);
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
    std::uint64_t epoch_ = 0;
    std::uint64_t lineEpoch_ = 0;
    std::uint64_t bufferedEpoch_ = 0;
    QUuid request_;
    ValidatedEngine profile_;
    OptionCollector options_;
    std::optional<Error> failure_;
    std::optional<std::pair<int, QProcess::ExitStatus>> pendingExit_;
    std::optional<RuntimeState> runtime_;
    std::optional<ValidatedEngine> expected_;
    bool disconnecting_ = false;
    bool awaitingExit_ = false;
    QByteArray output_;
    qint64 outputBytes_ = 0;
    qint64 preReadyBytes_ = 0;
    struct ActiveSearch {
        AnalysisRequest request;
        PreparedSearch prepared;
        AnalysisSnapshot snapshot;
        bool stopped = false;
        bool sentGo = false;
        bool receivedBestMove = false;
        std::optional<qint64> probeAt;
        qint64 lastActivity = 0;
        std::uint64_t goEpoch = 0;
    };
    std::optional<ActiveSearch> search_;
    std::set<QUuid> searchIds_; // Bounded per connection; IDs cannot be reused.
    QTimer searchWatch_;
    QTimer searchProgress_;
    QElapsedTimer searchClock_;
    QElapsedTimer outputWindow_;
    bool progressDirty_ = false;
    bool runtimeOutput_ = false;
    const int searchIdleMs_;
    const int searchResponseMs_;
    const AttemptMode mode_;
    const QStringList arguments_;
    const qsizetype outputLimit_;
    const int startMs_;
    const int handshakeMs_;
};
} // namespace chess_review::engine
