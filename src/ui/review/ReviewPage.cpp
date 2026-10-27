#include "ui/review/ReviewPage.h"
#include "app/ReviewMovePresentation.h"
#include "ui/EngineText.h"
#include <QAbstractTableModel>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringList>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>

namespace chess_review::ui {
namespace {
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
                (row.classification && row.classification->label ? tr(" (provisional)")
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
    auto* layout = new QVBoxLayout(this);
    auto* commands = new QHBoxLayout;
    auto* back = new QPushButton(tr("Back to Games"), this);
    back->setObjectName(QStringLiteral("reviewBack"));
    commands->addWidget(back);
    commands->addStretch();
    auto* settings = new QPushButton(tr("Manage Engines…"), this);
    commands->addWidget(settings);
    layout->addLayout(commands);
    connect(back, &QPushButton::clicked, this, &ReviewPage::backRequested);
    connect(settings, &QPushButton::clicked, this, &ReviewPage::settingsRequested);
    game_ = new QLabel(this);
    game_->setTextFormat(Qt::PlainText);
    game_->setWordWrap(true);
    layout->addWidget(game_);
    auto* scope =
        new QLabel(tr("Move classifications are provisional and depend on sufficient saved "
                      "evidence. Accuracy and guided explanations are not available yet."),
                   this);
    scope->setWordWrap(true);
    layout->addWidget(scope);
    auto* form = new QFormLayout;
    side_ = new QComboBox(this);
    side_->setObjectName(QStringLiteral("reviewSide"));
    side_->addItems({tr("Choose player…"), tr("White"), tr("Black")});
    engine_ = new QComboBox(this);
    engine_->setObjectName(QStringLiteral("reviewEngine"));
    engine_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    engine_->setMinimumContentsLength(20);
    quality_ = new QComboBox(this);
    quality_->setObjectName(QStringLiteral("reviewQuality"));
    quality_->addItems({tr("Fast"), tr("Standard"), tr("Thorough")});
    quality_->setCurrentIndex(static_cast<int>(controller.defaultQuality()));
    form->addRow(tr("&Player:"), side_);
    form->addRow(tr("&Engine:"), engine_);
    form->addRow(tr("&Quality:"), quality_);
    layout->addLayout(form);
    for (auto* control : {side_, engine_, quality_}) {
        control->installEventFilter(this);
    }
    reason_ = new QLabel(this);
    reason_->setTextFormat(Qt::PlainText);
    reason_->setWordWrap(true);
    layout->addWidget(reason_);
    auto* actions = new QHBoxLayout;
    start_ = new QPushButton(tr("Run Review"), this);
    start_->setObjectName(QStringLiteral("runReview"));
    cancel_ = new QPushButton(tr("Cancel scan"), this);
    cancel_->setObjectName(QStringLiteral("cancelReview"));
    actions->addWidget(start_);
    actions->addWidget(cancel_);
    actions->addStretch();
    layout->addLayout(actions);
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
    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("reviewResultsTabs"));
    auto* movesTab = new QWidget(tabs);
    auto* movesLayout = new QVBoxLayout(movesTab);
    auto* historyTab = new QWidget(tabs);
    auto* historyLayout = new QVBoxLayout(historyTab);
    tabs->addTab(movesTab, tr("&Moves"));
    tabs->addTab(historyTab, tr("Saved &runs"));
    layout->addWidget(tabs, 1);
    auto* historyHeading = new QLabel(
        tr("Select a saved run to inspect its moves or resume its original settings."), historyTab);
    historyHeading->setWordWrap(true);
    historyLayout->addWidget(historyHeading);
    runs_ = new QTableView(this);
    runs_->setObjectName(QStringLiteral("reviewRuns"));
    runs_->setAccessibleName(tr("Saved review runs"));
    model_ = new ReviewRunsModel(this);
    runs_->setModel(model_);
    runs_->setSelectionBehavior(QAbstractItemView::SelectRows);
    runs_->setSelectionMode(QAbstractItemView::SingleSelection);
    runs_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    runs_->horizontalHeader()->setStretchLastSection(true);
    runs_->setColumnWidth(0, 180);
    runs_->setColumnWidth(3, 210);
    historyLayout->addWidget(runs_, 1);
    auto* paging = new QHBoxLayout;
    previous_ = new QPushButton(tr("Previous runs"), this);
    next_ = new QPushButton(tr("Next runs"), this);
    resume_ = new QPushButton(tr("Resume selected run"), this);
    resume_->setObjectName(QStringLiteral("resumeReview"));
    auto* refresh = new QPushButton(tr("Refresh runs"), this);
    paging->addWidget(previous_);
    paging->addWidget(next_);
    paging->addWidget(refresh);
    paging->addStretch();
    paging->addWidget(resume_);
    historyLayout->addLayout(paging);
    auto* moveHeading = new QLabel(
        tr("All game moves — select a move to inspect its evidence and classification."), this);
    moveHeading->setWordWrap(true);
    movesLayout->addWidget(moveHeading);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    moves_ = new QTableView(splitter);
    moves_->setObjectName(QStringLiteral("reviewMoves"));
    moves_->setAccessibleName(tr("Game move classifications"));
    movesModel_ = new ReviewMovesModel(this);
    moves_->setModel(movesModel_);
    moves_->setSelectionBehavior(QAbstractItemView::SelectRows);
    moves_->setSelectionMode(QAbstractItemView::SingleSelection);
    moves_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    moves_->horizontalHeader()->setStretchLastSection(true);
    details_ = new QPlainTextEdit(splitter);
    details_->setObjectName(QStringLiteral("reviewMoveDetails"));
    details_->setAccessibleName(tr("Selected move evidence and rationale"));
    details_->setReadOnly(true);
    splitter->setChildrenCollapsible(false);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    movesLayout->addWidget(splitter, 1);
    auto* movePaging = new QHBoxLayout;
    previousMoves_ = new QPushButton(tr("Previous moves"), this);
    nextMoves_ = new QPushButton(tr("Next moves"), this);
    previousMoves_->setObjectName(QStringLiteral("previousReviewMoves"));
    nextMoves_->setObjectName(QStringLiteral("nextReviewMoves"));
    moveStatus_ = new QLabel(this);
    moveStatus_->setObjectName(QStringLiteral("reviewMoveStatus"));
    moveStatus_->setTextFormat(Qt::PlainText);
    moveStatus_->setWordWrap(true);
    movePaging->addWidget(previousMoves_);
    movePaging->addWidget(nextMoves_);
    movePaging->addWidget(moveStatus_, 1);
    movesLayout->addLayout(movePaging);
    connect(previousMoves_, &QPushButton::clicked, this,
            [this] { controller_.moves().showPage(controller_.moves().state().offset - 50); });
    connect(nextMoves_, &QPushButton::clicked, this,
            [this] { controller_.moves().showPage(controller_.moves().state().offset + 50); });
    connect(moves_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& current) {
                if (current.isValid()) {
                    controller_.moves().select(controller_.moves().state().offset + current.row() +
                                               1);
                }
            });
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
    update();
}
bool ReviewPage::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Wheel &&
        (watched == side_ || watched == engine_ || watched == quality_)) {
        event->ignore();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}
