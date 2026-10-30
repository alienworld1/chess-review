#include "ui/review/ReviewPage.h"
#include "app/ReviewMovePresentation.h"
#include "ui/EngineText.h"
#include "ui/chess/ChessBoardWidget.h"
#include "ui/chess/MoveTimeline.h"
#include <QAbstractTableModel>
#include <QAccessible>
#include <QColor>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringList>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>

namespace chess_review::ui {
namespace {
QColor reviewMoveColor(const persistence::ReviewMove* move, const QPalette& palette) {
    if (!move || !move->classification || !move->classification->label) {
        return palette.color(QPalette::Mid);
    }
    const bool dark = palette.color(QPalette::Window).lightness() < 128;
    switch (*move->classification->label) {
    case review::Classification::Best:
        return QColor(dark ? "#65c486" : "#227843");
    case review::Classification::Excellent:
        return QColor(dark ? "#62bac8" : "#25808e");
    case review::Classification::Good:
        return QColor(dark ? "#65c486" : "#227843");
    case review::Classification::Inaccuracy:
        return QColor(dark ? "#f0c46c" : "#9d660b");
    case review::Classification::Mistake:
        return QColor(dark ? "#f2a65e" : "#b45d18");
    case review::Classification::Blunder:
        return QColor(dark ? "#f07780" : "#c53d49");
    }
    return palette.color(QPalette::Mid);
}

QString sideText(chess::Color side) {
    return side == chess::Color::White ? QObject::tr("White") : QObject::tr("Black");
}
QString qualityText(review::Quality quality) {
    switch (quality) {
    case review::Quality::Fast:
        return QObject::tr("Fast");
    case review::Quality::Standard:
        return QObject::tr("Standard");
    case review::Quality::Thorough:
        return QObject::tr("Thorough");
    }
    return {};
}
QString phaseText(app::ReviewPhase phase) {
    switch (phase) {
    case app::ReviewPhase::Idle:
        return QObject::tr("Choose a player, engine and quality.");
    case app::ReviewPhase::Loading:
        return QObject::tr("Loading saved data…");
    case app::ReviewPhase::Connecting:
        return QObject::tr("Connecting review engine…");
    case app::ReviewPhase::Preparing:
        return QObject::tr("Preparing mainline position…");
    case app::ReviewPhase::LookingUp:
        return QObject::tr("Checking saved evidence…");
    case app::ReviewPhase::Scanning:
        return QObject::tr("Quick scanning… (may wait for manual analysis)");
    case app::ReviewPhase::Refining:
        return QObject::tr("Refining selected moments… (may wait for manual analysis)");
    case app::ReviewPhase::Saving:
        return QObject::tr("Saving evidence…");
    case app::ReviewPhase::Finished:
        return {};
    }
    return {};
}
} // namespace
class ReviewRunsModel final : public QAbstractTableModel {
  public:
    explicit ReviewRunsModel(QObject* parent) : QAbstractTableModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 5;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 ||
            section >= 5) {
            return {};
        }
        return QStringList{tr("Status"), tr("Player"), tr("Quality"), tr("Created (local time)"),
                           tr("Positions saved")}
            .at(section);
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() ||
            index.column() < 0 || index.column() >= 5 ||
            (role != Qt::DisplayRole && role != Qt::AccessibleTextRole)) {
            return {};
        }
        const auto& row = rows_[static_cast<std::size_t>(index.row())];
        if (row.metadataError) {
            return index.column() == 0 ? tr("Unreadable run (ID %1)").arg(row.id) : QString{};
        }
        return QStringList{app::reviewStatusText(row.status), sideText(row.selectedSide),
                           qualityText(row.quality),
                           row.createdAt.toLocalTime().toString(Qt::ISODate),
                           tr("%1 / %2").arg(row.completedPositions).arg(row.totalPositions)}
            .at(index.column());
    }
    std::optional<persistence::ReviewSummary> row(int index) const {
        if (index < 0 || index >= rowCount()) {
            return {};
        }
        return rows_[static_cast<std::size_t>(index)];
    }
    void reload(const std::vector<persistence::ReviewSummary>& rows) {
        if (rows_.size() == rows.size() &&
            std::equal(rows_.begin(), rows_.end(), rows.begin(), [](const auto& a, const auto& b) {
                return a.id == b.id && a.revision == b.revision &&
                       a.metadataError == b.metadataError;
            })) {
            return;
        }
        beginResetModel();
        rows_ = rows;
        endResetModel();
    }

  private:
    std::vector<persistence::ReviewSummary> rows_;
};
class ReviewMovesModel final : public QAbstractTableModel {
  public:
    explicit ReviewMovesModel(QObject* parent) : QAbstractTableModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 4;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 ||
            section >= 4) {
            return {};
        }
        return QStringList{tr("Move"), tr("Player"), tr("Played"), tr("Classification")}.at(
            section);
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() ||
            index.column() < 0 || index.column() >= 4 ||
            (role != Qt::DisplayRole && role != Qt::AccessibleTextRole)) {
            return {};
        }
        const auto& row = rows_[static_cast<std::size_t>(index.row())];
        return QStringList{
            QString::number(row.moveNumber) +
                (row.mover == chess::Color::White ? QStringLiteral(".") : QStringLiteral("...")),
            sideText(row.mover), row.san,
            app::classificationText(row.classification) +
                (row.classification && row.classification->label
                     ? (row.classification->stable ? tr(" (stable)") : tr(" (provisional)"))
                     : QString{}) +
                (row.contextPosition
                     ? tr(" · after opponent error at position %1").arg(*row.contextPosition)
                     : QString{})}
            .at(index.column());
    }
    void reload(const std::optional<persistence::ReviewMovePage>& page) {
        if (page && id_ == page->reviewId && revision_ == page->revision &&
            offset_ == page->offset) {
            return;
        }
        if (!page && rows_.empty()) {
            return;
        }
        beginResetModel();
        rows_ = page ? page->moves : std::vector<persistence::ReviewMove>{};
        id_ = page ? page->reviewId : 0;
        revision_ = page ? page->revision : -1;
        offset_ = page ? page->offset : -1;
        endResetModel();
    }

  private:
    std::vector<persistence::ReviewMove> rows_;
    review::ReviewId id_ = 0;
    std::int64_t revision_ = -1;
    int offset_ = -1;
};
ReviewPage::ReviewPage(app::ReviewGameController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setObjectName(QStringLiteral("reviewPage"));
    auto& navigation = controller_.navigation();
    auto* layout = new QVBoxLayout(this);
    auto* header = new QHBoxLayout;
    auto* back = new QPushButton(tr("Back to Games"), this);
    back->setObjectName(QStringLiteral("reviewBack"));
    header->addWidget(back);
    game_ = new QLabel(this);
    game_->setTextFormat(Qt::PlainText);
    game_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    header->addWidget(game_, 1);
    layout->addLayout(header);
    connect(back, &QPushButton::clicked, this, &ReviewPage::backRequested);
    compactSummary_ = new QLabel(this);
    compactSummary_->setObjectName(QStringLiteral("reviewCompactSummary"));
    compactSummary_->setTextFormat(Qt::PlainText);
    compactSummary_->setWordWrap(true);
    layout->addWidget(compactSummary_);
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setObjectName(QStringLiteral("reviewSplitter"));
    splitter_->setChildrenCollapsible(false);
    layout->addWidget(splitter_, 1);
    auto* left = new QWidget(splitter_);
    auto* boardLayout = new QVBoxLayout(left);
    boardLayout->setContentsMargins(0, 0, 0, 0);
    topPlayer_ = new QLabel(left);
    topPlayer_->setObjectName(QStringLiteral("reviewTopPlayer"));
    topPlayer_->setTextFormat(Qt::PlainText);
    topPlayer_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    boardLayout->addWidget(topPlayer_);
    auto* boardRow = new QHBoxLayout;
    evaluation_ = new QProgressBar(left);
    evaluation_->setObjectName(QStringLiteral("reviewEvaluationBar"));
    evaluation_->setOrientation(Qt::Vertical);
    evaluation_->setRange(0, 1000);
    evaluation_->setTextVisible(false);
    evaluation_->setFixedWidth(18);
    evaluation_->setAccessibleName(tr("White expected score, not win probability"));
    boardRow->addWidget(evaluation_);
    board_ = new ChessBoardWidget(left);
    board_->setObjectName(QStringLiteral("reviewBoard"));
    board_->setMinimumSize(248, 248);
    board_->setToolTip(
        tr("Read-only original game. Use the timeline or navigation buttons to explore moves."));
    boardRow->addWidget(board_, 1);
    boardLayout->addLayout(boardRow, 1);
    bottomPlayer_ = new QLabel(left);
    bottomPlayer_->setObjectName(QStringLiteral("reviewBottomPlayer"));
    bottomPlayer_->setTextFormat(Qt::PlainText);
    bottomPlayer_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    boardLayout->addWidget(bottomPlayer_);
    evaluationText_ = new QLabel(left);
    evaluationText_->setObjectName(QStringLiteral("reviewEvaluationText"));
    evaluationText_->setWordWrap(true);
    boardLayout->addWidget(evaluationText_);
    previewLabel_ = new QLabel(left);
    previewLabel_->setObjectName(QStringLiteral("reviewPreview"));
    previewLabel_->setWordWrap(true);
    boardLayout->addWidget(previewLabel_);
    auto* actions = new QHBoxLayout;
    const auto button = [this, actions](const QString& text, const QString& name) {
        auto* result = new QPushButton(text, this);
        result->setObjectName(name);
        actions->addWidget(result);
        return result;
    };
    firstMove_ = button(tr("Start"), QStringLiteral("reviewFirstMove"));
    previousMove_ = button(tr("Previous"), QStringLiteral("reviewPreviousMove"));
    nextMove_ = button(tr("Next"), QStringLiteral("reviewNextMove"));
    lastMove_ = button(tr("End"), QStringLiteral("reviewLastMove"));
    auto* flip = button(tr("Flip Board"), QStringLiteral("reviewFlipBoard"));
    boardLayout->addLayout(actions);
    connect(firstMove_, &QPushButton::clicked, &controller_.exploration(),
            &app::ReviewExplorationController::beginning);
    connect(previousMove_, &QPushButton::clicked, &controller_.exploration(),
            &app::ReviewExplorationController::previous);
    connect(nextMove_, &QPushButton::clicked, &controller_.exploration(),
            &app::ReviewExplorationController::next);
    connect(lastMove_, &QPushButton::clicked, &controller_.exploration(),
            &app::ReviewExplorationController::end);
    connect(flip, &QPushButton::clicked, &controller_.exploration(),
            &app::ReviewExplorationController::flip);
    connect(board_, &ChessBoardWidget::previousRequested, &controller_.exploration(),
            &app::ReviewExplorationController::previous);
    connect(board_, &ChessBoardWidget::nextRequested, &controller_.exploration(),
            &app::ReviewExplorationController::next);
    connect(board_, &ChessBoardWidget::beginningRequested, &controller_.exploration(),
            &app::ReviewExplorationController::beginning);
    connect(board_, &ChessBoardWidget::endRequested, &controller_.exploration(),
            &app::ReviewExplorationController::end);
    connect(&controller_.exploration(), &app::ReviewExplorationController::moveFeedback, board_,
            &ChessBoardWidget::flashMove);
    connect(board_, &ChessBoardWidget::squareSelected, &controller_.exploration(),
            &app::ReviewExplorationController::selectSquare);
    connect(board_, &ChessBoardWidget::selectionCleared, &controller_.exploration(),
            &app::ReviewExplorationController::clearSelection);
    connect(board_, &ChessBoardWidget::moveAttempted, &controller_.exploration(),
            &app::ReviewExplorationController::attemptMove);
    timeline_ = new MoveTimeline(left);
    timeline_->setObjectName(QStringLiteral("reviewTimeline"));
    timeline_->setAccessibleName(
        tr("Complete original game; hover previews, click selects; Left/Right navigates"));
    timeline_->setMaximumHeight(110);
    timeline_->setHoverPreview(true);
    boardLayout->addWidget(timeline_);
    const auto positionFor =
        [&navigation](app::NavigationTarget target) -> std::optional<app::ReviewTarget> {
        if (target.session != navigation.target(0).session) {
            return {};
        }
        const auto found = navigation.positionFor(target.node);
        return found ? std::optional{navigation.target(*found)} : std::nullopt;
    };
    connect(timeline_, &MoveTimeline::navigationRequested, this,
            [&navigation, positionFor](app::NavigationTarget target) {
                if (const auto position = positionFor(target)) {
                    navigation.select(*position);
                }
            });
    connect(timeline_, &MoveTimeline::previewRequested, this,
            [this, &navigation, positionFor](app::NavigationTarget target) {
                if (!controller_.exploration().active()) {
                    if (const auto position = positionFor(target)) {
                        navigation.preview(*position);
                    }
                }
            });
    connect(timeline_, &MoveTimeline::visibleRequested, this,
            [&navigation, positionFor](app::NavigationTarget first, app::NavigationTarget last) {
                const auto begin = positionFor(first);
                const auto end = positionFor(last);
                if (begin && end) {
                    navigation.visible(*begin, *end);
                }
            });
    connect(timeline_, &MoveTimeline::previewCleared, &navigation,
            &app::ReviewNavigationController::clearPreview);
    connect(timeline_, &MoveTimeline::previousRequested, &navigation,
            &app::ReviewNavigationController::previous);
    connect(timeline_, &MoveTimeline::nextRequested, &navigation,
            &app::ReviewNavigationController::next);
    connect(timeline_, &MoveTimeline::beginningRequested, &navigation,
            &app::ReviewNavigationController::beginning);
    connect(timeline_, &MoveTimeline::endRequested, &navigation,
            &app::ReviewNavigationController::end);
    timeline_->setMoveDescription([&navigation](chess::NodeId node) {
        const auto position = navigation.positionFor(node);
        if (!position || *position == 0) {
            return QString{};
        }
        return app::reviewMoveMarker(navigation.move(*position)) +
               (navigation.keyMoment(*position) ? tr(" · ◆ Key moment") : QString{});
    });
    timeline_->setMoveAccent([this, &navigation](chess::NodeId node) {
        const auto position = navigation.positionFor(node);
        return position && *position > 0
                   ? reviewMoveColor(navigation.move(*position), timeline_->palette())
                   : QColor{};
    });
    for (auto* control : {firstMove_, previousMove_, nextMove_, lastMove_, flip}) {
        control->installEventFilter(this);
    }
    auto* scroll = new QScrollArea(splitter_);
    scroll->setObjectName(QStringLiteral("reviewExplanationScroll"));
    scroll->setWidgetResizable(true);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* right = new QWidget(scroll);
    right->setMinimumWidth(300);
    scroll->setWidget(right);
    auto* rightLayout = new QVBoxLayout(right);
    auto* momentRow = new QHBoxLayout;
    rail_ = new QFrame(right);
    rail_->setFixedWidth(4);
    rail_->setAutoFillBackground(true);
    momentRow->addWidget(rail_);
    auto* moment = new QVBoxLayout;
    headline_ = new QLabel(right);
    headline_->setObjectName(QStringLiteral("reviewMomentHeadline"));
    headline_->setTextFormat(Qt::PlainText);
    headline_->setWordWrap(true);
    auto headingFont = headline_->font();
    headingFont.setBold(true);
    headline_->setFont(headingFont);
    moment->addWidget(headline_);
    explanation_ = new QLabel(right);
    explanation_->setObjectName(QStringLiteral("reviewMomentExplanation"));
    explanation_->setTextFormat(Qt::PlainText);
    explanation_->setWordWrap(true);
    explanation_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    moment->addWidget(explanation_);
    momentRow->addLayout(moment, 1);
    rightLayout->addLayout(momentRow);
    candidates_ = new QComboBox(right);
    candidates_->setObjectName(QStringLiteral("reviewCandidates"));
    candidates_->setAccessibleName(tr("Saved candidate continuations and root evaluations"));
    candidates_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    candidates_->setMinimumContentsLength(12);
    rightLayout->addWidget(candidates_);
    explorationStatus_ = new QLabel(right);
    explorationStatus_->setObjectName(QStringLiteral("reviewExplorationStatus"));
    explorationStatus_->setTextFormat(Qt::PlainText);
    explorationStatus_->setWordWrap(true);
    rightLayout->addWidget(explorationStatus_);
    auto* explorationActions = new QGridLayout;
    const auto action = [right, explorationActions](const QString& text, const QString& name,
                                                    int row, int column) {
        auto* control = new QPushButton(text, right);
        control->setObjectName(name);
        explorationActions->addWidget(control, row, column);
        return control;
    };
    showLine_ = action(tr("Show Line"), QStringLiteral("reviewShowLine"), 0, 0);
    tryAgain_ = action(tr("Try Again"), QStringLiteral("reviewTryAgain"), 0, 1);
    analyze_ = action(tr("Analyze Position"), QStringLiteral("reviewAnalyzePosition"), 1, 0);
    reloadExploration_ =
        action(tr("Reload candidates"), QStringLiteral("reviewReloadCandidates"), 1, 1);
    returnGame_ = action(tr("Return to Game"), QStringLiteral("reviewReturnToGame"), 2, 0);
    hint_ = action(tr("Hint"), QStringLiteral("reviewHint"), 3, 0);
    reveal_ = action(tr("Show Solution"), QStringLiteral("reviewShowSolution"), 3, 1);
    rightLayout->addLayout(explorationActions);
    auto& exploration = controller_.exploration();
    connect(showLine_, &QPushButton::clicked, this,
            [this] { controller_.exploration().showLine(candidates_->currentIndex()); });
    connect(candidates_, &QComboBox::activated, this, [this](int index) {
        if (controller_.exploration().state().mode == app::ExplorationMode::Line) {
            controller_.exploration().showLine(index);
        }
    });
    connect(tryAgain_, &QPushButton::clicked, &exploration,
            &app::ReviewExplorationController::tryAgain);
    connect(returnGame_, &QPushButton::clicked, &exploration,
            &app::ReviewExplorationController::returnToGame);
    connect(reveal_, &QPushButton::clicked, &exploration,
            &app::ReviewExplorationController::returnToGame);
    connect(hint_, &QPushButton::clicked, &exploration, &app::ReviewExplorationController::hint);
    connect(reloadExploration_, &QPushButton::clicked, &exploration,
            &app::ReviewExplorationController::reload);
    connect(analyze_, &QPushButton::clicked, this, &ReviewPage::analyzeRequested);
    connect(&exploration, &app::ReviewExplorationController::changed, this,
            &ReviewPage::updateNavigation);
    auto* keyActions = new QHBoxLayout;
    previousMoment_ = new QPushButton(tr("Previous key moment"), right);
    nextMoment_ = new QPushButton(tr("Next key moment"), right);
    previousMoment_->setObjectName(QStringLiteral("reviewPreviousMoment"));
    nextMoment_->setObjectName(QStringLiteral("reviewNextMoment"));
    keyActions->addWidget(previousMoment_);
    keyActions->addWidget(nextMoment_);
    rightLayout->addLayout(keyActions);
    connect(previousMoment_, &QPushButton::clicked, &navigation,
            &app::ReviewNavigationController::previousMoment);
    connect(nextMoment_, &QPushButton::clicked, &navigation,
            &app::ReviewNavigationController::nextMoment);
    const auto section = [this, right, rightLayout](const QString& title, bool expanded) {
        auto* toggle = new QPushButton(title, right);
        toggle->setCheckable(true);
        toggle->setChecked(expanded);
        auto* content = new QWidget(right);
        content->setVisible(expanded);
        rightLayout->addWidget(toggle);
        rightLayout->addWidget(content);
        connect(toggle, &QPushButton::toggled, content, &QWidget::setVisible);
        sections_.emplace_back(toggle, content);
        return std::pair{toggle, content};
    };
    const auto setupSection = section(tr("Review setup"), true);
    setupToggle_ = setupSection.first;
    setup_ = setupSection.second;
    auto* setupLayout = new QVBoxLayout(setup_);
    auto* form = new QFormLayout;
    side_ = new QComboBox(setup_);
    side_->setObjectName(QStringLiteral("reviewSide"));
    side_->addItems({tr("Choose player…"), tr("White"), tr("Black")});
    engine_ = new QComboBox(setup_);
    engine_->setObjectName(QStringLiteral("reviewEngine"));
    engine_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    engine_->setMinimumContentsLength(12);
    quality_ = new QComboBox(setup_);
    quality_->setObjectName(QStringLiteral("reviewQuality"));
    quality_->addItems({tr("Fast"), tr("Standard"), tr("Thorough")});
    quality_->setCurrentIndex(static_cast<int>(controller.defaultQuality()));
    form->addRow(tr("&Player:"), side_);
    form->addRow(tr("&Engine:"), engine_);
    form->addRow(tr("&Quality:"), quality_);
    setupLayout->addLayout(form);
    for (auto* control : {side_, engine_, quality_}) {
        control->installEventFilter(this);
    }
    reason_ = new QLabel(setup_);
    reason_->setTextFormat(Qt::PlainText);
    reason_->setWordWrap(true);
    setupLayout->addWidget(reason_);
    start_ = new QPushButton(tr("Run Review"), setup_);
    start_->setObjectName(QStringLiteral("runReview"));
    setupLayout->addWidget(start_);
    auto* settings = new QPushButton(tr("Manage Engines…"), setup_);
    setupLayout->addWidget(settings);
    connect(settings, &QPushButton::clicked, this, &ReviewPage::settingsRequested);
    cancel_ = new QPushButton(tr("Cancel review"), right);
    cancel_->setObjectName(QStringLiteral("cancelReview"));
    rightLayout->addWidget(cancel_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("reviewStatus"));
    status_->setAccessibleName(tr("Review progress"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    layout->addWidget(status_);
    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("reviewProgress"));
    progress_->setAccessibleName(tr("Positions durably saved"));
    layout->addWidget(progress_);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("reviewError"));
    error_->setTextFormat(Qt::PlainText);
    error_->setWordWrap(true);
    error_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(error_);
    auto* keySection = section(tr("Chronological key moments"), false).second;
    auto* keyLayout = new QVBoxLayout(keySection);
    moments_ = new QTableView(keySection);
    moments_->setObjectName(QStringLiteral("reviewKeyMoments"));
    moments_->setAccessibleName(tr("Chronological key moments"));
    momentsModel_ = new ReviewMovesModel(this);
    moments_->setModel(momentsModel_);
    moments_->setSelectionBehavior(QAbstractItemView::SelectRows);
    moments_->setSelectionMode(QAbstractItemView::SingleSelection);
    moments_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    moments_->horizontalHeader()->setStretchLastSection(true);
    keyLayout->addWidget(moments_);
    auto* keyPaging = new QHBoxLayout;
    previousMoments_ = new QPushButton(tr("Earlier moments"), keySection);
    nextMoments_ = new QPushButton(tr("Later moments"), keySection);
    keyPaging->addWidget(previousMoments_);
    keyPaging->addWidget(nextMoments_);
    keyLayout->addLayout(keyPaging);
    connect(previousMoments_, &QPushButton::clicked, this, [this] {
        if (controller_.state().overview) {
            controller_.refreshOverview(controller_.state().overview->keyOffset - 50);
        }
    });
    connect(nextMoments_, &QPushButton::clicked, this, [this] {
        if (controller_.state().overview) {
            controller_.refreshOverview(controller_.state().overview->keyOffset + 50);
        }
    });
    const auto openMoment = [this](const QModelIndex& index) {
        const auto& overview = controller_.state().overview;
        if (overview && index.isValid() &&
            index.row() < static_cast<int>(overview->keyMoments.size())) {
            controller_.selectMoment(
                overview->keyMoments[static_cast<std::size_t>(index.row())].positionIndex);
        }
    };
    connect(moments_, &QTableView::clicked, this, openMoment);
    connect(moments_, &QTableView::activated, this, openMoment);
    const auto detail = section(tr("Summary and evidence details"), false);
    detail.first->setObjectName(QStringLiteral("reviewDetailsToggle"));
    auto* detailSection = detail.second;
    auto* detailLayout = new QVBoxLayout(detailSection);
    auto* tabs = new QTabWidget(detailSection);
    tabs->setObjectName(QStringLiteral("reviewResultsTabs"));
    detailLayout->addWidget(tabs);
    auto* summaryTab = new QWidget(tabs);
    auto* summaryLayout = new QVBoxLayout(summaryTab);
    summary_ = new QPlainTextEdit(summaryTab);
    summary_->setObjectName(QStringLiteral("reviewSummary"));
    summary_->setAccessibleName(tr("Game accuracy, coverage and factual summary"));
    summary_->setReadOnly(true);
    summaryLayout->addWidget(summary_);
    refreshSummary_ = new QPushButton(tr("Refresh summary"), summaryTab);
    refreshSummary_->setObjectName(QStringLiteral("refreshReviewSummary"));
    summaryLayout->addWidget(refreshSummary_);
    connect(refreshSummary_, &QPushButton::clicked, this,
            [this] { controller_.refreshOverview(); });
    tabs->addTab(summaryTab, tr("Summary"));
    auto* movesTab = new QWidget(tabs);
    auto* movesLayout = new QVBoxLayout(movesTab);
    moves_ = new QTableView(movesTab);
    moves_->setObjectName(QStringLiteral("reviewMoves"));
    moves_->setAccessibleName(tr("Saved move evidence page"));
    movesModel_ = new ReviewMovesModel(this);
    moves_->setModel(movesModel_);
    moves_->setSelectionBehavior(QAbstractItemView::SelectRows);
    moves_->setSelectionMode(QAbstractItemView::SingleSelection);
    moves_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    moves_->horizontalHeader()->setStretchLastSection(true);
    movesLayout->addWidget(moves_);
    details_ = new QPlainTextEdit(movesTab);
    details_->setObjectName(QStringLiteral("reviewMoveDetails"));
    details_->setAccessibleName(tr("Selected move evidence and rationale"));
    details_->setReadOnly(true);
    movesLayout->addWidget(details_);
    auto* paging = new QHBoxLayout;
    previousMoves_ = new QPushButton(tr("Previous moves"), movesTab);
    nextMoves_ = new QPushButton(tr("Next moves"), movesTab);
    previousMoves_->setObjectName(QStringLiteral("previousReviewMoves"));
    nextMoves_->setObjectName(QStringLiteral("nextReviewMoves"));
    paging->addWidget(previousMoves_);
    paging->addWidget(nextMoves_);
    movesLayout->addLayout(paging);
    moveStatus_ = new QLabel(right);
    moveStatus_->setObjectName(QStringLiteral("reviewMoveStatus"));
    moveStatus_->setTextFormat(Qt::PlainText);
    moveStatus_->setWordWrap(true);
    rightLayout->addWidget(moveStatus_);
    retryMoves_ = new QPushButton(tr("Retry move evidence"), right);
    retryMoves_->setObjectName(QStringLiteral("retryReviewMoves"));
    rightLayout->addWidget(retryMoves_);
    connect(retryMoves_, &QPushButton::clicked, &navigation,
            &app::ReviewNavigationController::retryEvidence);
    connect(previousMoves_, &QPushButton::clicked, this, [this] {
        controller_.selectMoment(std::max(1, controller_.moves().state().offset - 49));
    });
    connect(nextMoves_, &QPushButton::clicked, this,
            [this] { controller_.selectMoment(controller_.moves().state().offset + 51); });
    connect(moves_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& index) {
                if (index.isValid()) {
                    controller_.selectMoment(controller_.moves().state().offset + index.row() + 1);
                }
            });
    tabs->addTab(movesTab, tr("Evidence"));
    auto* history = section(tr("Saved review runs"), false).second;
    auto* historyLayout = new QVBoxLayout(history);
    runs_ = new QTableView(history);
    runs_->setObjectName(QStringLiteral("reviewRuns"));
    runs_->setAccessibleName(tr("Saved review runs"));
    model_ = new ReviewRunsModel(this);
    runs_->setModel(model_);
    runs_->setSelectionBehavior(QAbstractItemView::SelectRows);
    runs_->setSelectionMode(QAbstractItemView::SingleSelection);
    runs_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    runs_->horizontalHeader()->setStretchLastSection(true);
    historyLayout->addWidget(runs_);
    auto* runPaging = new QHBoxLayout;
    previous_ = new QPushButton(tr("Previous runs"), history);
    next_ = new QPushButton(tr("Next runs"), history);
    auto* refresh = new QPushButton(tr("Refresh runs"), history);
    runPaging->addWidget(previous_);
    runPaging->addWidget(next_);
    historyLayout->addLayout(runPaging);
    historyLayout->addWidget(refresh);
    resume_ = new QPushButton(tr("Resume selected run"), history);
    resume_->setObjectName(QStringLiteral("resumeReview"));
    historyLayout->addWidget(resume_);
    connect(refresh, &QPushButton::clicked, this,
            [this] { controller_.refreshRuns(controller_.state().page); });
    connect(previous_, &QPushButton::clicked, this,
            [this] { controller_.refreshRuns(controller_.state().page - 1); });
    connect(next_, &QPushButton::clicked, this,
            [this] { controller_.refreshRuns(controller_.state().page + 1); });
    connect(resume_, &QPushButton::clicked, this, [this] {
        if (const auto row = model_->row(runs_->currentIndex().row())) {
            controller_.resume(row->id);
        }
    });
    connect(runs_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] {
        if (const auto row = model_->row(runs_->currentIndex().row()); row && !row->metadataError) {
            controller_.inspect(row->id);
        }
        update();
    });
    connect(start_, &QPushButton::clicked, this, [this] {
        if (side_->currentIndex() > 0) {
            controller_.start(side_->currentIndex() == 1 ? chess::Color::White
                                                         : chess::Color::Black,
                              engine_->currentData().toLongLong(),
                              static_cast<review::Quality>(quality_->currentIndex()));
        }
    });
    connect(cancel_, &QPushButton::clicked, &controller_, &app::ReviewGameController::cancel);
    connect(side_, &QComboBox::currentIndexChanged, this, &ReviewPage::update);
    connect(engine_, &QComboBox::currentIndexChanged, this, &ReviewPage::update);
    connect(&controller_, &app::ReviewGameController::changed, this, &ReviewPage::update);
    connect(&navigation, &app::ReviewNavigationController::changed, this,
            &ReviewPage::updateNavigation);
    connect(splitter_, &QSplitter::splitterMoved, this, [this] {
        if (!applyingSplit_) {
            const auto sizes = splitter_->sizes();
            const int total = sizes[0] + sizes[1];
            if (total > 0) {
                splitShare_ = std::clamp(sizes[0] * 100 / total, 1, 99);
            }
        }
    });
    rightLayout->addStretch();
    setSplitShare(55);
    update();
}
void ReviewPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    QTimer::singleShot(0, this, [this] { setSplitShare(splitShare_); });
}
void ReviewPage::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange ||
        event->type() == QEvent::StyleChange) {
        updateNavigation();
    }
}
int ReviewPage::splitShare() const { return splitShare_; }
void ReviewPage::setSplitShare(int share) {
    splitShare_ = std::clamp(share, 1, 99);
    applyingSplit_ = true;
    const int width = std::max(100, splitter_->width());
    splitter_->setSizes({width * splitShare_ / 100, width * (100 - splitShare_) / 100});
    applyingSplit_ = false;
}
void ReviewPage::hideEvent(QHideEvent* event) {
    controller_.navigation().clearPreview();
    if (controller_.exploration().hidesSolution()) {
        controller_.exploration().returnToGame();
    }
    QWidget::hideEvent(event);
}
bool ReviewPage::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress && watched != side_ && watched != engine_ &&
        watched != quality_) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->modifiers() == Qt::NoModifier) {
            auto& nav = controller_.exploration();
            switch (key->key()) {
            case Qt::Key_Left:
                nav.previous();
                return true;
            case Qt::Key_Right:
                nav.next();
                return true;
            case Qt::Key_Home:
                nav.beginning();
                return true;
            case Qt::Key_End:
                nav.end();
                return true;
            default:
                break;
            }
        }
    }
    if (event->type() == QEvent::Wheel &&
        (watched == side_ || watched == engine_ || watched == quality_)) {
        event->ignore();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}
