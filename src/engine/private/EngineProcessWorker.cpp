#include "engine/private/EngineProcessWorker.h"
#include <QRegularExpression>
#include <algorithm>
#include <utility>
namespace chess_review::engine {
namespace {
bool sameDiscovery(const ValidatedEngine& saved, const ValidatedEngine& fresh) {
    if (saved.reportedName != fresh.reportedName || saved.reportedAuthor != fresh.reportedAuthor ||
        saved.capabilities.options.size() != fresh.capabilities.options.size()) {
        return false;
    }
    auto options = fresh.capabilities.options;
    std::sort(options.begin(), options.end(), [](const UciOption& left, const UciOption& right) {
        return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
    });
    for (const auto& expected : saved.capabilities.options) {
        const auto found =
            std::lower_bound(options.cbegin(), options.cend(), expected.name,
                             [](const UciOption& value, const QString& name) {
                                 return value.name.compare(name, Qt::CaseInsensitive) < 0;
                             });
        if (found == options.cend() ||
            found->name.compare(expected.name, Qt::CaseInsensitive) != 0) {
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
PathResult inspectExecutable(const QString& path) { return resolve(path); }
EngineProcessWorker::EngineProcessWorker(AttemptMode mode, ProcessConfiguration configuration,
                                         QObject* parent)
    : QObject(parent), process_(this), timer_(this), chunks_(this), file_(this), searchWatch_(this),
      searchProgress_(this), searchIdleMs_(configuration.searchIdleMs),
      searchResponseMs_(configuration.searchResponseMs), mode_(mode),
      arguments_(std::move(configuration.arguments)), outputLimit_(configuration.outputLimit),
      startMs_(configuration.startMs), handshakeMs_(configuration.handshakeMs) {
    searchWatch_.setInterval(50);
    searchProgress_.setInterval(100);
    searchProgress_.setTimerType(Qt::PreciseTimer);
    connect(&searchWatch_, &QTimer::timeout, this, &EngineProcessWorker::watchSearch);
    connect(&searchProgress_, &QTimer::timeout, this, [this] {
        if (search_ && std::exchange(progressDirty_, false)) {
            const auto snapshot = search_->snapshot;
            emit analysisUpdated(snapshot);
        }
    });
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    chunks_.setSingleShot(true);
    connect(&chunks_, &QTimer::timeout, this, &EngineProcessWorker::hashChunk);
    connect(&timer_, &QTimer::timeout, this, &EngineProcessWorker::timeout);
    connect(&process_, &QProcess::started, this, [this] {
        if (stage_ != Stage::Starting) {
            return;
        }
        if (!transition(Stage::AwaitingUciOk)) {
            return;
        }
        publish(RuntimePhase::Handshaking);
        if (stage_ != Stage::AwaitingUciOk) {
            return;
        }
        timer_.start(handshakeMs_);
        send("uci\n");
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { drain(false); });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] { drain(true); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (stage_ == Stage::Finished) {
            return;
        }
        if (error == QProcess::FailedToStart) {
            awaitingExit_ = false;
            fail({ErrorCode::Start, QStringLiteral("The executable could not start. Check that "
                                                   "it is compatible with this system.")});
        } else if (error != QProcess::Crashed) {
            fail({ErrorCode::Crash,
                  QStringLiteral("Communication with the engine failed. Reconnect to try again.")});
        }
    });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        // Deadlines control the child, not the time spent draining its final buffered output.
        timer_.stop();
        searchWatch_.stop();
        searchProgress_.stop();
        awaitingExit_ = false;
        pendingExit_ = std::pair{code, status};
        drain(false);
        drain(true);
        completeExit();
    });
}

void EngineProcessWorker::connectEngine(QUuid session, const EngineProfile& profile) {
    Q_ASSERT(mode_ == AttemptMode::Runtime);
    runtime_ = RuntimeState{session, profile.id, RuntimePhase::CheckingExecutable, {}, {}};
    expected_ = profile.engine;
    validate(session, profile.engine.executablePath);
}

void EngineProcessWorker::disconnectEngine(QUuid session) {
    if (stage_ == Stage::Finished || !runtime_ || runtime_->session != session || disconnecting_) {
        return;
    }
    disconnecting_ = true;
    publish(RuntimePhase::Disconnecting);
    chunks_.stop();
    file_.close();
    if (search_ && !failure_) {
        stopAnalysis(session, search_->request.id);
        return;
    }
    if (stage_ != Stage::FailureStopping) {
        quitRuntime();
    }
}

void EngineProcessWorker::quitRuntime() {
    output_.clear();
    if (process_.state() == QProcess::NotRunning) {
        if (!awaitingExit_ && !pendingExit_) {
            finishRuntime();
        }
        return;
    }
    if (stage_ != Stage::Quitting && stage_ != Stage::Terminating && stage_ != Stage::Killing) {
        if (!transition(Stage::Quitting)) {
            return;
        }
        timer_.start(1000);
        send("quit\n");
    }
}

void EngineProcessWorker::advanceEpoch() {
    process_.setReadChannel(QProcess::StandardOutput);
    bufferedEpoch_ = epoch_;
    preReadyBytes_ = process_.bytesAvailable();
    ++epoch_;
}

void EngineProcessWorker::validate(QUuid request, const QString& path) {
    Q_ASSERT(stage_ == Stage::Created);
    request_ = request;
    const auto resolved = resolve(path);
    if (const auto* error = std::get_if<Error>(&resolved)) {
        finish(*error);
        return;
    }
    // The user approved the resolved target, not a newly redirected symbolic alias.
    if (std::get<QString>(resolved) != path) {
        finish(
            Error{ErrorCode::Changed,
                  QStringLiteral(
                      "The selected executable path changed. Select it again before validation.")});
        return;
    }
    profile_.executablePath = path;
    publish(RuntimePhase::CheckingExecutable);
    if (stage_ != Stage::Created) {
        return;
    }
    beginHash(false);
}

void EngineProcessWorker::cancel() {
    if (mode_ == AttemptMode::Registration && stage_ != Stage::Finished) {
        fail({ErrorCode::Cancelled, QStringLiteral("Engine registration cancelled.")});
    }
}

void EngineProcessWorker::close() {
    if (runtime_) {
        disconnectEngine(runtime_->session);
    } else {
        cancel();
    }
}

bool EngineProcessWorker::transition(Stage next) {
    if (!canTransition(stage_, next)) {
        Q_ASSERT_X(false, "EngineProcessWorker", "Invalid lifecycle transition");
        fail({ErrorCode::Protocol, QStringLiteral("Invalid engine lifecycle transition.")});
        return false;
    }
    stage_ = next;
    emit stateChanged(next);
    return stage_ == next;
}

void EngineProcessWorker::beginHash(bool after) {
    if (!transition(after ? Stage::CheckingIntegrity : Stage::CheckingExecutable)) {
        return;
    }
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
    if (stage_ != (after ? Stage::CheckingIntegrity : Stage::CheckingExecutable)) {
        return;
    }
    hash_.reset();
    chunks_.start(0);
}

void EngineProcessWorker::hashChunk() {
    if (stage_ != Stage::CheckingExecutable && stage_ != Stage::CheckingIntegrity) {
        return;
    }
    // Read the observed length, so a growing regular file cannot extend hashing forever.
    const auto bytes = file_.read(std::min<qint64>(65536, remaining_));
    if (file_.error() != QFileDevice::NoError) {
        fail({ErrorCode::Read, QStringLiteral("Cannot finish reading the selected executable.")});
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
    if (stage_ == Stage::CheckingIntegrity) {
        if (digest != profile_.executableSha256) {
            fail(
                {ErrorCode::Changed,
                 QStringLiteral("The executable changed during validation. No engine was added.")});
            return;
        }
        profile_.lastValidatedAt = QDateTime::currentDateTimeUtc();
        if (!validEngine(profile_)) {
            fail({ErrorCode::Protocol,
                  QStringLiteral("The engine supplied invalid identity or capability data.")});
            return;
        }
        if (runtime_) {
            if (!transition(Stage::Ready)) {
                return;
            }
            timer_.stop();
            runtimeOutput_ = true;
            outputBytes_ = 0;
            outputWindow_.start();
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
    if (!transition(Stage::Starting)) {
        return;
    }
    publish(RuntimePhase::Starting);
    if (stage_ != Stage::Starting) {
        return;
    }
    emit progress(request_, QStringLiteral("Starting engine and detecting UCI capabilities…"));
    if (stage_ != Stage::Starting) {
        return;
    }
    timer_.start(startMs_);
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    awaitingExit_ = true;
    process_.start(profile_.executablePath, arguments_);
}

void EngineProcessWorker::send(const QByteArray& bytes) {
    if (process_.write(bytes) != bytes.size()) {
        fail({ErrorCode::Crash,
              QStringLiteral("Could not send a command to the engine. Reconnect to try again.")});
    }
}

void EngineProcessWorker::drain(bool errors) {
    if (stage_ == Stage::Finished) {
        return;
    }
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
        if (failure_ && stage_ != Stage::FailureStopping) {
            continue;
        }
        if (runtimeOutput_ && outputWindow_.elapsed() >= 1000) {
            outputBytes_ = 0;
            outputWindow_.restart();
        }
        outputBytes_ += bytes.size();
        if (outputBytes_ > outputLimit_) {
            fail({ErrorCode::OutputLimit, QStringLiteral("The engine exceeded the output limit.")});
            continue;
        }
        if (errors ||
            (disconnecting_ && (!search_ || stage_ == Stage::Quitting ||
                                stage_ == Stage::Terminating || stage_ == Stage::Killing))) {
            continue;
        }
        const auto receivedEpoch = beforeReady ? bufferedEpoch_ : epoch_;
        for (const auto byte : bytes) {
            // A reply in this chunk can begin explicit teardown. Do not parse its
            // remaining bytes after quit; the entire chunk was already accounted.
            if ((failure_ && stage_ != Stage::FailureStopping) ||
                (disconnecting_ && (!search_ || stage_ == Stage::Quitting ||
                                    stage_ == Stage::Terminating || stage_ == Stage::Killing))) {
                break;
            }
            if (byte == '\n' || byte == '\r') {
                if (!output_.isEmpty()) {
                    const auto line = std::exchange(output_, {});
                    handleLine(line, lineEpoch_);
                }
            } else {
                if (output_.isEmpty()) {
                    lineEpoch_ = receivedEpoch;
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

void EngineProcessWorker::completeExit() {
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
    if (stage_ == Stage::Finished) {
        return;
    }
    if (!failure_ && (stage_ != Stage::Quitting || exit.first != 0 ||
                      exit.second != QProcess::NormalExit || !output_.isEmpty())) {
        recordFailure(
            {ErrorCode::Crash,
             QStringLiteral("The engine exited unexpectedly or did not shut down cleanly. "
                            "Reconnect to try again.")});
    }
    if (runtime_) {
        finishRuntime();
    } else if (failure_) {
        finish(*failure_);
    } else {
        beginHash(true);
    }
}

void EngineProcessWorker::handleLine(const QByteArray& bytes, std::uint64_t receivedEpoch) {
    if (!bytes.isValidUtf8() || bytes.contains('\0')) {
        protocolFailure();
        return;
    }
    const auto line = QString::fromUtf8(bytes);
    const auto normalized = line.simplified();
    if (stage_ == Stage::FailureStopping) {
        const auto tokens = normalized.split(u' ');
        if (tokens.front() == QStringLiteral("bestmove")) {
            // Validate the terminal reply, but never change evidence after failure.
            // Valid and invalid terminal replies both end the stop grace period.
            if (receivedEpoch < search_->goEpoch || receivedEpoch > epoch_) {
                protocolFailure();
                return;
            }
            const auto terminal = parseBestMove(tokens, search_->request);
            if (const auto* error = std::get_if<Error>(&terminal)) {
                fail(*error);
            } else {
                quitRuntime();
            }
        } else if (normalized == QStringLiteral("readyok") && search_->probeAt &&
                   receivedEpoch == epoch_ && process_.bytesToWrite() == 0) {
            search_->probeAt.reset();
        } else if (tokens.front() == QStringLiteral("readyok") ||
                   tokens.front() == QStringLiteral("uciok") ||
                   tokens.front() == QStringLiteral("id") ||
                   tokens.front() == QStringLiteral("option")) {
            protocolFailure();
        }
        return;
    }
    if (normalized == QStringLiteral("info") || normalized.startsWith(QStringLiteral("info ")) ||
        normalized == QStringLiteral("bestmove") ||
        normalized.startsWith(QStringLiteral("bestmove "))) {
        searchLine(normalized.split(u' '), receivedEpoch);
        return;
    }
    if (normalized.startsWith(QStringLiteral("uciok ")) ||
        normalized.startsWith(QStringLiteral("readyok "))) {
        protocolFailure();
        return;
    }
    if (normalized == QStringLiteral("uciok")) {
        if (stage_ != Stage::AwaitingUciOk || profile_.reportedName.trimmed().isEmpty()) {
            protocolFailure();
            return;
        }
        profile_.capabilities = options_.capabilities();
        if (runtime_ && !sameDiscovery(*expected_, profile_)) {
            fail({ErrorCode::Changed,
                  QStringLiteral(
                      "The engine identity or options differ "
                      "from its saved registration. The saved profile was kept unchanged.")});
            return;
        }
        if (!transition(Stage::AwaitingReadyOk)) {
            return;
        }
        advanceEpoch();
        timer_.start(handshakeMs_);
        emit progress(request_, QStringLiteral("Waiting for engine readiness…"));
        if (stage_ != Stage::AwaitingReadyOk) {
            return;
        }
        send("isready\n");
        return;
    }
    if (normalized == QStringLiteral("readyok")) {
        if (search_) {
            searchReady(receivedEpoch);
            return;
        }
        // Bytes already received cannot answer a command sent later.
        if (stage_ != Stage::AwaitingReadyOk || receivedEpoch != epoch_ ||
            process_.bytesToWrite() != 0) {
            protocolFailure();
            return;
        }
        if (runtime_) {
            timer_.stop();
            beginHash(true);
            return;
        }
        if (!transition(Stage::Quitting)) {
            return;
        }
        timer_.start(1000);
        emit progress(request_, QStringLiteral("Closing validation engine…"));
        send("quit\n");
        return;
    }
    const bool optionLine =
        normalized == QStringLiteral("option") || normalized.startsWith(QStringLiteral("option "));
    if (normalized == QStringLiteral("id") || normalized.startsWith(QStringLiteral("id ")) ||
        optionLine) {
        if (stage_ != Stage::AwaitingUciOk) {
            protocolFailure();
            return;
        }
        if (optionLine) {
            auto parsed = parseOption(line);
            if (const auto* error = std::get_if<Error>(&parsed)) {
                fail(*error);
                return;
            }
            if (auto error = options_.append(std::get<UciOption>(std::move(parsed)))) {
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

void EngineProcessWorker::protocolFailure() {
    fail({ErrorCode::Protocol,
          search_ ? QStringLiteral("The engine returned an invalid search protocol response. "
                                   "Reconnect a compatible engine.")
          : stage_ == Stage::Ready
              ? QStringLiteral("The engine returned an unexpected UCI response while idle. "
                               "Reconnect a compatible engine.")
              : QStringLiteral("The executable returned an invalid UCI handshake. Choose a "
                               "compatible UCI engine.")});
}

void EngineProcessWorker::timeout() {
    if (stage_ == Stage::Finished || pendingExit_) {
        return;
    }
    if (stage_ == Stage::FailureStopping) {
        quitRuntime();
        return;
    }
    if (stage_ == Stage::Killing) {
        process_.kill();
        timer_.start(1000);
        return;
    }
    if (stage_ == Stage::Terminating) {
        if (!transition(Stage::Killing)) {
            return;
        }
        process_.kill();
        timer_.start(1000);
        return;
    }
    if (stage_ == Stage::Quitting) {
        if (!failure_) {
            recordFailure(
                {ErrorCode::Cleanup,
                 QStringLiteral("The engine did not exit after quit and required forced cleanup. "
                                "Reconnect to try again.")});
        }
        if (!transition(Stage::Terminating)) {
            return;
        }
        process_.terminate();
        timer_.start(1000);
        return;
    }
    QString message;
    switch (stage_) {
    case Stage::Starting:
        message = QStringLiteral("The engine did not start in time. Check executable compatibility "
                                 "and try connecting again.");
        break;
    case Stage::ConfiguringSearch:
        message = QStringLiteral(
            "The engine did not become ready to start analysis. Reconnect to try again.");
        break;
    case Stage::Stopping:
        message = QStringLiteral("The engine did not finish after stop. Reconnect to try again.");
        break;
    case Stage::SynchronizingSearch:
        message = QStringLiteral(
            "The engine did not synchronize after analysis. Reconnect to try again.");
        break;
    default:
        message = QStringLiteral("The engine did not complete its UCI handshake in time. "
                                 "Choose a compatible UCI engine and try again.");
        break;
    }
    fail({ErrorCode::Timeout, std::move(message)});
}

FailureStage EngineProcessWorker::failureStage() const {
    switch (stage_) {
    case Stage::Created:
    case Stage::CheckingExecutable:
        return FailureStage::ExecutableCheck;
    case Stage::Starting:
        return FailureStage::Launch;
    case Stage::AwaitingUciOk:
    case Stage::AwaitingReadyOk:
        return FailureStage::Handshake;
    case Stage::CheckingIntegrity:
        return FailureStage::IntegrityVerification;
    case Stage::Ready:
        return FailureStage::IdleConnection;
    case Stage::ConfiguringSearch:
        return FailureStage::SearchSetup;
    case Stage::Analyzing:
        return FailureStage::Analysis;
    case Stage::Stopping:
    case Stage::FailureStopping:
        return FailureStage::Stop;
    case Stage::SynchronizingSearch:
        return FailureStage::FinalSynchronization;
    default:
        return FailureStage::Cleanup;
    }
}

void EngineProcessWorker::recordFailure(Error error) {
    if (!failure_) {
        if (!error.stage) {
            error.stage = failureStage();
        }
        failure_ = std::move(error);
    }
}

void EngineProcessWorker::fail(Error error, bool stopFirst) {
    if (stage_ == Stage::Finished) {
        return;
    }
    const bool first = !failure_;
    recordFailure(std::move(error));
    chunks_.stop();
    searchWatch_.stop();
    searchProgress_.stop();
    progressDirty_ = false;
    file_.close();
    if (stopFirst && first && stage_ == Stage::Analyzing && search_ && search_->sentGo &&
        process_.state() == QProcess::Running) {
        if (!transition(Stage::FailureStopping)) {
            return;
        }
        timer_.start(searchResponseMs_);
        send("stop\n");
        if (stage_ == Stage::FailureStopping) {
            publish(RuntimePhase::Disconnecting);
        }
        return;
    }
    // Keep the first error even when a write, reply or exit fails during recovery.
    if (runtime_ && !disconnecting_) {
        publish(RuntimePhase::Disconnecting);
    }
    if (process_.state() == QProcess::NotRunning && !awaitingExit_ && !pendingExit_) {
        finish(*failure_);
    } else {
        quitRuntime();
    }
}

void EngineProcessWorker::finish(ValidationResult result) {
    if (stage_ == Stage::Finished) {
        return;
    }
    if (runtime_) {
        if (const auto* error = std::get_if<Error>(&result)) {
            recordFailure(*error);
        }
        finishRuntime();
        return;
    }
    Q_ASSERT(!awaitingExit_ && !pendingExit_ && process_.state() == QProcess::NotRunning);
    timer_.stop();
    chunks_.stop();
    file_.close();
    if (auto* error = std::get_if<Error>(&result); error && !error->stage) {
        error->stage = failureStage();
    }
    if (!transition(Stage::Finished)) {
        return;
    }
    emit completed(request_, result);
    emit finished();
}

void EngineProcessWorker::publish(RuntimePhase phase) {
    if (!runtime_) {
        return;
    }
    runtime_->phase = phase;
    if (phase != RuntimePhase::Ready) {
        runtime_->discovery.reset();
    }
    // A direct listener may disconnect and reset runtime_ before later listeners run.
    const auto snapshot = *runtime_;
    emit runtimeChanged(snapshot);
}

void EngineProcessWorker::finishRuntime() {
    if (stage_ == Stage::Finished) {
        return;
    }
    Q_ASSERT(!awaitingExit_ && !pendingExit_ && process_.state() == QProcess::NotRunning);
    timer_.stop();
    chunks_.stop();
    file_.close();
    if (!transition(Stage::Finished)) {
        return;
    }
    if (search_) {
        finishAnalysis(failure_ ? AnalysisOutcome::Failed : AnalysisOutcome::Stopped, failure_);
    }
    runtime_->discovery.reset();
    runtime_->error = failure_;
    publish(failure_ ? RuntimePhase::Failed : RuntimePhase::Disconnected);
    runtime_.reset();
    expected_.reset();
    disconnecting_ = false;
    emit finished();
}
} // namespace chess_review::engine
