#include "app/StartupCoordinator.h"
#include "diagnostics/Logger.h"
#include <QCoreApplication>
#include <QEvent>
#include <utility>

namespace chess_review::app {
StartupCoordinator::StartupCoordinator(QObject* parent) : QObject(parent) { connectServices(); }
StartupCoordinator::StartupCoordinator(diagnostics::Logger& logger, QObject* parent)
    : QObject(parent), logger_(&logger), database_(logger) {
    connectServices();
    connect(&logger, &diagnostics::Logger::closed, this, [this] {
        state_ = Lifecycle::Stopped;
        emit finished();
    });
}
void StartupCoordinator::connectServices() {
    connect(&configuration_, &ReviewConfiguration::changed, this, [this] {
        if (state_ == Lifecycle::Ready) {
            engines_.setReady(true);
        }
    });
    connect(&configuration_, &ReviewConfiguration::closed, this, [this] {
        configurationClosed_ = true;
        finishServices();
    });
    connect(&engineAnalysis_, &EngineAnalysisController::closed, this, [this] {
        liveClosed_ = true;
        closeSupervisorWhenProducersFinish();
    });
    connect(&reviewGame_, &ReviewGameController::closed, this, [this] {
        reviewClosed_ = true;
        closeSupervisorWhenProducersFinish();
        closeStorageWhenProducersFinish();
    });
    connect(&library_, &GameLibraryController::opened, &analysis_, &AnalysisController::openGame);
    connect(&library_, &GameLibraryController::openFailed, this, [this] { resume_.reset(); });
    connect(&analysis_, &AnalysisController::gameOpened, this, [this] {
        const auto resume = std::exchange(resume_, std::nullopt);
        if (resume && analysis_.gameId() == resume->id) {
            analysis_.navigate(chess::NodeId{resume->node});
        }
    });
    connect(&analysis_, &AnalysisController::closed, this, [this] {
        analysisClosed_ = true;
        finishServices();
    });
    QCoreApplication::instance()->installEventFilter(this);
    connect(&localImports_, &ImportController::closed, this, [this] {
        importsClosed_ = true;
        closeStorageWhenProducersFinish();
    });
    connect(&engines_, &EngineSettingsController::closed, this, [this] {
        enginesClosed_ = true;
        closeStorageWhenProducersFinish();
    });
    connect(&localImports_, &ImportController::committed, &library_,
            &GameLibraryController::importedGame);
    connect(&localImports_, &ImportController::finished, &library_,
            [this] { library_.refresh(true); });
    connect(&database_, &persistence::Database::progress, this, &StartupCoordinator::progress);
    connect(&database_, &persistence::Database::ready, this, [this] {
        if (state_ == Lifecycle::Stopping || state_ == Lifecycle::Stopped) {
            return;
        }
        state_ = Lifecycle::Ready;
        localImports_.setReady(true);
        engines_.setReady(configuration_.state() != ConfigurationState::Loading);
        library_.setReady(true);
        reviewGame_.setReady(true);
        // A user may start exploring while storage is still initializing. Do not
        // replace that work when a delayed automatic recovery becomes available.
        if (analysis_.hasContent() || (analysis_.busy() && !analysis_.history().empty())) {
            resume_.reset();
        }
        if (resume_) {
            library_.open(resume_->id);
        }
        emit ready();
    });
    connect(&database_, &persistence::Database::failed, this,
            [this](const persistence::DatabaseError& error) {
                if (state_ == Lifecycle::Stopping || state_ == Lifecycle::Stopped) {
                    return;
                }
                state_ = Lifecycle::Failed;
                emit failed(storageDiagnostic(error));
            });
    connect(&database_, &persistence::Database::closed, this, [this] {
        storageClosed_ = true;
        finishServices();
    });
    connect(&uiState_, &UiStateStore::closed, this, [this] {
        preferencesClosed_ = true;
        finishServices();
    });
    connect(&uiState_, &UiStateStore::failed, this, [this](UiStateError error) {
        if (logger_) {
            diagnostics::Context context;
            context.errorCode = static_cast<int>(error);
            logger_->record(diagnostics::Event::UiStateFailed, context,
                            diagnostics::Severity::Warning);
        }
        emit uiStateFailed(error);
    });
}
void StartupCoordinator::closeSupervisorWhenProducersFinish() {
    if (liveClosed_ && reviewClosed_) {
        engineSupervisor_.close();
    }
}
void StartupCoordinator::closeStorageWhenProducersFinish() {
    if (importsClosed_ && enginesClosed_ && reviewClosed_) {
        database_.close();
    }
}
void StartupCoordinator::finishServices() {
    if (!analysisClosed_ || !storageClosed_ || !preferencesClosed_ || !configurationClosed_ ||
        closingLogger_) {
        return;
    }
    closingLogger_ = true;
    if (logger_) {
        logger_->close();
    } else {
        state_ = Lifecycle::Stopped;
        emit finished();
    }
}
bool StartupCoordinator::eventFilter(QObject* watched, QEvent* event) {
    if (watched == QCoreApplication::instance() && event->type() == QEvent::Quit &&
        state_ != Lifecycle::Stopped) {
        if (confirmQuit_ && state_ != Lifecycle::Stopping) {
            emit quitRequested();
        } else {
            shutdown();
        }
        return true;
    }
    return QObject::eventFilter(watched, event);
}
void StartupCoordinator::start(const QString& databasePath,
                               const std::optional<QString>& uiStateFilename,
                               const std::optional<QString>& configFilename) {
    if (started_ || state_ == Lifecycle::Stopping || state_ == Lifecycle::Stopped) {
        return;
    }
    started_ = true;
    configuration_.load(configFilename);
    if (!uiStateFilename) {
        emit initialUiState({});
        if (state_ != Lifecycle::Stopping) {
            database_.initialize(databasePath);
        }
        return;
    }
    connect(&uiState_, &UiStateStore::loaded, this, [this, databasePath](const UiState& state) {
        resume_ = state.game;
        emit initialUiState(state);
        if (state_ != Lifecycle::Stopping) {
            database_.initialize(databasePath);
        }
    });
    uiState_.load(*uiStateFilename);
}
void StartupCoordinator::setFinalUiState(const UiState& state) {
    finalUiState_ = state;
    finalUiState_->game.reset();
    if (const auto id = analysis_.gameId()) {
        // Session-only branches are absent from the stored tree; reopening falls back
        // to its root when the remembered node does not exist.
        finalUiState_->game = GameResume{*id, analysis_.currentNode().value};
    }
}
Lifecycle StartupCoordinator::state() const { return state_; }
void StartupCoordinator::shutdown() {
    if (state_ == Lifecycle::Stopping || state_ == Lifecycle::Stopped) {
        return;
    }
    state_ = Lifecycle::Stopping;
    emit stopping();
    configuration_.close();
    analysis_.close();
    library_.setReady(false);
    engines_.close();
    engineAnalysis_.close();
    reviewGame_.close();
    localImports_.close();
    uiState_.saveAndClose(finalUiState_);
}
} // namespace chess_review::app
