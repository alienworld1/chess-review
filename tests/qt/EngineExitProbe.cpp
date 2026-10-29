#include "EngineControllerTestAccess.h"
#include "EngineTestSupport.h"
#include "ui/settings/EngineSettingsPage.h"
#include "ui/shell/MainWindow.h"
#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QPushButton>
#include <QTimer>
#include <cstdio>
namespace chess_review::ui {
class EnginePageTestAccess {
  public:
    static app::EngineSettingsController& controller(EngineSettingsPage& page) {
        return page.controller_;
    }
};
} // namespace chess_review::ui
namespace {
void installDriver() {
    QTimer::singleShot(0, QCoreApplication::instance(), [] {
        auto* poll = new QTimer(QCoreApplication::instance());
        QObject::connect(poll, &QTimer::timeout, poll, [poll] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* window = qobject_cast<chess_review::ui::MainWindow*>(widget);
                if (!window || !window->isVisible()) {
                    continue;
                }
                auto* page = window->findChild<chess_review::ui::EngineSettingsPage*>();
                if (!page) {
                    qFatal("Missing engine page");
                }
                auto& controller = chess_review::ui::EnginePageTestAccess::controller(*page);
                if (!controller.state().ready) {
                    continue;
                }
                poll->stop();
                const auto mode = qEnvironmentVariable("CHESS_REVIEW_ENGINE_PROBE_MODE");
                if (mode == QStringLiteral("reopen")) {
                    QObject::connect(&controller,
                                     &chess_review::app::EngineSettingsController::changed, window,
                                     [window, &controller] {
                                         if (controller.state().profiles.size() == 1) {
                                             std::fputs("engine-reopened\n", stdout);
                                             std::fflush(stdout);
                                             window->close();
                                         }
                                     });
                    controller.refresh();
                    return;
                }
                chess_review::engine::ValidatorTestAccess::configure(
                    chess_review::app::EngineControllerTestAccess::validator(controller),
                    qEnvironmentVariable("CHESS_REVIEW_ENGINE_PROBE_SCENARIO"), 10000);
                if (mode.endsWith(QStringLiteral("analyzing"))) {
                    auto& supervisor =
                        chess_review::app::EngineControllerTestAccess::validator(controller);
                    if (mode == QStringLiteral("live-analyzing")) {
                        QObject::connect(
                            &supervisor, &chess_review::engine::EngineSupervisor::scheduledChanged,
                            window, [window](const chess_review::engine::ScheduledState& state) {
                                if (state.phase ==
                                    chess_review::engine::ScheduledPhase::Analyzing) {
                                    std::fputs("engine-close-requested\n", stdout);
                                    std::fflush(stdout);
                                    window->close();
                                }
                            });
                    }
                    QObject::connect(
                        &supervisor, &chess_review::engine::EngineSupervisor::searchChanged, window,
                        [window](const chess_review::engine::SearchState& state) {
                            if (state.phase == chess_review::engine::SearchPhase::Analyzing) {
                                std::fputs("engine-close-requested\n", stdout);
                                std::fflush(stdout);
                                window->close();
                            }
                        });
                }
                QObject::connect(
                    &controller, &chess_review::app::EngineSettingsController::changed, window,
                    [window, &controller, mode] {
                        using State = chess_review::app::EngineRegistrationState;
                        const auto state = controller.state().registration;
                        if ((mode == QStringLiteral("connected") ||
                             mode.endsWith(QStringLiteral("analyzing"))) &&
                            controller.state().profiles.size() == 1 &&
                            controller.state().runtime.session.isNull()) {
                            if (mode.endsWith(QStringLiteral("analyzing"))) {
                                chess_review::engine::ValidatorTestAccess::configure(
                                    chess_review::app::EngineControllerTestAccess::validator(
                                        controller),
                                    qEnvironmentVariable(
                                        "CHESS_REVIEW_ENGINE_PROBE_SEARCH_SCENARIO"),
                                    10000);
                            }
                            controller.connectEngine(controller.state().profiles.front().id);
                            return;
                        }
                        if (mode.endsWith(QStringLiteral("analyzing")) &&
                            controller.state().runtime.phase ==
                                chess_review::engine::RuntimePhase::Ready) {
                            if (mode == QStringLiteral("live-analyzing")) {
                                auto* analyze =
                                    window->findChild<QAction*>(QStringLiteral("analyzeAction"));
                                auto* start =
                                    window->findChild<QWidget*>(QStringLiteral("analyzePage"))
                                        ->findChild<QPushButton*>(
                                            QStringLiteral("startEngineAnalysis"));
                                if (!analyze || !start) {
                                    qFatal("Missing live analysis controls");
                                }
                                analyze->trigger();
                                auto* startPoll = new QTimer(window);
                                QObject::connect(startPoll, &QTimer::timeout, startPoll,
                                                 [startPoll, start] {
                                                     if (start->isEnabled()) {
                                                         startPoll->stop();
                                                         start->click();
                                                     }
                                                 });
                                startPoll->start(5);
                                return;
                            }
                            chess_review::engine::AnalysisRequest request;
                            request.id = QUuid::createUuid();
                            request.limit = chess_review::engine::InfiniteLimit{};
                            chess_review::app::EngineControllerTestAccess::validator(controller)
                                .analyze(controller.state().runtime.session, request);
                            return;
                        }
                        if (state == State::Selected) {
                            controller.registerSelected();
                            return;
                        }
                        if ((mode == QStringLiteral("validating") && state == State::Validating &&
                             controller.state().message ==
                                 QStringLiteral("Waiting for engine readiness…")) ||
                            (mode == QStringLiteral("saving") && state == State::Saving) ||
                            (mode == QStringLiteral("connected") &&
                             controller.state().runtime.phase ==
                                 chess_review::engine::RuntimePhase::Ready) ||
                            (mode == QStringLiteral("add") &&
                             controller.state().profiles.size() == 1)) {
                            std::fputs("engine-close-requested\n", stdout);
                            std::fflush(stdout);
                            window->close();
                        }
                    });
                controller.selectExecutable(
                    QFileInfo(qEnvironmentVariable("CHESS_REVIEW_ENGINE_PROBE_BINARY"))
                        .canonicalFilePath());
                return;
            }
        });
        poll->start(5);
    });
}
} // namespace
Q_COREAPP_STARTUP_FUNCTION(installDriver)
