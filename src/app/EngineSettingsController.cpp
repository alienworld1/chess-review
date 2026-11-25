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
                if (state.phase == engine::RuntimePhase::Ready) {
                    state_.missingProfiles.erase(state.profileId);
                } else if (state.phase == engine::RuntimePhase::Failed && state.error &&
                           state.error->code == engine::ErrorCode::Path) {
                    state_.missingProfiles.insert(state.profileId);
                }
                if (!engine::runtimeActive(state.phase)) {
                    analysisAttempts_.clear();
                    state_.analyzing = false;
                }
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
                    pendingRelocationId_.reset();
                    state_.relocatingProfileId.reset();
                    state_.registration = EngineRegistrationState::Idle;
                    state_.message = error->message;
                } else {
                    state_.selectedPath = std::get<QString>(result);
                    state_.registration = EngineRegistrationState::Selected;
                    state_.relocatingProfileId = std::exchange(pendingRelocationId_, std::nullopt);
                    state_.message =
                        state_.relocatingProfileId
                            ? tr("Validation will run software from this path on your computer "
                                 "to verify it matches the missing engine.")
                            : QStringLiteral("Validation will run software from this path on your "
                                             "computer. Only add an executable you trust.");
                }
                emit changed();
            });
    connect(&supervisor_, &engine::EngineSupervisor::enginesDetected, this,
            [this](QUuid request, const QList<engine::DetectedEngine>& candidates) {
                if (closing_ || request != detection_) {
                    return;
                }
                state_.detecting = false;
                QList<engine::DetectedEngine> available;
                for (const auto& candidate : candidates) {
                    const bool registered = std::ranges::any_of(
                        state_.profiles, [&candidate](const engine::EngineProfile& profile) {
                            return profile.engine.executablePath == candidate.path;
                        });
                    if (!registered) {
                        available.push_back(candidate);
                    }
                }
                state_.detectedEngines = std::move(available);
                if (candidates.isEmpty()) {
                    state_.detectMessage =
                        tr("No supported UCI engines were found in standard folders. Use Choose "
                           "executable… to select an engine manually.");
                } else if (state_.detectedEngines.isEmpty()) {
                    state_.detectMessage =
                        tr("All detected UCI engines in standard folders are already registered.");
                } else {
                    state_.detectMessage =
                        tr("Found %n available engine(s). Select one to inspect before adding.",
                           nullptr, static_cast<int>(state_.detectedEngines.size()));
                }
                emit changed();
            });
    connect(&supervisor_, &engine::EngineSupervisor::executablesChecked, this,
            [this](QUuid request, const QList<std::int64_t>& missingIds) {
                if (closing_ || request != executableCheck_) {
                    return;
                }
                std::set<std::int64_t> missing;
                for (const auto id : missingIds) {
                    if (std::ranges::any_of(state_.profiles, [id](const auto& profile) {
                            return profile.id == id;
                        })) {
                        missing.insert(id);
                    }
                }
                if (state_.missingProfiles != missing) {
                    state_.missingProfiles = std::move(missing);
                    emit changed();
                }
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
            } else if (state_.relocatingProfileId) {
                saving_ = true;
                state_.editing = true;
                edit_ = registration_;
                state_.registration = EngineRegistrationState::Saving;
                state_.message = tr("Updating engine location…");
                database_.editEngine(
                    edit_, *state_.relocatingProfileId,
                    persistence::RelocateEngine{std::get<engine::ValidatedEngine>(result)});
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
                    if (requestedOffset_ == 0) {
                        // Registration assigns the first profile as the default.
                        const auto preferred = std::ranges::find_if(
                            profiles, [](const auto& profile) { return profile.isDefault; });
                        state_.defaultProfileId = preferred == profiles.end()
                                                      ? std::nullopt
                                                      : std::optional{preferred->id};
                        state_.defaultDisplayName =
                            preferred == profiles.end() ? QString{} : preferred->displayName;
                    }
                    state_.profiles = std::move(profiles);
                    ++state_.profilesRevision;
                    state_.offset = requestedOffset_;
                    state_.listError.clear();
                    state_.detectedEngines.removeIf([this](const engine::DetectedEngine& item) {
                        return std::ranges::any_of(state_.profiles, [&item](const auto& profile) {
                            return profile.engine.executablePath == item.path;
                        });
                    });
                    std::erase_if(state_.missingProfiles, [this](std::int64_t id) {
                        return std::ranges::none_of(state_.profiles, [id](const auto& profile) {
                            return profile.id == id;
                        });
                    });
                    QList<std::pair<std::int64_t, QString>> checkList;
                    checkList.reserve(static_cast<qsizetype>(state_.profiles.size()));
                    for (const auto& profile : state_.profiles) {
                        checkList.push_back({profile.id, profile.engine.executablePath});
                    }
                    if (checkList.isEmpty()) {
                        executableCheck_ = {};
                        state_.missingProfiles.clear();
                    } else {
                        executableCheck_ = QUuid::createUuid();
                        supervisor_.checkExecutables(executableCheck_, checkList);
                    }
                }
                emit changed();
            });
    connect(&database_, &persistence::Database::engineEdited, this,
            [this](QUuid request, const persistence::EditEngineResult& result) {
                if (!state_.editing || request != edit_) {
                    return;
                }
                state_.editing = false;
                if (saving_ && state_.relocatingProfileId) {
                    saving_ = false;
                    const auto relocatedId =
                        std::exchange(state_.relocatingProfileId, std::nullopt);
                    if (!result) {
                        state_.missingProfiles.erase(*relocatedId);
                        state_.profileMessage.clear();
                        state_.message = tr("Missing engine located and verified.");
                        if (!closing_) {
                            state_.registration = EngineRegistrationState::Idle;
                            state_.selectedPath.clear();
                            refresh();
                        }
                    } else {
                        state_.message = result->message;
                        if (!closing_) {
                            state_.registration = EngineRegistrationState::Selected;
                        }
                    }
                    emit changed();
                    finishClose();
                    return;
                }
                state_.profileMessage = result ? result->message : tr("Engine profile updated.");
                if (!result && !closing_) {
                    refresh();
                } else {
                    emit changed();
                }
                finishClose();
            });
    connect(&supervisor_, &engine::EngineSupervisor::searchChanged, this,
            [this](const engine::SearchState& search) {
                if (!closing_ && search.session == state_.runtime.session) {
                    analysisAttempts_.insert(search.request);
                    if (!state_.analyzing) {
                        state_.analyzing = true;
                        emit activityChanged();
                    }
                }
            });
    connect(&supervisor_, &engine::EngineSupervisor::scheduledChanged, this,
            [this](const engine::ScheduledState& search) {
                if (!closing_ && search.session == state_.runtime.session && search.attempt) {
                    analysisAttempts_.insert(*search.attempt);
                    if (!state_.analyzing) {
                        state_.analyzing = true;
                        emit activityChanged();
                    }
                }
            });
    const auto finishScheduled = [this](const engine::ScheduledState& search) {
        if (!closing_ && search.session == state_.runtime.session && search.attempt &&
            analysisAttempts_.erase(*search.attempt) != 0 && analysisAttempts_.empty()) {
            state_.analyzing = false;
            emit activityChanged();
        }
    };
    connect(&supervisor_, &engine::EngineSupervisor::scheduledInterrupted, this,
            [finishScheduled](const engine::ScheduledEvidence& evidence) {
                finishScheduled(evidence.identity);
            });
    connect(&supervisor_, &engine::EngineSupervisor::scheduledFinished, this,
            [finishScheduled](const engine::ScheduledResult& result) {
                finishScheduled(result.identity);
            });
    connect(&supervisor_, &engine::EngineSupervisor::analysisFinished, this,
            [this](const engine::AnalysisResult& result) {
                if (closing_ || result.evidence.session != state_.runtime.session) {
                    return;
                }
                const auto request = result.evidence.request;
                const bool wasAnalyzing = result.outcome != engine::AnalysisOutcome::Rejected &&
                                          analysisAttempts_.erase(request) != 0 &&
                                          analysisAttempts_.empty();
                if (wasAnalyzing) {
                    state_.analyzing = false;
                }
                if (request == test_) {
                    test_ = {};
                    state_.testing = false;
                    if (result.outcome == engine::AnalysisOutcome::Completed &&
                        result.evidence.bestMove) {
                        state_.testMessage = tr("Test passed. The engine found a legal move.");
                    } else {
                        state_.testMessage = result.error
                                                 ? result.error->message
                                                 : tr("Test ended without a legal best move.");
                    }
                    emit activityChanged();
                } else if (wasAnalyzing) {
                    emit activityChanged();
                }
            });
    connect(&supervisor_, &engine::EngineSupervisor::closed, this, [this] {
        supervisorClosed_ = true;
        finishClose();
    });
}
void EngineSettingsController::setReady(bool ready) {
    if (closing_ || state_.ready == ready) {
        return;
    }
    state_.ready = ready;
    if (ready) {
        refresh();
    }
    emit changed();
}
void EngineSettingsController::selectExecutable(const QString& path) {
    if (closing_ || !state_.ready || saving_ || state_.editing ||
        engine::runtimeActive(state_.runtime.phase) ||
        state_.registration == EngineRegistrationState::Validating) {
        return;
    }
    pendingRelocationId_.reset();
    state_.relocatingProfileId.reset();
    selection_ = QUuid::createUuid();
    state_.selectedPath.clear();
    state_.registration = EngineRegistrationState::Inspecting;
    state_.message = QStringLiteral("Checking executable path…");
    supervisor_.inspect(selection_, path);
    emit changed();
}
void EngineSettingsController::relocateProfile(std::int64_t profileId, const QString& path) {
    if (closing_ || !state_.ready || saving_ || state_.editing ||
        engine::runtimeActive(state_.runtime.phase) ||
        state_.registration == EngineRegistrationState::Validating ||
        std::ranges::none_of(state_.profiles,
                             [profileId](const auto& p) { return p.id == profileId; })) {
        return;
    }
    pendingRelocationId_ = profileId;
    state_.relocatingProfileId.reset();
    selection_ = QUuid::createUuid();
    state_.selectedPath.clear();
    state_.registration = EngineRegistrationState::Inspecting;
    state_.message = tr("Checking replacement executable path…");
    supervisor_.inspect(selection_, path);
    emit changed();
}
void EngineSettingsController::detectInstalledEngines() {
    if (closing_ || !state_.ready || state_.detecting) {
        return;
    }
    detection_ = QUuid::createUuid();
    state_.detecting = true;
    state_.detectMessage = tr("Searching standard folders for UCI engines…");
    emit changed();
    supervisor_.detectEngines(detection_);
}
void EngineSettingsController::registerSelected() {
    if (closing_ || !state_.ready || state_.editing ||
        engine::runtimeActive(state_.runtime.phase) ||
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
        pendingRelocationId_.reset();
        state_.relocatingProfileId.reset();
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
bool EngineSettingsController::connectEngine(std::int64_t profileId) {
    if (closing_ || !state_.ready || saving_ || state_.editing || state_.listing ||
        engine::runtimeActive(state_.runtime.phase) ||
        state_.registration == EngineRegistrationState::Inspecting ||
        state_.registration == EngineRegistrationState::Validating) {
        return false;
    }
    const auto found = std::find_if(
        state_.profiles.begin(), state_.profiles.end(),
        [profileId](const engine::EngineProfile& profile) { return profile.id == profileId; });
    if (found == state_.profiles.end()) {
        return false;
    }
    state_.testMessage.clear();
    state_.runtime = {
        QUuid::createUuid(), profileId, engine::RuntimePhase::CheckingExecutable, {}, {}};
    state_.connectedDisplayName = found->displayName;
    supervisor_.connectEngine(state_.runtime.session, *found, poolLimits_);
    emit changed();
    return true;
}
bool EngineSettingsController::canEditProfile() const {
    return !closing_ && state_.ready && !state_.listing && !state_.editing &&
           !engine::runtimeActive(state_.runtime.phase) &&
           (state_.registration == EngineRegistrationState::Idle ||
            state_.registration == EngineRegistrationState::Selected);
}
void EngineSettingsController::editProfile(std::int64_t profileId,
                                           persistence::EngineProfileEdit edit) {
    if (!canEditProfile() ||
        std::ranges::none_of(state_.profiles, [profileId](const auto& profile) {
            return profile.id == profileId;
        })) {
        return;
    }
    state_.editing = true;
    state_.profileMessage = tr("Saving engine profile…");
    edit_ = QUuid::createUuid();
    database_.editEngine(edit_, profileId, std::move(edit));
    emit changed();
}
void EngineSettingsController::testEngine(std::int64_t profileId) {
    if (closing_ || state_.runtime.phase != engine::RuntimePhase::Ready ||
        state_.runtime.profileId != profileId || state_.testing || state_.analyzing) {
        return;
    }
    engine::AnalysisRequest request;
    request.id = QUuid::createUuid();
    request.limit = engine::DepthLimit{4};
    test_ = request.id;
    state_.testing = true;
    state_.testMessage = tr("Testing the engine on the starting position…");
    supervisor_.analyze(state_.runtime.session, request);
    emit activityChanged();
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
    if (!closing_ || !supervisorClosed_ || saving_ || state_.editing || closed_) {
        return;
    }
    closed_ = true;
    emit closed();
}
} // namespace chess_review::app
