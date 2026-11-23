#include "ui/analysis/AnalysisPage.h"
#include "ui/Theme.h"
#include "ui/analysis/EngineAnalysisPanel.h"
#include "ui/analysis/MoveNavigation.h"
#include "ui/chess/ChessBoardWidget.h"
#include <QAccessible>
#include <QAction>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>
#include <utility>

namespace chess_review::ui {
AnalysisPage::AnalysisPage(app::AnalysisController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setObjectName(QStringLiteral("analyzePage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    notice_ = new QLabel(this);
    notice_->setObjectName(QStringLiteral("analysisGameInfo"));
    notice_->setTextFormat(Qt::PlainText);
    notice_->setWordWrap(true);
    setTextRole(*notice_, "secondary");
    layout->addWidget(notice_);
    auto* commands = new QHBoxLayout;
    commands->setObjectName(QStringLiteral("analysisCommands"));
    commands->setSpacing(8);
    auto* reset = new QPushButton(tr("New Analysis"), this);
    reset->setObjectName(QStringLiteral("newAnalysisButton"));
    setCompactAction(*reset);
    setIcon(*reset, QStringLiteral("plus"));
    const auto updateReset = [this, reset] {
        const bool game = controller_.activeTab() > 0;
        reset->setText(game ? tr("Scratch Analysis") : tr("New Analysis"));
        reset->setToolTip(game ? tr("Open the scratch board and keep this game open.")
                               : tr("Start over after the current position update finishes."));
        reset->setEnabled(!controller_.busy());
    };
    connect(&controller_, &app::AnalysisController::changed, reset, updateReset);
    updateReset();
    connect(reset, &QPushButton::clicked, this, [this] {
        if (controller_.busy()) {
            return;
        }
        if (controller_.activeTab() > 0) {
            selectGameTab(0);
        } else {
            requestDiscard([this] { controller_.reset(); });
        }
    });
    commands->addWidget(reset);
    auto* flip = new QPushButton(tr("Flip Board"), this);
    flip->setObjectName(QStringLiteral("flipBoardButton"));
    setQuietAction(*flip);
    setCompactAction(*flip);
    setIcon(*flip, QStringLiteral("flip"));
    connect(flip, &QPushButton::clicked, &controller_, &app::AnalysisController::flip);
    commands->addWidget(flip);
    auto* options = new QToolButton(this);
    options->setObjectName(QStringLiteral("boardOptionsButton"));
    options->setText(tr("Board Options"));
    options->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    options->setPopupMode(QToolButton::InstantPopup);
    setQuietAction(*options);
    setCompactAction(*options);
    setIcon(*options, QStringLiteral("more"));
    auto* menu = new QMenu(options);
    auto* position = menu->addAction(tr("Set Position from FEN…"));
    position->setObjectName(QStringLiteral("setAnalysisPosition"));
    connect(position, &QAction::triggered, this, &AnalysisPage::setPosition);
    connect(menu, &QMenu::aboutToShow, position, [this, position] {
        position->setText(controller_.activeTab() > 0 ? tr("Set Scratch Position from FEN…")
                                                      : tr("Set Position from FEN…"));
        position->setEnabled(!controller_.busy());
    });
    menu->addSeparator();
    coordinates_ = menu->addAction(tr("Show Coordinates"));
    coordinates_->setObjectName(QStringLiteral("analysisCoordinatesAction"));
    coordinates_->setCheckable(true);
    animation_ = menu->addAction(tr("Animate Moves"));
    animation_->setObjectName(QStringLiteral("analysisAnimationAction"));
    animation_->setCheckable(true);
    auto* clear = menu->addAction(tr("Clear Current Annotations"));
    connect(clear, &QAction::triggered, this, [this] {
        if (!displayingPreview_) {
            controller_.clearAnnotations();
        }
    });
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
            setPreferences(preferences_);
        });
        dialog->open();
    });
    auto* help = menu->addAction(tr("Board Gestures and Keyboard Help"));
    help->setObjectName(QStringLiteral("boardControlsHelp"));
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
            "<tr><td><b>Timeline / PGN tree</b></td><td>Click any move to navigate. "
            "Hover shows details without changing the board.</td></tr>"
            "<tr><td><b>PGN keyboard</b></td><td>Up/Down browse lines; Ctrl+Left/Right "
            "focus moves within a line; Enter selects. Plus/minus expands/collapses "
            "variations. Left/Right navigate history.</td></tr>"
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
    auto* preferenceControls = new QWidget(this);
    auto* preferences = new QHBoxLayout(preferenceControls);
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
    color_->addItems({tr("Color 1"), tr("Color 2"), tr("Color 3"), tr("Color 4")});
    colorLabel->setBuddy(color_);
    preferences->addWidget(colorLabel);
    preferences->addWidget(color_);
    preferences->addStretch();
    menu->addSeparator();
    auto* preferenceAction = new QWidgetAction(menu);
    preferenceAction->setDefaultWidget(preferenceControls);
    menu->addAction(preferenceAction);
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setObjectName(QStringLiteral("analysisSplitter"));
    splitter_->setChildrenCollapsible(false);
    auto* boardPane = new QWidget(splitter_);
    auto* boardLayout = new QVBoxLayout(boardPane);
    boardLayout_ = boardLayout;
    boardLayout->setContentsMargins(0, 0, 0, 0);
    boardLayout->setSpacing(4);
    layout->addWidget(splitter_, 1);
    topPlayer_ = new QLabel(boardPane);
    topPlayer_->setTextFormat(Qt::PlainText);
    topPlayer_->setAlignment(Qt::AlignHCenter);
    setTextRole(*topPlayer_, "strong");
    boardLayout->addWidget(topPlayer_);
    board_ = new ChessBoardWidget(boardPane);
    board_->setMinimumSize(248, 248);
    boardLayout->addWidget(board_, 1);
    bottomPlayer_ = new QLabel(this);
    bottomPlayer_->setTextFormat(Qt::PlainText);
    bottomPlayer_->setAlignment(Qt::AlignHCenter);
    setTextRole(*bottomPlayer_, "strong");
    boardLayout->addWidget(bottomPlayer_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("analysisStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setAccessibleName(tr("Analysis status"));
    setTextRole(*status_, "caption");
    boardLayout->addWidget(status_);
    auto* navigationWidget = new QWidget(boardPane);
    auto* navigation = new QHBoxLayout(navigationWidget);
    navigation->setContentsMargins(0, 0, 0, 0);
    navigation->setSpacing(4);
    navigation->addStretch();
    const auto addNavigation = [this, navigation, navigationWidget](const QString& text,
                                                                    auto method) {
        auto* button = new QPushButton(text, navigationWidget);
        setQuietAction(*button);
        setCompactAction(*button);
        connect(button, &QPushButton::clicked, &controller_, method);
        const auto refreshButton = [this, button, method] {
            const bool backward = method == &app::AnalysisController::previous ||
                                  method == &app::AnalysisController::beginning;
            button->setEnabled(backward ? controller_.canPrevious() : controller_.canNext());
        };
        connect(&controller_, &app::AnalysisController::changed, button, refreshButton);
        refreshButton();
        navigation->addWidget(button);
    };
    addNavigation(tr("Beginning"), &app::AnalysisController::beginning);
    addNavigation(tr("Previous"), &app::AnalysisController::previous);
    addNavigation(tr("Next"), &app::AnalysisController::next);
    addNavigation(tr("End"), &app::AnalysisController::end);
    navigation->addStretch();
    boardLayout->addWidget(navigationWidget);

    const auto addShortcut = [this, navigationWidget](Qt::Key key, auto method) {
        auto* shortcut = new QShortcut(QKeySequence(key), navigationWidget);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, &controller_, method);
    };
    addShortcut(Qt::Key_Left, &app::AnalysisController::previous);
    addShortcut(Qt::Key_Right, &app::AnalysisController::next);
    addShortcut(Qt::Key_Home, &app::AnalysisController::beginning);
    addShortcut(Qt::Key_End, &app::AnalysisController::end);
    timeline_ = new MoveTimelineView(controller_, boardPane);
    boardLayout->addWidget(timeline_);
    notation_ = new QWidget(splitter_);
    setPanelSurface(*notation_);
    auto* notationLayout = new QVBoxLayout(notation_);
    notationLayout->setContentsMargins(12, 10, 12, 12);
    notationLayout->setSpacing(8);
    auto* notationCommands = new QHBoxLayout;
    notationCommands->setSpacing(6);
    edit_ = new QPushButton(tr("Edit Note…"), notation_);
    edit_->setObjectName(QStringLiteral("editAnalysisNote"));
    setQuietAction(*edit_);
    setIcon(*edit_, QStringLiteral("note"));
    variations_ = new QPushButton(tr("Variations"), notation_);
    variations_->setObjectName(QStringLiteral("analysisVariationActions"));
    setQuietAction(*variations_);
    setIcon(*variations_, QStringLiteral("variations"));
    save_ = new QPushButton(tr("Save PGN…"), notation_);
    save_->setObjectName(QStringLiteral("saveAnalysisPgn"));
    save_->setToolTip(tr("Save a PGN copy with the current lines, notes, and board markings."));
    setQuietAction(*save_);
    setIcon(*save_, QStringLiteral("save"));
    notationCommands->addWidget(edit_);
    notationCommands->addWidget(variations_);
    notationCommands->addStretch();
    notationCommands->addWidget(save_);
    notationLayout->addLayout(notationCommands);
    tree_ = new GameTreeView(controller_, notation_);
    tree_->setFrameShape(QFrame::NoFrame);
    tree_->setMinimumHeight(64);
    setFlatSurface(*tree_);
    notationLayout->addWidget(tree_, 1);
    notes_ = new QPlainTextEdit(notation_);
    notes_->setObjectName(QStringLiteral("positionNotes"));
    notes_->setAccessibleName(tr("PGN comments and numeric annotation glyphs"));
    notes_->setReadOnly(true);
    notes_->setMaximumHeight(88);
    notationLayout->addWidget(notes_);
    operationNotice_ = new QLabel(notation_);
    operationNotice_->setTextFormat(Qt::PlainText);
    operationNotice_->setWordWrap(true);
    setTextRole(*operationNotice_, "secondary");
    notationLayout->addWidget(operationNotice_);
    operationNotice_->hide();
    connect(tree_, &GameTreeView::variationMenuRequested, this, &AnalysisPage::showVariationMenu);
    connect(edit_, &QPushButton::clicked, this,
            [this] { editComment(controller_.target(controller_.currentNode())); });
    connect(variations_, &QPushButton::clicked, this, [this] {
        showVariationMenu(controller_.target(controller_.currentNode()),
                          variations_->mapToGlobal(variations_->rect().bottomLeft()));
    });
    connect(save_, &QPushButton::clicked, this, [this] { savePgn(); });
    connect(&controller_, &app::AnalysisController::pgnExported, this, [this] {
        operationNotice_->setText(tr("Analysis PGN saved."));
        operationNotice_->show();
        if (auto done = std::exchange(afterExport_, {})) {
            done();
        }
    });
    connect(&controller_, &app::AnalysisController::failed, this, [this] { afterExport_ = {}; });
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 1);
    connect(splitter_, &QSplitter::splitterMoved, this, [this] {
        const auto sizes = splitter_->sizes();
        const auto total = sizes[0] + sizes[1];
        if (total > 0) {
            preferences_.analysisSplit = std::clamp(sizes[0] * 100 / total, 1, 99);
        }
    });
    connect(&controller_, &app::AnalysisController::changed, this, &AnalysisPage::refresh);
    connect(&controller_, &app::AnalysisController::failed, status_, &QLabel::setText);
    connect(&controller_, &app::AnalysisController::moveAccepted, board_,
            &ChessBoardWidget::animateMove);
    connect(&controller_, &app::AnalysisController::navigationAnimationRequested, board_,
            &ChessBoardWidget::animateMove);
    connect(board_, &ChessBoardWidget::squareSelected, &controller_,
            &app::AnalysisController::selectSquare);
    connect(board_, &ChessBoardWidget::selectionCleared, &controller_,
            &app::AnalysisController::clearSelection);
    connect(board_, &ChessBoardWidget::moveAttempted, &controller_,
            &app::AnalysisController::attemptMove);
    connect(board_, &ChessBoardWidget::annotationToggled, &controller_,
            &app::AnalysisController::toggleAnnotation);
    connect(board_, &ChessBoardWidget::previousRequested, this, [this] {
        if (engine_ && engine_->preview()) {
            engine_->stepPreview(-1);
        } else {
            controller_.previous();
        }
    });
    connect(board_, &ChessBoardWidget::nextRequested, this, [this] {
        if (engine_ && engine_->preview()) {
            engine_->stepPreview(1);
        } else {
            controller_.next();
        }
    });
    connect(board_, &ChessBoardWidget::beginningRequested, &controller_,
            &app::AnalysisController::beginning);
    connect(board_, &ChessBoardWidget::endRequested, &controller_, &app::AnalysisController::end);
    connect(input_, &QComboBox::currentIndexChanged, this, [this](int index) {
        preferences_.inputMode =
            index == 0 ? app::BoardInputMode::Drag : app::BoardInputMode::Click;
        board_->setPreferences(preferences_);
    });
    connect(color_, &QComboBox::currentIndexChanged, board_, &ChessBoardWidget::setAnnotationColor);
    const auto changeDisplayOptions = [this] {
        setBoardDisplayOptions(coordinates_->isChecked(), animation_->isChecked());
        emit boardDisplayOptionsChanged(preferences_.coordinates, preferences_.animation);
    };
    connect(coordinates_, &QAction::toggled, this, changeDisplayOptions);
    connect(animation_, &QAction::toggled, this, changeDisplayOptions);
    preferences_.blackAtBottom = controller_.board().blackAtBottom;
    setPreferences(preferences_);
    refresh();
}
void AnalysisPage::installEngineAnalysis(app::EngineAnalysisController& engine) {
    engine_ = &engine;
    auto* rightPane = new QSplitter(Qt::Vertical, this);
    detailSplitter_ = rightPane;
    rightPane->setObjectName(QStringLiteral("analysisDetailSplitter"));
    rightPane->setChildrenCollapsible(false);
    splitter_->replaceWidget(splitter_->indexOf(notation_), rightPane);
    auto* panel = new EngineAnalysisPanel(engine, rightPane);
    rightPane->addWidget(panel);
    rightPane->addWidget(notation_);
    rightPane->setStretchFactor(1, 1);
    connect(rightPane, &QSplitter::splitterMoved, this, [this] {
        const auto sizes = detailSplitter_->sizes();
        const auto total = sizes[0] + sizes[1];
        if (total > 0) {
            preferences_.analysisDetailSplit = std::clamp(sizes[0] * 100 / total, 1, 99);
        }
    });
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    escape->setEnabled(false);
    connect(escape, &QShortcut::activated, &engine, &app::EngineAnalysisController::dismissPreview);
    connect(&engine, &app::EngineAnalysisController::changed, this, [this, &engine, escape] {
        escape->setEnabled(engine.preview().has_value());
        if (preferences_.analysisMultiPv != engine.state().requestedLines) {
            preferences_.analysisMultiPv = engine.state().requestedLines;
            emit analysisMultiPvChanged(preferences_.analysisMultiPv);
        }
        if (engine.preview().has_value() != displayingPreview_) {
            refresh();
        } else if (displayingPreview_) {
            refreshBoard();
        }
    });
    engine.setMultiPv(preferences_.analysisMultiPv);
    auto* boardFrame = new QWidget(this);
    boardFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    boardFrame->setMinimumHeight(248);
    auto* bar = new EvaluationBarWidget(*board_, boardFrame);
    const int boardIndex = boardLayout_->indexOf(board_);
    boardLayout_->removeWidget(board_);
    auto* boardRow = new QHBoxLayout(boardFrame);
    boardRow->setContentsMargins(28, 0, 0, 0);
    boardRow->addWidget(board_, 1);
    boardLayout_->insertWidget(boardIndex, boardFrame, 1);
    connect(panel, &EngineAnalysisPanel::manageEnginesRequested, this,
            &AnalysisPage::manageEnginesRequested);
    connect(&engine, &app::EngineAnalysisController::changed, bar, [&engine, bar] {
        bar->setEvaluation(engine.preview() ? app::EngineLinePresentation{} : engine.state().line);
    });
    connect(&controller_, &app::AnalysisController::changed, bar, [bar] { bar->update(); });
    bar->setEvaluation(engine.state().line);
}
void AnalysisPage::setPreferences(const app::BoardPreferences& preferences) {
    const QSignalBlocker inputBlock(input_);
    preferences_ = preferences;
    input_->setCurrentIndex(preferences.inputMode == app::BoardInputMode::Drag ? 0 : 1);
    setBoardDisplayOptions(preferences.coordinates, preferences.animation);
    for (std::size_t i = 0; i < preferences.annotationColors.size(); ++i) {
        const auto index = static_cast<int>(i);
        QPixmap swatch(16, 16);
        swatch.fill(QColor(preferences.annotationColors[i]));
        color_->setItemIcon(index, QIcon(swatch));
        color_->setItemData(index, preferences.annotationColors[i], Qt::ToolTipRole);
    }
    board_->setPreferences(preferences);
    setAnalysisMultiPv(preferences.analysisMultiPv);
    controller_.setOrientation(preferences.blackAtBottom);
    applySplit();
}
void AnalysisPage::setBoardDisplayOptions(bool coordinates, bool animation) {
    const QSignalBlocker coordinatesBlock(coordinates_);
    const QSignalBlocker animationBlock(animation_);
    preferences_.coordinates = coordinates;
    preferences_.animation = animation;
    coordinates_->setChecked(coordinates);
    animation_->setChecked(animation);
    board_->setDisplayOptions(coordinates, animation);
}
app::BoardPreferences AnalysisPage::preferences() const {
    auto result = preferences_;
    result.blackAtBottom = controller_.board().blackAtBottom;
    return result;
}
void AnalysisPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const auto orientation = width() < 800 ? Qt::Vertical : Qt::Horizontal;
    if (orientation != splitter_->orientation()) {
        const QSignalBlocker block(splitter_);
        splitter_->setOrientation(orientation);
        applySplit();
    }
}
void AnalysisPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    applySplit();
    refreshTabs();
}
void AnalysisPage::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (closeTab_) {
        closeTab_->setEnabled(false);
    }
}
void AnalysisPage::applySplit() {
    const QSignalBlocker block(splitter_);
    const int extent =
        splitter_->orientation() == Qt::Horizontal ? splitter_->width() : splitter_->height();
    const int board = extent * preferences_.analysisSplit / 100;
    splitter_->setSizes({board, extent - board});
    if (detailSplitter_) {
        const QSignalBlocker detailBlock(detailSplitter_);
        const auto detailExtent = detailSplitter_->height();
        const auto engine = detailExtent * preferences_.analysisDetailSplit / 100;
        detailSplitter_->setSizes({engine, detailExtent - engine});
    }
}
void AnalysisPage::setAnalysisMultiPv(int count) {
    preferences_.analysisMultiPv = count;
    if (engine_) {
        engine_->setMultiPv(count);
    }
}
void AnalysisPage::installReviewNavigation() {
    auto* page = qobject_cast<QVBoxLayout*>(layout());
    page->removeWidget(notice_);
    auto* header = new QHBoxLayout;
    header->setSpacing(14);
    auto* back = new QPushButton(tr("Back to Review"), this);
    back->setObjectName(QStringLiteral("backToReview"));
    back->setToolTip(tr("Return to the review position (Alt+Left)."));
    setQuietAction(*back);
    setIcon(*back, QStringLiteral("arrow-left"));
    header->addWidget(back, 0, Qt::AlignVCenter);
    auto* context = new QVBoxLayout;
    context->setSpacing(2);
    setTextRole(*notice_, "heading");
    context->addWidget(notice_);
    reviewPosition_ = new QLabel(this);
    reviewPosition_->setTextFormat(Qt::PlainText);
    reviewPosition_->setWordWrap(true);
    setTextRole(*reviewPosition_, "secondary");
    context->addWidget(reviewPosition_);
    header->addLayout(context, 1);
    unsavedReviewEdits_ = new QLabel(tr("Unsaved edits"), this);
    setTextRole(*unsavedReviewEdits_, "secondary");
    header->addWidget(unsavedReviewEdits_, 0, Qt::AlignVCenter);
    page->insertLayout(0, header);

    auto* shortcut = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Left), this);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, back, &QPushButton::click);
    connect(back, &QPushButton::clicked, this, &AnalysisPage::backToReviewRequested);
}
void AnalysisPage::setReviewContext(const QString& game, const QString& position) {
    reviewGame_ = game;
    reviewPosition_->setText(position);
    refresh();
}
void AnalysisPage::refreshBoard() {
    displayingPreview_ = engine_ && engine_->preview();
    if (displayingPreview_) {
        auto state = engine_->previewBoard().value_or(controller_.board());
        state.interactive = false;
        state.selection.reset();
        state.annotations.clear();
        state.positionDescription = tr("Engine preview (%1)").arg(state.positionDescription);
        board_->setState(state);
        status_->setText(state.positionDescription);
        return;
    }
    board_->setState(controller_.board());
    status_->setText(!controller_.error().isEmpty() ? controller_.error()
                     : controller_.busy()           ? tr("Updating analysis…")
                                                    : controller_.board().positionDescription);
}
void AnalysisPage::refresh() {
    refreshBoard();
    preferences_.blackAtBottom = controller_.board().blackAtBottom;
    undo_->setEnabled(!displayingPreview_ && controller_.board().interactive &&
                      controller_.canUndoAnnotation());
    redo_->setEnabled(!displayingPreview_ && controller_.board().interactive &&
                      controller_.canRedoAnnotation());
    const auto white =
        controller_.whitePlayer().isEmpty() ? tr("White") : controller_.whitePlayer();
    const auto black =
        controller_.blackPlayer().isEmpty() ? tr("Black") : controller_.blackPlayer();
    topPlayer_->setText(controller_.board().blackAtBottom ? white : black);
    bottomPlayer_->setText(controller_.board().blackAtBottom ? black : white);
    const auto target = controller_.target(controller_.currentNode());
    edit_->setEnabled(!displayingPreview_ && controller_.canEdit(target));
    variations_->setEnabled(!displayingPreview_ && controller_.canDelete(target));
    save_->setEnabled(controller_.canEdit(target));
    if (reviewPosition_) {
        notice_->setText(reviewGame_.isEmpty() ? tr("Review analysis") : reviewGame_);
        unsavedReviewEdits_->setVisible(controller_.hasContent());
    } else {
        const auto game = controller_.gameId() ? controller_.gameDescription() : tr("Analysis");
        notice_->setText(controller_.hasContent() ? tr("%1 (unsaved edits)").arg(game) : game);
    }
    if (controller_.hasContent()) {
        operationNotice_->hide();
    }
    if (notes_->toPlainText() != controller_.positionNotes()) {
        notes_->setPlainText(controller_.positionNotes());
    }
    notes_->setVisible(!controller_.positionNotes().isEmpty());
    QAccessibleEvent event(status_, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&event);
    refreshTabs();
    if (tabAction_ && !controller_.busy()) {
        QTimer::singleShot(0, this, &AnalysisPage::finishTabAction);
    }
}
void AnalysisPage::setPosition() {
    if (controller_.busy()) {
        return;
    }
    const auto& current = controller_.position();
    const auto initial = current ? QString::fromStdString(current->fen()) : QString{};
    useTab(0, [this, initial] { requestDiscard([this, initial] { editPosition(initial); }); });
}
void AnalysisPage::editPosition(const QString& initialFen) {
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Set Analysis Position"));
    auto* layout = new QVBoxLayout(dialog);
    auto* label = new QLabel(tr("Edit or paste a standard position in &FEN notation."), dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* fen = new QLineEdit(dialog);
    fen->setObjectName(QStringLiteral("analysisFen"));
    // Keep one extra character so truncation cannot turn oversize input into valid FEN.
    fen->setMaxLength(static_cast<int>(chess::maximumFenBytes + 1));
    fen->setText(initialFen);
    fen->selectAll();
    label->setBuddy(fen);
    layout->addWidget(fen);
    auto* status = new QLabel(dialog);
    status->setObjectName(QStringLiteral("analysisFenStatus"));
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
    auto* apply = buttons->addButton(tr("Set Position"), QDialogButtonBox::AcceptRole);
    apply->setObjectName(QStringLiteral("applyAnalysisFen"));
    apply->setDefault(true);
    apply->setEnabled(!fen->text().isEmpty());
    layout->addWidget(buttons);
    connect(fen, &QLineEdit::textChanged, apply,
            [apply](const QString& value) { apply->setEnabled(!value.isEmpty()); });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, fen, status, apply] {
        if (controller_.busy()) {
            return;
        }
        apply->setEnabled(false);
        fen->setReadOnly(true);
        status->setText(tr("Checking position…"));
        controller_.reset(fen->text().toStdString());
    });
    connect(&controller_, &app::AnalysisController::changed, dialog,
            [this, dialog, fen, status, apply] {
                if (!fen->isReadOnly() || controller_.busy()) {
                    return;
                }
                if (controller_.error().isEmpty()) {
                    dialog->accept();
                    return;
                }
                status->setText(controller_.error());
                fen->setReadOnly(false);
                fen->setFocus();
                apply->setEnabled(true);
            });
    dialog->resize(550, dialog->sizeHint().height());
    dialog->open();
}
void AnalysisPage::editComment(app::NavigationTarget target) {
    if (!controller_.canEdit(target) || target.node != controller_.currentNode()) {
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Edit Position Note"));
    auto* layout = new QVBoxLayout(dialog);
    const auto makeEditor = [dialog, layout](const QString& labelText, const QString& value) {
        auto* label = new QLabel(labelText, dialog);
        auto* editor = new QPlainTextEdit(value, dialog);
        label->setBuddy(editor);
        layout->addWidget(label);
        layout->addWidget(editor, 1);
        return editor;
    };
    const auto& notes = controller_.notes();
    QPlainTextEdit* before = nullptr;
    const bool move = controller_.canDelete(target);
    if (move || !notes.before.isEmpty()) {
        before = makeEditor(tr("Comment &before the move"), notes.before);
    }
    auto* after =
        makeEditor(move ? tr("Comment &after the move") : tr("&Position comment"), notes.after);
    after->setObjectName(QStringLiteral("analysisCommentEditor"));
    QLineEdit* nags = nullptr;
    if (move) {
        auto* nagLabel = new QLabel(tr("Numeric &glyphs"), dialog);
        nags = new QLineEdit(dialog);
        nags->setPlaceholderText(tr("For example, $1 $14"));
        nags->setToolTip(tr("PGN numeric annotation glyphs from $0 to $255, separated by spaces."));
        QStringList numbers;
        for (const auto nag : notes.nags) {
            numbers.append(QStringLiteral("$%1").arg(nag));
        }
        nags->setText(numbers.join(u' '));
        nagLabel->setBuddy(nags);
        auto* glyphRow = new QHBoxLayout;
        glyphRow->addWidget(nagLabel);
        glyphRow->addWidget(nags);
        layout->addLayout(glyphRow);
    }
    auto* error = new QLabel(dialog);
    error->setWordWrap(true);
    layout->addWidget(error);
    error->hide();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [=, this] {
        app::AnalysisNotes edited{
            before ? before->toPlainText() : QString{}, after->toPlainText(), {}};
        constexpr auto commentLimit =
            static_cast<qsizetype>(chess::GameTreeLimits{}.maxCommentBytes);
        if (edited.before.toUtf8().size() > commentLimit ||
            edited.after.toUtf8().size() > commentLimit) {
            error->setText(tr("Each PGN comment can contain up to 64 KiB of text."));
            error->show();
            return;
        }
        if (nags) {
            const auto entries =
                nags->text().split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            for (auto entry : entries) {
                if (entry.startsWith(u'$')) {
                    entry.remove(0, 1);
                }
                bool valid = false;
                const auto value = entry.toUInt(&valid);
                if (!valid || value > 255) {
                    error->setText(tr("Enter glyph numbers from 0 to 255, separated by spaces."));
                    error->show();
                    nags->setFocus();
                    return;
                }
                edited.nags.push_back(static_cast<std::uint8_t>(value));
            }
        }
        controller_.setNotes(target, edited);
        dialog->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    dialog->resize(520, move ? 450 : 280);
    after->setFocus();
    dialog->open();
}
void AnalysisPage::showVariationMenu(app::NavigationTarget target, const QPoint& globalPosition) {
    if (displayingPreview_ || !controller_.canEdit(target)) {
        return;
    }
    auto* menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (target.node == controller_.currentNode()) {
        auto* note = menu->addAction(tr("Edit Note…"));
        connect(note, &QAction::triggered, this, [this, target] { editComment(target); });
        menu->addSeparator();
    }
    auto* promote = menu->addAction(tr("Promote to Main Continuation"));
    promote->setEnabled(controller_.canPromote(target));
    connect(promote, &QAction::triggered, this,
            [this, target] { controller_.promoteVariation(target); });
    auto* remove = menu->addAction(tr("Delete Line…"));
    remove->setEnabled(controller_.canDelete(target));
    connect(remove, &QAction::triggered, this, [this, target] { confirmDelete(target); });
    menu->popup(globalPosition);
}
void AnalysisPage::confirmDelete(app::NavigationTarget target) {
    if (!controller_.canDelete(target)) {
        return;
    }
    auto* dialog =
        new QMessageBox(QMessageBox::Question, tr("Delete variation?"),
                        tr("Delete %1 and all of its continuations?")
                            .arg(controller_.navigationTree()->nodes.at(target.node).label),
                        QMessageBox::Yes | QMessageBox::Cancel, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setTextFormat(Qt::PlainText);
    dialog->setDefaultButton(QMessageBox::Cancel);
    connect(dialog, &QDialog::finished, this, [this, target](int result) {
        if (result == QMessageBox::Yes) {
            controller_.deleteVariation(target);
        }
    });
    dialog->open();
}
void AnalysisPage::savePgn(std::function<void()> onSaved) {
    if (!controller_.canEdit(controller_.target(controller_.currentNode()))) {
        return;
    }
    auto* dialog =
        new QFileDialog(this, tr("Save Analysis PGN"), QString{}, tr("PGN files (*.pgn)"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setDefaultSuffix(QStringLiteral("pgn"));
    dialog->selectFile(QStringLiteral("analysis.pgn"));
    connect(dialog, &QFileDialog::fileSelected, this,
            [this, onSaved = std::move(onSaved)](const QString& path) mutable {
                if (controller_.canEdit(controller_.target(controller_.currentNode()))) {
                    afterExport_ = std::move(onSaved);
                    controller_.exportPgn(path);
                }
            });
    dialog->open();
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
    auto* dialog = new QMessageBox(
        QMessageBox::Question, tr("Discard analysis?"),
        controller_.activeTab() > 0
            ? tr("Save a PGN copy of your edits to %1 before leaving?")
                  .arg(controller_.gameDescription())
            : tr("Save a PGN copy of your variations and annotations before leaving?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    dialog->setTextFormat(Qt::PlainText);
    dialog->button(QMessageBox::Save)->setText(tr("Save PGN…"));
    auto* saveBeforeDiscard = dialog->button(QMessageBox::Save);
    const auto updateSave = [this, saveBeforeDiscard] {
        saveBeforeDiscard->setEnabled(
            controller_.canEdit(controller_.target(controller_.currentNode())));
    };
    connect(&controller_, &app::AnalysisController::changed, dialog, updateSave);
    updateSave();
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("discardAnalysisDialog"));
    dialog->setDefaultButton(QMessageBox::Cancel);
    connect(dialog, &QDialog::finished, this,
            [this, onDiscard = std::move(onDiscard)](int result) mutable {
                confirming_ = false;
                if (result == QMessageBox::Save) {
                    savePgn(std::move(onDiscard));
                } else if (result == QMessageBox::Discard) {
                    onDiscard();
                }
            });
    dialog->open();
}
void AnalysisPage::deactivate() {
    auto state = controller_.board();
    state.interactive = false;
    board_->setState(state);
    tabAction_.reset();
    if (closeTab_) {
        closeTab_->setEnabled(false);
    }
    setEnabled(false);
}
} // namespace chess_review::ui
