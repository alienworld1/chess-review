#include "app/EngineSettingsController.h"
#include "diagnostics/Logger.h"

#include <algorithm>
#include <utility>

namespace chess_review::app {
namespace {
bool acceptsRuntimePhase(engine::RuntimePhase current, engine::RuntimePhase next) {
    using engine::RuntimePhase;
    if (next == RuntimePhase::Failed || next == RuntimePhase::Disconnected ||
        next == RuntimePhase::Disconnecting) {
        return engine::runtimeActive(current);
    }
    switch (current) {
    case RuntimePhase::CheckingExecutable:
        return next == RuntimePhase::CheckingExecutable || next == RuntimePhase::Starting;
    case RuntimePhase::Starting:
        return next == RuntimePhase::Handshaking;
    case RuntimePhase::Handshaking:
        return next == RuntimePhase::Ready;
    default:
        return false;
    }
}
} // namespace
EngineSettingsController::EngineSettingsController(persistence::Database& database,
                                                   engine::EngineSupervisor& supervisor,
                                                   diagnostics::Logger* logger, QObject* parent)
    : QObject(parent), database_(database), logger_(logger), supervisor_(supervisor) {
    connect(&supervisor_, &engine::EngineSupervisor::runtimeChanged, this,
            [this](const engine::RuntimeState& state) {
                if (state.session != state_.runtime.session ||
                    state.profileId != state_.runtime.profileId ||
                    !acceptsRuntimePhase(state_.runtime.phase, state.phase)) {
                    return;
                }
                state_.runtime = state;
                if (logger_) {
                    diagnostics::Context context;
                    context.count = static_cast<quint64>(state.phase);
                    if (state.error) {
                        context.errorCode = static_cast<int>(state.error->code);
                    }
                    logger_->record(diagnostics::Event::EngineRuntimeChanged, context);
                }
                emit changed();
            });
    connect(&supervisor_, &engine::EngineSupervisor::inspected, this,
            [this](QUuid request, const engine::PathResult& result) {
                if (closing_ || request != selection_ ||
                    state_.registration != EngineRegistrationState::Inspecting) {
                    return;
                }
                if (const auto* error = std::get_if<engine::Error>(&result)) {
                    state_.registration = EngineRegistrationState::Idle;
                    state_.message = error->message;
                } else {
                    state_.selectedPath = std::get<QString>(result);
                    state_.registration = EngineRegistrationState::Selected;
                    state_.message =
                        QStringLiteral("Validation will run software from this path on your "
                                       "computer. Only add an executable you trust.");
                }
                emit changed();
            });
    connect(&supervisor_, &engine::EngineSupervisor::progress, this,
            [this](QUuid request, const QString& message) {
                if (!closing_ && !cancelRequested_ && request == registration_ &&
                    state_.registration == EngineRegistrationState::Validating) {
                    state_.message = message;
                    emit changed();
                }
            });
    connect(
        &supervisor_, &engine::EngineSupervisor::completed, this,
        [this](QUuid request, const engine::ValidationResult& result) {
            if (request != registration_ ||
                state_.registration != EngineRegistrationState::Validating) {
                return;
            }
            if (logger_) {
                diagnostics::Context context;
                if (const auto* error = std::get_if<engine::Error>(&result)) {
                    context.errorCode = static_cast<int>(error->code);
                }
                logger_->record(context.errorCode ? diagnostics::Event::EngineValidationFailed
                                                  : diagnostics::Event::EngineValidationSucceeded,
                                context);
            }
            if (closing_) {
                return;
            }
            if (cancelRequested_) {
                state_.registration = EngineRegistrationState::Selected;
                state_.message = QStringLiteral("Engine registration cancelled.");
            } else if (const auto* error = std::get_if<engine::Error>(&result)) {
                state_.registration = EngineRegistrationState::Selected;
                state_.message = error->message;
            } else {
                saving_ = true;
                state_.registration = EngineRegistrationState::Saving;
                state_.message = QStringLiteral("Saving validated engine…");
                database_.registerEngine(registration_, std::get<engine::ValidatedEngine>(result));
            }
            emit changed();
        });
    connect(&database_, &persistence::Database::engineRegistered, this,
            [this](QUuid request, const persistence::RegisterEngineResult& result) {
                if (!saving_ || request != registration_) {
                    return;
                }
                saving_ = false;
                if (logger_) {
                    diagnostics::Context context;
                    if (const auto* error = std::get_if<engine::Error>(&result)) {
                        context.errorCode = static_cast<int>(error->code);
                    } else {
                        context.count =
                            std::get<persistence::RegisteredEngine>(result).duplicate ? 0 : 1;
                    }
                    logger_->record(diagnostics::Event::EngineRegistrationFinished, context);
                }
                if (const auto* error = std::get_if<engine::Error>(&result)) {
                    state_.message = error->message;
                    if (!closing_) {
                        state_.registration = EngineRegistrationState::Selected;
                    }
                } else {
                    const auto& registered = std::get<persistence::RegisteredEngine>(result);
                    state_.message =
                        registered.duplicate
                            ? QStringLiteral("This binary is already registered. Its saved path "
                                             "and validation time were kept.")
                            : QStringLiteral("Engine registered successfully.");
                    if (!closing_) {
                        state_.registration = EngineRegistrationState::Idle;
                        state_.selectedPath.clear();
                        refresh();
                    }
                }
                emit changed();
                finishClose();
            });
    connect(&database_, &persistence::Database::enginesListed, this,
            [this](QUuid request, const persistence::ListEnginesResult& result) {
                if (closing_ || request != listing_) {
                    return;
                }
                state_.listing = false;
                if (const auto* error = std::get_if<engine::Error>(&result)) {
                    state_.listError = error->message;
                } else {
                    auto profiles = std::get<std::vector<engine::EngineProfile>>(result);
                    state_.hasNext = profiles.size() > 50;
                    if (state_.hasNext) {
                        profiles.pop_back();
                    }
                    state_.profiles = std::move(profiles);
                    ++state_.profilesRevision;
                    state_.offset = requestedOffset_;
                    state_.listError.clear();
                }
                emit changed();
            });
    connect(&supervisor_, &engine::EngineSupervisor::closed, this, [this] {
        supervisorClosed_ = true;
        finishClose();
    });
}
void EngineSettingsController::setReady(bool ready) {
    if (closing_) {
        return;
    }
    state_.ready = ready;
    if (ready) {
        refresh();
    }
    emit changed();
}
void EngineSettingsController::selectExecutable(const QString& path) {
    if (closing_ || !state_.ready || saving_ || engine::runtimeActive(state_.runtime.phase) ||
        state_.registration == EngineRegistrationState::Validating) {
        return;
    }
    selection_ = QUuid::createUuid();
    state_.selectedPath.clear();
    state_.registration = EngineRegistrationState::Inspecting;
    state_.message = QStringLiteral("Checking executable path…");
    supervisor_.inspect(selection_, path);
    emit changed();
}
void EngineSettingsController::registerSelected() {
    if (closing_ || !state_.ready || engine::runtimeActive(state_.runtime.phase) ||
        state_.registration != EngineRegistrationState::Selected) {
        return;
    }
    if (logger_) {
        logger_->record(diagnostics::Event::EngineValidationStarted);
    }
    cancelRequested_ = false;
    registration_ = QUuid::createUuid();
    state_.registration = EngineRegistrationState::Validating;
    state_.message = QStringLiteral("Preparing executable validation…");
    supervisor_.validate(registration_, state_.selectedPath);
    emit changed();
}
void EngineSettingsController::cancel() {
    if (closing_ || saving_) {
        return;
    }
    if (state_.registration == EngineRegistrationState::Validating) {
        cancelRequested_ = true;
        state_.message = QStringLiteral("Cancelling and closing engine…");
        supervisor_.cancel();
    } else {
        selection_ = {};
        state_.selectedPath.clear();
        state_.registration = EngineRegistrationState::Idle;
        state_.message.clear();
    }
    emit changed();
}
void EngineSettingsController::refresh(std::int64_t offset) {
    if (closing_ || !state_.ready || offset < 0) {
        return;
    }
    listing_ = QUuid::createUuid();
    requestedOffset_ = offset;
    state_.listing = true;
    database_.listEngines(listing_, offset, 51);
    emit changed();
}
void EngineSettingsController::connectEngine(std::int64_t profileId) {
    if (closing_ || !state_.ready || saving_ || engine::runtimeActive(state_.runtime.phase) ||
        state_.registration == EngineRegistrationState::Inspecting ||
        state_.registration == EngineRegistrationState::Validating) {
        return;
    }
    const auto found = std::find_if(
        state_.profiles.begin(), state_.profiles.end(),
        [profileId](const engine::EngineProfile& profile) { return profile.id == profileId; });
    if (found == state_.profiles.end()) {
        return;
    }
    state_.runtime = {
        QUuid::createUuid(), profileId, engine::RuntimePhase::CheckingExecutable, {}, {}};
    state_.connectedDisplayName = found->displayName;
    supervisor_.connectEngine(state_.runtime.session, *found);
    emit changed();
}
void EngineSettingsController::disconnectEngine() {
    if (closing_ || !engine::runtimeActive(state_.runtime.phase) ||
        state_.runtime.phase == engine::RuntimePhase::Disconnecting) {
        return;
    }
    state_.runtime.phase = engine::RuntimePhase::Disconnecting;
    state_.runtime.discovery.reset();
    supervisor_.disconnectEngine(state_.runtime.session);
    emit changed();
}
void EngineSettingsController::close() {
    if (closing_) {
        return;
    }
    closing_ = true;
    state_.ready = false;
    state_.registration = EngineRegistrationState::Closing;
    if (engine::runtimeActive(state_.runtime.phase)) {
        state_.runtime.phase = engine::RuntimePhase::Disconnecting;
        state_.runtime.discovery.reset();
    }
    emit changed();
}
void EngineSettingsController::finishClose() {
    if (!closing_ || !supervisorClosed_ || saving_ || closed_) {
        return;
    }
    closed_ = true;
    emit closed();
}
} // namespace chess_review::app
