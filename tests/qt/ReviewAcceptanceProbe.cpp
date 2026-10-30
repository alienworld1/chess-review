#include "EngineControllerTestAccess.h"
#include "ReviewAcceptanceSupport.h"
#include "app/ReviewMovePresentation.h"
#include "app/ReviewWorkspaceController.h"
#include "ui/chess/ChessBoardWidget.h"
#include "ui/chess/MoveTimeline.h"
#include "ui/review/ReviewPage.h"
#include "ui/settings/EngineSettingsPage.h"
#include "ui/shell/MainWindow.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>
#include <QTimer>
#include <algorithm>
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
using namespace chess_review;
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "acceptance failed: %s\n", message);
        std::fflush(stderr);
        qFatal("Acceptance probe failed");
    }
}
template <typename Predicate> void until(Predicate predicate, const char* message, int ms = 15000) {
    require(QTest::qWaitFor(predicate, ms), message);
}
template <typename T> T* child(QObject& parent, const char* name) {
    auto* value = parent.findChild<T*>(QString::fromLatin1(name));
    require(value != nullptr, name);
    return value;
}
void report(QJsonObject value) {
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
void key(QWidget* widget, Qt::Key value) {
    QTest::keyPress(widget, value);
    QTest::keyRelease(widget, value);
}
void reveal(ui::MainWindow& window, QWidget* widget) {
    const auto scrolls = window.findChildren<QScrollArea*>();
    for (auto iterator = scrolls.crbegin(); iterator != scrolls.crend(); ++iterator) {
        auto* scroll = *iterator;
        if (scroll->widget() && scroll->widget()->isAncestorOf(widget)) {
            scroll->ensureWidgetVisible(widget);
        }
    }
    QCoreApplication::processEvents();
}
void activate(ui::MainWindow& window, const char* name) {
    auto* button = child<QPushButton>(window, name);
    reveal(window, button);
    require(button->isVisible() && button->isEnabled(), name);
    button->setFocus(Qt::TabFocusReason);
    key(button, Qt::Key_Space);
}
void choose(QComboBox& combo, int index) {
    require(combo.isVisible() && combo.isEnabled(), "visible choice");
    combo.setFocus(Qt::TabFocusReason);
    key(&combo, Qt::Key_Home);
    for (int i = 0; i < index; ++i) {
        key(&combo, Qt::Key_Down);
    }
    require(combo.currentIndex() == index, "keyboard choice");
}

void render(ui::MainWindow& window, const QString& state, const char* primary) {
    const auto directory = qEnvironmentVariable("CHESS_REVIEW_ACCEPTANCE_RENDER_DIR");
    if (directory.isEmpty()) {
        return;
    }
    const auto originalPalette = qApp->palette();
    const auto originalFont = window.font();
    const auto originalSize = window.size();
    for (const bool dark : {false, true}) {
        auto palette = originalPalette;
        if (dark) {
            palette.setColor(QPalette::Window, QColor(QStringLiteral("#202732")));
            palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#e5eaf0")));
            palette.setColor(QPalette::Base, QColor(QStringLiteral("#171e27")));
            palette.setColor(QPalette::Text, QColor(QStringLiteral("#e5eaf0")));
            palette.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#aab4c0")));
            palette.setColor(QPalette::Button, QColor(QStringLiteral("#303b49")));
            palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#e5eaf0")));
        }
        qApp->setPalette(palette);
        for (const bool enlarged : {false, true}) {
            auto font = originalFont;
            if (enlarged) {
                font.setPointSize(font.pointSize() + 5);
            }
            window.setFont(font);
            for (const auto size : {QSize(1100, 720), QSize(1360, 850), QSize(1800, 1000)}) {
                window.resize(size);
                QCoreApplication::processEvents();
                require(window.size() == size, "render logical size");
                auto* button = child<QAbstractButton>(window, primary);
                reveal(window, button);
                button->setFocus(Qt::TabFocusReason);
                QCoreApplication::processEvents();
                require(button->isVisible() && !button->text().isEmpty(), "named primary control");
                auto* shell = child<QScrollArea>(window, "shellScrollArea");
                require(shell->viewport()->rect().contains(
                            button->mapTo(shell->viewport(), button->rect().center())),
                        "primary control reachable");
                if (auto* board =
                        window.findChild<ui::ChessBoardWidget*>(QStringLiteral("reviewBoard"));
                    board && board->isVisible()) {
                    require(board->boardRect().width() == board->boardRect().height(),
                            "square rendered board");
                }
                const auto name =
                    QStringLiteral("%1-%2-%3-%4x%5.png")
                        .arg(state, dark ? QStringLiteral("dark") : QStringLiteral("light"),
                             enlarged ? QStringLiteral("large-font")
                                      : QStringLiteral("normal-font"))
                        .arg(size.width())
                        .arg(size.height());
                const auto pixmap = window.grab();
                require(pixmap.size() == QSize(qRound(size.width() * window.devicePixelRatioF()),
                                               qRound(size.height() * window.devicePixelRatioF())),
                        "render physical size");
                require(pixmap.save(QDir(directory).filePath(name)), "save acceptance render");
            }
        }
    }
    qApp->setPalette(originalPalette);
    window.setFont(originalFont);
    window.resize(originalSize);
    QCoreApplication::processEvents();
}

void run(ui::MainWindow& window) {
    auto& settings = ui::EnginePageTestAccess::controller(
        *child<ui::EngineSettingsPage>(window, "settingsPage"));
    auto& review = child<ui::ReviewPage>(window, "reviewPage")->controller();
    auto& navigation = review.navigation();
    auto& exploration = review.exploration();
    auto& supervisor = app::EngineControllerTestAccess::validator(settings);
    const auto mode = qEnvironmentVariable("CHESS_REVIEW_ACCEPTANCE_MODE");
    const auto input = qEnvironmentVariable("CHESS_REVIEW_ACCEPTANCE_PGN");
    const auto real = qEnvironmentVariable("CHESS_REVIEW_STOCKFISH");
    until([&] { return settings.state().ready && !settings.state().listing; }, "startup ready");
    window.resize(1100, 720);
    const auto script = [&](int first, int end, int delay, bool crash) {
        const auto document = review_acceptance::readGame(input);
        engine::ValidatorTestAccess::configure(
            supervisor,
            engine_test::scenario(qEnvironmentVariable("CHESS_REVIEW_ACCEPTANCE_SCRIPT"),
                                  review_acceptance::searches(*document, first, end, delay, crash)),
            10000);
        engine::ValidatorTestAccess::configureSearch(supervisor, 15000, 3000);
    };
    if (mode == QStringLiteral("reopen") || mode == QStringLiteral("recover") ||
        mode == QStringLiteral("resume")) {
        until([&] { return review.state().run && !review.state().busy && navigation.ready(); },
              "saved review restored");
        require(settings.state().runtime.phase == engine::RuntimePhase::Disconnected,
                "restoration must not connect an engine");
        if (mode != QStringLiteral("resume")) {
            require(review.state().run->status == (mode == QStringLiteral("recover")
                                                       ? review::JobStatus::Interrupted
                                                       : review::JobStatus::Complete),
                    "restored status");
            until([&] { return review.state().overview.has_value(); }, "restored overview");
            report({{"event", "restored"},
                    {"game", static_cast<qint64>(review.state().gameId)},
                    {"run", static_cast<qint64>(review.state().run->id)},
                    {"moment", navigation.selected()},
                    {"side", static_cast<int>(review.state().run->selectedSide)},
                    {"completed", review.state().run->completedPositions},
                    {"revision", static_cast<qint64>(review.state().run->revision)}});
            window.close();
            return;
        }
        const auto saved = *review.state().run;
        require(saved.status == review::JobStatus::Interrupted, "explicit resume only");
        script(saved.completedPositions, -1, 0, false);
        for (auto* button : window.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Saved review runs") && !button->isChecked()) {
                button->click();
            }
        }
        auto* runs = child<QTableView>(window, "reviewRuns");
        until([&] { return runs->model()->rowCount() == 1 && runs->isEnabled(); },
              "saved run list");
        runs->setCurrentIndex(runs->model()->index(0, 0));
        until([&] { return child<QPushButton>(window, "resumeReview")->isEnabled(); },
              "resume enabled");
        activate(window, "resumeReview");
        until(
            [&] {
                return !review.taskState().busy && review.state().run &&
                       review.state().run->status == review::JobStatus::Complete;
            },
            "resumed same job", 90000);
        require(review.state().run->id == saved.id &&
                    review.state().run->selectedSide == saved.selectedSide &&
                    review.state().run->quality == saved.quality,
                "resume identity and settings");
        report({{"event", "resumed"},
                {"run", static_cast<qint64>(saved.id)},
                {"completed", review.state().run->completedPositions}});
        window.close();
        return;
    }

    render(window, QStringLiteral("home"), "homeImportButton");
    // File-picker selection is supplied directly; validation, registration and all subsequent
    // primary actions use production commands and visible widgets, never repository seeding.
    child<QAction>(window, "engineSettingsAction")->trigger();
    if (real.isEmpty()) {
        engine::ValidatorTestAccess::configure(
            supervisor,
            engine_test::scenario(qEnvironmentVariable("CHESS_REVIEW_ACCEPTANCE_SCRIPT"),
                                  engine_test::handshake(review_acceptance::handshake())),
            10000);
    }
    settings.selectExecutable(real.isEmpty() ? QString::fromUtf8(FAKE_UCI_ENGINE) : real);
    until([&] { return settings.state().registration == app::EngineRegistrationState::Selected; },
          "executable selected");
    activate(window, "registerEngine");
    until([&] { return !settings.state().listing && settings.state().profiles.size() == 1; },
          "engine registered");
    render(window, QStringLiteral("settings"), "registerEngine");
    child<QAction>(window, "pastePgnAction")->trigger();
    auto* editor = child<QPlainTextEdit>(window, "pastePgnEditor");
    QFile pgn(input);
    require(pgn.open(QIODevice::ReadOnly), "read PGN");
    editor->setPlainText(QString::fromUtf8(pgn.readAll()));
    activate(window, "confirmPasteImport");
    auto* games = child<QTableView>(window, "gamesTable");
    until([&] { return games->isVisible() && games->model()->rowCount() == 1; }, "game imported");
    render(window, QStringLiteral("games"), "reviewGameButton");
    games->setFocus(Qt::TabFocusReason);
    key(games, Qt::Key_Home);
    if (!games->currentIndex().isValid()) {
        games->setCurrentIndex(games->model()->index(0, 0));
    }
    activate(window, "reviewGameButton");
    until([&] { return navigation.ready() && !review.state().busy; }, "game selected");
    choose(*child<QComboBox>(window, "reviewSide"), 2);
    choose(*child<QComboBox>(window, "reviewQuality"), 1);
    if (real.isEmpty()) {
        script(0,
               mode == QStringLiteral("stress")   ? 24
               : mode == QStringLiteral("cancel") ? 3
                                                  : -1,
               mode == QStringLiteral("interrupt") ? 300
               : mode == QStringLiteral("stress")  ? 40
                                                   : 0,
               mode == QStringLiteral("crash"));
    }
    int searches = 0;
    int heartbeats = 0;
    qint64 maxGap = 0;
    QElapsedTimer elapsed;
    elapsed.start();
    qint64 previous = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
        const auto now = elapsed.elapsed();
        maxGap = std::max(maxGap, now - previous);
        previous = now;
        ++heartbeats;
    });
    heartbeat.start(5);
    const auto searchConnection =
        QObject::connect(&supervisor, &engine::EngineSupervisor::scheduledFinished, &heartbeat,
                         [&](const engine::ScheduledResult&) { ++searches; });
    bool cancellationRequested = false;
    const auto cancelConnection =
        QObject::connect(&review, &app::ReviewGameController::taskChanged, &heartbeat, [&] {
            const auto& state = review.taskState();
            const int cancelAt = mode == QStringLiteral("stress") ? 24 : 3;
            if ((mode == QStringLiteral("cancel") || mode == QStringLiteral("stress")) &&
                state.active && state.run && state.run->completedPositions == cancelAt &&
                !state.cancelling && !cancellationRequested) {
                // Synchronous checkpoint notification prevents a further request being admitted.
                cancellationRequested = true;
                auto* cancel = child<QPushButton>(window, "cancelReview");
                require(cancel->isVisible() && cancel->isEnabled(),
                        "cancel available at checkpoint");
                cancel->click();
            }
        });
    activate(window, "runReview");
    until([&] { return review.taskState().run.has_value(); }, "review created");
    if (mode == QStringLiteral("interrupt")) {
        until([&] { return review.taskState().run->completedPositions >= 3; },
              "durable checkpoint");
        QSignalSpy committed(&review.workspace(),
                             &app::ReviewWorkspaceController::contextCommitted);
        review.selectMoment(2);
        until([&] { return !committed.empty(); }, "context acknowledged");
        report({{"event", "checkpoint"},
                {"game", static_cast<qint64>(review.state().gameId)},
                {"run", static_cast<qint64>(review.taskState().run->id)},
                {"moment", 2},
                {"completed", review.taskState().run->completedPositions}});
        // Parent kills this process after observing the acknowledged checkpoint.
        until([] { return false; }, "parent did not terminate interrupted probe", 30000);
    }
    if (mode == QStringLiteral("stress")) {
        require(navigation.line().size() == 513, "512-ply topology");
        auto* board = child<ui::ChessBoardWidget>(window, "reviewBoard");
        board->setFocus();
        key(board, Qt::Key_End);
        until([&] { return navigation.displayed() == 512; }, "navigate while reviewing");
        key(board, Qt::Key_Home);
        until([&] { return navigation.displayed() == 0; }, "return to root while reviewing");
        child<QAction>(window, "homeAction")->trigger();
        auto* dialog = child<QMessageBox>(window, "leaveReviewDialog");
        bool continued = false;
        for (auto* button : dialog->buttons()) {
            if (button->text() == QStringLiteral("Continue in background")) {
                button->click();
                continued = true;
                break;
            }
        }
        require(continued, "background choice");
        activate(window, "reviewTaskStatus");
        until([&] { return child<ui::ReviewPage>(window, "reviewPage")->isVisible(); },
              "return to task");
    }
    until([&] { return !review.taskState().busy; }, "review finished",
          real.isEmpty() ? 90000 : 300000);
    QObject::disconnect(cancelConnection);
    QObject::disconnect(searchConnection);
    heartbeat.stop();
    const auto saved = *review.state().run;
    if (mode == QStringLiteral("crash")) {
        require(saved.status == review::JobStatus::Failed && !review.state().error.isEmpty(),
                "engine crash visible");
        require(saved.completedPositions == 1, "crash retains completed evidence");
    } else if (mode == QStringLiteral("cancel") || mode == QStringLiteral("stress")) {
        require(saved.status == review::JobStatus::Cancelled, "cancelled status");
        require(saved.completedPositions == (mode == QStringLiteral("stress") ? 24 : 3),
                "cancel preserves committed work");
        require(heartbeats > 0, "event loop serviced");
    } else {
        require(saved.status == review::JobStatus::Complete &&
                    saved.completedPositions == saved.totalPositions &&
                    review.state().error.isEmpty(),
                "complete root-to-final review");
        until(
            [&] {
                return review.state().overview &&
                       review.state().overview->revision == saved.revision;
            },
            "current summary");
        require(
            child<QPlainTextEdit>(window, "reviewSummary")
                ->toPlainText()
                .contains(app::reviewOverviewText(*review.state().overview, saved.selectedSide)),
            "displayed accuracy and coverage");
        const auto document = review_acceptance::readGame(input);
        require(saved.totalPositions == static_cast<int>(document->originalMainline().size()) + 1,
                "all game positions covered");
        if (!document->originalMainline().empty()) {
            require(review.state().overview->sides[0].evaluated > 0 &&
                        review.state().overview->sides[1].accuracy.has_value(),
                    "useful review");
            require(review.state().overview->totalKeyMoments > 0, "selected player key moments");
        }
    }
    report({{"event", "reviewed"},
            {"game", static_cast<qint64>(saved.gameId)},
            {"run", static_cast<qint64>(saved.id)},
            {"completed", saved.completedPositions},
            {"total", saved.totalPositions},
            {"searches", searches},
            {"heartbeats", heartbeats},
            {"max_gap_ms", maxGap},
            {"elapsed_ms", elapsed.elapsed()}});

    if (mode == QStringLiteral("journey") || mode == QStringLiteral("real")) {
        activate(window, "reviewNextMoment");
        until([&] { return !navigation.pending() && exploration.state().canRetry; }, "key moment");
        const int moment = navigation.selected();
        render(window, QStringLiteral("review"), "reviewNextMoment");
        const auto* move = navigation.move(moment);
        require(move && move->classification && move->classification->loss &&
                    move->beforeEvidenceId > 0 && move->afterEvidenceId > 0 &&
                    !move->beforeAttempt.isEmpty() && !move->afterAttempt.isEmpty(),
                "traceable label");
        require(child<QLabel>(window, "reviewMomentHeadline")->text() ==
                    app::reviewMomentHeadline(move, moment),
                "displayed classification");
        const auto pieces = navigation.board().pieces;
        auto* timeline = child<ui::MoveTimeline>(window, "reviewTimeline");
        const auto preview = timeline->model()->index(1, 1);
        reveal(window, timeline);
        timeline->scrollTo(preview);
        QCoreApplication::processEvents();
        QTest::mouseMove(&window, QPoint(1, 1));
        require(timeline->viewport()->rect().contains(timeline->visualRect(preview).center()),
                "preview cell visible");
        QTest::mouseMove(timeline->viewport(), timeline->visualRect(preview).center());
        until([&] { return navigation.previewing(); }, "hover preview");
        require(navigation.selected() == moment, "preview cannot commit");
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(timeline, &leave);
        until([&] { return !navigation.pending() && !navigation.previewing(); },
              "preview restored");
        require(navigation.board().pieces == pieces, "preview restores board");
        activate(window, "reviewShowLine");
        until([&] { return exploration.state().canAnalyze; }, "line ready");
        activate(window, "reviewNextMove");
        until([&] { return exploration.state().step == 1 && exploration.state().canAnalyze; },
              "legal continuation step");
        const auto lineBoard = exploration.board().pieces;
        activate(window, "reviewAnalyzePosition");
        until(
            [&] {
                return !exploration.linkedAnalysis().busy() &&
                       exploration.linkedAnalysis().position();
            },
            "linked analysis ready");
        require(exploration.linkedAnalysis().board().pieces == lineBoard,
                "analysis starts at line");
        render(window, QStringLiteral("analysis"), "backToReview");
        activate(window, "backToReview");
        require(navigation.selected() == moment && exploration.state().step == 1 &&
                    exploration.board().pieces == lineBoard,
                "exact analysis return");
        activate(window, "reviewReturnToGame");
        until([&] { return exploration.state().canRetry; }, "retry available");
        auto root = *navigation.position();
        require(root.undo().has_value(), "retry root");
        require(real.isEmpty() || navigation.move(moment)->classification->bestMove.has_value(),
                "verified best move");
        const auto alternative = real.isEmpty()
                                     ? root.legalMoves().at(1)
                                     : *navigation.move(moment)->classification->bestMove;
        activate(window, "reviewTryAgain");
        auto* board = child<ui::ChessBoardWidget>(window, "reviewBoard");
        until([&] { return board->state().interactive; }, "practice board");
        require(child<QLabel>(window, "reviewMomentExplanation")->text().isEmpty() &&
                    child<QPlainTextEdit>(window, "reviewMoveDetails")->toPlainText().isEmpty() &&
                    child<QComboBox>(window, "reviewCandidates")->count() == 0,
                "practice hides solutions");
        render(window, QStringLiteral("practice"), "reviewHint");
        QSignalSpy practiceSearches(&supervisor, &engine::EngineSupervisor::scheduledChanged);
        QTest::mousePress(board, Qt::LeftButton, Qt::NoModifier,
                          board->squareRect(alternative.from).center().toPoint());
        QTest::mouseMove(board, board->squareRect(alternative.to).center().toPoint());
        QTest::mouseRelease(board, Qt::LeftButton, Qt::NoModifier,
                            board->squareRect(alternative.to).center().toPoint());
        until([&] { return exploration.state().mode == app::ExplorationMode::Success; },
              "demonstrated equivalent accepted");
        require(practiceSearches.empty(), "equivalent uses durable evidence");
        until([&] { return exploration.state().mode == app::ExplorationMode::Game; },
              "practice return");
        require(review.state().run->revision == saved.revision, "practice cannot rewrite review");
        report({{"event", "explored"},
                {"game", static_cast<qint64>(saved.gameId)},
                {"run", static_cast<qint64>(saved.id)},
                {"moment", moment},
                {"side", static_cast<int>(saved.selectedSide)}});
    }
    // Closing immediately also exercises the production debounce flush/drain path.
    window.close();
}

void installDriver() {
    QTimer::singleShot(0, QCoreApplication::instance(), [] {
        auto* poll = new QTimer(QCoreApplication::instance());
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, poll,
                         [] { report({{"event", "quit"}}); });
        QObject::connect(poll, &QTimer::timeout, poll, [poll] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* window = qobject_cast<ui::MainWindow*>(widget);
                if (window && window->isVisible()) {
                    poll->stop();
                    run(*window);
                    return;
                }
            }
        });
        poll->start(10);
    });
}
} // namespace
Q_COREAPP_STARTUP_FUNCTION(installDriver)
