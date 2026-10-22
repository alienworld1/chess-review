#pragma once

namespace chess_review::engine {
// Internal phases: never persisted or exposed as public runtime diagnostic codes.
enum class ProcessStage {
    Created,
    CheckingExecutable,
    Starting,
    AwaitingUciOk,
    AwaitingReadyOk,
    CheckingIntegrity,
    Ready,
    ConfiguringSearch,
    Analyzing,
    Stopping,
    SynchronizingSearch,
    Quitting,
    Terminating,
    Killing,
    Finished
};

constexpr bool canTransition(ProcessStage current, ProcessStage next) {
    using enum ProcessStage;
    if (current == Finished) {
        return false;
    }
    // Completion requires separate process-exit/drain checks in the worker.
    if (next == Finished) {
        return true;
    }
    if (next == Quitting) {
        return current == Starting || current == AwaitingUciOk || current == AwaitingReadyOk ||
               current == CheckingIntegrity || current == Ready || current == ConfiguringSearch ||
               current == Analyzing || current == Stopping || current == SynchronizingSearch;
    }
    switch (current) {
    case Created:
        return next == CheckingExecutable;
    case CheckingExecutable:
        return next == Starting;
    case Starting:
        return next == AwaitingUciOk;
    case AwaitingUciOk:
        return next == AwaitingReadyOk;
    case AwaitingReadyOk:
        return next == CheckingIntegrity;
    case CheckingIntegrity:
        return next == Ready;
    case Ready:
        return next == ConfiguringSearch;
    case ConfiguringSearch:
        return next == Analyzing || next == SynchronizingSearch;
    case Analyzing:
        return next == Stopping || next == SynchronizingSearch;
    case Stopping:
        return next == SynchronizingSearch;
    case SynchronizingSearch:
        return next == Ready;
    case Quitting:
        return next == Terminating || next == CheckingIntegrity;
    case Terminating:
        return next == Killing;
    default:
        return false;
    }
}
} // namespace chess_review::engine
