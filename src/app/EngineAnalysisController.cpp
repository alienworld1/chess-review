#include "app/EngineAnalysisController.h"
#include <QMetaObject>
#include <QStringList>
#include <QTimer>
#include <algorithm>
#include <exception>
#include <utility>

namespace chess_review::app {
EngineLinePresentation presentEngineLine(const chess::Position& root,
                                         const engine::AnalysisLine& line) {
    EngineLinePresentation result;
    result.depth = line.statistics.depth;
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
                    state_.status = tr("Analyzing");
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
                request_ = {};
                admitted_ = false;
                attempt_.reset();
                cancelling_ = false;
                if (result.outcome == engine::ScheduledOutcome::Failed ||
                    result.outcome == engine::ScheduledOutcome::Rejected) {
                    following_ = false;
                    state_.status = tr("Failed");
                    state_.error = result.error ? result.error->message : tr("Analysis failed.");
                } else if (result.outcome == engine::ScheduledOutcome::Completed &&
                           revision_ != 0 && revision_ == board_.board().revision) {
                    following_ = false;
                    state_.status = tr("Completed");
                } else if (!following_) {
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
void EngineAnalysisController::updateControls() {
    state_.canStart = !closing_ && visible_ && !following_ && request_.isNull() && !board_.busy() &&
                      board_.canNavigate() && board_.board().interactive && board_.position() &&
                      settings_.state().runtime.phase == engine::RuntimePhase::Ready;
    state_.canStop = !closing_ && (following_ || !request_.isNull());
    emit changed();
}
void EngineAnalysisController::runtimeChanged() {
    const auto& settings = settings_.state();
    state_.engineName = settings.connectedDisplayName;
    if (settings.runtime.phase != engine::RuntimePhase::Ready) {
        following_ = false;
        cancel();
        switch (settings.runtime.phase) {
        case engine::RuntimePhase::Disconnected:
            state_.status = tr("No engine connected");
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
        state_.line = {};
        state_.status = tr("Ready");
        state_.error.clear();
    }
    updateControls();
}
void EngineAnalysisController::boardChanged() {
    if (!board_.canNavigate()) {
        following_ = false;
    }
    if (!board_.canNavigate() || !board_.board().interactive || board_.busy() ||
        revision_ != board_.board().revision) {
        if (revision_ != 0) {
            ++epoch_;
            revision_ = 0;
            pending_.reset();
            state_.line = {};
        }
        cancel();
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
    if (!closing_ && following_ && visible_ && request_.isNull() && !board_.busy() &&
        board_.canNavigate() && board_.board().interactive && board_.position() &&
        settings_.state().runtime.phase == engine::RuntimePhase::Ready) {
        root_ = board_.position();
        revision_ = board_.board().revision;
        ++epoch_;
        pending_.reset();
        state_.line = {};
        state_.error.clear();
        session_ = settings_.state().runtime.session;
        request_ = QUuid::createUuid();
        admitted_ = false;
        engine::AnalysisRequest request;
        request.id = request_;
        request.position = *root_;
        request.limit = engine::InfiniteLimit{};
        state_.status = tr("Queued");
        supervisor_.scheduleAnalysis(session_, request,
                                     engine::AnalysisPriority::InteractiveAnalysis);
    }
    updateControls();
}
void EngineAnalysisController::start() {
    if (state_.canStart) {
        following_ = true;
        reconcile();
    }
}
void EngineAnalysisController::stop() {
    following_ = false;
    cancel();
    if (request_.isNull() && state_.canStop) {
        state_.status = tr("Stopped");
    }
    updateControls();
}
void EngineAnalysisController::setVisible(bool visible) {
    visible_ = visible;
    if (!visible) {
        stop();
        return;
    }
    updateControls();
}
void EngineAnalysisController::acceptEvidence(const engine::ScheduledEvidence& evidence) {
    if (closing_ || request_.isNull() || !attempt_ || evidence.identity.session != session_ ||
        evidence.identity.request != request_ || evidence.identity.attempt != attempt_ ||
        evidence.evidence.session != session_ || evidence.evidence.request != *attempt_ ||
        revision_ == 0 || revision_ != board_.board().revision || board_.busy()) {
        return;
    }
    pending_ = evidence.evidence;
    formatNext();
}
void EngineAnalysisController::formatNext() {
    if (closing_ || formattingBusy_ || !pending_) {
        return;
    }
    auto snapshot = std::move(*pending_);
    pending_.reset();
    const auto found = std::find_if(snapshot.lines.begin(), snapshot.lines.end(),
                                    [](const auto& line) { return line.rank == 1; });
    if (found == snapshot.lines.end() || !root_) {
        return;
    }
    formattingBusy_ = true;
    const auto epoch = epoch_;
    formatting_.start([this, root = root_, line = *found, epoch] {
        EngineLinePresentation presentation;
        try {
            presentation = presentEngineLine(*root, line);
        } catch (const std::exception&) {
            // Presentation failure cannot turn partial replay into chess evidence.
        }
        QMetaObject::invokeMethod(
            this,
            [this, epoch, presentation = std::move(presentation)] {
                formattingBusy_ = false;
                QTimer::singleShot(0, this, [this] {
                    formatNext();
                    finishClose();
                });
                if (!closing_ && epoch == epoch_) {
                    state_.line = presentation;
                    emit changed();
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
