#include "engine/EngineSupervisor.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <utility>

namespace chess_review::engine {
namespace {
bool sameDiscovery(const ValidatedEngine& saved, const ValidatedEngine& fresh) {
    if (saved.reportedName != fresh.reportedName || saved.reportedAuthor != fresh.reportedAuthor ||
        saved.capabilities.options.size() != fresh.capabilities.options.size()) {
        return false;
    }
    for (const auto& expected : saved.capabilities.options) {
        const auto found =
            std::find_if(fresh.capabilities.options.begin(), fresh.capabilities.options.end(),
                         [&](const UciOption& value) {
                             return value.name.compare(expected.name, Qt::CaseInsensitive) == 0;
                         });
        if (found == fresh.capabilities.options.end()) {
            return false;
        }
        auto comparable = *found;
        comparable.name = expected.name;
        if (comparable != expected) {
            return false;
        }
    }
    return true;
}
PathResult resolve(const QString& path) {
    if (path.isEmpty() || path.size() > 4096 || !path.isValidUtf16() ||
        path.contains(QChar::Null) || path.contains(u'\n') || path.contains(u'\r')) {
        return Error{ErrorCode::Path, QStringLiteral("Choose a valid local executable path.")};
    }
    const QFileInfo file(path);
    const auto canonical = file.canonicalFilePath();
    if (canonical.isEmpty() || canonical.size() > 4096 || !file.isFile() || !file.isReadable() ||
        !file.isExecutable()) {
        return Error{
            ErrorCode::Path,
            QStringLiteral(
                "Choose a readable, executable regular file. Check its location and permissions.")};
    }
    return canonical;
}
} // namespace
class SupervisorWorker final : public QObject {
    Q_OBJECT
  public:
    SupervisorWorker() : process_(this), timer_(this), chunks_(this), file_(this) {
        timer_.setSingleShot(true);
        timer_.setTimerType(Qt::PreciseTimer);
        chunks_.setSingleShot(true);
        connect(&chunks_, &QTimer::timeout, this, &SupervisorWorker::hashChunk);
        connect(&timer_, &QTimer::timeout, this, &SupervisorWorker::timeout);
        connect(&process_, &QProcess::started, this, [this] {
            if (stage_ != Stage::Starting) {
                return;
            }
            stage_ = Stage::Uci;
            publish(RuntimePhase::Handshaking);
            timer_.start(handshakeMs_);
            send("uci\n");
        });
        connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { drain(false); });
        connect(&process_, &QProcess::readyReadStandardError, this, [this] { drain(true); });
        connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (stage_ == Stage::Idle) {
                return;
            }
            if (error == QProcess::FailedToStart) {
                fail({ErrorCode::Start, QStringLiteral("The executable could not start. Check that "
                                                       "it is compatible with this system.")});
            } else if (error != QProcess::Crashed) {
                fail({ErrorCode::Crash, QStringLiteral("Communication with the engine failed.")});
            }
        });
        connect(&process_, &QProcess::finished, this,
                [this](int code, QProcess::ExitStatus status) {
                    pendingExit_ = std::pair{code, status};
                    drain(false);
                    drain(true);
                    completeExit();
                });
    }
    void inspect(QUuid request, const QString& path) { emit inspected(request, resolve(path)); }
    void connectEngine(QUuid session, const EngineProfile& profile) {
        if (closing_ || stage_ != Stage::Idle) {
            emit runtimeChanged(
                RuntimeState{session,
                             profile.id,
                             RuntimePhase::Failed,
                             {},
                             Error{closing_ ? ErrorCode::Closing : ErrorCode::Busy,
                                   QStringLiteral("Engine supervision is unavailable or busy.")}});
            return;
        }
        if (session.isNull() || profile.id <= 0 || !validEngine(profile.engine) ||
            !profile.optionOverrides.isEmpty()) {
            emit runtimeChanged(RuntimeState{
                session,
                profile.id,
                RuntimePhase::Failed,
                {},
                Error{
                    ErrorCode::InvalidRecord,
                    QStringLiteral("The saved engine is invalid or has unsupported overrides.")}});
            return;
        }
        runtime_ = RuntimeState{session, profile.id, RuntimePhase::CheckingExecutable, {}, {}};
        expected_ = profile.engine;
        validate(session, profile.engine.executablePath);
    }
    void disconnectEngine(QUuid session) {
        if (!runtime_ || runtime_->session != session || disconnecting_) {
            return;
        }
        disconnecting_ = true;
        publish(RuntimePhase::Disconnecting);
        chunks_.stop();
        file_.close();
        if (process_.state() == QProcess::NotRunning) {
            if (pendingExit_) {
                return;
            }
            finishRuntime();
            return;
        }
        if (stage_ != Stage::Quitting && stage_ != Stage::Terminating && stage_ != Stage::Killing) {
            stage_ = Stage::Quitting;
            timer_.start(1000);
            send("quit\n");
        }
    }
    void validate(QUuid request, const QString& path) {
        if (closing_ || stage_ != Stage::Idle) {
            emit completed(request,
                           Error{closing_ ? ErrorCode::Closing : ErrorCode::Busy,
                                 QStringLiteral("Engine validation is unavailable or busy.")});
            return;
        }
        request_ = request;
        failure_.reset();
        snapshot_.reset();
        profile_ = {};
        output_.clear();
        outputBytes_ = 0;
        preReadyBytes_ = 0;
        const auto resolved = resolve(path);
        if (const auto* error = std::get_if<Error>(&resolved)) {
            finish(*error);
            return;
        }
        // The user approved the resolved target, not a newly redirected symbolic alias.
        if (std::get<QString>(resolved) != path) {
            finish(Error{
                ErrorCode::Changed,
                QStringLiteral(
                    "The selected executable path changed. Select it again before validation.")});
            return;
        }
        profile_.executablePath = path;
        publish(RuntimePhase::CheckingExecutable);
        beginHash(false);
    }
    void cancel() {
        if (!runtime_ && stage_ != Stage::Idle) {
            fail({ErrorCode::Cancelled, QStringLiteral("Engine registration cancelled.")});
        }
    }
    void close() {
        closing_ = true;
        if (stage_ == Stage::Idle) {
            QThread::currentThread()->quit();
        } else if (runtime_) {
            disconnectEngine(runtime_->session);
        } else {
            cancel();
        }
    }
    void configure(QStringList arguments, int startMs, int handshakeMs, qsizetype outputLimit) {
        if (stage_ != Stage::Idle) {
            return;
        }
        arguments_ = std::move(arguments);
        startMs_ = startMs;
        handshakeMs_ = handshakeMs;
        outputLimit_ = outputLimit;
    }
  signals:
    void inspected(QUuid request, const chess_review::engine::PathResult& result);
    void progress(QUuid request, const QString& message);
    void completed(QUuid request, const chess_review::engine::ValidationResult& result);
    void runtimeChanged(const chess_review::engine::RuntimeState& state);

  private:
    enum class Stage {
        Idle,
        HashBefore,
        Starting,
        Uci,
        Ready,
        Connected,
        Quitting,
        Terminating,
        Killing,
        HashAfter
    };
    void beginHash(bool after) {
        stage_ = after ? Stage::HashAfter : Stage::HashBefore;
        const auto path = resolve(profile_.executablePath);
        if (!std::holds_alternative<QString>(path) ||
            std::get<QString>(path) != profile_.executablePath) {
            fail({ErrorCode::Changed,
                  QStringLiteral("The executable changed during validation. Select it again.")});
            return;
        }
        file_.setFileName(profile_.executablePath);
        if (!file_.open(QIODevice::ReadOnly)) {
            fail({ErrorCode::Read, QStringLiteral("Cannot read the selected executable.")});
            return;
        }
        const QFileInfo current(profile_.executablePath);
        if (!snapshot_) {
            snapshot_ = current;
        }
        if (current.size() != snapshot_->size() ||
            current.lastModified() != snapshot_->lastModified()) {
            fail({ErrorCode::Changed, QStringLiteral("The executable changed during validation. "
                                                     "Select it again.")});
            return;
        }
        remaining_ = snapshot_->size();
        emit progress(request_, after ? QStringLiteral("Checking executable is unchanged…")
                                      : QStringLiteral("Hashing executable…"));
        hash_.reset();
        chunks_.start(0);
    }
    void hashChunk() {
        if (stage_ != Stage::HashBefore && stage_ != Stage::HashAfter) {
            return;
        }
        // Read the observed length, so a growing regular file cannot extend hashing forever.
        const auto bytes = file_.read(std::min<qint64>(65536, remaining_));
        if (file_.error() != QFileDevice::NoError) {
            fail({ErrorCode::Read,
                  QStringLiteral("Cannot finish reading the selected executable.")});
            return;
        }
        hash_.addData(bytes);
        remaining_ -= bytes.size();
        if (remaining_ > 0 && !bytes.isEmpty()) {
            chunks_.start(0);
            return;
        }
        const QFileInfo current(profile_.executablePath);
        const auto resolved = resolve(profile_.executablePath);
        if (remaining_ != 0 || file_.size() != snapshot_->size() ||
            current.size() != snapshot_->size() ||
            current.lastModified() != snapshot_->lastModified() ||
            !std::holds_alternative<QString>(resolved) ||
            std::get<QString>(resolved) != profile_.executablePath) {
            fail({ErrorCode::Changed, QStringLiteral("The executable changed while being read. "
                                                     "Select it again.")});
            return;
        }
        file_.close();
        const auto digest = hash_.result();
        if (stage_ == Stage::HashAfter) {
            if (digest != profile_.executableSha256) {
                fail({ErrorCode::Changed,
                      QStringLiteral(
                          "The executable changed during validation. No engine was added.")});
                return;
            }
            profile_.lastValidatedAt = QDateTime::currentDateTimeUtc();
            if (!validEngine(profile_)) {
                fail({ErrorCode::Protocol,
                      QStringLiteral("The engine supplied invalid identity or capability data.")});
                return;
            }
            if (runtime_) {
                stage_ = Stage::Connected;
                timer_.stop();
                runtime_->discovery = profile_;
                publish(RuntimePhase::Ready);
            } else {
                finish(profile_);
            }
            return;
        }
        if (runtime_ && digest != expected_->executableSha256) {
            fail({ErrorCode::Changed,
                  QStringLiteral("The executable differs from its saved "
                                 "registration. Replacement management is not available yet.")});
            return;
        }
        profile_.executableSha256 = digest;
        stage_ = Stage::Starting;
        publish(RuntimePhase::Starting);
        emit progress(request_, QStringLiteral("Starting engine and detecting UCI capabilities…"));
        timer_.start(startMs_);
        process_.setProcessChannelMode(QProcess::SeparateChannels);
        process_.start(profile_.executablePath, arguments_);
    }
    void send(const QByteArray& bytes) {
        if (process_.write(bytes) != bytes.size()) {
            fail({ErrorCode::Crash, QStringLiteral("Could not send a command to the engine.")});
        }
    }
    void drain(bool errors) {
        // Read bounded chunks; never materialize an unbounded readAll buffer.
        process_.setReadChannel(errors ? QProcess::StandardError : QProcess::StandardOutput);
        qsizetype drained = 0;
        while (process_.bytesAvailable() > 0 && drained < 65536) {
            const bool beforeReady = !errors && preReadyBytes_ > 0;
            const auto bytes =
                process_.read(beforeReady ? std::min<qint64>(4096, preReadyBytes_) : 4096);
            if (beforeReady) {
                preReadyBytes_ -= bytes.size();
            }
            drained += bytes.size();
            if (bytes.isEmpty()) {
                break;
            }
            if (failure_) {
                continue;
            }
            outputBytes_ += bytes.size();
            if (outputBytes_ > outputLimit_) {
                fail({ErrorCode::OutputLimit,
                      QStringLiteral("The engine exceeded the output limit.")});
                continue;
            }
            if (errors) {
                continue;
            }
            const auto receivedStage = beforeReady ? Stage::Uci : stage_;
            for (const auto byte : bytes) {
                if (failure_) {
                    break;
                }
                if (byte == '\n' || byte == '\r') {
                    if (!output_.isEmpty()) {
                        const auto line = std::exchange(output_, {});
                        handleLine(line, lineStage_);
                    }
                } else {
                    if (output_.isEmpty()) {
                        lineStage_ = receivedStage;
                    }
                    output_.append(byte);
                    if (output_.size() > 65536) {
                        fail({ErrorCode::OutputLimit,
                              QStringLiteral("The engine produced an oversized UCI line.")});
                    }
                }
            }
        }
        if (process_.bytesAvailable() > 0) {
            QTimer::singleShot(0, this, [this, errors] {
                drain(errors);
                completeExit();
            });
        }
    }
    void completeExit() {
        if (!pendingExit_) {
            return;
        }
        process_.setReadChannel(QProcess::StandardOutput);
        if (process_.bytesAvailable() > 0) {
            return;
        }
        process_.setReadChannel(QProcess::StandardError);
        if (process_.bytesAvailable() > 0) {
            return;
        }
        const auto exit = *std::exchange(pendingExit_, std::nullopt);
        timer_.stop();
        if (stage_ == Stage::Idle) {
            return;
        }
        if (!failure_ && (stage_ != Stage::Quitting || exit.first != 0 ||
                          exit.second != QProcess::NormalExit || !output_.isEmpty())) {
            failure_ = Error{
                ErrorCode::Crash,
                QStringLiteral("The engine exited unexpectedly or did not shut down cleanly.")};
        }
        if (runtime_) {
            finishRuntime();
        } else if (failure_) {
            finish(*failure_);
        } else {
            beginHash(true);
        }
    }
    void handleLine(const QByteArray& bytes, Stage receivedStage) {
        if (!bytes.isValidUtf8() || bytes.contains('\0')) {
            protocolFailure();
            return;
        }
        const auto line = QString::fromUtf8(bytes);
        const auto normalized = line.simplified();
        if (normalized == QStringLiteral("bestmove") ||
            normalized.startsWith(QStringLiteral("bestmove ")) ||
            normalized.startsWith(QStringLiteral("uciok ")) ||
            normalized.startsWith(QStringLiteral("readyok "))) {
            protocolFailure();
            return;
        }
        if (normalized == QStringLiteral("uciok")) {
            if (stage_ != Stage::Uci || profile_.reportedName.trimmed().isEmpty()) {
                protocolFailure();
                return;
            }
            if (runtime_ && !sameDiscovery(*expected_, profile_)) {
                fail({ErrorCode::Changed,
                      QStringLiteral(
                          "The engine identity or options differ "
                          "from its saved registration. The saved profile was kept unchanged.")});
                return;
            }
            stage_ = Stage::Ready;
            preReadyBytes_ = process_.bytesAvailable();
            timer_.start(handshakeMs_);
            emit progress(request_, QStringLiteral("Waiting for engine readiness…"));
            send("isready\n");
            return;
        }
        if (normalized == QStringLiteral("readyok")) {
            // Bytes already read with uciok cannot answer the isready command it triggers.
            if (stage_ != Stage::Ready || receivedStage != Stage::Ready ||
                process_.bytesToWrite() != 0) {
                protocolFailure();
                return;
            }
            if (runtime_) {
                timer_.stop();
                beginHash(true);
                return;
            }
            stage_ = Stage::Quitting;
            timer_.start(1000);
            emit progress(request_, QStringLiteral("Closing validation engine…"));
            send("quit\n");
            return;
        }
        const bool optionLine = normalized == QStringLiteral("option") ||
                                normalized.startsWith(QStringLiteral("option "));
        if (normalized == QStringLiteral("id") || normalized.startsWith(QStringLiteral("id ")) ||
            optionLine) {
            if (stage_ != Stage::Uci) {
                protocolFailure();
                return;
            }
            if (optionLine) {
                auto parsed = parseOption(line);
                if (const auto* error = std::get_if<Error>(&parsed)) {
                    fail(*error);
                    return;
                }
                if (auto error = appendOption(profile_.capabilities,
                                              std::get<UciOption>(std::move(parsed)))) {
                    fail(*error);
                }
                return;
            }
            static const QRegularExpression identity(
                QStringLiteral("^\\s*id\\s+(name|author)[ \\t](.*)$"));
            const auto match = identity.match(line);
            if (!match.hasMatch()) {
                if (normalized == QStringLiteral("id") || normalized == QStringLiteral("id name") ||
                    normalized == QStringLiteral("id author")) {
                    protocolFailure();
                }
                return;
            }
            const auto value = match.captured(2);
            if (value.trimmed().isEmpty() || value.size() > 4096) {
                protocolFailure();
                return;
            }
            if (match.captured(1) == QStringLiteral("name")) {
                if (!profile_.reportedName.isEmpty()) {
                    protocolFailure();
                    return;
                }
                profile_.reportedName = value;
            } else {
                if (profile_.reportedAuthor) {
                    protocolFailure();
                    return;
                }
                profile_.reportedAuthor = value;
            }
        }
    }
    void protocolFailure() {
        fail({ErrorCode::Protocol, QStringLiteral("The executable returned an invalid UCI "
                                                  "handshake. Choose a compatible UCI engine.")});
    }
    void timeout() {
        if (stage_ == Stage::Killing) {
            process_.kill();
            timer_.start(1000);
            return;
        }
        if (stage_ == Stage::Terminating) {
            stage_ = Stage::Killing;
            process_.kill();
            timer_.start(1000);
            return;
        }
        if (stage_ == Stage::Quitting) {
            if (!failure_) {
                failure_ =
                    Error{ErrorCode::Cleanup,
                          QStringLiteral(
                              "The engine did not exit after quit and required forced cleanup.")};
            }
            stage_ = Stage::Terminating;
            process_.terminate();
            timer_.start(1000);
            return;
        }
        fail({ErrorCode::Timeout, QStringLiteral("The engine did not start or complete the UCI "
                                                 "handshake within the allowed time.")});
    }
    void fail(Error error) {
        if (!failure_) {
            failure_ = std::move(error);
        }
        if (runtime_ && !disconnecting_) {
            publish(RuntimePhase::Disconnecting);
        }
        chunks_.stop();
        file_.close();
        if (process_.state() == QProcess::NotRunning) {
            if (pendingExit_) {
                return;
            }
            finish(*failure_);
            return;
        }
        if (stage_ == Stage::Quitting || stage_ == Stage::Terminating || stage_ == Stage::Killing) {
            return;
        }
        stage_ = Stage::Quitting;
        process_.write("quit\n");
        timer_.start(1000);
    }
    void finish(ValidationResult result) {
        if (runtime_) {
            if (const auto* error = std::get_if<Error>(&result)) {
                failure_ = *error;
            }
            finishRuntime();
            return;
        }
        timer_.stop();
        chunks_.stop();
        file_.close();
        stage_ = Stage::Idle;
        emit completed(request_, result);
        if (closing_) {
            QThread::currentThread()->quit();
        }
    }
    void publish(RuntimePhase phase) {
        if (!runtime_) {
            return;
        }
        runtime_->phase = phase;
        if (phase != RuntimePhase::Ready) {
            runtime_->discovery.reset();
        }
        emit runtimeChanged(*runtime_);
    }
    void finishRuntime() {
        timer_.stop();
        chunks_.stop();
        file_.close();
        stage_ = Stage::Idle;
        runtime_->discovery.reset();
        runtime_->error = failure_;
        publish(failure_ ? RuntimePhase::Failed : RuntimePhase::Disconnected);
        runtime_.reset();
        expected_.reset();
        disconnecting_ = false;
        if (closing_) {
            QThread::currentThread()->quit();
        }
    }
    QProcess process_;
    QTimer timer_;
    QTimer chunks_;
    QFile file_;
    std::optional<QFileInfo> snapshot_;
    qint64 remaining_ = 0;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    Stage stage_ = Stage::Idle;
    Stage lineStage_ = Stage::Idle;
    QUuid request_;
    ValidatedEngine profile_;
    std::optional<Error> failure_;
    std::optional<std::pair<int, QProcess::ExitStatus>> pendingExit_;
    std::optional<RuntimeState> runtime_;
    std::optional<ValidatedEngine> expected_;
    bool disconnecting_ = false;
    QByteArray output_;
    qint64 outputBytes_ = 0;
    qint64 preReadyBytes_ = 0;
    bool closing_ = false;
    QStringList arguments_;
    qsizetype outputLimit_ = 1024 * 1024;
    int startMs_ = 5000;
    int handshakeMs_ = 10000;
};
struct EngineSupervisor::Impl {
    QThread thread;
    SupervisorWorker* worker = nullptr;
    bool closing = false;
};
EngineSupervisor::EngineSupervisor(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
    qRegisterMetaType<PathResult>();
    qRegisterMetaType<ValidationResult>();
    qRegisterMetaType<RuntimeState>();
    impl_->worker = new SupervisorWorker;
    impl_->worker->moveToThread(&impl_->thread);
    impl_->thread.setObjectName(QStringLiteral("engine-supervisor"));
    connect(&impl_->thread, &QThread::finished, impl_->worker, &QObject::deleteLater);
    connect(&impl_->thread, &QThread::finished, this, &EngineSupervisor::finishClose);
    connect(impl_->worker, &SupervisorWorker::inspected, this, &EngineSupervisor::inspected);
    connect(impl_->worker, &SupervisorWorker::progress, this, &EngineSupervisor::progress);
    connect(impl_->worker, &SupervisorWorker::completed, this, &EngineSupervisor::completed);
    connect(impl_->worker, &SupervisorWorker::runtimeChanged, this,
            &EngineSupervisor::runtimeChanged);
    impl_->thread.start();
}
EngineSupervisor::~EngineSupervisor() { Q_ASSERT(!impl_->thread.isRunning()); }
void EngineSupervisor::inspect(QUuid request, const QString& path) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->closing) {
        QTimer::singleShot(0, this, [this, request] {
            emit inspected(request, Error{ErrorCode::Closing,
                                          QStringLiteral("Engine validation is closing.")});
        });
        return;
    }
    QMetaObject::invokeMethod(
        impl_->worker, [worker = impl_->worker, request, path] { worker->inspect(request, path); });
}
void EngineSupervisor::validate(QUuid request, const QString& path) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->closing) {
        QTimer::singleShot(0, this, [this, request] {
            emit completed(request, Error{ErrorCode::Closing,
                                          QStringLiteral("Engine validation is closing.")});
        });
        return;
    }
    QMetaObject::invokeMethod(impl_->worker, [worker = impl_->worker, request, path] {
        worker->validate(request, path);
    });
}
void EngineSupervisor::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->closing) {
        return;
    }
    QMetaObject::invokeMethod(impl_->worker, &SupervisorWorker::cancel);
}
void EngineSupervisor::connectEngine(QUuid session, const EngineProfile& profile) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->closing) {
        QTimer::singleShot(0, this, [this, session, id = profile.id] {
            emit runtimeChanged(RuntimeState{
                session,
                id,
                RuntimePhase::Failed,
                {},
                Error{ErrorCode::Closing, QStringLiteral("Engine supervision is closing.")}});
        });
        return;
    }
    QMetaObject::invokeMethod(impl_->worker, [worker = impl_->worker, session, profile] {
        worker->connectEngine(session, profile);
    });
}
void EngineSupervisor::disconnectEngine(QUuid session) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!impl_->closing) {
        QMetaObject::invokeMethod(impl_->worker, [worker = impl_->worker, session] {
            worker->disconnectEngine(session);
        });
    }
}
void EngineSupervisor::close() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (std::exchange(impl_->closing, true)) {
        return;
    }
    QMetaObject::invokeMethod(impl_->worker, &SupervisorWorker::close);
}
void EngineSupervisor::finishClose() {
    if (!impl_->thread.wait(0)) {
        QTimer::singleShot(5, this, &EngineSupervisor::finishClose);
        return;
    }
    impl_->worker = nullptr;
    emit closed();
}
void EngineSupervisor::setTestArguments(QStringList arguments, int startMs, int handshakeMs,
                                        qsizetype outputLimit) {
    QMetaObject::invokeMethod(
        impl_->worker,
        [worker = impl_->worker, arguments = std::move(arguments), startMs, handshakeMs,
         outputLimit] { worker->configure(arguments, startMs, handshakeMs, outputLimit); });
}
} // namespace chess_review::engine
#include "EngineSupervisor.moc"
