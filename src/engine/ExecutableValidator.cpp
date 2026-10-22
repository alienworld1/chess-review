#include "engine/ExecutableValidator.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <utility>

namespace chess_review::engine {
namespace {
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
class ValidationWorker final : public QObject {
    Q_OBJECT
  public:
    ValidationWorker() : process_(this), timer_(this), chunks_(this), file_(this) {
        timer_.setSingleShot(true);
        timer_.setTimerType(Qt::PreciseTimer);
        chunks_.setSingleShot(true);
        connect(&chunks_, &QTimer::timeout, this, &ValidationWorker::hashChunk);
        connect(&timer_, &QTimer::timeout, this, &ValidationWorker::timeout);
        connect(&process_, &QProcess::started, this, [this] {
            if (stage_ != Stage::Starting) {
                return;
            }
            stage_ = Stage::Uci;
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
        connect(
            &process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
                drain(false);
                drain(true);
                timer_.stop();
                if (stage_ == Stage::Idle) {
                    return;
                }
                if (!failure_ && (stage_ != Stage::Quitting || code != 0 ||
                                  status != QProcess::NormalExit || !output_.isEmpty())) {
                    failure_ =
                        Error{ErrorCode::Crash,
                              QStringLiteral(
                                  "The engine exited unexpectedly or did not shut down cleanly.")};
                }
                if (failure_) {
                    finish(*failure_);
                } else {
                    beginHash(true);
                }
            });
    }
    void inspect(QUuid request, const QString& path) { emit inspected(request, resolve(path)); }
    void validate(QUuid request, const QString& path) {
        if (closing_ || stage_ != Stage::Idle) {
            emit completed(request,
                           Error{closing_ ? ErrorCode::Closing : ErrorCode::Busy,
                                 QStringLiteral("Engine validation is unavailable or busy.")});
            return;
        }
        request_ = request;
        failure_.reset();
        profile_ = {};
        output_.clear();
        outputBytes_ = 0;
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
        beginHash(false);
    }
    void cancel() {
        if (stage_ != Stage::Idle) {
            fail({ErrorCode::Cancelled, QStringLiteral("Engine registration cancelled.")});
        }
    }
    void close() {
        closing_ = true;
        if (stage_ == Stage::Idle) {
            QThread::currentThread()->quit();
        } else {
            cancel();
        }
    }
    void configure(QStringList arguments, int startMs, int handshakeMs, qsizetype outputLimit) {
        arguments_ = std::move(arguments);
        startMs_ = startMs;
        handshakeMs_ = handshakeMs;
        outputLimit_ = outputLimit;
    }
  signals:
    void inspected(QUuid request, const chess_review::engine::PathResult& result);
    void progress(QUuid request, const QString& message);
    void completed(QUuid request, const chess_review::engine::ValidationResult& result);

  private:
    enum class Stage {
        Idle,
        HashBefore,
        Starting,
        Uci,
        Ready,
        Quitting,
        Terminating,
        Killing,
        HashAfter
    };
    void beginHash(bool after) {
        stage_ = after ? Stage::HashAfter : Stage::HashBefore;
        emit progress(request_, after ? QStringLiteral("Checking executable is unchanged…")
                                      : QStringLiteral("Hashing executable…"));
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
        hash_.reset();
        chunks_.start(0);
    }
    void hashChunk() {
        const auto bytes = file_.read(65536);
        if (bytes.isEmpty() && file_.error() != QFileDevice::NoError) {
            fail({ErrorCode::Read,
                  QStringLiteral("Cannot finish reading the selected executable.")});
            return;
        }
        hash_.addData(bytes);
        if (!file_.atEnd()) {
            chunks_.start(0);
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
            finish(profile_);
            return;
        }
        profile_.executableSha256 = digest;
        stage_ = Stage::Starting;
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
        while (process_.bytesAvailable() > 0) {
            const auto bytes = process_.read(4096);
            if (bytes.isEmpty()) {
                break;
            }
            outputBytes_ += bytes.size();
            if (failure_) {
                continue;
            }
            if (outputBytes_ > outputLimit_) {
                fail({ErrorCode::OutputLimit,
                      QStringLiteral("The engine exceeded the validation output limit.")});
                continue;
            }
            if (errors) {
                continue;
            }
            for (const auto byte : bytes) {
                if (failure_) {
                    break;
                }
                if (byte == '\n' || byte == '\r') {
                    if (!output_.isEmpty()) {
                        const auto line = std::exchange(output_, {});
                        handleLine(line);
                    }
                } else {
                    output_.append(byte);
                    if (output_.size() > 65536) {
                        fail({ErrorCode::OutputLimit,
                              QStringLiteral("The engine produced an oversized UCI line.")});
                    }
                }
            }
        }
    }
    void handleLine(const QByteArray& bytes) {
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
            stage_ = Stage::Ready;
            timer_.start(handshakeMs_);
            emit progress(request_, QStringLiteral("Waiting for engine readiness…"));
            send("isready\n");
            return;
        }
        if (normalized == QStringLiteral("readyok")) {
            if (stage_ != Stage::Ready) {
                protocolFailure();
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
                failure_ = Error{
                    ErrorCode::Cleanup,
                    QStringLiteral("The engine did not exit after quit. No engine was added.")};
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
        chunks_.stop();
        file_.close();
        if (process_.state() == QProcess::NotRunning) {
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
        timer_.stop();
        chunks_.stop();
        file_.close();
        stage_ = Stage::Idle;
        emit completed(request_, result);
        if (closing_) {
            QThread::currentThread()->quit();
        }
    }
    QProcess process_;
    QTimer timer_;
    QTimer chunks_;
    QFile file_;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    Stage stage_ = Stage::Idle;
    QUuid request_;
    ValidatedEngine profile_;
    std::optional<Error> failure_;
    QByteArray output_;
    qint64 outputBytes_ = 0;
    bool closing_ = false;
    QStringList arguments_;
    qsizetype outputLimit_ = 1024 * 1024;
    int startMs_ = 5000;
    int handshakeMs_ = 10000;
};
struct ExecutableValidator::Impl {
    QThread thread;
    ValidationWorker* worker = nullptr;
    bool closing = false;
};
ExecutableValidator::ExecutableValidator(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
    qRegisterMetaType<PathResult>();
    qRegisterMetaType<ValidationResult>();
    impl_->worker = new ValidationWorker;
    impl_->worker->moveToThread(&impl_->thread);
    impl_->thread.setObjectName(QStringLiteral("engine-supervisor"));
    connect(&impl_->thread, &QThread::finished, impl_->worker, &QObject::deleteLater);
    connect(&impl_->thread, &QThread::finished, this, &ExecutableValidator::finishClose);
    connect(impl_->worker, &ValidationWorker::inspected, this, &ExecutableValidator::inspected);
    connect(impl_->worker, &ValidationWorker::progress, this, &ExecutableValidator::progress);
    connect(impl_->worker, &ValidationWorker::completed, this, &ExecutableValidator::completed);
    impl_->thread.start();
}
ExecutableValidator::~ExecutableValidator() { Q_ASSERT(!impl_->thread.isRunning()); }
void ExecutableValidator::inspect(QUuid request, const QString& path) {
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
void ExecutableValidator::validate(QUuid request, const QString& path) {
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
void ExecutableValidator::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (impl_->closing) {
        return;
    }
    QMetaObject::invokeMethod(impl_->worker, &ValidationWorker::cancel);
}
void ExecutableValidator::close() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (std::exchange(impl_->closing, true)) {
        return;
    }
    QMetaObject::invokeMethod(impl_->worker, &ValidationWorker::close);
}
void ExecutableValidator::finishClose() {
    if (!impl_->thread.wait(0)) {
        QTimer::singleShot(5, this, &ExecutableValidator::finishClose);
        return;
    }
    impl_->worker = nullptr;
    emit closed();
}
void ExecutableValidator::setTestArguments(QStringList arguments, int startMs, int handshakeMs,
                                           qsizetype outputLimit) {
    QMetaObject::invokeMethod(
        impl_->worker,
        [worker = impl_->worker, arguments = std::move(arguments), startMs, handshakeMs,
         outputLimit] { worker->configure(arguments, startMs, handshakeMs, outputLimit); });
}
} // namespace chess_review::engine
#include "ExecutableValidator.moc"