void ReviewPage::update() {
    const auto& state = controller_.state();
    const std::optional<review::ReviewId> run =
        state.run ? std::optional{state.run->id} : std::nullopt;
    if (run != shownRun_ || gameId_ != state.gameId) {
        shownRun_ = run;
        setupToggle_->setChecked(!run);
    }
    cancel_->setVisible(state.active);
    if (gameId_ != state.gameId) {
        gameId_ = state.gameId;
        side_->setCurrentIndex(0);
        quality_->setCurrentIndex(static_cast<int>(controller_.defaultQuality()));
    }
    const auto& engines = controller_.engines();
    if (profilesRevision_ != engines.profilesRevision ||
        (engine_->count() == 0 && !engines.profiles.empty())) {
        profilesRevision_ = engines.profilesRevision;
        const auto selected = engine_->currentData();
        const QSignalBlocker blocker(engine_);
        engine_->clear();
        for (const auto& profile : engines.profiles) {
            engine_->addItem(clippedEngineText(presentedEngineText(profile.displayName)),
                             QVariant::fromValue<qlonglong>(profile.id));
        }
        const auto index = engine_->findData(selected);
        if (index >= 0) {
            engine_->setCurrentIndex(index);
        }
    }
    game_->setText(tr("Game review: %1").arg(state.game));
    game_->setToolTip(QStringLiteral("<qt>%1</qt>").arg(game_->text().toHtmlEscaped()));
    const auto blocker = controller_.startBlocker(engine_->currentData().toLongLong());
    reason_->setText(blocker.isEmpty() && side_->currentIndex() == 0
                         ? tr("Choose the player whose perspective this run will retain.")
                         : blocker);
    start_->setEnabled(blocker.isEmpty() && side_->currentIndex() > 0);
    side_->setEnabled(!state.busy);
    engine_->setEnabled(!state.busy);
    quality_->setEnabled(!state.busy);
    cancel_->setEnabled(state.active && !state.cancelling);
    QString status =
        state.phase == app::ReviewPhase::Finished ? state.message : phaseText(state.phase);
    if (state.run) {
        status += tr("\nRun %1 · %2 · %3 · %4 of %5 positions saved")
                      .arg(state.run->id)
                      .arg(sideText(state.run->selectedSide), qualityText(state.run->quality))
                      .arg(state.run->completedPositions)
                      .arg(state.run->totalPositions);
    }
    const auto engineName = state.engine;
    if (!engineName.isEmpty()) {
        status += tr("\nRun engine: %1").arg(clippedEngineText(presentedEngineText(engineName)));
    }
    if (state.cancelling) {
        status = tr("Cancelling; saving completed evidence…\n") + status;
    }
    status_->setToolTip(status);
    status_->setAccessibleDescription(status);
    status_->setText(state.cancelling ? tr("Cancelling; saving completed evidence…")
                     : state.run      ? tr("%1 · %2 · %3 · saved %4/%5")
                                       .arg(state.active ? phaseText(state.phase)
                                                         : app::reviewStatusText(state.run->status),
                                            sideText(state.run->selectedSide),
                                            qualityText(state.run->quality))
                                       .arg(state.run->completedPositions)
                                       .arg(state.run->totalPositions)
                                 : phaseText(state.phase));
    progress_->setVisible(state.active);
    progress_->setRange(0, state.run ? state.run->totalPositions : 1);
    progress_->setValue(state.run ? state.run->completedPositions : 0);
    if (state.active && state.refinementTotal > 0) {
        progress_->setRange(0, state.refinementTotal);
        progress_->setValue(state.refinementDone);
        progress_->setFormat(tr("Refinement: %v of %m positions"));
    } else {
        progress_->setFormat(QStringLiteral("%p%"));
    }
    error_->setText(
        state.error + (state.notice.isEmpty() ? QString{} : QLatin1Char('\n') + state.notice) +
        (state.compatibilityNotice.isEmpty() ? QString{}
                                             : QLatin1Char('\n') + state.compatibilityNotice));
    error_->setVisible(!error_->text().isEmpty());
    runs_->setEnabled(!state.listing);
    model_->reload(state.runs);
    const auto selected = model_->row(runs_->currentIndex().row());
    const auto resumeBlocker = controller_.resumeBlocker();
    resume_->setToolTip(resumeBlocker);
    resume_->setEnabled(!state.busy && resumeBlocker.isEmpty() && selected &&
                        !selected->metadataError && selected->status >= review::JobStatus::Partial);
    previous_->setEnabled(!state.inspecting && state.page > 0);
    next_->setEnabled(!state.inspecting && state.hasNext);
    refreshSummary_->setEnabled(state.run && !state.busy);
    previousMoments_->setEnabled(state.overview && state.overview->keyOffset > 0 && !state.busy);
    nextMoments_->setEnabled(state.overview &&
                             state.overview->keyOffset + 50 < state.overview->totalKeyMoments &&
                             !state.busy);
    QString summaryText =
        state.overviewError.isEmpty()
            ? QString{}
            : tr("Summary could not be refreshed: %1\n\n").arg(state.overviewError);
    if (state.overview && state.run) {
        if (!state.overviewError.isEmpty()) {
            summaryText += tr("Last successfully loaded summary:\n");
        }
        summaryText += app::reviewOverviewText(*state.overview, state.run->selectedSide);
        if (state.run && state.overview->revision != state.run->revision) {
            summaryText.prepend(tr("Updating evidence; summary reflects an earlier checkpoint.\n"));
        }
        persistence::ReviewMovePage page;
        page.reviewId = state.overview->reviewId;
        page.revision = state.overview->revision;
        page.offset = state.overview->keyOffset;
        page.moves = state.overview->keyMoments;
        momentsModel_->reload(page);
    } else {
        summaryText += tr(
            "Summary is available after evidence is saved. Refresh to inspect partial coverage.");
        momentsModel_->reload(std::nullopt);
    }
    if (summary_->toPlainText() != summaryText) {
        summary_->setPlainText(summaryText);
    }
    const auto& moveState = controller_.moves().state();
    const QSignalBlocker moveSelection(moves_->selectionModel());
    movesModel_->reload(moveState.page);
    if (moveState.page && moveState.selectedPosition > moveState.page->offset &&
        moveState.selectedPosition <=
            moveState.page->offset + static_cast<int>(moveState.page->moves.size())) {
        const int row = moveState.selectedPosition - moveState.page->offset - 1;
        moves_->setCurrentIndex(movesModel_->index(row, 0));
        const auto text =
            app::reviewMoveDetails(moveState.page->moves[static_cast<std::size_t>(row)]);
        if (details_->toPlainText() != text) {
            details_->setPlainText(text);
        }
    } else {
        details_->clear();
    }
    if (controller_.navigation().selected() == 0) {
        details_->clear();
    }
    updateNavigation();
    const int total = state.run ? state.run->totalPositions - 1 : 0;
    retryMoves_->setVisible(!controller_.navigation().evidenceError().isEmpty());
    retryMoves_->setEnabled(state.run && !moveState.loading);
    previousMoves_->setEnabled(state.run && moveState.offset > 0);
    nextMoves_->setEnabled(state.run && moveState.offset + 50 < total);
    moveStatus_->setText(!controller_.navigation().evidenceError().isEmpty()
                             ? controller_.navigation().evidenceError()
                         : moveState.loading ? tr("Loading move evidence…")
                                             : tr("%1 moves · showing %2–%3")
                                                   .arg(total)
                                                   .arg(total ? moveState.offset + 1 : 0)
                                                   .arg(std::min(moveState.offset + 50, total)));
}
void ReviewPage::updateNavigation() {
    auto& nav = controller_.navigation();
    const auto& state = controller_.state();
    if (nav.selected() == 0) {
        const QSignalBlocker moveSelection(moves_->selectionModel());
        moves_->setCurrentIndex({});
        moves_->clearSelection();
    }
    const auto& displayedBoard = controller_.exploration().board();
    const bool orientationChanged = board_->state().blackAtBottom != displayedBoard.blackAtBottom;
    board_->setState(displayedBoard);
    if (orientationChanged) {
        app::BoardPreferences preferences;
        preferences.blackAtBottom = displayedBoard.blackAtBottom;
        board_->setPreferences(preferences);
    }
    const bool black = nav.board().blackAtBottom;
    const auto white = nav.whitePlayer().isEmpty() ? tr("White") : nav.whitePlayer();
    const auto blackPlayer = nav.blackPlayer().isEmpty() ? tr("Black") : nav.blackPlayer();
    topPlayer_->setText(black ? white : blackPlayer);
    topPlayer_->setToolTip(QStringLiteral("<qt>%1</qt>").arg(topPlayer_->text().toHtmlEscaped()));
    bottomPlayer_->setText(black ? blackPlayer : white);
    bottomPlayer_->setToolTip(
        QStringLiteral("<qt>%1</qt>").arg(bottomPlayer_->text().toHtmlEscaped()));
    const auto evaluation = nav.evaluation();
    evaluation_->setEnabled(evaluation.whiteExpected.has_value());
    if (evaluation.whiteExpected) {
        evaluation_->setValue(qRound(*evaluation.whiteExpected * 1000));
    } else {
        evaluation_->reset();
    }
    evaluation_->setInvertedAppearance(black);
    evaluation_->setAccessibleDescription(evaluation.description);
    evaluation_->setToolTip(evaluation.description);
    evaluationText_->setText(evaluation.description);
    previewLabel_->setText(
        !nav.error().isEmpty() ? nav.error()
        : !nav.ready()         ? tr("Loading original game…")
        : nav.previewing() && nav.pending()
            ? tr("Loading preview %1 — board still shows position %2; selected position %3")
                  .arg(nav.requested())
                  .arg(nav.displayed())
                  .arg(nav.selected())
        : nav.previewing() ? tr("Previewing position %1 — selected position %2 is unchanged")
                                 .arg(nav.requested())
                                 .arg(nav.selected())
        : nav.pending() ? tr("Loading position %1…").arg(nav.requested())
                        : tr("Position %1 of %2")
                              .arg(nav.selected())
                              .arg(std::max(0, static_cast<int>(nav.line().size()) - 1)));
    if (nav.ready()) {
        const auto& line = nav.line();
        timeline_->setSnapshot(
            nav.tree(), line, nav.target(0).session, line[static_cast<std::size_t>(nav.selected())],
            nav.pending() ? std::optional{line[static_cast<std::size_t>(nav.requested())]}
                          : std::nullopt,
            true);
    } else {
        timeline_->setSnapshot({}, {}, nav.target(0).session, chess::NodeId{0}, std::nullopt,
                               false);
    }
    firstMove_->setEnabled(nav.ready() && nav.selected() > 0);
    previousMove_->setEnabled(firstMove_->isEnabled());
    lastMove_->setEnabled(nav.ready() && nav.selected() + 1 < static_cast<int>(nav.line().size()));
    nextMove_->setEnabled(lastMove_->isEnabled());
    previousMoment_->setEnabled(nav.canPreviousMoment());
    nextMoment_->setEnabled(nav.canNextMoment());
    const auto* move = controller_.exploration().moment();
    std::optional<persistence::ReviewMove> contextualMove;
    if (move && !controller_.exploration().active() && nav.opponentContext(nav.selected())) {
        contextualMove = *move;
        contextualMove->contextPosition = nav.opponentContext(nav.selected());
        move = &*contextualMove;
    }
    headline_->setText(app::reviewMomentHeadline(move, nav.selected()));
    explanation_->setText(app::reviewMomentExplanation(move, nav.selected()));
    const auto detail = move ? app::reviewMoveDetails(*move) : QString{};
    if (details_->toPlainText() != detail) {
        details_->setPlainText(detail);
    }
    auto palette = rail_->palette();
    palette.setColor(QPalette::Window, reviewMoveColor(move, palette));
    rail_->setPalette(palette);
    QString compact;
    if (state.overview && state.run) {
        const auto& side = state.overview->sides[static_cast<std::size_t>(state.run->selectedSide)];
        compact =
            tr("%1 · accuracy %2 · evaluated %3/%4 · stable %5/%4 · %6 key moments")
                .arg(sideText(state.run->selectedSide))
                .arg(side.accuracy ? QString::number(*side.accuracy, 'f', 0) + QLatin1Char('%')
                                   : tr("unavailable"))
                .arg(side.evaluated)
                .arg(side.total)
                .arg(side.stable)
                .arg(state.overview->totalKeyMoments);
        if (side.total > 0 && side.evaluated == 0) {
            compact += tr(" · no usable move evaluations; inspect evidence details");
        }
        if (state.overview->revision != state.run->revision) {
            compact += tr(" · earlier checkpoint");
        }
        if (!state.overviewError.isEmpty()) {
            compact += tr(" · last successful summary; refresh failed");
        }
    } else {
        compact = tr("Accuracy and coverage appear as evidence is saved.");
        if (!state.overviewError.isEmpty()) {
            compact += tr(" Summary unavailable: %1").arg(state.overviewError);
        }
    }
    compactSummary_->setText(compact);
    updateExploration();
    retryMoves_->setVisible(!nav.evidenceError().isEmpty());
    if (!nav.evidenceError().isEmpty()) {
        moveStatus_->setText(nav.evidenceError());
    }
}
void ReviewPage::updateExploration() {
    auto& exploration = controller_.exploration();
    const auto& state = exploration.state();
    const bool hidden = exploration.hidesSolution();
    const bool line = state.mode == app::ExplorationMode::Line;
    // Clear solution text as well as hiding its widgets, including accessibility values.
    if (hidden) {
        explanation_->clear();
        details_->clear();
        headline_->setText(tr("Find a stronger move"));
    }
    explanation_->setVisible(!hidden);
    compactSummary_->setVisible(!hidden);
    evaluation_->setVisible(!exploration.active());
    evaluationText_->setVisible(!exploration.active());
    if (exploration.active()) {
        evaluation_->reset();
        evaluation_->setAccessibleDescription({});
        evaluation_->setToolTip({});
        evaluationText_->clear();
        previewLabel_->setText(
            state.pendingBoard ? tr("Loading exploration position…")
            : line ? tr("Candidate line · move %1 of %2").arg(state.step).arg(state.length)
                   : tr("Practice position"));
    }
    board_->setToolTip(
        hidden ? tr("Play one legal move to try again.")
        : line ? tr("Candidate preview. Use Previous and Next to step through the line.")
               : tr("Read-only original game. Use the timeline to select a moment."));
    timeline_->setHoverPreview(!exploration.active());
    timeline_->setVisible(!hidden);
    for (const auto& [toggle, content] : sections_) {
        toggle->setVisible(!hidden);
        content->setVisible(!hidden && toggle->isChecked());
    }
    previousMoment_->setVisible(!hidden);
    nextMoment_->setVisible(!hidden);
    candidates_->setVisible(!hidden);
    if (hidden) {
        candidates_->clear();
    } else {
        QStringList current;
        for (int i = 0; i < candidates_->count(); ++i) {
            current << candidates_->itemText(i);
        }
        if (current != state.candidates) {
            const QSignalBlocker blocker(candidates_);
            candidates_->clear();
            candidates_->addItems(state.candidates);
        }
    }
    if (line && candidates_->currentIndex() != state.candidate) {
        const QSignalBlocker blocker(candidates_);
        candidates_->setCurrentIndex(state.candidate);
    }
    showLine_->setVisible(!hidden);
    showLine_->setEnabled(!state.loading && !state.candidates.empty());
    tryAgain_->setVisible(!hidden);
    tryAgain_->setEnabled(state.canRetry);
    analyze_->setVisible(!hidden);
    analyze_->setEnabled(state.canAnalyze);
    reloadExploration_->setVisible(!hidden && !state.loading && state.candidates.empty());
    reloadExploration_->setEnabled(!exploration.active() &&
                                   controller_.navigation().selected() > 0);
    returnGame_->setVisible(exploration.active());
    returnGame_->setText(hidden ? tr("Cancel practice") : tr("Return to Game"));
    hint_->setVisible(hidden);
    hint_->setEnabled(state.mode == app::ExplorationMode::Retry);
    reveal_->setVisible(hidden);
    auto status = state.loading ? tr("Loading candidate evidence…") : state.message;
    if (!state.notice.isEmpty()) {
        status += QLatin1Char('\n') + state.notice;
    }
    if (explorationStatus_->text() != status) {
        explorationStatus_->setText(status);
        if (hidden && !status.isEmpty()) {
            QAccessibleAnnouncementEvent announcement(explorationStatus_, status);
            QAccessible::updateAccessibility(&announcement);
        }
    }
    if (line) {
        firstMove_->setEnabled(state.step > 0);
        previousMove_->setEnabled(state.step > 0);
        nextMove_->setEnabled(state.step < state.length);
        lastMove_->setEnabled(state.step < state.length);
    } else if (hidden) {
        firstMove_->setEnabled(false);
        previousMove_->setEnabled(false);
        nextMove_->setEnabled(false);
        lastMove_->setEnabled(false);
    }
}
} // namespace chess_review::ui
