#include "engine/private/EngineProcessWorker.h"
#include <utility>

namespace chess_review::engine {
void EngineProcessWorker::analyze(QUuid session, const AnalysisRequest& request) {
    AnalysisSnapshot snapshot;
    snapshot.session = session;
    snapshot.request = request.id;
    snapshot.limit = request.limit;
    snapshot.requestedMultiPv = request.multiPv;
    snapshot.wdlRequested = request.wantWdl;
    const auto reject = [this, &snapshot](Error error) {
        emit analysisFinished({snapshot, AnalysisOutcome::Rejected, std::move(error)});
    };
    if (!runtime_ || runtime_->session != session || disconnecting_ || stage_ != Stage::Ready ||
        search_) {
        reject({ErrorCode::Busy,
                QStringLiteral("Search requires the matching ready engine connection.")});
        return;
    }
    // The bound prevents an unbounded lifetime UUID registry on a persistent connection.
    if (searchIds_.contains(request.id) || searchIds_.size() >= 65536) {
        reject(
            {ErrorCode::InvalidRequest,
             QStringLiteral("Use a fresh search identifier, or reconnect after 65,536 searches.")});
        return;
    }
    auto prepared = prepareSearch(request, profile_.capabilities);
    if (const auto* error = std::get_if<Error>(&prepared)) {
        reject(*error);
        return;
    }
    auto commands = std::get<PreparedSearch>(std::move(prepared));
    snapshot.effectiveMultiPv = commands.effectiveMultiPv;
    snapshot.wdlSupported = commands.wdlSupported;
    searchIds_.insert(request.id);
    search_.emplace(ActiveSearch{
        request, std::move(commands), std::move(snapshot), false, false, false, {}, 0, 0});
    searchClock_.start();
    if (!transition(Stage::ConfiguringSearch)) {
        return;
    }
    advanceEpoch();
    timer_.start(searchResponseMs_);
    publishSearch(SearchPhase::Configuring);
    if (stage_ != Stage::ConfiguringSearch || failure_) {
        return;
    }
    send(search_->prepared.options + "ucinewgame\nisready\n");
}
void EngineProcessWorker::publishSearch(SearchPhase phase) {
    if (search_) {
        const SearchState snapshot{search_->snapshot.session, search_->request.id, phase};
        emit searchChanged(snapshot);
    }
}
void EngineProcessWorker::startSearch() {
    timer_.stop();
    if (search_->stopped) {
        // No go was sent, so no bestmove is due; the configuration barrier has drained.
        if (!transition(Stage::SynchronizingSearch)) {
            return;
        }
        if (!transition(Stage::Ready)) {
            return;
        }
        if (disconnecting_) {
            quitRuntime();
        } else {
            finishAnalysis(AnalysisOutcome::Stopped);
        }
        return;
    }
    if (!transition(Stage::Analyzing)) {
        return;
    }
    if (search_->stopped) {
        if (!transition(Stage::SynchronizingSearch) || !transition(Stage::Ready)) {
            return;
        }
        if (disconnecting_) {
            quitRuntime();
        } else {
            finishAnalysis(AnalysisOutcome::Stopped);
        }
        return;
    }
    advanceEpoch();
    timer_.stop();
    search_->goEpoch = epoch_;
    search_->sentGo = true;
    searchClock_.restart();
    search_->lastActivity = 0;
    searchWatch_.start();
    searchProgress_.start();
    // Commands precede notifications so a direct stop listener cannot send stop before go.
    send(search_->prepared.position + search_->prepared.go);
    if (stage_ == Stage::Analyzing && !failure_) {
        publishSearch(SearchPhase::Analyzing);
    }
}
void EngineProcessWorker::stopAnalysis(QUuid session, QUuid requestId) {
    if (!search_ || search_->snapshot.session != session || search_->request.id != requestId ||
        failure_ || search_->receivedBestMove || search_->stopped) {
        return;
    }
    search_->stopped = true;
    if (stage_ == Stage::ConfiguringSearch || !search_->sentGo) {
        publishSearch(SearchPhase::Stopping);
        return;
    }
    if (stage_ != Stage::Analyzing || !transition(Stage::Stopping)) {
        return;
    }
    timer_.start(searchResponseMs_);
    send("stop\n");
    if (stage_ == Stage::Stopping && !failure_) {
        publishSearch(SearchPhase::Stopping);
    }
}
void EngineProcessWorker::searchReady(std::uint64_t receivedEpoch) {
    if (receivedEpoch != epoch_ || process_.bytesToWrite() != 0) {
        protocolFailure();
        return;
    }
    if (search_->probeAt) {
        search_->probeAt.reset();
        search_->lastActivity = searchClock_.elapsed();
        if (stage_ == Stage::SynchronizingSearch) {
            advanceEpoch();
            timer_.start(searchResponseMs_);
            send("isready\n");
        }
        return;
    }
    if (stage_ == Stage::ConfiguringSearch) {
        startSearch();
        return;
    }
    if (stage_ != Stage::SynchronizingSearch) {
        protocolFailure();
        return;
    }
    timer_.stop();
    if (!transition(Stage::Ready)) {
        return;
    }
    if (disconnecting_) {
        quitRuntime();
    } else {
        finishAnalysis(search_->stopped ? AnalysisOutcome::Stopped : AnalysisOutcome::Completed);
    }
}
void EngineProcessWorker::searchLine(const QStringList& tokens, std::uint64_t receivedEpoch) {
    const bool best = tokens.front() == QStringLiteral("bestmove");
    if (!search_ || (stage_ != Stage::Analyzing && stage_ != Stage::Stopping)) {
        if (best) {
            protocolFailure();
        }
        return;
    }
    if (receivedEpoch < search_->goEpoch || receivedEpoch > epoch_) {
        protocolFailure();
        return;
    }
    if (!best) {
        auto parsed = parseSearchInfo(tokens, search_->request, search_->snapshot.effectiveMultiPv);
        if (const auto* error = std::get_if<Error>(&parsed)) {
            fail(*error);
            return;
        }
        auto update = std::get<InfoUpdate>(std::move(parsed));
        const auto& stats = update.line.statistics;
        if (update.hasEvidence || stats.nodes || stats.timeMs || stats.nps || stats.depth ||
            stats.selectiveDepth || stats.tablebaseHits || stats.hashFull) {
            search_->lastActivity = searchClock_.elapsed();
            updateEvidence(search_->snapshot, std::move(update));
            progressDirty_ = true;
        }
        return;
    }
    if (std::holds_alternative<InfiniteLimit>(search_->request.limit) && !search_->stopped) {
        protocolFailure();
        return;
    }
    auto parsed = parseBestMove(tokens, search_->request);
    if (const auto* error = std::get_if<Error>(&parsed)) {
        fail(*error);
        return;
    }
    const auto bestMove = std::get<BestMove>(parsed);
    search_->snapshot.bestMove = bestMove.move;
    search_->snapshot.ponder = bestMove.ponder;
    search_->receivedBestMove = true;
    searchProgress_.stop();
    if (!transition(Stage::SynchronizingSearch)) {
        return;
    }
    timer_.start(searchResponseMs_);
    if (!search_->probeAt) {
        advanceEpoch();
        send("isready\n");
    }
    if (stage_ == Stage::SynchronizingSearch && !failure_) {
        const auto snapshot = search_->snapshot;
        progressDirty_ = false;
        emit analysisUpdated(snapshot);
        if (stage_ == Stage::SynchronizingSearch && !failure_) {
            publishSearch(SearchPhase::Synchronizing);
        }
    }
}
void EngineProcessWorker::watchSearch() {
    if (!search_ || failure_) {
        return;
    }
    const auto now = searchClock_.elapsed();
    if (search_->probeAt && now - *search_->probeAt >= searchResponseMs_) {
        fail({ErrorCode::Timeout,
              QStringLiteral("The engine did not answer its search readiness probe.")});
        return;
    }
    if (stage_ != Stage::Analyzing) {
        return;
    }
    if (const auto* limit = std::get_if<TimeLimit>(&search_->request.limit);
        limit && now >= static_cast<qint64>(limit->milliseconds) + searchResponseMs_) {
        fail({ErrorCode::Timeout, QStringLiteral("The engine did not finish its timed search.")});
        return;
    }
    if (!search_->probeAt && now - search_->lastActivity >= searchIdleMs_) {
        search_->probeAt = now;
        advanceEpoch();
        send("isready\n");
    }
}
void EngineProcessWorker::finishAnalysis(AnalysisOutcome outcome, std::optional<Error> error) {
    if (!search_) {
        return;
    }
    searchWatch_.stop();
    searchProgress_.stop();
    progressDirty_ = false;
    const auto completed = std::exchange(search_, std::nullopt);
    const AnalysisResult result{completed->snapshot, outcome, std::move(error)};
    if (!completed->receivedBestMove) {
        emit analysisUpdated(result.evidence);
    }
    emit analysisFinished(result);
}

} // namespace chess_review::engine
