#include "ui/analysis/AnalysisPage.h"
#include "ui/chess/ChessBoardWidget.h"
#include <QAccessible>
#include <QAction>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>

namespace chess_review::ui {
AnalysisPage::AnalysisPage(app::AnalysisController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setObjectName(QStringLiteral("analyzePage"));
    auto* layout = new QVBoxLayout(this);
    auto* notice = new QLabel(
        tr("Analysis — session only. Moves and annotations are discarded on exit."), this);
    notice->setTextFormat(Qt::PlainText);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto* commands = new QHBoxLayout;
    auto* reset = new QPushButton(tr("New Analysis"), this);
    reset->setObjectName(QStringLiteral("newAnalysisButton"));
    reset->setToolTip(tr("Start over after the current position update finishes."));
    connect(&controller_, &app::AnalysisController::changed, reset,
            [this, reset] { reset->setEnabled(!controller_.busy()); });
    connect(reset, &QPushButton::clicked, this, [this] {
        if (!controller_.busy()) {
            requestDiscard([this] { controller_.reset(); });
        }
    });
    commands->addWidget(reset);
    auto* flip = new QPushButton(tr("Flip Board"), this);
    flip->setObjectName(QStringLiteral("flipBoardButton"));
    connect(flip, &QPushButton::clicked, &controller_, &app::AnalysisController::flip);
    commands->addWidget(flip);
    auto* options = new QToolButton(this);
    options->setObjectName(QStringLiteral("boardOptionsButton"));
    options->setText(tr("Board Options"));
    options->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(options);
    coordinates_ = menu->addAction(tr("Show Coordinates"));
    coordinates_->setCheckable(true);
    animation_ = menu->addAction(tr("Animate Moves"));
    animation_->setCheckable(true);
    auto* clear = menu->addAction(tr("Clear Current Annotations"));
    connect(clear, &QAction::triggered, &controller_, &app::AnalysisController::clearAnnotations);
    undo_ = menu->addAction(tr("Undo Annotation"));
    undo_->setShortcut(QKeySequence::Undo);
    undo_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(undo_);
    connect(undo_, &QAction::triggered, &controller_, &app::AnalysisController::undoAnnotation);
    redo_ = menu->addAction(tr("Redo Annotation"));
    redo_->setShortcut(QKeySequence::Redo);
    redo_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(redo_);
    connect(redo_, &QAction::triggered, &controller_, &app::AnalysisController::redoAnnotation);
    auto* customize = menu->addAction(tr("Customize Selected Annotation Color…"));
    connect(customize, &QAction::triggered, this, [this] {
        auto* dialog = new QColorDialog(
            QColor(preferences_.annotationColors[static_cast<std::size_t>(color_->currentIndex())]),
            this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(tr("Annotation color"));
        const auto index = static_cast<std::size_t>(color_->currentIndex());
        connect(dialog, &QColorDialog::colorSelected, this, [this, index](const QColor& color) {
            preferences_.annotationColors[index] = color.name();
            board_->setPreferences(preferences_);
        });
        dialog->open();
    });
    auto* help = menu->addAction(tr("Board Gestures and Keyboard Help"));
    connect(help, &QAction::triggered, this, [this] {
        auto* dialog = new QDialog(this);
        dialog->setObjectName(QStringLiteral("boardControlsDialog"));
        dialog->setWindowTitle(tr("Board controls"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        auto* contents = new QVBoxLayout(dialog);
        auto* reference = new QTextBrowser(dialog);
        reference->setObjectName(QStringLiteral("boardControlsReference"));
        reference->setAccessibleName(tr("Board controls reference"));
        reference->setHtml(tr(
            "<h3>Moving pieces</h3>"
            "<table cellspacing='8' width='100%'>"
            "<tr><td width='30%'><b>Drag</b></td><td>Drop a piece on its destination. "
            "Illegal moves return to their starting square.</td></tr>"
            "<tr><td><b>Click-click</b></td><td>Select this input mode in the toolbar, "
            "then click the piece and its destination.</td></tr>"
            "<tr><td><b>Enter / Space</b></td><td>Enter starts keyboard square navigation. "
            "Use arrow keys to move focus, then Enter or Space to select a piece or play.</td></tr>"
            "<tr><td><b>Escape</b></td><td>Cancel the active gesture or popup, "
            "then selection, then keyboard square navigation.</td></tr></table>"
            "<h3>Move history</h3>"
            "<table cellspacing='8' width='100%'>"
            "<tr><td width='30%'><b>Left / Right</b></td><td>Previous / next move, "
            "when outside keyboard square navigation.</td></tr>"
            "<tr><td><b>Home / End</b></td><td>Beginning / end of the chosen line.</td></tr>"
            "<tr><td><b>Flip Board</b></td><td>Use the toolbar or View menu "
            "(Ctrl+F).</td></tr></table>"
            "<h3>Arrows and highlights</h3>"
            "<table cellspacing='8' width='100%'>"
            "<tr><td width='30%'><b>Right-click</b></td><td>Toggle a square highlight.</td></tr>"
            "<tr><td><b>Right-drag</b></td><td>Toggle an arrow between squares.</td></tr>"
            "<tr><td><b>H</b></td><td>Highlight the focused square in keyboard square "
            "navigation.</td></tr>"
            "<tr><td><b>A, then A</b></td><td>Mark the arrow start, navigate to its end, "
            "then press A again.</td></tr></table>"
            "<h3>Colors and board options</h3>"
            "<table cellspacing='8' width='100%'>"
            "<tr><td width='30%'><b>No modifier</b></td><td>Use the annotation color selected in "
            "the toolbar.</td></tr>"
            "<tr><td><b>Shift</b></td><td>Color 2 (red by default).</td></tr>"
            "<tr><td><b>Ctrl</b></td><td>Color 3 (yellow by default).</td></tr>"
            "<tr><td><b>Ctrl+Shift</b></td><td>Color 4 (blue by default).</td></tr>"
            "<tr><td><b>Board Options</b></td><td>Undo, redo, or clear annotations; "
            "customize colors; toggle coordinates and click-move animation.</td></tr></table>"));
        contents->addWidget(reference);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        contents->addWidget(buttons);
        dialog->resize(640, 560);
        dialog->open();
    });
    options->setMenu(menu);
    commands->addWidget(options);
    commands->addStretch();
    layout->addLayout(commands);
    auto* preferences = new QHBoxLayout;
    auto* inputLabel = new QLabel(tr("Move input:"), this);
    input_ = new QComboBox(this);
    input_->setObjectName(QStringLiteral("boardInputMode"));
    input_->addItems({tr("Drag"), tr("Click-click")});
    inputLabel->setBuddy(input_);
    preferences->addWidget(inputLabel);
    preferences->addWidget(input_);
    auto* colorLabel = new QLabel(tr("Annotation:"), this);
    color_ = new QComboBox(this);
    color_->setObjectName(QStringLiteral("annotationColor"));
    color_->addItems(
        {tr("Green / color 1"), tr("Red / color 2"), tr("Yellow / color 3"), tr("Blue / color 4")});
    colorLabel->setBuddy(color_);
    preferences->addWidget(colorLabel);
    preferences->addWidget(color_);
    preferences->addStretch();
    layout->addLayout(preferences);
    topPlayer_ = new QLabel(this);
    topPlayer_->setAlignment(Qt::AlignHCenter);
    layout->addWidget(topPlayer_);
    board_ = new ChessBoardWidget(this);
    layout->addWidget(board_, 1);
    bottomPlayer_ = new QLabel(this);
    bottomPlayer_->setAlignment(Qt::AlignHCenter);
    layout->addWidget(bottomPlayer_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("analysisStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setAccessibleName(tr("Analysis status"));
    layout->addWidget(status_);
    auto* navigationWidget = new QWidget(this);
    auto* navigation = new QHBoxLayout(navigationWidget);
    navigation->setContentsMargins(0, 0, 0, 0);
    const auto addNavigation = [this, navigation, navigationWidget](const QString& text,
                                                                    auto method) {
        auto* button = new QPushButton(text, navigationWidget);
        connect(button, &QPushButton::clicked, &controller_, method);
        navigation->addWidget(button);
    };
    addNavigation(tr("Beginning"), &app::AnalysisController::beginning);
    addNavigation(tr("Previous"), &app::AnalysisController::previous);
    addNavigation(tr("Next"), &app::AnalysisController::next);
    addNavigation(tr("End"), &app::AnalysisController::end);
    layout->addWidget(navigationWidget);
    connect(&controller_, &app::AnalysisController::changed, navigationWidget,
            [this, navigationWidget] { navigationWidget->setEnabled(!controller_.busy()); });
    const auto addShortcut = [this, navigationWidget](Qt::Key key, auto method) {
        auto* shortcut = new QShortcut(QKeySequence(key), navigationWidget);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, &controller_, method);
    };
    addShortcut(Qt::Key_Left, &app::AnalysisController::previous);
    addShortcut(Qt::Key_Right, &app::AnalysisController::next);
    addShortcut(Qt::Key_Home, &app::AnalysisController::beginning);
    addShortcut(Qt::Key_End, &app::AnalysisController::end);
    auto* historyLabel = new QLabel(tr("History:"), this);
    history_ = new QComboBox(this);
    history_->setObjectName(QStringLiteral("analysisHistory"));
    historyLabel->setBuddy(history_);
    auto* continuationLabel = new QLabel(tr("Continue with:"), this);
    continuations_ = new QComboBox(this);
    continuations_->setObjectName(QStringLiteral("analysisContinuations"));
    continuationLabel->setBuddy(continuations_);
    auto* moves = new QHBoxLayout;
    moves->addWidget(historyLabel);
    moves->addWidget(history_, 1);
    moves->addWidget(continuationLabel);
    moves->addWidget(continuations_, 1);
    layout->addLayout(moves);
    connect(history_, &QComboBox::activated, this, [this](int index) {
        const auto& history = controller_.history();
        if (index >= 0 && static_cast<std::size_t>(index) < history.size()) {
            controller_.navigate(history[static_cast<std::size_t>(index)].id);
        }
    });
    connect(continuations_, &QComboBox::activated, this, [this](int index) {
        const auto& continuations = controller_.continuations();
        if (index > 0 && static_cast<std::size_t>(index - 1) < continuations.size()) {
            controller_.navigate(continuations[static_cast<std::size_t>(index - 1)].id);
        }
    });
    connect(&controller_, &app::AnalysisController::changed, this, &AnalysisPage::refresh);
    connect(&controller_, &app::AnalysisController::failed, status_, &QLabel::setText);
    connect(&controller_, &app::AnalysisController::moveAccepted, board_,
            &ChessBoardWidget::animateMove);
    connect(board_, &ChessBoardWidget::squareSelected, &controller_,
            &app::AnalysisController::selectSquare);
    connect(board_, &ChessBoardWidget::selectionCleared, &controller_,
            &app::AnalysisController::clearSelection);
    connect(board_, &ChessBoardWidget::moveAttempted, &controller_,
            &app::AnalysisController::attemptMove);
    connect(board_, &ChessBoardWidget::annotationToggled, &controller_,
            &app::AnalysisController::toggleAnnotation);
    connect(board_, &ChessBoardWidget::previousRequested, &controller_,
            &app::AnalysisController::previous);
    connect(board_, &ChessBoardWidget::nextRequested, &controller_, &app::AnalysisController::next);
    connect(board_, &ChessBoardWidget::beginningRequested, &controller_,
            &app::AnalysisController::beginning);
    connect(board_, &ChessBoardWidget::endRequested, &controller_, &app::AnalysisController::end);
    connect(input_, &QComboBox::currentIndexChanged, this, [this](int index) {
        preferences_.inputMode =
            index == 0 ? app::BoardInputMode::Drag : app::BoardInputMode::Click;
        board_->setPreferences(preferences_);
    });
    connect(color_, &QComboBox::currentIndexChanged, board_, &ChessBoardWidget::setAnnotationColor);
    connect(coordinates_, &QAction::toggled, this, [this](bool enabled) {
        preferences_.coordinates = enabled;
        board_->setPreferences(preferences_);
    });
    connect(animation_, &QAction::toggled, this, [this](bool enabled) {
        preferences_.animation = enabled;
        board_->setPreferences(preferences_);
    });
    setPreferences(preferences_);
    refresh();
}
void AnalysisPage::setPreferences(const app::BoardPreferences& preferences) {
    const QSignalBlocker inputBlock(input_);
    const QSignalBlocker coordinatesBlock(coordinates_);
    const QSignalBlocker animationBlock(animation_);
    preferences_ = preferences;
    input_->setCurrentIndex(preferences.inputMode == app::BoardInputMode::Drag ? 0 : 1);
    coordinates_->setChecked(preferences.coordinates);
    animation_->setChecked(preferences.animation);
    board_->setPreferences(preferences);
}
void AnalysisPage::refresh() {
    board_->setState(controller_.board());
    const QSignalBlocker historyBlock(history_);
    const QSignalBlocker continuationBlock(continuations_);
    if (displayedRevision_ != controller_.board().revision) {
        displayedRevision_ = controller_.board().revision;
        history_->clear();
        for (const auto& entry : controller_.history()) {
            history_->addItem(entry.label);
        }
        history_->setCurrentIndex(history_->count() - 1);
        continuations_->clear();
        continuations_->addItem(tr("Choose continuation"));
        for (const auto& entry : controller_.continuations()) {
            continuations_->addItem(entry.label);
        }
    }
    history_->setEnabled(!controller_.busy());
    continuations_->setEnabled(!controller_.busy() && !controller_.continuations().empty());
    undo_->setEnabled(controller_.board().interactive && controller_.canUndoAnnotation());
    redo_->setEnabled(controller_.board().interactive && controller_.canRedoAnnotation());
    topPlayer_->setText(controller_.board().blackAtBottom ? tr("White") : tr("Black"));
    bottomPlayer_->setText(controller_.board().blackAtBottom ? tr("Black") : tr("White"));
    status_->setText(controller_.busy() ? tr("Updating position…")
                                        : controller_.board().positionDescription);
    QAccessibleEvent event(status_, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
}
void AnalysisPage::requestDiscard(std::function<void()> onDiscard) {
    if (!controller_.hasContent() && !controller_.busy()) {
        onDiscard();
        return;
    }
    if (confirming_) {
        return;
    }
    confirming_ = true;
    auto* dialog =
        new QMessageBox(QMessageBox::Question, tr("Discard analysis?"),
                        tr("This analysis is session-only. Discard its moves and annotations?"),
                        QMessageBox::Discard | QMessageBox::Cancel, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("discardAnalysisDialog"));
    dialog->setDefaultButton(QMessageBox::Cancel);
    connect(dialog, &QDialog::finished, this, [this, onDiscard = std::move(onDiscard)](int result) {
        confirming_ = false;
        if (result == QMessageBox::Discard) {
            onDiscard();
        }
    });
    dialog->open();
}
void AnalysisPage::deactivate() {
    auto state = controller_.board();
    state.interactive = false;
    board_->setState(state);
    setEnabled(false);
}
} // namespace chess_review::ui
