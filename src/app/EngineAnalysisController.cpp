#include "app/EngineAnalysisController.h"
#include "review/Evidence.h"
#include <QMetaObject>
#include <QStringList>
#include <QTimer>
#include <algorithm>
#include <exception>
#include <span>
#include <utility>

namespace chess_review::app {
namespace {
constexpr int maximumDeepenDepth = 64;
} // namespace
EngineLinePresentation presentEngineLine(const chess::Position& root,
                                         const engine::AnalysisLine& line) {
    EngineLinePresentation result;
    result.depth = line.statistics.depth;
    result.rank = line.rank;
    if (line.score) {
        const auto& score = *line.score;
        const bool reverse = root.sideToMove() == chess::Color::Black;
        const bool negative = (score.value < 0) != reverse;
        // Unsigned magnitude also represents INT64_MIN without signed overflow.
        const auto magnitude = score.value < 0
                                   ? std::uint64_t{0} - static_cast<std::uint64_t>(score.value)
                                   : static_cast<std::uint64_t>(score.value);
        auto bound = score.bound;
        if (reverse && bound != engine::ScoreBound::Exact) {
            bound = bound == engine::ScoreBound::Lower ? engine::ScoreBound::Upper
                                                       : engine::ScoreBound::Lower;
        }
        const auto prefix = bound == engine::ScoreBound::Lower   ? QStringLiteral("≥ ")
                            : bound == engine::ScoreBound::Upper ? QStringLiteral("≤ ")
                                                                 : QString{};
        if (score.type == engine::ScoreType::Mate) {
            // UCI mate 0 describes the already-mated root side.
            const bool blackFavored = magnitude == 0 ? !reverse : negative;
            const auto side = blackFavored ? QObject::tr("Black") : QObject::tr("White");
            if (bound == engine::ScoreBound::Exact) {
                result.evaluation = QObject::tr("%1 mates in %2").arg(side).arg(magnitude);
                result.whiteShare = blackFavored ? 0.0 : 1.0;
            } else {
                // A bound may include draw/loss scores; it is not a guaranteed mating result.
                result.evaluation =
                    prefix + QObject::tr("%1M%2 (%3 mating score)")
                                 .arg(blackFavored ? QStringLiteral("−") : QStringLiteral("+"))
                                 .arg(magnitude)
                                 .arg(side);
            }
        } else {
            const bool signedNegative = magnitude != 0 && negative;
            result.evaluation = prefix +
                                (signedNegative ? QStringLiteral("−") : QStringLiteral("+")) +
                                QStringLiteral("%1.%2")
                                    .arg(magnitude / 100)
                                    .arg(magnitude % 100, 2, 10, QLatin1Char('0'));
            if (bound == engine::ScoreBound::Exact) {
                const double cp = static_cast<double>(std::min(magnitude, std::uint64_t{1000}));
                result.whiteShare = 0.5 + (negative ? -cp : cp) / 2000.0;
            }
        }
    }
    auto position = root;
    QStringList moves;
    for (std::size_t i = 0; i < line.pv.size(); ++i) {
        const auto& move = line.pv[i];
        const auto san = position.san(move);
        if (const auto* text = std::get_if<std::string>(&san)) {
            if (position.sideToMove() == chess::Color::White) {
                moves.append(QStringLiteral("%1.").arg(position.fullmoveNumber()));
            } else if (i == 0) {
                moves.append(QStringLiteral("%1...").arg(position.fullmoveNumber()));
            }
            moves.append(QString::fromStdString(*text));
        } else {
            return {}; // Fail closed rather than publish a partial, invalid line.
        }
        if (std::holds_alternative<chess::Error>(position.play(move))) {
            return {};
        }
    }
    result.pv = moves.join(QLatin1Char(' '));
    result.moves = line.pv;
    return result;
}

EngineAnalysisController::EngineAnalysisController(AnalysisController& board,
                                                   EngineSettingsController& settings,
                                                   engine::EngineSupervisor& supervisor,
                                                   QObject* parent)
    : QObject(parent), board_(board), settings_(settings), supervisor_(supervisor) {
    formatting_.setMaxThreadCount(1);
    connect(&board_, &AnalysisController::changed, this, &EngineAnalysisController::boardChanged);
    connect(&settings_, &EngineSettingsController::changed, this,
            &EngineAnalysisController::runtimeChanged);
    connect(&supervisor_, &engine::EngineSupervisor::scheduledChanged, this,
            [this](const engine::ScheduledState& state) {
                if (request_.isNull() || state.session != session_ || state.request != request_) {
                    return;
                }
                admitted_ = true;
                if (attempt_ != state.attempt) {
                    attempt_ = state.attempt;
                    ++epoch_;
                    pending_.reset();
                }
                // Cancellation changes control state, but queued phases still identify
                // the attempt whose lossless terminal evidence must be consumed.
                if (cancelling_) {
                    return;
                }
                switch (state.phase) {
                case engine::ScheduledPhase::Queued:
                    state_.status = tr("Queued");
                    break;
                case engine::ScheduledPhase::Configuring:
                    state_.status = tr("Configuring");
                    break;
                case engine::ScheduledPhase::Analyzing:
                    state_.status = state_.targetDepth && mode_ == Mode::Deepening
                                        ? tr("Deepening to depth %1").arg(*state_.targetDepth)
                                        : tr("Analyzing");
                    break;
                case engine::ScheduledPhase::Stopping:
                    state_.status = tr("Stopping");
                    break;
                case engine::ScheduledPhase::Synchronizing:
                    state_.status = tr("Synchronizing");
                    break;
                }
                updateControls();
            });
    connect(&supervisor_, &engine::EngineSupervisor::scheduledUpdated, this,
            &EngineAnalysisController::acceptEvidence);
    connect(&supervisor_, &engine::EngineSupervisor::scheduledFinished, this,
            [this](const engine::ScheduledResult& result) {
                if (request_.isNull() || result.identity.session != session_ ||
                    result.identity.request != request_) {
                    return;
                }
                // Rejected duplicate submissions do not terminate the admitted request.
                if (admitted_ && result.outcome == engine::ScheduledOutcome::Rejected) {
                    return;
                }
                if (result.evidence) {
                    acceptEvidence(*result.evidence);
                }
                const bool interrupted = cancelling_;
                request_ = {};
                admitted_ = false;
                attempt_.reset();
                cancelling_ = false;
                if (result.outcome == engine::ScheduledOutcome::Failed ||
                    result.outcome == engine::ScheduledOutcome::Rejected) {
                    mode_ = Mode::Stopped;
                    state_.targetDepth.reset();
                    state_.status = tr("Failed");
                    state_.error = result.error ? result.error->message : tr("Analysis failed.");
                } else if (!interrupted && result.outcome == engine::ScheduledOutcome::Completed &&
                           revision_ != 0 && revision_ == board_.board().revision) {
                    mode_ = Mode::Stopped;
                    std::optional<int> depth;
                    if (result.evidence) {
                        const auto& snapshot = result.evidence->evidence;
                        depth = snapshot.completedIteration.empty()
                                    ? snapshot.commonDepth
                                    : snapshot.completedIteration.front().statistics.depth;
                    }
                    state_.status =
                        state_.targetDepth && (!depth || *depth < *state_.targetDepth)
                            ? tr("Finished before reaching depth %1").arg(*state_.targetDepth)
                            : tr("Completed");
                    state_.targetDepth.reset();
                } else if (mode_ == Mode::Paused) {
                    state_.status = tr("Paused");
                } else if (mode_ == Mode::Stopped) {
                    state_.status = tr("Stopped");
                }
                // Queue continuation before notifying observers. Qt cancels it if a
                // nested event loop retires this consumer during changed().
                QTimer::singleShot(0, this, &EngineAnalysisController::finishClose);
                if (settings_.state().runtime.phase != engine::RuntimePhase::Ready) {
                    runtimeChanged();
                } else {
                    reconcile();
                }
            });
    runtimeChanged();
}
EngineAnalysisController::~EngineAnalysisController() {
    Q_ASSERT_X(formatting_.waitForDone(0), "EngineAnalysisController", "Await closed()");
}
bool EngineAnalysisController::boardReady() const {
    return !closing_ && visible_ && !board_.busy() && board_.canNavigate() &&
           board_.board().interactive && board_.position();
}
void EngineAnalysisController::updateControls() {
    const auto& runtime = settings_.state().runtime;
    state_.availableLines = 1;
    if (runtime.discovery && runtime.discovery->capabilities.supportsMultiPv) {
        const auto& options = runtime.discovery->capabilities.options;
        const auto found = std::find_if(options.begin(), options.end(), [](const auto& option) {
            return option.name.compare(QStringLiteral("MultiPV"), Qt::CaseInsensitive) == 0;
        });
        if (found != options.end() && found->maximum) {
            const auto available = std::max<std::size_t>(board_.board().legalMoves.size(), 1);
            state_.availableLines = static_cast<int>(
                std::min<qint64>({*found->maximum, BoardPreferences::maxAnalysisMultiPv,
                                  static_cast<qint64>(available)}));
        }
    }
    const bool ready = boardReady() && runtime.phase == engine::RuntimePhase::Ready;
    state_.paused = mode_ == Mode::Paused;
    state_.canStart = ready && !preview_ && request_.isNull() &&
                      (mode_ == Mode::Stopped || mode_ == Mode::Paused);
    state_.canPause = !closing_ && (mode_ == Mode::Following || mode_ == Mode::Deepening);
    state_.canStop = !closing_ && (mode_ != Mode::Stopped || (!request_.isNull() && !cancelling_));
    state_.canDeepen = ready && !preview_ && !cancelling_ && mode_ != Mode::Deepening &&
                       !board_.board().legalMoves.empty() &&
                       state_.line.depth.value_or(0) < maximumDeepenDepth;
    emit changed();
}
void EngineAnalysisController::runtimeChanged() {
    const auto& settings = settings_.state();
    state_.engineName = settings.connectedDisplayName;
    if (settings.runtime.phase != engine::RuntimePhase::Ready) {
        mode_ = Mode::Stopped;
        state_.targetDepth.reset();
        cancel();
        switch (settings.runtime.phase) {
        case engine::RuntimePhase::Disconnected:
            state_.status = tr("No engine connected");
            state_.error.clear();
            break;
        case engine::RuntimePhase::Failed:
            state_.status = tr("Failed");
            state_.error = settings.runtime.error ? settings.runtime.error->message : QString{};
            break;
        case engine::RuntimePhase::Disconnecting:
            state_.status = tr("Disconnecting");
            break;
        default:
            state_.status = tr("Starting engine");
            break;
        }
    } else if (session_ != settings.runtime.session || state_.status.isEmpty()) {
        session_ = settings.runtime.session;
        ++epoch_;
        revision_ = 0;
        pending_.reset();
        clearPreview();
        clearEvidence();
        state_.status = tr("Ready");
        state_.error.clear();
    }
    updateControls();
}
void EngineAnalysisController::boardChanged() {
    if ((!board_.canNavigate() && !board_.switchingTabs()) ||
        (!board_.busy() && !board_.board().interactive)) {
        if (mode_ != Mode::Stopped && request_.isNull()) {
            state_.status = tr("Stopped");
        }
        mode_ = Mode::Stopped;
        state_.targetDepth.reset();
    }
    if (!board_.canNavigate() || board_.positionChanging() ||
        revision_ != board_.board().revision) {
        if (revision_ != 0) {
            ++epoch_;
            revision_ = 0;
            pending_.reset();
            clearPreview();
            clearEvidence();
        }
        if (mode_ == Mode::Deepening) {
            mode_ = Mode::Stopped;
            state_.targetDepth.reset();
        }
        cancel();
    }
    if (previewBoard_) {
        previewBoard_->blackAtBottom = board_.board().blackAtBottom;
    }
    reconcile();
}
void EngineAnalysisController::cancel() {
    if (!request_.isNull() && !cancelling_) {
        cancelling_ = true;
        state_.status = tr("Stopping");
        supervisor_.cancelScheduledAnalysis(session_, request_);
    }
}
void EngineAnalysisController::reconcile() {
    if ((mode_ == Mode::Following || mode_ == Mode::Deepening) && request_.isNull() &&
        boardReady() && settings_.state().runtime.phase == engine::RuntimePhase::Ready) {
        root_ = board_.position();
        revision_ = board_.board().revision;
        ++epoch_;
        pending_.reset();
        state_.error.clear();
        session_ = settings_.state().runtime.session;
        request_ = QUuid::createUuid();
        admitted_ = false;
        engine::AnalysisRequest request;
        request.id = request_;
        request.position = *root_;
        request.multiPv = state_.requestedLines;
        // Terminal roots can return null bestmove immediately, even for go infinite.
        if (board_.board().legalMoves.empty()) {
            request.limit = engine::DepthLimit{1};
        } else if (state_.targetDepth) {
            request.limit = engine::DepthLimit{static_cast<std::uint32_t>(*state_.targetDepth)};
        } else {
            request.limit = engine::InfiniteLimit{};
        }
        state_.status = tr("Queued");
        supervisor_.scheduleAnalysis(session_, request,
                                     engine::AnalysisPriority::InteractiveAnalysis);
    }
    updateControls();
}
void EngineAnalysisController::start() {
    if (state_.canStart) {
        mode_ = Mode::Following;
        reconcile();
    }
}
void EngineAnalysisController::pause() {
    if (!state_.canPause) {
        return;
    }
    mode_ = Mode::Paused;
    state_.targetDepth.reset();
    state_.status = tr("Paused");
    cancel();
    updateControls();
}
void EngineAnalysisController::stop() {
    const bool wasActive = state_.canStop;
    mode_ = Mode::Stopped;
    state_.targetDepth.reset();
    cancel();
    if (request_.isNull() && wasActive) {
        state_.status = tr("Stopped");
    }
    updateControls();
}
void EngineAnalysisController::deepen() {
    if (!state_.canDeepen) {
        return;
    }
    mode_ = Mode::Deepening;
    state_.targetDepth =
        std::max(18, std::min(state_.line.depth.value_or(0), maximumDeepenDepth - 4) + 4);
    cancel();
    reconcile();
}
void EngineAnalysisController::setMultiPv(int count) {
    count = std::clamp(count, 1, BoardPreferences::maxAnalysisMultiPv);
    if (count == state_.requestedLines) {
        return;
    }
    const bool changedWidth = std::min(count, state_.availableLines) !=
                              std::min(state_.requestedLines, state_.availableLines);
    state_.requestedLines = count;
    if (changedWidth && (mode_ == Mode::Following || mode_ == Mode::Deepening)) {
        cancel();
    }
    reconcile();
}
void EngineAnalysisController::setVisible(bool visible, HiddenSearch hiddenSearch) {
    if (visible_ == visible) {
        return;
    }
    visible_ = visible;
    if (!visible) {
        clearPreview();
        if (hiddenSearch == HiddenSearch::Stop) {
            stop();
            return;
        }
        cancel();
    }
    reconcile();
}
void EngineAnalysisController::acceptEvidence(const engine::ScheduledEvidence& evidence) {
    if (closing_ || request_.isNull() || !attempt_ || evidence.identity.session != session_ ||
        evidence.identity.request != request_ || evidence.identity.attempt != attempt_ ||
        evidence.evidence.session != session_ || evidence.evidence.request != *attempt_ ||
        revision_ == 0 || revision_ != board_.board().revision || board_.positionChanging()) {
        return;
    }
    receivedLiveEvidence_ = true;
    pending_ = evidence.evidence;
    formatNext();
}
void EngineAnalysisController::formatNext() {
    if (closing_ || formattingBusy_ || !pending_) {
        return;
    }
    auto snapshot = std::move(*pending_);
    pending_.reset();
    if (snapshot.lines.empty() || !root_) {
        return;
    }
    formattingBusy_ = true;
    const auto epoch = epoch_;
    formatting_.start([this, root = root_, snapshot = std::move(snapshot), epoch] {
        std::vector<EngineLinePresentation> candidates;
        const bool saved = snapshot.session.isNull();
        try {
            if (!saved ||
                (snapshot.provenance && snapshot.provenance->position == root->context())) {
                auto lines = std::span<const engine::AnalysisLine>(snapshot.lines);
                if (!snapshot.completedIteration.empty() &&
                    (!snapshot.commonDepth ||
                     snapshot.completedIteration.front().statistics.depth >=
                         snapshot.commonDepth)) {
                    lines = snapshot.completedIteration;
                } else if (!saved && snapshot.effectiveMultiPv > 1) {
                    // A rank reshuffle can briefly repeat a root move. Only the proven
                    // distinct prefix is a set of competing lines; keep rank 1 visible.
                    lines = lines.first(std::min(lines.size(), static_cast<std::size_t>(std::max(
                                                                   1, snapshot.achievedWidth))));
                }
                for (const auto& line : lines) {
                    auto presentation = presentEngineLine(*root, line);
                    if (!presentation.pv.isEmpty() || !presentation.evaluation.isEmpty()) {
                        candidates.push_back(std::move(presentation));
                    }
                }
            }
        } catch (const std::exception&) {
            candidates.clear();
        }
        QMetaObject::invokeMethod(
            this,
            [this, epoch, saved, candidates = std::move(candidates),
             stats = snapshot.statistics]() mutable {
                formattingBusy_ = false;
                QTimer::singleShot(0, this, [this] {
                    preparePreview();
                    formatNext();
                    finishClose();
                });
                if (!closing_ && epoch == epoch_ && !candidates.empty()) {
                    const auto best = std::find_if(candidates.begin(), candidates.end(),
                                                   [](const auto& line) { return line.rank == 1; });
                    state_.line = best == candidates.end() ? EngineLinePresentation{} : *best;
                    state_.candidates = std::move(candidates);
                    state_.statistics = stats;
                    state_.savedEvidence = saved;
                    ++state_.candidatesRevision;
                    updateControls();
                }
            },
            Qt::QueuedConnection);
    });
}
void EngineAnalysisController::showSavedReviewEvidence(const review::PositionEvidence& evidence) {
    if (closing_ || !request_.isNull() || !board_.position() || board_.busy() ||
        receivedLiveEvidence_ || !evidence.complete || evidence.restricted ||
        evidence.candidates.empty()) {
        return;
    }
    engine::AnalysisSnapshot snapshot;
    auto provenance = std::make_shared<engine::AnalysisProvenance>();
    provenance->position = evidence.position;
    snapshot.provenance = std::move(provenance);
    const auto perspective = board_.board().sideToMove;
    for (const auto& candidate : evidence.candidates) {
        if (candidate.evaluation.perspective != perspective || !candidate.evaluation.score) {
            continue;
        }
        const auto score = *candidate.evaluation.score;
        const auto bound = score.bound == review::Bound::Exact   ? engine::ScoreBound::Exact
                           : score.bound == review::Bound::Lower ? engine::ScoreBound::Lower
                                                                 : engine::ScoreBound::Upper;
        engine::AnalysisLine line;
        line.rank = candidate.rank;
        line.score = engine::AnalysisScore{score.kind == review::ScoreKind::Mate
                                               ? engine::ScoreType::Mate
                                               : engine::ScoreType::Centipawns,
                                           score.value, bound};
        line.pv = candidate.continuation;
        line.statistics.depth = candidate.strength.depth;
        snapshot.lines.push_back(std::move(line));
    }
    snapshot.statistics.nodes = evidence.strength.nodes;
    snapshot.statistics.timeMs = evidence.strength.timeMs;
    root_ = board_.position();
    revision_ = board_.board().revision;
    ++epoch_;
    pending_ = std::move(snapshot);
    formatNext();
}
void EngineAnalysisController::clearEvidence() {
    receivedLiveEvidence_ = false;
    state_.line = {};
    state_.candidates.clear();
    state_.statistics = {};
    state_.savedEvidence = false;
    ++state_.candidatesRevision;
}
void EngineAnalysisController::previewLine(int rank) {
    if (!boardReady() || !root_ || revision_ != board_.board().revision) {
        return;
    }
    const auto candidate = std::find_if(state_.candidates.begin(), state_.candidates.end(),
                                        [rank](const auto& line) { return line.rank == rank; });
    if (candidate != state_.candidates.end() && !candidate->moves.empty()) {
        preview_ = *candidate;
        previewPly_ = 1;
        previewBoard_.reset();
        previewPending_ = true;
        ++previewEpoch_;
        preparePreview();
        updateControls();
    }
}
void EngineAnalysisController::stepPreview(int delta) {
    if (!preview_) {
        return;
    }
    const auto ply = std::clamp(previewPly_ + delta, 0, static_cast<int>(preview_->moves.size()));
    if (ply != previewPly_) {
        previewPly_ = ply;
        previewPending_ = true;
        ++previewEpoch_;
        preparePreview();
        updateControls();
    }
}
void EngineAnalysisController::clearPreview() {
    ++previewEpoch_;
    previewPending_ = false;
    preview_.reset();
    previewBoard_.reset();
    previewPly_ = 0;
}
void EngineAnalysisController::dismissPreview() {
    if (preview_) {
        clearPreview();
        updateControls();
    }
}
void EngineAnalysisController::addPreview() {
    if (!preview_ || !boardReady() || revision_ != board_.board().revision) {
        return;
    }
    auto moves = std::move(preview_->moves);
    clearPreview();
    board_.addEngineLine(std::move(moves), revision_);
}
void EngineAnalysisController::preparePreview() {
    if (closing_ || formattingBusy_ || !previewPending_ || !preview_ || !root_) {
        return;
    }
    formattingBusy_ = true;
    previewPending_ = false;
    formatting_.start(
        [this, root = root_, moves = preview_->moves, ply = previewPly_, epoch = previewEpoch_] {
            std::optional<BoardViewState> board;
            try {
                auto position = *root;
                std::optional<chess::Move> lastMove;
                int completed = 0;
                for (const auto& move : std::span(moves).first(static_cast<std::size_t>(ply))) {
                    if (std::holds_alternative<chess::Error>(position.play(move))) {
                        break;
                    }
                    lastMove = move;
                    ++completed;
                }
                if (completed == ply) {
                    board = makeAnalysisBoard(position, lastMove, false);
                }
            } catch (const std::exception&) {
                // Keep the editable board untouched if the preview could not be prepared.
            }
            QMetaObject::invokeMethod(
                this,
                [this, epoch, board = std::move(board)]() mutable {
                    formattingBusy_ = false;
                    QTimer::singleShot(0, this, [this] {
                        preparePreview();
                        formatNext();
                        finishClose();
                    });
                    if (!closing_ && epoch == previewEpoch_) {
                        if (board) {
                            board->blackAtBottom = board_.board().blackAtBottom;
                            previewBoard_ = std::move(board);
                        } else {
                            clearPreview();
                        }
                        updateControls();
                    }
                },
                Qt::QueuedConnection);
        });
}
void EngineAnalysisController::close() {
    if (closing_) {
        return;
    }
    closing_ = true;
    clearPreview();
    ++epoch_;
    pending_.reset();
    QTimer::singleShot(0, this, &EngineAnalysisController::finishClose);
    stop(); // Last: changed() can run a nested event loop and deliver closed().
}
void EngineAnalysisController::finishClose() {
    if (!closing_ || closed_ || !request_.isNull() || formattingBusy_) {
        return;
    }
    if (!formatting_.waitForDone(0)) {
        QTimer::singleShot(5, this, &EngineAnalysisController::finishClose);
        return;
    }
    closed_ = true;
    emit closed();
}
} // namespace chess_review::app
