#include "app/StartupCoordinator.h"
#include "diagnostics/Logger.h"
#include <QCoreApplication>
#include <QEvent>

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
    connect(&analysis_, &AnalysisController::closed, this, [this] {
        analysisClosed_ = true;
        finishServices();
    });
    QCoreApplication::instance()->installEventFilter(this);
    connect(&localImports_, &ImportController::closed, &database_, &persistence::Database::close);
    connect(&localImports_, &ImportController::committed, &library_,
            &GameLibraryController::importedGame);
    connect(&localImports_, &ImportController::finished, &library_,
            [this] { library_.refresh(true); });
    connect(&database_, &persistence::Database::progress, this, &StartupCoordinator::progress);
    connect(&database_, &persistence::Database::ready, this, [this] {
        state_ = Lifecycle::Ready;
        localImports_.setReady(true);
        library_.setReady(true);
        emit ready();
    });
    connect(&database_, &persistence::Database::failed, this,
            [this](const persistence::DatabaseError& error) {
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
void StartupCoordinator::finishServices() {
    if (!analysisClosed_ || !storageClosed_ || !preferencesClosed_ || closingLogger_) {
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
                               const std::optional<QString>& uiStateFilename) {
    if (started_ || state_ == Lifecycle::Stopping || state_ == Lifecycle::Stopped) {
        return;
    }
    started_ = true;
    if (!uiStateFilename) {
        emit initialUiState({});
        if (state_ != Lifecycle::Stopping) {
            database_.initialize(databasePath);
        }
        return;
    }
    connect(&uiState_, &UiStateStore::loaded, this, [this, databasePath](const UiState& state) {
        emit initialUiState(state);
        if (state_ != Lifecycle::Stopping) {
            database_.initialize(databasePath);
        }
    });
    uiState_.load(*uiStateFilename);
}
void StartupCoordinator::setFinalUiState(const UiState& state) { finalUiState_ = state; }
Lifecycle StartupCoordinator::state() const { return state_; }
void StartupCoordinator::shutdown() {
    if (state_ == Lifecycle::Stopping || state_ == Lifecycle::Stopped) {
        return;
    }
    state_ = Lifecycle::Stopping;
    emit stopping();
    analysis_.close();
    library_.setReady(false);
    localImports_.close();
    uiState_.saveAndClose(finalUiState_);
}
} // namespace chess_review::app