void ReviewPage::update() {
    const auto& state = controller_.state();
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
    game_->setText(tr("Quick review: %1").arg(state.game));
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
    status_->setText(status);
    progress_->setRange(0, state.run ? state.run->totalPositions : 1);
    progress_->setValue(state.run ? state.run->completedPositions : 0);
    error_->setText(state.error +
                    (state.notice.isEmpty() ? QString{} : QLatin1Char('\n') + state.notice));
    runs_->setEnabled(!state.active);
    model_->reload(state.runs);
    const auto selected = model_->row(runs_->currentIndex().row());
    resume_->setEnabled(!state.busy && selected && !selected->metadataError &&
                        selected->status >= review::JobStatus::Partial);
    previous_->setEnabled(!state.listing && state.page > 0);
    next_->setEnabled(!state.listing && state.hasNext);
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
    const int total = state.run ? state.run->totalPositions - 1 : 0;
    previousMoves_->setEnabled(state.run && moveState.offset > 0);
    nextMoves_->setEnabled(state.run && moveState.offset + 50 < total);
    moveStatus_->setText(!moveState.error.isEmpty() ? moveState.error
                         : moveState.loading        ? tr("Loading move evidence…")
                                                    : tr("%1 moves · showing %2–%3")
                                                   .arg(total)
                                                   .arg(total ? moveState.offset + 1 : 0)
                                                   .arg(std::min(moveState.offset + 50, total)));
}
} // namespace chess_review::ui
