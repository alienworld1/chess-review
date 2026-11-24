#include "ui/review/ReviewPage.h"
#include "app/ReviewMovePresentation.h"
#include "app/ReviewWorkspaceController.h"
#include "ui/EngineText.h"
#include "ui/OpeningText.h"
#include "ui/Theme.h"
#include "ui/analysis/EngineAnalysisPanel.h"
#include "ui/chess/ChessBoardWidget.h"
#include "ui/chess/MoveTimeline.h"
#include <QAbstractScrollArea>
#include <QAbstractTableModel>
#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringList>
#include <QStyle>
#include <QTabWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>
#include <array>

namespace chess_review::ui {
class ClassificationBadge final : public QWidget {
  public:
    explicit ClassificationBadge(QWidget* parent) : QWidget(parent) { setFixedSize(26, 26); }
    void setClassification(std::optional<review::Classification> value) {
        if (classification_ != value) {
            classification_ = value;
            setVisible(classification_.has_value());
            update();
        }
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        if (!classification_) {
            return;
        }
        QPainter painter(this);
        paintClassificationBadge(painter, QRectF(rect()).adjusted(1, 1, -1, -1), *classification_);
    }

  private:
    std::optional<review::Classification> classification_;
};

class PlayerSwatch final : public QWidget {
  public:
    explicit PlayerSwatch(QWidget* parent) : QWidget(parent) { setFixedSize(11, 11); }
    void setWhite(bool white) {
        if (white_ != white) {
            white_ = white;
            update();
        }
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(
            QPen(QColor(white_ ? QStringLiteral("#9aa6b4") : QStringLiteral("#525e6e")), 1.0));
        painter.setBrush(QColor(white_ ? QStringLiteral("#f5f4ef") : QStringLiteral("#262d36")));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 2.5, 2.5);
    }

  private:
    bool white_ = true;
};

namespace {
void updateOpeningLabel(QLabel& label, const QString& text) {
    label.setText(text);
    label.setToolTip(text.isEmpty() ? QString{}
                                    : QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped()));
    label.setVisible(!text.isEmpty());
}

QColor reviewMoveColor(const persistence::ReviewMove* move) {
    return classificationColor(move && move->classification ? move->classification->label
                                                            : std::nullopt);
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
void bindFields(QTableWidget& table, const QList<QPair<QString, QString>>& fields) {
    table.setRowCount(static_cast<int>(fields.size()));
    for (int row = 0; row < fields.size(); ++row) {
        table.setItem(row, 0, new QTableWidgetItem(fields[row].first));
        auto* value = new QTableWidgetItem(fields[row].second);
        value->setToolTip(fields[row].second);
        table.setItem(row, 1, value);
    }
}
} // namespace
class ReviewComparisonModel final : public QAbstractTableModel {
  public:
    explicit ReviewComparisonModel(QObject* parent) : QAbstractTableModel(parent) {}
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 3;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 ||
            section > 2) {
            return {};
        }
        return QStringList{tr("Move"), tr("Score"), tr("Line")}.at(section);
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() ||
            index.column() < 0 || index.column() >= 3) {
            return {};
        }
        const auto& row = rows_[static_cast<std::size_t>(index.row())];
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) {
            return index.column() == 0                                  ? row.move
                   : index.column() == 1                                ? row.score
                   : lines_[static_cast<std::size_t>(index.row())] >= 0 ? tr("Show line")
                                                                        : QString{};
        }
        if (role == Qt::ToolTipRole || role == Qt::AccessibleDescriptionRole) {
            return row.detail;
        }
        return {};
    }
    int line(int row) const {
        return row >= 0 && row < rowCount() ? lines_[static_cast<std::size_t>(row)] : -1;
    }
    void bind(const persistence::ReviewMove* move, const std::vector<int>& lines) {
        std::vector<app::ReviewComparisonPresentation> rows;
        if (move) {
            const auto count = std::min(
                {move->alternatives.moves.size(), move->comparisonSans.size(), lines.size()});
            for (std::size_t i = 0; i < count; ++i) {
                rows.push_back(app::presentReviewComparison(move->alternatives.moves[i],
                                                            move->comparisonSans[i]));
            }
        }
        if (rows_ == rows && lines_ == lines) {
            return;
        }
        beginResetModel();
        rows_ = std::move(rows);
        lines_ = lines;
        endResetModel();
    }

  private:
    std::vector<app::ReviewComparisonPresentation> rows_;
    std::vector<int> lines_;
};
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
                     ? (row.pendingRevision          ? tr(" (updating)")
                        : row.classification->stable ? QString{}
                                                     : tr(" (preliminary)"))
                     : QString{}) +
                (row.contextPosition ? tr("\nAfter an opponent mistake") : QString{})}
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
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* header = new QHBoxLayout;
    header->setSpacing(10);
    auto* back = new QPushButton(tr("Back to Games"), this);
    back->setObjectName(QStringLiteral("reviewBack"));
    setQuietAction(*back);
    setIcon(*back, QStringLiteral("arrow-left"));
    header->addWidget(back);
    game_ = new QLabel(this);
    game_->setTextFormat(Qt::PlainText);
    game_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* gameHeading = new QVBoxLayout;
    gameHeading->setSpacing(1);
    gameHeading->addWidget(game_);
    headerOpening_ = new QLabel(this);
    headerOpening_->setObjectName(QStringLiteral("reviewHeaderOpening"));
    headerOpening_->setTextFormat(Qt::PlainText);
    headerOpening_->setWordWrap(true);
    headerOpening_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setTextRole(*headerOpening_, "caption");
    headerOpening_->hide();
    gameHeading->addWidget(headerOpening_);
    header->addLayout(gameHeading, 1);
    setTextRole(*game_, "heading");
    auto* overviewButton = new QPushButton(tr("Summary"), this);
    overviewButton->setObjectName(QStringLiteral("reviewSummaryButton"));
    setQuietAction(*overviewButton);
    setIcon(*overviewButton, QStringLiteral("summary"));
    header->addWidget(overviewButton);
    connect(overviewButton, &QPushButton::clicked, this, [this] {
        controller_.exploration().returnToGame();
        guiding_ = false;
        updateSummary();
    });
    exportPgn_ = new QPushButton(tr("Export PGN…"), this);
    exportPgn_->setObjectName(QStringLiteral("reviewExportPgn"));
    setQuietAction(*exportPgn_);
    setIcon(*exportPgn_, QStringLiteral("export"));
    header->addWidget(exportPgn_);
    connect(exportPgn_, &QPushButton::clicked, this, &ReviewPage::savePgn);
    auto* options = new QToolButton(this);
    options->setObjectName(QStringLiteral("reviewOptions"));
    options->setText(tr("Review options"));
    options->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    options->setPopupMode(QToolButton::InstantPopup);
    setQuietAction(*options);
    setIcon(*options, QStringLiteral("more"));
    auto* optionsMenu = new QMenu(options);
    options->setMenu(optionsMenu);
    header->addWidget(options);
    layout->addLayout(header);
    for (auto* control : {static_cast<QWidget*>(back), static_cast<QWidget*>(overviewButton),
                          static_cast<QWidget*>(exportPgn_), static_cast<QWidget*>(options)}) {
        control->installEventFilter(this);
    }
    connect(back, &QPushButton::clicked, this, &ReviewPage::backRequested);
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setObjectName(QStringLiteral("reviewSplitter"));
    splitter_->setChildrenCollapsible(false);
    layout->addWidget(splitter_, 1);
    auto* left = new QWidget(splitter_);
    auto* boardLayout = new QVBoxLayout(left);
    boardLayout->setContentsMargins(0, 0, 0, 0);
    boardLayout->setSpacing(6);
    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(28, 0, 0, 0);
    topRow->setSpacing(6);
    topSwatch_ = new PlayerSwatch(left);
    topRow->addWidget(topSwatch_);
    topPlayer_ = new QLabel(left);
    topPlayer_->setObjectName(QStringLiteral("reviewTopPlayer"));
    topPlayer_->setTextFormat(Qt::PlainText);
    topPlayer_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setTextRole(*topPlayer_, "strong");
    topRow->addWidget(topPlayer_, 1);
    boardLayout->addLayout(topRow);
    auto* boardFrame = new QWidget(left);
    boardFrame->setMinimumHeight(248);
    boardFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    auto* boardRow = new QHBoxLayout(boardFrame);
    boardRow->setContentsMargins(28, 0, 0, 0);
    board_ = new ChessBoardWidget(boardFrame);
    board_->setObjectName(QStringLiteral("reviewBoard"));
    board_->setMinimumSize(248, 248);
    board_->setToolTip(tr("Use the timeline or arrows to explore your game."));
    boardRow->addWidget(board_, 1);
    evaluation_ = new EvaluationBarWidget(*board_, boardFrame);
    evaluation_->setObjectName(QStringLiteral("reviewEvaluationBar"));
    boardLayout->addWidget(boardFrame, 1);
    auto* bottomRow = new QHBoxLayout;
    bottomRow->setContentsMargins(28, 0, 0, 0);
    bottomRow->setSpacing(6);
    bottomSwatch_ = new PlayerSwatch(left);
    bottomRow->addWidget(bottomSwatch_);
    bottomPlayer_ = new QLabel(left);
    bottomPlayer_->setObjectName(QStringLiteral("reviewBottomPlayer"));
    bottomPlayer_->setTextFormat(Qt::PlainText);
    bottomPlayer_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setTextRole(*bottomPlayer_, "strong");
    bottomRow->addWidget(bottomPlayer_, 1);
    boardLayout->addLayout(bottomRow);
    previewLabel_ = new QLabel(left);
    previewLabel_->setObjectName(QStringLiteral("reviewPreview"));
    previewLabel_->setWordWrap(true);
    setTextRole(*previewLabel_, "caption");
    boardLayout->addWidget(previewLabel_);
    auto* actions = new QHBoxLayout;
    actions->setSpacing(4);
    actions->addStretch();
    const auto button = [this, actions](const QString& text, const QString& name) {
        auto* result = new QPushButton(text, this);
        result->setObjectName(name);
        setQuietAction(*result);
        actions->addWidget(result);
        return result;
    };
    firstMove_ = button(tr("Start"), QStringLiteral("reviewFirstMove"));
    previousMove_ = button(tr("Previous"), QStringLiteral("reviewPreviousMove"));
    nextMove_ = button(tr("Next"), QStringLiteral("reviewNextMove"));
    lastMove_ = button(tr("End"), QStringLiteral("reviewLastMove"));
    const auto navigationIcon = [](QPushButton* control, const QString& iconName) {
        setIconOnly(*control, iconName);
        control->setFixedWidth(36);
    };
    navigationIcon(firstMove_, QStringLiteral("skip-start"));
    navigationIcon(previousMove_, QStringLiteral("chevron-left"));
    navigationIcon(nextMove_, QStringLiteral("chevron-right"));
    navigationIcon(lastMove_, QStringLiteral("skip-end"));
    auto* flip = button(tr("Flip Board"), QStringLiteral("reviewFlipBoard"));
    setIcon(*flip, QStringLiteral("flip"));
    actions->addStretch();
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
    connect(&controller_.exploration(), &app::ReviewExplorationController::moveAnimated, board_,
            &ChessBoardWidget::animateMove);
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
    connect(timeline_, &MoveTimeline::previousRequested, &controller_.exploration(),
            &app::ReviewExplorationController::previous);
    connect(timeline_, &MoveTimeline::nextRequested, &controller_.exploration(),
            &app::ReviewExplorationController::next);
    connect(timeline_, &MoveTimeline::beginningRequested, &controller_.exploration(),
            &app::ReviewExplorationController::beginning);
    connect(timeline_, &MoveTimeline::endRequested, &controller_.exploration(),
            &app::ReviewExplorationController::end);
    timeline_->setMoveDescription([&navigation](chess::NodeId node) {
        const auto position = navigation.positionFor(node);
        if (!position || *position == 0) {
            return QString{};
        }
        return app::reviewMoveMarker(navigation.move(*position)) +
               (navigation.keyMoment(*position) ? tr("\nKey moment") : QString{});
    });
    timeline_->setMoveMarker([&navigation](chess::NodeId node) {
        const auto position = navigation.positionFor(node);
        if (!position || *position == 0) {
            return QString{};
        }
        const auto* move = navigation.move(*position);
        auto marker = move && move->classification
                          ? app::classificationSymbol(move->classification->label)
                          : QString{};
        if (navigation.keyMoment(*position)) {
            marker += QStringLiteral(" ◆");
        }
        return marker;
    });
    timeline_->setMoveAccent([&navigation](chess::NodeId node) {
        const auto position = navigation.positionFor(node);
        return position && *position > 0 ? reviewMoveColor(navigation.move(*position)) : QColor{};
    });
    timeline_->setMoveClassification(
        [&navigation](chess::NodeId node) -> std::optional<review::Classification> {
            const auto position = navigation.positionFor(node);
            if (!position || *position == 0) {
                return std::nullopt;
            }
            const auto* move = navigation.move(*position);
            return move && move->classification ? move->classification->label : std::nullopt;
        });
    timeline_->setMoveKeyMoment([&navigation](chess::NodeId node) {
        const auto position = navigation.positionFor(node);
        return position && *position > 0 && navigation.keyMoment(*position);
    });
    timeline_->setMaximumHeight(104);
    for (auto* control : {firstMove_, previousMove_, nextMove_, lastMove_, flip}) {
        control->installEventFilter(this);
    }
    explanationScroll_ = new QScrollArea(splitter_);
    auto* scroll = explanationScroll_;
    scroll->setObjectName(QStringLiteral("reviewExplanationScroll"));
    scroll->setWidgetResizable(true);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* right = new QWidget(scroll);
    right->setMinimumWidth(300);
    scroll->setWidget(right);
    auto* rightRoot = new QVBoxLayout(right);
    rightRoot->setContentsMargins(4, 4, 8, 8);
    rightRoot->setSpacing(12);
    summaryPanel_ = new QWidget(right);
    summaryPanel_->setObjectName(QStringLiteral("reviewOverviewPanel"));
    setPanelSurface(*summaryPanel_);
    auto* overviewLayout = new QVBoxLayout(summaryPanel_);
    overviewLayout->setContentsMargins(18, 16, 18, 16);
    overviewLayout->setSpacing(10);
    auto* summaryTitle = new QLabel(tr("Game Review"), summaryPanel_);
    setTextRole(*summaryTitle, "heading");
    overviewLayout->addWidget(summaryTitle);
    resultValue_ = new QLabel(summaryPanel_);
    resultValue_->setTextFormat(Qt::PlainText);
    resultValue_->setWordWrap(true);
    setTextRole(*resultValue_, "secondary");
    overviewLayout->addWidget(resultValue_);
    summaryOpening_ = new QLabel(summaryPanel_);
    summaryOpening_->setObjectName(QStringLiteral("reviewSummaryOpening"));
    summaryOpening_->setTextFormat(Qt::PlainText);
    summaryOpening_->setWordWrap(true);
    summaryOpening_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setTextRole(*summaryOpening_, "secondary");
    summaryOpening_->hide();
    overviewLayout->addWidget(summaryOpening_);
    auto* metrics = new QHBoxLayout;
    metrics->setSpacing(10);
    const auto metric = [this, metrics](const QString& title, const char* name) {
        auto* tile = new QWidget(summaryPanel_);
        setSubtleSurface(*tile);
        auto* tileLayout = new QVBoxLayout(tile);
        tileLayout->setContentsMargins(12, 10, 12, 10);
        tileLayout->setSpacing(2);
        auto* value = new QLabel(QStringLiteral("—"), tile);
        value->setObjectName(QString::fromLatin1(name));
        setTextRole(*value, "metric");
        tileLayout->addWidget(value);
        auto* label = new QLabel(title, tile);
        setTextRole(*label, "caption");
        tileLayout->addWidget(label);
        metrics->addWidget(tile, 1);
        return value;
    };
    accuracyValue_ = metric(tr("Accuracy"), "reviewAccuracyValue");
    accuracyValue_->setWordWrap(true);
    momentsValue_ = metric(tr("Key moments"), "reviewMomentsValue");
    coverageValue_ = new QLabel(summaryPanel_);
    coverageValue_->setWordWrap(true);
    setTextRole(*coverageValue_, "caption");
    overviewLayout->addLayout(metrics);
    overviewLayout->addWidget(coverageValue_);
    classificationCounts_ = new QTableWidget(0, 2, summaryPanel_);
    classificationCounts_->setObjectName(QStringLiteral("reviewClassificationCounts"));
    classificationCounts_->setAccessibleName(tr("Move classifications"));
    classificationCounts_->setHorizontalHeaderLabels({tr("Move quality"), tr("Moves")});
    classificationCounts_->verticalHeader()->hide();
    classificationCounts_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    classificationCounts_->horizontalHeader()->setSectionResizeMode(1,
                                                                    QHeaderView::ResizeToContents);
    classificationCounts_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    classificationCounts_->setSelectionMode(QAbstractItemView::NoSelection);
    classificationCounts_->setFocusPolicy(Qt::NoFocus);
    classificationCounts_->setShowGrid(false);
    classificationCounts_->setIconSize(QSize(18, 18));
    setFlatSurface(*classificationCounts_);
    classificationCounts_->setMaximumHeight(240);
    overviewLayout->addWidget(classificationCounts_);
    summaryState_ = new QLabel(summaryPanel_);
    summaryState_->setObjectName(QStringLiteral("reviewSummaryState"));
    summaryState_->setWordWrap(true);
    setTextRole(*summaryState_, "caption");
    summaryState_->hide();
    overviewLayout->addWidget(summaryState_);
    startGuided_ = new QPushButton(tr("Start review"), summaryPanel_);
    startGuided_->setObjectName(QStringLiteral("startGuidedReview"));
    setPrimaryAction(*startGuided_);
    setIcon(*startGuided_, QStringLiteral("play"));
    startGuided_->installEventFilter(this);
    overviewLayout->addWidget(startGuided_);
    connect(startGuided_, &QPushButton::clicked, this, [this] {
        guiding_ = true;
        updateSummary();
        controller_.navigation().beginning();
        controller_.navigation().nextMoment();
    });
    rightRoot->addWidget(summaryPanel_);
    lesson_ = new QWidget(right);
    lesson_->setObjectName(QStringLiteral("reviewLesson"));
    rightRoot->addWidget(lesson_);
    auto* rightLayout = new QVBoxLayout(lesson_);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(12);
    auto* coachCard = new QWidget(lesson_);
    setPanelSurface(*coachCard);
    auto* momentRow = new QHBoxLayout(coachCard);
    momentRow->setContentsMargins(14, 14, 16, 14);
    momentRow->setSpacing(12);
    rail_ = new QFrame(coachCard);
    rail_->setFixedWidth(4);
    rail_->setAutoFillBackground(true);
    momentRow->addWidget(rail_);
    auto* moment = new QVBoxLayout;
    moment->setSpacing(8);
    auto* classRow = new QHBoxLayout;
    classRow->setSpacing(8);
    badge_ = new ClassificationBadge(coachCard);
    classRow->addWidget(badge_);
    headline_ = new QLabel(coachCard);
    headline_->setObjectName(QStringLiteral("reviewMomentHeadline"));
    headline_->setTextFormat(Qt::PlainText);
    headline_->setWordWrap(true);
    setTextRole(*headline_, "heading");
    classRow->addWidget(headline_);
    classification_ = new QLabel(coachCard);
    classification_->setObjectName(QStringLiteral("reviewClassification"));
    setTextRole(*classification_, "badge");
    classRow->addWidget(classification_);
    classRow->addStretch();
    availability_ = new QLabel(coachCard);
    availability_->setObjectName(QStringLiteral("reviewAvailability"));
    setTextRole(*availability_, "caption");
    classRow->addWidget(availability_);
    moment->addLayout(classRow);
    explanation_ = new QLabel(coachCard);
    explanation_->setObjectName(QStringLiteral("reviewMomentExplanation"));
    explanation_->setTextFormat(Qt::PlainText);
    explanation_->setWordWrap(true);
    explanation_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    setTextRole(*explanation_, "lesson");
    moment->addWidget(explanation_);
    alternativeBox_ = new QWidget(coachCard);
    setSubtleSurface(*alternativeBox_);
    auto* altLayout = new QHBoxLayout(alternativeBox_);
    altLayout->setContentsMargins(12, 8, 12, 8);
    alternative_ = new QLabel(alternativeBox_);
    alternative_->setObjectName(QStringLiteral("reviewAlternative"));
    alternative_->setTextFormat(Qt::PlainText);
    setTextRole(*alternative_, "strong");
    altLayout->addWidget(alternative_, 1);
    moment->addWidget(alternativeBox_);
    explorationBox_ = new QWidget(coachCard);
    setSubtleSurface(*explorationBox_);
    auto* expStatusLayout = new QVBoxLayout(explorationBox_);
    expStatusLayout->setContentsMargins(12, 8, 12, 8);
    expStatusLayout->setSpacing(6);
    linePreview_ = new QWidget(explorationBox_);
    linePreview_->setObjectName(QStringLiteral("reviewLinePreview"));
    auto* linePreviewLayout = new QVBoxLayout(linePreview_);
    linePreviewLayout->setContentsMargins(0, 0, 0, 0);
    linePreviewLayout->setSpacing(4);
    auto* lineHeader = new QHBoxLayout;
    lineHeader->setSpacing(4);
    lineStepLabel_ = new QLabel(linePreview_);
    lineStepLabel_->setObjectName(QStringLiteral("reviewLineStep"));
    setTextRole(*lineStepLabel_, "strong");
    lineHeader->addWidget(lineStepLabel_);
    lineHeader->addStretch();
    const auto lineButton = [this, lineHeader](const QString& text, const QString& name,
                                               const QString& icon, const QString& tip) {
        auto* control = new QPushButton(text, linePreview_);
        control->setObjectName(name);
        setQuietAction(*control);
        setIconOnly(*control, icon);
        control->setToolTip(tip);
        control->setFixedWidth(30);
        control->installEventFilter(this);
        lineHeader->addWidget(control);
        return control;
    };
    lineFirst_ = lineButton(tr("Start"), QStringLiteral("reviewLineFirst"),
                            QStringLiteral("skip-start"), tr("Start of line (Home)"));
    linePrevious_ = lineButton(tr("Previous"), QStringLiteral("reviewLinePrevious"),
                               QStringLiteral("chevron-left"), tr("Previous move in line (Left)"));
    linePlayPause_ = lineButton(tr("Play"), QStringLiteral("reviewLinePlayPause"),
                                QStringLiteral("play"), tr("Play continuation"));
    lineNext_ = lineButton(tr("Next"), QStringLiteral("reviewLineNext"),
                           QStringLiteral("chevron-right"), tr("Next move in line (Right)"));
    lineLast_ = lineButton(tr("End"), QStringLiteral("reviewLineLast"), QStringLiteral("skip-end"),
                           tr("End of line (End)"));
    linePreviewLayout->addLayout(lineHeader);
    lineMoves_ = new QLabel(linePreview_);
    lineMoves_->setObjectName(QStringLiteral("reviewLineMoves"));
    lineMoves_->setTextFormat(Qt::RichText);
    lineMoves_->setWordWrap(true);
    lineMoves_->setOpenExternalLinks(false);
    lineMoves_->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    lineMoves_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    lineMoves_->installEventFilter(this);
    linePreviewLayout->addWidget(lineMoves_);
    linePreview_->hide();
    expStatusLayout->addWidget(linePreview_);
    explorationStatus_ = new QLabel(explorationBox_);
    explorationStatus_->setObjectName(QStringLiteral("reviewExplorationStatus"));
    explorationStatus_->setTextFormat(Qt::PlainText);
    explorationStatus_->setWordWrap(true);
    explorationStatus_->installEventFilter(this);
    expStatusLayout->addWidget(explorationStatus_);
    moment->addWidget(explorationBox_);
    impact_ = new QLabel(coachCard);
    impact_->setWordWrap(true);
    setTextRole(*impact_, "secondary");
    momentRow->addLayout(moment, 1);
    rightLayout->addWidget(coachCard);
    chessDetailsToggle_ = new QPushButton(tr("More about this move"), lesson_);
    chessDetailsToggle_->setObjectName(QStringLiteral("reviewChessDetailsToggle"));
    chessDetailsToggle_->setCheckable(true);
    setQuietAction(*chessDetailsToggle_);
    setIcon(*chessDetailsToggle_, QStringLiteral("chevron-down"));
    chessDetails_ = new QWidget(lesson_);
    setSubtleSurface(*chessDetails_);
    auto* chessLayout = new QVBoxLayout(chessDetails_);
    chessLayout->setContentsMargins(12, 10, 12, 10);
    chessLayout->setSpacing(8);
    chessLayout->addWidget(impact_);
    connect(chessDetailsToggle_, &QPushButton::toggled, chessDetails_, &QWidget::setVisible);
    chessDetails_->hide();
    comparison_ = new QWidget(chessDetails_);
    comparison_->setObjectName(QStringLiteral("reviewComparison"));
    auto* comparisonLayout = new QVBoxLayout(comparison_);
    comparisonLayout->setContentsMargins(0, 0, 0, 0);
    comparisonLayout->setSpacing(6);
    comparisonStatus_ = new QLabel(comparison_);
    comparisonStatus_->setObjectName(QStringLiteral("reviewComparisonStatus"));
    comparisonStatus_->setTextFormat(Qt::PlainText);
    comparisonStatus_->setWordWrap(true);
    setTextRole(*comparisonStatus_, "caption");
    comparisonLayout->addWidget(comparisonStatus_);
    comparisonRows_ = new QTableView(comparison_);
    comparisonRows_->setObjectName(QStringLiteral("reviewComparisonRows"));
    comparisonRows_->setAccessibleName(tr("Played move and supported alternatives"));
    comparisonModel_ = new ReviewComparisonModel(comparisonRows_);
    comparisonRows_->setModel(comparisonModel_);
    comparisonRows_->horizontalHeader()->show();
    comparisonRows_->verticalHeader()->hide();
    comparisonRows_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    comparisonRows_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    comparisonRows_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    comparisonRows_->setShowGrid(false);
    comparisonRows_->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    comparisonRows_->setWordWrap(true);
    comparisonRows_->setTextElideMode(Qt::ElideNone);
    comparisonRows_->setSelectionBehavior(QAbstractItemView::SelectRows);
    comparisonRows_->setSelectionMode(QAbstractItemView::SingleSelection);
    comparisonRows_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    comparisonRows_->setMinimumHeight(160);
    comparisonRows_->setMaximumHeight(320);
    comparisonLayout->addWidget(comparisonRows_);
    const auto replayComparison = [this](const QModelIndex& index) {
        const int line = comparisonModel_->line(index.row());
        if (line >= 0) {
            controller_.exploration().showLine(line);
        }
    };
    connect(comparisonRows_, &QTableView::activated, this, replayComparison);
    connect(comparisonRows_, &QTableView::clicked, this,
            [replayComparison](const QModelIndex& index) {
                if (index.column() == 2) {
                    replayComparison(index);
                }
            });
    chessLayout->addWidget(comparison_);
    candidates_ = new QComboBox(chessDetails_);
    candidates_->setObjectName(QStringLiteral("reviewCandidates"));
    candidates_->setAccessibleName(tr("Saved candidate continuations and root evaluations"));
    candidates_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    candidates_->setMinimumContentsLength(12);
    chessLayout->addWidget(candidates_);
    auto* keyActions = new QHBoxLayout;
    keyActions->setSpacing(8);
    previousMoment_ = new QPushButton(tr("Previous moment"), lesson_);
    nextMoment_ = new QPushButton(tr("Continue"), lesson_);
    setIcon(*previousMoment_, QStringLiteral("chevron-left"));
    setPrimaryAction(*nextMoment_);
    setIcon(*nextMoment_, QStringLiteral("chevron-right"));
    previousMoment_->setObjectName(QStringLiteral("reviewPreviousMoment"));
    nextMoment_->setObjectName(QStringLiteral("reviewNextMoment"));
    keyActions->addWidget(previousMoment_);
    keyActions->addWidget(nextMoment_, 1);
    rightLayout->addLayout(keyActions);
    auto* secondaryGroup = new QVBoxLayout;
    secondaryGroup->setSpacing(6);
    auto* primaryExploreRow = new QHBoxLayout;
    primaryExploreRow->setSpacing(8);
    auto* extraExploreRow = new QHBoxLayout;
    extraExploreRow->setSpacing(8);
    auto* retryRow = new QHBoxLayout;
    retryRow->setSpacing(8);
    const auto action = [this](const QString& text, const QString& name, const QString& icon) {
        auto* control = new QPushButton(text, lesson_);
        control->setObjectName(name);
        setIcon(*control, icon);
        return control;
    };
    tryAgain_ = action(tr("Try Again"), QStringLiteral("reviewTryAgain"), QStringLiteral("retry"));
    showLine_ = action(tr("Show Line"), QStringLiteral("reviewShowLine"), QStringLiteral("line"));
    analyze_ = action(tr("Analyze Position"), QStringLiteral("reviewAnalyzePosition"),
                      QStringLiteral("analyze"));
    reloadExploration_ = action(tr("Reload candidates"), QStringLiteral("reviewReloadCandidates"),
                                QStringLiteral("refresh"));
    returnGame_ = action(tr("Return to Game"), QStringLiteral("reviewReturnToGame"),
                         QStringLiteral("arrow-left"));
    hint_ = action(tr("Hint"), QStringLiteral("reviewHint"), QStringLiteral("hint"));
    setPrimaryAction(*hint_);
    reveal_ =
        action(tr("Show Solution"), QStringLiteral("reviewShowSolution"), QStringLiteral("eye"));
    primaryExploreRow->addWidget(tryAgain_, 1);
    primaryExploreRow->addWidget(showLine_, 1);
    extraExploreRow->addWidget(analyze_, 1);
    extraExploreRow->addWidget(returnGame_, 1);
    extraExploreRow->addWidget(reloadExploration_, 1);
    retryRow->addWidget(hint_, 1);
    retryRow->addWidget(reveal_, 1);
    secondaryGroup->addLayout(retryRow);
    secondaryGroup->addLayout(primaryExploreRow);
    secondaryGroup->addLayout(extraExploreRow);
    rightLayout->addLayout(secondaryGroup);
    auto& exploration = controller_.exploration();
    connect(lineFirst_, &QPushButton::clicked, &exploration,
            &app::ReviewExplorationController::beginning);
    connect(linePrevious_, &QPushButton::clicked, &exploration,
            &app::ReviewExplorationController::previous);
    connect(linePlayPause_, &QPushButton::clicked, &exploration,
            &app::ReviewExplorationController::togglePlayback);
    connect(lineNext_, &QPushButton::clicked, &exploration,
            &app::ReviewExplorationController::next);
    connect(lineLast_, &QPushButton::clicked, &exploration, &app::ReviewExplorationController::end);
    connect(lineMoves_, &QLabel::linkActivated, this, [this](const QString& link) {
        bool ok = false;
        const int targetStep = link.toInt(&ok);
        if (ok && targetStep >= 1 && targetStep <= controller_.exploration().state().length) {
            controller_.exploration().step(targetStep);
        }
    });
    connect(showLine_, &QPushButton::clicked, this, [this] {
        const int candidate = std::max(0, candidates_->currentIndex());
        const auto& exp = controller_.exploration().state();
        if (exp.mode == app::ExplorationMode::Line && exp.candidate == candidate) {
            controller_.exploration().togglePlayback();
        } else {
            controller_.exploration().showLine(candidate);
        }
    });
    for (auto* control : {showLine_, tryAgain_, returnGame_, analyze_, previousMoment_, nextMoment_,
                          chessDetailsToggle_, hint_, reveal_, reloadExploration_}) {
        control->installEventFilter(this);
    }
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
            &app::ReviewExplorationController::revealSolution);
    connect(board_, &ChessBoardWidget::escapeRequested, this, [&exploration] {
        if (exploration.active()) {
            exploration.returnToGame();
        }
    });
    connect(hint_, &QPushButton::clicked, &exploration, &app::ReviewExplorationController::hint);
    connect(reloadExploration_, &QPushButton::clicked, &exploration,
            &app::ReviewExplorationController::reload);
    connect(analyze_, &QPushButton::clicked, this, &ReviewPage::analyzeRequested);
    connect(&exploration, &app::ReviewExplorationController::changed, this,
            &ReviewPage::updateNavigation);
    connect(previousMoment_, &QPushButton::clicked, &navigation,
            &app::ReviewNavigationController::previousMoment);
    connect(nextMoment_, &QPushButton::clicked, this, [this] {
        if (controller_.navigation().canNextMoment()) {
            controller_.navigation().nextMoment();
        } else {
            guiding_ = false;
            updateSummary();
        }
    });
    rightLayout->addWidget(chessDetailsToggle_, 0, Qt::AlignLeft);
    rightLayout->addWidget(chessDetails_);
    const auto section = [this, right, rightRoot, optionsMenu](const QString& title,
                                                               bool expanded) {
        auto* toggle = optionsMenu->addAction(title);
        toggle->setCheckable(true);
        toggle->setChecked(expanded);
        if (title == tr("Technical inspector")) {
            inspectorAction_ = toggle;
        }
        QWidget* content;
        if (expanded) {
            content = new QWidget(right);
            rightRoot->insertWidget(0, content);
            connect(toggle, &QAction::toggled, content, &QWidget::setVisible);
        } else {
            auto* dialog = new QDialog(this);
            dialog->setWindowTitle(title);
            dialog->resize(760, 620);
            auto* dialogLayout = new QVBoxLayout(dialog);
            content = new QWidget(dialog);
            dialogLayout->addWidget(content, 1);
            auto* close = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
            dialogLayout->addWidget(close);
            connect(close, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
            connect(toggle, &QAction::toggled, dialog, &QWidget::setVisible);
            connect(dialog, &QDialog::finished, toggle, [toggle] { toggle->setChecked(false); });
        }
        sections_.emplace_back(toggle, content);
        return std::pair{toggle, content};
    };
    const auto setupSection = section(tr("Review settings"), true);
    setupToggle_ = setupSection.first;
    setupToggle_->setObjectName(QStringLiteral("reviewSetupAction"));
    setup_ = setupSection.second;
    setPanelSurface(*setup_);
    auto* setupLayout = new QVBoxLayout(setup_);
    setupLayout->setContentsMargins(18, 16, 18, 16);
    setupLayout->setSpacing(12);
    auto* setupTitle = new QLabel(tr("Let’s review your game"), setup_);
    setTextRole(*setupTitle, "heading");
    setupLayout->addWidget(setupTitle);
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
    setupLayout->addLayout(form);
    auto* engineOptions = new QPushButton(tr("Engine and review options"), setup_);
    engineOptions->setObjectName(QStringLiteral("reviewEngineOptions"));
    engineOptions->setCheckable(true);
    setQuietAction(*engineOptions);
    setIcon(*engineOptions, QStringLiteral("chevron-down"));
    setupLayout->addWidget(engineOptions, 0, Qt::AlignLeft);
    auto* advanced = new QWidget(setup_);
    auto* advancedForm = new QFormLayout(advanced);
    advancedForm->setContentsMargins(0, 0, 0, 0);
    advancedForm->addRow(tr("&Engine:"), engine_);
    advancedForm->addRow(tr("&Quality:"), quality_);
    bookEnabled_ = new QCheckBox(tr("Classify opening-book moves"), advanced);
    bookEnabled_->setObjectName(QStringLiteral("reviewBookEnabled"));
    connect(bookEnabled_, &QCheckBox::clicked, this,
            [this](bool enabled) { bookOverride_ = enabled; });
    advancedForm->addRow(bookEnabled_);
    setupLayout->addWidget(advanced);
    advanced->hide();
    connect(engineOptions, &QPushButton::toggled, advanced, &QWidget::setVisible);
    for (auto* control : {side_, engine_, quality_}) {
        control->installEventFilter(this);
    }
    reason_ = new QLabel(setup_);
    reason_->setTextFormat(Qt::PlainText);
    reason_->setWordWrap(true);
    setTextRole(*reason_, "secondary");
    setupLayout->addWidget(reason_);
    auto* setupActions = new QHBoxLayout;
    setupActions->setSpacing(8);
    start_ = new QPushButton(tr("Analyze game"), setup_);
    start_->setObjectName(QStringLiteral("runReview"));
    setPrimaryAction(*start_);
    setIcon(*start_, QStringLiteral("play"));
    setupActions->addWidget(start_, 1);
    auto* settings = new QPushButton(tr("Set up engine…"), setup_);
    setQuietAction(*settings);
    setIcon(*settings, QStringLiteral("engine"));
    setupActions->addWidget(settings);
    setupLayout->addLayout(setupActions);
    connect(settings, &QPushButton::clicked, this, &ReviewPage::settingsRequested);
    cancel_ = new QPushButton(tr("Cancel review"), right);
    cancel_->setObjectName(QStringLiteral("cancelReview"));
    setIcon(*cancel_, QStringLiteral("stop"));
    rightRoot->addWidget(cancel_, 0, Qt::AlignLeft);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("reviewStatus"));
    status_->setAccessibleName(tr("Review progress"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    setTextRole(*status_, "caption");
    layout->addWidget(status_);
    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("reviewProgress"));
    progress_->setAccessibleName(tr("Review progress"));
    progress_->setTextVisible(false);
    progress_->setMaximumHeight(6);
    layout->addWidget(progress_);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("reviewError"));
    error_->setTextFormat(Qt::PlainText);
    error_->setWordWrap(true);
    error_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(error_);
    exportStatus_ = new QLabel(this);
    exportStatus_->setObjectName(QStringLiteral("reviewExportStatus"));
    exportStatus_->setAccessibleName(tr("PGN export status"));
    exportStatus_->setTextFormat(Qt::PlainText);
    exportStatus_->setWordWrap(true);
    exportStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                           Qt::TextSelectableByKeyboard);
    exportStatus_->hide();
    layout->addWidget(exportStatus_);
    auto& workspace = controller_.workspace();
    connect(&workspace, &app::ReviewWorkspaceController::exportChanged, this,
            &ReviewPage::updateExportAction);
    connect(&workspace, &app::ReviewWorkspaceController::exportFinished, this,
            [this](const QString& filename, const QString& error) {
                const auto status =
                    error.isEmpty()
                        ? tr("Saved annotated PGN as %1.").arg(QFileInfo(filename).fileName())
                        : error;
                exportStatus_->setText(status);
                exportStatus_->show();
                if (isVisible()) {
                    QAccessibleAnnouncementEvent announcement(exportStatus_, status);
                    QAccessible::updateAccessibility(&announcement);
                }
            });
    auto* keySection = section(tr("Key moments"), false).second;
    auto* keyLayout = new QVBoxLayout(keySection);
    moments_ = new QTableView(keySection);
    moments_->setObjectName(QStringLiteral("reviewKeyMoments"));
    moments_->setAccessibleName(tr("Key moments"));
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
    const auto detail = section(tr("Technical inspector"), false);
    detail.first->setObjectName(QStringLiteral("reviewDetailsToggle"));
    auto* detailSection = detail.second;
    auto* detailLayout = new QVBoxLayout(detailSection);
    inspectorFields_ = new QTableWidget(0, 2, detailSection);
    inspectorFields_->setObjectName(QStringLiteral("reviewInspectorFields"));
    inspectorFields_->setHorizontalHeaderLabels({tr("Field"), tr("Value")});
    inspectorFields_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    inspectorFields_->verticalHeader()->hide();
    inspectorFields_->setColumnWidth(0, 190);
    inspectorFields_->setMinimumHeight(220);
    inspectorFields_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    detailLayout->addWidget(inspectorFields_);
    auto* tabs = new QTabWidget(detailSection);
    tabs->setObjectName(QStringLiteral("reviewResultsTabs"));
    detailLayout->addWidget(tabs);
    auto* summaryTab = new QWidget(tabs);
    auto* summaryLayout = new QVBoxLayout(summaryTab);
    overviewFields_ = new QTableWidget(0, 2, summaryTab);
    overviewFields_->setObjectName(QStringLiteral("reviewOverviewFields"));
    overviewFields_->setHorizontalHeaderLabels({tr("Measure"), tr("Value")});
    overviewFields_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    overviewFields_->verticalHeader()->hide();
    overviewFields_->setColumnWidth(0, 190);
    overviewFields_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    summaryLayout->addWidget(overviewFields_);
    auto* rawReport = new QWidget(tabs);
    auto* rawLayout = new QVBoxLayout(rawReport);
    summary_ = new QPlainTextEdit(rawReport);
    summary_->setObjectName(QStringLiteral("reviewSummary"));
    summary_->setAccessibleName(tr("Game accuracy, coverage and factual summary"));
    summary_->setReadOnly(true);
    rawLayout->addWidget(summary_);
    refreshSummary_ = new QPushButton(tr("Refresh summary"), rawReport);
    refreshSummary_->setObjectName(QStringLiteral("refreshReviewSummary"));
    rawLayout->addWidget(refreshSummary_);
    connect(refreshSummary_, &QPushButton::clicked, this,
            [this] { controller_.refreshOverview(); });
    tabs->addTab(summaryTab, tr("Overview"));
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
    moves_->setMinimumHeight(140);
    moves_->horizontalHeader()->setStretchLastSection(true);
    movesLayout->addWidget(moves_, 1);
    details_ = new QPlainTextEdit(movesTab);
    details_->setObjectName(QStringLiteral("reviewMoveDetails"));
    details_->setAccessibleName(tr("Selected move evidence and rationale"));
    details_->setReadOnly(true);
    movesLayout->addWidget(details_, 1);
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
    detailLayout->addWidget(moveStatus_);
    retryMoves_ = new QPushButton(tr("Retry move evidence"), right);
    retryMoves_->setObjectName(QStringLiteral("retryReviewMoves"));
    detailLayout->addWidget(retryMoves_);
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
    tabs->addTab(rawReport, tr("Raw report"));
    const auto historySection = section(tr("Saved reviews"), false);
    historySection.first->setObjectName(QStringLiteral("reviewHistoryAction"));
    auto* history = historySection.second;
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
            controller_.start(
                side_->currentIndex() == 1 ? chess::Color::White : chess::Color::Black,
                engine_->currentData().toLongLong(),
                static_cast<review::Quality>(quality_->currentIndex()), bookOverride_);
        }
    });
    connect(cancel_, &QPushButton::clicked, &controller_, &app::ReviewGameController::cancel);
    connect(side_, &QComboBox::currentIndexChanged, this, &ReviewPage::update);
    connect(engine_, &QComboBox::currentIndexChanged, this, [this] {
        engineOverride_ = engine_->currentData().toLongLong();
        update();
    });
    connect(quality_, &QComboBox::currentIndexChanged, this,
            [this](int index) { qualityOverride_ = static_cast<review::Quality>(index); });
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
    rightRoot->addStretch();
    setVerbose(false);
    setSplitShare(55);
    update();
}
void ReviewPage::setVerbose(bool verbose) {
    verbose_ = verbose;
    if (inspectorAction_) {
        inspectorAction_->setObjectName(QStringLiteral("reviewTechnicalInspectorAction"));
        inspectorAction_->setVisible(verbose);
        if (!verbose) {
            inspectorAction_->setChecked(false);
        }
    }
    updateNavigation();
}
void ReviewPage::updateSummary() {
    const auto& state = controller_.state();
    const bool hasRun = state.run.has_value();
    summaryPanel_->setVisible(hasRun && !guiding_);
    lesson_->setVisible(hasRun && guiding_);
    startGuided_->setEnabled(hasRun && controller_.navigation().ready());
    auto opening = openingText(state.opening);
    if (state.run && state.opening && state.opening->bookPlies) {
        const int plies = *state.opening->bookPlies;
        if (!opening.isEmpty()) {
            opening += u'\n';
        }
        if (plies == 0) {
            opening += tr("No opening-book moves found.");
        } else if (plies == state.run->totalPositions - 1) {
            opening += tr("In book through the final move.");
        } else {
            opening += plies % 2 != 0 ? tr("Book ends after White’s move %1.").arg((plies + 1) / 2)
                                      : tr("Book ends after Black’s move %1.").arg(plies / 2);
        }
    }
    updateOpeningLabel(*summaryOpening_, opening);
    classificationCounts_->setRowCount(0);
    if (!state.run || !state.overview) {
        accuracyValue_->setText(QStringLiteral("—"));
        momentsValue_->setText(QStringLiteral("—"));
        coverageValue_->setText(tr("Your summary will appear as analysis progresses."));
        summaryState_->setText(state.overviewError.isEmpty()
                                   ? tr("Preparing your review…")
                                   : tr("The summary could not be loaded."));
        summaryState_->setVisible(true);
        return;
    }
    const auto& overview = *state.overview;
    const auto& side = overview.sides[static_cast<std::size_t>(state.run->selectedSide)];
    if (verbose_) {
        const auto& scan = overview.quickScan[static_cast<std::size_t>(state.run->selectedSide)];
        bindFields(*overviewFields_,
                   {{tr("Result"), overview.result},
                    {tr("Player"), sideText(state.run->selectedSide)},
                    {tr("Accuracy"),
                     side.accuracy ? tr("%1%").arg(*side.accuracy, 0, 'f', 1) : tr("Unavailable")},
                    {tr("Analyzed moves"), tr("%1 / %2").arg(side.evaluated).arg(side.total)},
                    {tr("Stable assessments"), QString::number(side.stable)},
                    {tr("Withheld assessments"), QString::number(side.withheld)},
                    {tr("Unprocessed moves"), QString::number(side.unprocessed)},
                    {tr("Quick scan usable"), QString::number(scan.usable)},
                    {tr("Quick scan gaps"), QString::number(scan.unusable)},
                    {tr("Quick scan awaiting"), QString::number(scan.awaiting)},
                    {tr("Invalid scan evidence"), QString::number(scan.invalid)}});
    }
    const auto& white = overview.sides[static_cast<std::size_t>(chess::Color::White)];
    const auto& black = overview.sides[static_cast<std::size_t>(chess::Color::Black)];
    const auto accuracy = [](const review::SideSummary& summary) {
        return summary.accuracy ? tr("%L1%").arg(*summary.accuracy, 0, 'f', 0)
                                : QStringLiteral("—");
    };
    const bool both = overview.reviewBothSides;
    resultValue_->setText(both ? tr("Reviewing both players · Result: %1").arg(overview.result)
                               : tr("Reviewing %2 (%1) · Result: %3")
                                     .arg(sideText(state.run->selectedSide),
                                          state.run->selectedSide == chess::Color::White
                                              ? controller_.navigation().whitePlayer()
                                              : controller_.navigation().blackPlayer(),
                                          overview.result));
    accuracyValue_->setText(both ? tr("White %1 · Black %2").arg(accuracy(white), accuracy(black))
                                 : accuracy(side));
    momentsValue_->setText(QString::number(overview.totalKeyMoments));
    const int evaluated = both ? white.evaluated + black.evaluated : side.evaluated;
    const int total = both ? white.total + black.total : side.total;
    const int stable = both ? white.stable + black.stable : side.stable;
    coverageValue_->setText(tr("%1 of %2 moves analyzed").arg(evaluated).arg(total));
    classificationCounts_->setColumnCount(both ? 3 : 2);
    classificationCounts_->setHorizontalHeaderLabels(
        both ? QStringList{tr("Move quality"), tr("White"), tr("Black")}
             : QStringList{tr("Move quality"), tr("Moves")});
    if (both) {
        classificationCounts_->horizontalHeader()->setSectionResizeMode(
            2, QHeaderView::ResizeToContents);
    }
    auto countFont = classificationCounts_->font();
    countFont.setWeight(QFont::DemiBold);
    const auto countBadgeSize = std::max(18, classificationCounts_->fontMetrics().height() + 2);
    classificationCounts_->setIconSize(QSize(countBadgeSize, countBadgeSize));
    static constexpr std::array summaryOrder{
        review::Classification::Brilliant,  review::Classification::Great,
        review::Classification::Best,       review::Classification::Excellent,
        review::Classification::Good,       review::Classification::Book,
        review::Classification::Inaccuracy, review::Classification::Mistake,
        review::Classification::Miss,       review::Classification::Blunder};
    for (const auto label : summaryOrder) {
        const auto i = static_cast<std::size_t>(label);
        if ((both ? white.counts[i] + black.counts[i] : side.counts[i]) == 0) {
            continue;
        }
        review::ClassificationResult result;
        result.label = label;
        const int row = classificationCounts_->rowCount();
        classificationCounts_->insertRow(row);
        classificationCounts_->setItem(
            row, 0,
            new QTableWidgetItem(
                QIcon(classificationPixmap(label, countBadgeSize, devicePixelRatioF())),
                app::classificationText(result)));
        auto* firstCount =
            new QTableWidgetItem(QString::number(both ? white.counts[i] : side.counts[i]));
        firstCount->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        firstCount->setFont(countFont);
        classificationCounts_->setItem(row, 1, firstCount);
        if (both) {
            auto* blackCount = new QTableWidgetItem(QString::number(black.counts[i]));
            blackCount->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            blackCount->setFont(countFont);
            classificationCounts_->setItem(row, 2, blackCount);
        }
    }
    classificationCounts_->setVisible(classificationCounts_->rowCount() > 0);
    classificationCounts_->horizontalHeader()->setVisible(both);
    const int rowHeight = std::max(28, classificationCounts_->fontMetrics().height() + 12);
    classificationCounts_->verticalHeader()->setDefaultSectionSize(rowHeight);
    const int headerHeight =
        both ? classificationCounts_->horizontalHeader()->sizeHint().height() : 0;
    classificationCounts_->setFixedHeight(
        std::min(240, headerHeight + classificationCounts_->rowCount() * rowHeight + 4));
    summaryState_->setText(
        !state.overviewError.isEmpty() ? tr("Summary update failed. Showing the previous results.")
        : state.overview->revision != state.run->revision ? tr("Updating analysis")
        : state.active ? tr("Analysis continues. You can start reviewing now.")
        : state.run->status != review::JobStatus::Complete
            ? tr("Review incomplete. Your progress is saved.")
        : evaluated < total  ? tr("Accuracy covers analyzed moves only.")
        : stable < evaluated ? tr("Some moves are preliminary.")
                             : QString{});
    summaryState_->setVisible(!summaryState_->text().isEmpty());
}
void ReviewPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    updateExploration();
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
    analysisReadingGeneration_.reset();
    controller_.navigation().clearPreview();
    controller_.exploration().pauseLine();
    if (controller_.exploration().hidesSolution()) {
        controller_.exploration().returnToGame();
    }
    QWidget::hideEvent(event);
}
bool ReviewPage::eventFilter(QObject* watched, QEvent* event) {
    if ((watched == explorationStatus_ || watched == lineMoves_) &&
        event->type() == QEvent::Resize) {
        fitRetryFeedback();
    }
    if (event->type() == QEvent::KeyPress && !QApplication::activePopupWidget() &&
        watched != side_ && watched != engine_ && watched != quality_) {
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
void ReviewPage::keyPressEvent(QKeyEvent* event) {
    if (!QApplication::activePopupWidget()) {
        if (event->key() == Qt::Key_Escape && controller_.exploration().active()) {
            controller_.exploration().returnToGame();
            event->accept();
            return;
        }
        if (event->modifiers() == Qt::NoModifier) {
            auto& nav = controller_.exploration();
            switch (event->key()) {
            case Qt::Key_Left:
                nav.previous();
                event->accept();
                return;
            case Qt::Key_Right:
                nav.next();
                event->accept();
                return;
            case Qt::Key_Home:
                nav.beginning();
                event->accept();
                return;
            case Qt::Key_End:
                nav.end();
                event->accept();
                return;
            default:
                break;
            }
        }
    }
    QWidget::keyPressEvent(event);
}
void ReviewPage::update() {
    const auto& state = controller_.state();
    const std::optional<review::ReviewId> run =
        state.run ? std::optional{state.run->id} : std::nullopt;
    if (run != shownRun_ || gameId_ != state.gameId) {
        ++readingRestoreGeneration_;
        shownRun_ = run;
        guiding_ = false;
        lastPosition_ = controller_.navigation().selected();
        setupToggle_->setChecked(!run);
        if (!controller_.workspace().exportingPgn()) {
            exportStatus_->hide();
        }
    }
    updateExportAction();
    cancel_->setVisible(state.active);
    const bool gameChanged = gameId_ != state.gameId;
    if (gameChanged) {
        gameId_ = state.gameId;
        side_->setCurrentIndex(0);
        qualityOverride_.reset();
        engineOverride_.reset();
        bookOverride_.reset();
    }
    {
        const QSignalBlocker blocker(quality_);
        quality_->setCurrentIndex(
            static_cast<int>(qualityOverride_.value_or(controller_.defaultQuality())));
    }
    bookEnabled_->setChecked(bookOverride_.value_or(controller_.defaultBookEnabled()));
    const auto& engines = controller_.engines();
    if (gameChanged || profilesRevision_ != engines.profilesRevision ||
        (engine_->count() == 0 && !engines.profiles.empty())) {
        profilesRevision_ = engines.profilesRevision;
        const auto selected =
            engineOverride_ ? QVariant::fromValue<qlonglong>(*engineOverride_) : QVariant{};
        const QSignalBlocker blocker(engine_);
        engine_->clear();
        for (const auto& profile : engines.profiles) {
            engine_->addItem(clippedEngineText(presentedEngineText(profile.displayName)),
                             QVariant::fromValue<qlonglong>(profile.id));
        }
        const auto index = engine_->findData(selected);
        if (index >= 0) {
            engine_->setCurrentIndex(index);
        } else {
            const auto preferred =
                std::find_if(engines.profiles.begin(), engines.profiles.end(),
                             [](const auto& profile) { return profile.isDefault; });
            if (preferred != engines.profiles.end()) {
                engine_->setCurrentIndex(static_cast<int>(preferred - engines.profiles.begin()));
            }
        }
    }
    game_->setText(state.game);
    game_->setToolTip(QStringLiteral("<qt>%1</qt>").arg(game_->text().toHtmlEscaped()));
    updateOpeningLabel(*headerOpening_, openingText(state.opening));
    const auto blocker = controller_.startBlocker(engine_->currentData().toLongLong());
    reason_->setText(
        blocker.isEmpty() && side_->currentIndex() == 0 ? tr("Which side did you play?") : blocker);
    start_->setEnabled(blocker.isEmpty() && side_->currentIndex() > 0);
    side_->setEnabled(!state.busy);
    engine_->setEnabled(!state.busy);
    quality_->setEnabled(!state.busy);
    bookEnabled_->setEnabled(!state.busy);
    cancel_->setEnabled(state.active && !state.cancelling);
    QString status =
        state.phase == app::ReviewPhase::Finished ? state.message : phaseText(state.phase);
    if (state.run) {
        status += tr("\nRun %1\n%2\n%3\n%4 of %5 positions saved")
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
    status_->setToolTip(verbose_ ? status : QString{});
    const auto simpleStatus =
        state.cancelling ? tr("Stopping review…")
        : state.active && state.run && state.run->status == review::JobStatus::Deriving
            ? tr("Updating summary…")
        : state.active && state.refinementTotal > 0
            ? tr("Checking key moments (%1 of %2 positions)")
                  .arg(state.refinementDone)
                  .arg(state.refinementTotal)
        : state.active && state.run ? tr("Scanning game (%1 of %2 positions)")
                                          .arg(state.run->completedPositions)
                                          .arg(state.run->totalPositions)
        : state.active ? tr("Preparing game review…")
        : state.run    ? (state.run->status == review::JobStatus::Complete ? tr("Review complete")
                                                                           : tr("Review incomplete"))
                       : QString{};
    status_->setAccessibleDescription(simpleStatus);
    status_->setText(simpleStatus);
    status_->setVisible(!simpleStatus.isEmpty());
    progress_->setVisible(state.active);
    progress_->setRange(0, state.run ? state.run->totalPositions : 1);
    progress_->setValue(state.run ? state.run->completedPositions : 0);
    if (state.active && state.refinementTotal > 0) {
        progress_->setRange(0, state.refinementTotal);
        progress_->setValue(state.refinementDone);
        progress_->setFormat(
            state.escalationRound > 0
                ? tr("Deeper round %1 of 2: %v of %m positions").arg(state.escalationRound)
                : tr("Refinement: %v of %m positions"));
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
                                             : tr("%1 moves\nshowing %2–%3")
                                                   .arg(total)
                                                   .arg(total ? moveState.offset + 1 : 0)
                                                   .arg(std::min(moveState.offset + 50, total)));
}
void ReviewPage::savePgn() {
    auto& workspace = controller_.workspace();
    if (!workspace.canExportPgn()) {
        return;
    }
    const auto& state = workspace.state();
    const auto run = state.run->id;
    const auto revision = state.run->revision;
    static const QRegularExpression invalid(QStringLiteral(R"([\p{C}<>:"/\\|?*]+)"));
    auto name = state.game.left(72);
    if (!name.isEmpty() && name.back().isHighSurrogate()) {
        name.chop(1);
    }
    name = name.replace(invalid, QStringLiteral(" ")).simplified();
    auto* dialog =
        new QFileDialog(this, tr("Save Annotated PGN"), QString{}, tr("PGN files (*.pgn)"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setDefaultSuffix(QStringLiteral("pgn"));
    dialog->selectFile(name.isEmpty() || name.startsWith(QLatin1Char('.'))
                           ? QStringLiteral("review.pgn")
                           : QStringLiteral("%1 - review.pgn").arg(name));
    connect(dialog, &QFileDialog::fileSelected, &workspace,
            [&workspace, run, revision](const QString& path) {
                workspace.exportPgn(path, run, revision);
            });
    dialog->open();
}

void ReviewPage::updateExportAction() {
    const auto& workspace = controller_.workspace();
    const bool saving = workspace.exportingPgn();
    exportPgn_->setText(saving ? tr("Exporting PGN…") : tr("Export PGN…"));
    const bool available = workspace.canExportPgn();
    exportPgn_->setEnabled(available);
    exportPgn_->setToolTip(
        saving       ? tr("Saving annotated PGN.")
        : !available ? tr("Available after a review has saved results.")
        : workspace.state().run->status == review::JobStatus::Complete
            ? tr("Save the game and its review annotations to a PGN file.")
            : tr("Save the full game with the review annotations available so far."));
    if (saving && exportStatus_->text() != tr("Preparing annotated PGN…")) {
        exportStatus_->setText(tr("Preparing annotated PGN…"));
        exportStatus_->show();
    }
}

void ReviewPage::setBoardDisplayOptions(bool coordinates, bool animation) {
    board_->setDisplayOptions(coordinates, animation);
}

void ReviewPage::updateNavigation() {
    auto& nav = controller_.navigation();
    const auto& state = controller_.state();
    if (nav.selected() == 0) {
        const QSignalBlocker moveSelection(moves_->selectionModel());
        moves_->setCurrentIndex({});
        moves_->clearSelection();
    }
    const auto& activeBoard = controller_.exploration().board();
    board_->setState(activeBoard);
    const bool black = activeBoard.blackAtBottom;
    topSwatch_->setWhite(black);
    bottomSwatch_->setWhite(!black);
    const auto white = nav.whitePlayer().isEmpty() ? tr("White") : nav.whitePlayer();
    const auto blackPlayer = nav.blackPlayer().isEmpty() ? tr("Black") : nav.blackPlayer();
    topPlayer_->setText(black ? white : blackPlayer);
    topPlayer_->setToolTip(QStringLiteral("<qt>%1</qt>").arg(topPlayer_->text().toHtmlEscaped()));
    bottomPlayer_->setText(black ? blackPlayer : white);
    bottomPlayer_->setToolTip(
        QStringLiteral("<qt>%1</qt>").arg(bottomPlayer_->text().toHtmlEscaped()));
    const auto evaluation = nav.evaluation();
    const auto evaluationDescription =
        verbose_ ? evaluation.description
        : evaluation.whiteExpected
            ? tr("White expected score: %1%%2")
                  .arg(*evaluation.whiteExpected * 100, 0, 'f', 1)
                  .arg(evaluation.approximate ? tr(" (approximate)") : QString{})
            : tr("Evaluation unavailable");
    evaluation_->setExpectedScore(evaluation.whiteExpected, evaluationDescription);
    previewLabel_->setText(!nav.error().isEmpty()              ? nav.error()
                           : !nav.ready()                      ? tr("Loading game…")
                           : nav.previewing() && nav.pending() ? tr("Loading preview…")
                           : nav.previewing()                  ? tr("Preview")
                           : nav.pending()                     ? tr("Loading move…")
                                                               : QString{});
    if (nav.selected() != lastPosition_) {
        ++readingRestoreGeneration_;
        lastPosition_ = nav.selected();
        if (nav.selected() > 0) {
            guiding_ = true;
        }
    }
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
    nextMoment_->setEnabled(nav.ready());
    nextMoment_->setText(nav.canNextMoment() ? tr("Continue") : tr("Finish review"));
    const auto* move = controller_.exploration().moment();
    std::optional<persistence::ReviewMove> contextualMove;
    if (move && !controller_.exploration().active() && nav.opponentContext(nav.selected())) {
        contextualMove = *move;
        contextualMove->contextPosition = nav.opponentContext(nav.selected());
        move = &*contextualMove;
    }
    const auto presentation = app::presentReviewMoment(move, nav.selected(), state.active);
    headline_->setText(presentation.move);
    const std::optional<review::Classification> classLabel =
        move && move->classification ? move->classification->label : std::nullopt;
    badge_->setClassification(classLabel);
    classification_->setText(presentation.classification);
    classification_->setVisible(!presentation.classification.isEmpty());
    const int classificationId = classLabel ? static_cast<int>(*classLabel) : -1;
    if (classification_->property("classification").toInt() != classificationId ||
        !classification_->property("classification").isValid()) {
        classification_->setProperty("classification", classificationId);
        classification_->style()->unpolish(classification_);
        classification_->style()->polish(classification_);
    }
    availability_->setText(presentation.availability);
    availability_->setVisible(!presentation.availability.isEmpty());
    explanation_->setText(presentation.explanation);
    alternative_->setText(presentation.alternative.isEmpty()
                              ? QString{}
                              : tr("Best move: %1").arg(presentation.alternative));
    alternativeBox_->setVisible(!presentation.alternative.isEmpty());
    impact_->setText(presentation.impact);
    const auto detail = move ? app::reviewMoveDetails(*move) : QString{};
    if (details_->toPlainText() != detail) {
        details_->setPlainText(detail);
    }
    QPalette railPalette;
    railPalette.setColor(QPalette::Window, reviewMoveColor(move));
    rail_->setPalette(railPalette);
    updateSummary();
    if (verbose_) {
        const auto& run = state.run;
        const QList<QPair<QString, QString>> fields{
            {tr("Review ID"), run ? QString::number(run->id) : QStringLiteral("—")},
            {tr("Revision"), run ? QString::number(run->revision) : QStringLiteral("—")},
            {tr("Engine"), presentedEngineText(state.engine)},
            {tr("Move"), presentation.move},
            {tr("Classification"), presentation.classification},
            {tr("Availability"), presentation.availability},
            {tr("Impact"), presentation.impact},
            {tr("Before evidence ID"),
             move ? QString::number(move->beforeEvidenceId) : QStringLiteral("—")},
            {tr("After evidence ID"),
             move ? QString::number(move->afterEvidenceId) : QStringLiteral("—")},
            {tr("Before depth"), move && move->beforeStrength.depth
                                     ? QString::number(*move->beforeStrength.depth)
                                     : QStringLiteral("—")},
            {tr("After depth"), move && move->afterStrength.depth
                                    ? QString::number(*move->afterStrength.depth)
                                    : QStringLiteral("—")}};
        bindFields(*inspectorFields_, fields);
    }
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
    if (hidden && !retryReadingState_) {
        captureRetryReadingState();
    }
    // Clear solution text as well as hiding its widgets, including accessibility values.
    if (hidden) {
        explanation_->clear();
        details_->clear();
        headline_->setText(tr("Find a stronger move"));
    }
    explanation_->setVisible(!hidden);
    if (hidden) {
        badge_->setClassification(std::nullopt);
    }
    QPalette railPalette;
    railPalette.setColor(QPalette::Window, hidden ? palette().color(QPalette::Mid)
                                                  : reviewMoveColor(exploration.moment()));
    rail_->setPalette(railPalette);
    classification_->setVisible(!hidden && !classification_->text().trimmed().isEmpty());
    availability_->setVisible(!hidden && !availability_->text().isEmpty());
    alternativeBox_->setVisible(!hidden && !alternative_->text().isEmpty());
    if (hidden) {
        classification_->clear();
        availability_->clear();
        alternative_->clear();
        impact_->clear();
        inspectorFields_->clearContents();
        overviewFields_->clearContents();
        summary_->clear();
        chessDetailsToggle_->setChecked(false);
    }
    chessDetailsToggle_->setVisible(!hidden);
    chessDetails_->setVisible(!hidden && chessDetailsToggle_->isChecked());
    evaluation_->setVisible(!exploration.active());
    if (exploration.active()) {
        evaluation_->setExpectedScore(std::nullopt, {});
        previewLabel_->setText(
            state.pendingBoard ? tr("Loading exploration position…")
            : line             ? (state.step == 0
                                      ? (state.length == 1 ? tr("Line preview · 1 move")
                                                           : tr("Line preview · %1 moves").arg(state.length))
                                      : tr("Line preview: move %1 of %2").arg(state.step).arg(state.length))
                               : tr("Practice position"));
    }
    board_->setToolTip(hidden ? tr("Play one legal move to try again.")
                       : line ? tr("Use Left/Right or the line controls to step through the moves.")
                              : tr("Use Left/Right or click a move below to navigate the game."));
    timeline_->setHoverPreview(!exploration.active());
    timeline_->setVisible(!hidden);
    for (const auto& [toggle, content] : sections_) {
        if (hidden && toggle != setupToggle_) {
            toggle->setChecked(false);
        }
        if (toggle == setupToggle_) {
            content->setVisible(!hidden && toggle->isChecked());
        }
    }
    previousMoment_->setVisible(!hidden);
    nextMoment_->setVisible(!hidden);
    comparison_->setVisible(!hidden);
    const bool hasComparison = !hidden && !state.comparisonRows.isEmpty();
    const auto comparisonLines = hidden ? std::vector<int>{} : state.comparisonLines;
    const auto* currentMoment = exploration.moment();
    const auto comparisonStatus = verbose_ ? state.comparisonStatus
                                  : currentMoment && currentMoment->alternatives.needsEvidence()
                                      ? tr("Preliminary comparison.")
                                      : QString{};
    comparisonStatus_->setText(hidden ? QString{} : comparisonStatus);
    comparisonStatus_->setVisible(!hidden && !comparisonStatus.isEmpty());
    comparisonModel_->bind(hidden ? nullptr : exploration.moment(), comparisonLines);
    comparisonRows_->setVisible(hasComparison);
    if (line) {
        for (int row = 0; row < comparisonModel_->rowCount(); ++row) {
            if (comparisonModel_->line(row) == state.candidate &&
                comparisonRows_->currentIndex().row() != row) {
                comparisonRows_->setCurrentIndex(comparisonModel_->index(row, 0));
                break;
            }
        }
    }
    candidates_->setVisible(!hidden);
    if (hidden) {
        candidates_->clear();
    } else {
        QStringList current;
        for (int i = 0; i < candidates_->count(); ++i) {
            current << candidates_->itemText(i);
        }
        if (current != state.candidateNames) {
            const QSignalBlocker blocker(candidates_);
            candidates_->clear();
            candidates_->addItems(state.candidateNames);
            for (int i = 0; i < candidates_->count() && i < state.candidates.size(); ++i) {
                candidates_->setItemData(i, state.candidates.at(i), Qt::ToolTipRole);
            }
        }
    }
    if (line && candidates_->currentIndex() != state.candidate) {
        const QSignalBlocker blocker(candidates_);
        candidates_->setCurrentIndex(state.candidate);
    }
    const bool lineReady = line && !state.pendingBoard;
    showLine_->setVisible(!hidden);
    if (line) {
        const QString showIcon = state.playing ? QStringLiteral("pause") : QStringLiteral("play");
        if (showLine_->property("themeIcon").toString() != showIcon) {
            setIcon(*showLine_, showIcon);
        }
        showLine_->setText(state.playing                ? tr("Pause Line")
                           : state.step >= state.length ? tr("Replay Line")
                                                        : tr("Play Line"));
        showLine_->setEnabled(!state.loading && !state.candidates.empty() && state.length > 0 &&
                              (state.playing || lineReady));
        showLine_->setToolTip(state.playing ? tr("Pause line playback.")
                              : state.step >= state.length
                                  ? tr("Replay this continuation from the start.")
                                  : tr("Play through the continuation moves on the board."));
    } else {
        if (showLine_->property("themeIcon").toString() != QStringLiteral("line")) {
            setIcon(*showLine_, QStringLiteral("line"));
        }
        showLine_->setText(tr("Show Line"));
        showLine_->setEnabled(!state.loading && !state.candidates.empty());
        showLine_->setToolTip(tr("Replay the engine’s continuation on the board."));
    }
    linePreview_->setVisible(line && !hidden);
    if (!line || hidden) {
        lineStepLabel_->clear();
        lineMoves_->clear();
        lineMoves_->hide();
    } else {
        lineStepLabel_->setText(state.pendingBoard ? tr("Loading line…")
                                : state.step == 0
                                    ? (state.length == 1 ? tr("Start · 1 move")
                                                         : tr("Start · %1 moves").arg(state.length))
                                    : tr("Move %1 of %2").arg(state.step).arg(state.length));
        if (state.lineMoves.isEmpty()) {
            lineMoves_->clear();
            lineMoves_->hide();
        } else {
            const auto& pal = palette();
            const QString highlightBg = pal.color(QPalette::Highlight).name(QColor::HexRgb);
            const QString highlightFg = pal.color(QPalette::HighlightedText).name(QColor::HexRgb);
            const QString linkFg = pal.color(QPalette::Link).name(QColor::HexRgb);
            const QString normalFg = pal.color(QPalette::WindowText).name(QColor::HexRgb);
            QStringList tokens;
            tokens.reserve(state.lineMoves.size());
            for (int i = 0; i < state.lineMoves.size(); ++i) {
                const int targetStep = i + 1;
                const QString label = state.lineMoves.at(i).toHtmlEscaped();
                if (targetStep == state.step) {
                    tokens << QStringLiteral(
                                  "<a href=\"%1\" style=\"background-color: %2; color: %3; "
                                  "font-weight: bold; text-decoration: none;\">&nbsp;%4&nbsp;</a>")
                                  .arg(targetStep)
                                  .arg(highlightBg, highlightFg, label);
                } else if (i == state.step) {
                    tokens << QStringLiteral("<a href=\"%1\" style=\"color: %2; font-weight: bold; "
                                             "text-decoration: underline;\">%3</a>")
                                  .arg(targetStep)
                                  .arg(linkFg, label);
                } else {
                    tokens
                        << QStringLiteral(
                               "<a href=\"%1\" style=\"color: %2; text-decoration: none;\">%3</a>")
                               .arg(targetStep)
                               .arg(normalFg, label);
                }
            }
            const QString html = tokens.join(QLatin1Char(' '));
            if (lineMoves_->text() != html) {
                lineMoves_->setText(html);
            }
            lineMoves_->show();
        }
        lineFirst_->setEnabled(lineReady && state.step > 0);
        linePrevious_->setEnabled(lineReady && state.step > 0);
        linePlayPause_->setEnabled(state.length > 0 && (state.playing || lineReady));
        lineNext_->setEnabled(lineReady && state.step < state.length);
        lineLast_->setEnabled(lineReady && state.step < state.length);
        const QString playIcon = state.playing ? QStringLiteral("pause") : QStringLiteral("play");
        if (linePlayPause_->property("themeIcon").toString() != playIcon) {
            setIcon(*linePlayPause_, playIcon);
        }
        const QString playLabel = state.playing                ? tr("Pause")
                                  : state.step >= state.length ? tr("Replay")
                                                               : tr("Play");
        linePlayPause_->setAccessibleName(playLabel);
        linePlayPause_->setToolTip(state.playing                ? tr("Pause continuation")
                                   : state.step >= state.length ? tr("Replay continuation")
                                                                : tr("Play continuation"));
    }
    tryAgain_->setVisible(!hidden);
    tryAgain_->setEnabled(state.canRetry);
    tryAgain_->setToolTip(state.retryToolTip);
    analyze_->setVisible(!hidden);
    analyze_->setEnabled(state.canAnalyze);
    reloadExploration_->setVisible(!hidden && !state.loading && state.candidates.empty());
    reloadExploration_->setEnabled(!exploration.active() &&
                                   controller_.navigation().selected() > 0);
    returnGame_->setVisible(exploration.active());
    returnGame_->setText(hidden ? tr("Cancel practice") : tr("Return to Game"));
    const QString returnIcon = hidden ? QStringLiteral("close") : QStringLiteral("arrow-left");
    if (returnGame_->property("themeIcon").toString() != returnIcon) {
        setIcon(*returnGame_, returnIcon);
    }
    hint_->setVisible(hidden);
    hint_->setEnabled(state.mode == app::ExplorationMode::Retry && !state.pendingBoard &&
                      state.hintLevel != review::RetryHintLevel::Destination);
    reveal_->setVisible(hidden);
    const bool showMessage =
        verbose_ || (exploration.active() && state.mode != app::ExplorationMode::Line);
    auto status =
        state.loading ? tr("Loading continuation…") : (showMessage ? state.message : QString{});
    if (hidden && !state.hints.isEmpty()) {
        status += QStringLiteral("\n\n") + state.hints.join(QLatin1Char('\n'));
    }
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
    explorationStatus_->setVisible(!status.isEmpty());
    explorationBox_->setVisible((line && !hidden) || !status.isEmpty());
    fitRetryFeedback();
    if (hidden && !retryBoardFocused_ && board_->state().interactive && isVisible()) {
        retryBoardFocused_ = true;
        board_->setFocus(Qt::OtherFocusReason);
    }
    if (!exploration.active() && retryReadingState_ && isVisible()) {
        restoreRetryReadingState();
    }
    restoreRetryFocus();
    finishAnalysisReturn();
    if (line) {
        firstMove_->setEnabled(lineReady && state.step > 0);
        previousMove_->setEnabled(lineReady && state.step > 0);
        nextMove_->setEnabled(lineReady && state.step < state.length);
        lastMove_->setEnabled(lineReady && state.step < state.length);
    } else if (hidden) {
        firstMove_->setEnabled(false);
        previousMove_->setEnabled(false);
        nextMove_->setEnabled(false);
        lastMove_->setEnabled(false);
    }
}
void ReviewPage::fitRetryFeedback() {
    // QLabel's one-line minimum can otherwise clip cumulative wrapped hints.
    // Let Qt measure the text at the pane's actual width; the pane owns overflow.
    const int statusHeight =
        !explorationStatus_->text().isEmpty() && explorationStatus_->width() > 0
            ? std::max(0, explorationStatus_->heightForWidth(explorationStatus_->width()))
            : 0;
    if (explorationStatus_->minimumHeight() != statusHeight) {
        explorationStatus_->setMinimumHeight(statusHeight);
    }
    const int movesHeight = !lineMoves_->text().isEmpty() && lineMoves_->width() > 0
                                ? std::max(0, lineMoves_->heightForWidth(lineMoves_->width()))
                                : 0;
    if (lineMoves_->minimumHeight() != movesHeight) {
        lineMoves_->setMinimumHeight(movesHeight);
    }
}
QString ReviewPage::analysisDescription() const {
    const auto& exploration = controller_.exploration().state();
    const auto move = headline_->text();
    if (controller_.navigation().selected() == 0) {
        return tr("Starting position from Review");
    }
    if (exploration.mode == app::ExplorationMode::Line) {
        return tr("Continuation for %1 (step %2 of %3)")
            .arg(move)
            .arg(exploration.step)
            .arg(exploration.length);
    }
    return tr("Review position after %1").arg(move);
}
std::optional<ReviewPage::ReadingState> ReviewPage::captureReadingState() const {
    const auto& state = controller_.state();
    if (!state.run) {
        return {};
    }
    ReadingState saved{state.gameId,
                       state.run->id,
                       controller_.navigation().selected(),
                       chessDetailsToggle_->isChecked(),
                       {},
                       {}};
    for (const auto& [toggle, content] : sections_) {
        saved.sections.push_back(toggle->isChecked());
    }
    const auto capture = [&saved](QAbstractScrollArea* area) {
        saved.scrollBars.emplace_back(area->horizontalScrollBar(),
                                      area->horizontalScrollBar()->value());
        saved.scrollBars.emplace_back(area->verticalScrollBar(),
                                      area->verticalScrollBar()->value());
    };
    capture(explanationScroll_);
    for (auto* area : std::array<QAbstractScrollArea*, 9>{comparisonRows_, moves_, moments_, runs_,
                                                          details_, summary_, inspectorFields_,
                                                          overviewFields_, classificationCounts_}) {
        capture(area);
    }
    for (auto* ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
        if (auto* area = qobject_cast<QAbstractScrollArea*>(ancestor)) {
            capture(area);
        }
    }
    return saved;
}
bool ReviewPage::matchesReadingState(const ReadingState& saved) const {
    const auto& state = controller_.state();
    return state.run && state.gameId == saved.game && state.run->id == saved.run &&
           controller_.navigation().selected() == saved.position;
}
void ReviewPage::restoreReadingSections(const ReadingState& saved) {
    chessDetailsToggle_->setChecked(saved.details);
    for (std::size_t i = 0; i < saved.sections.size(); ++i) {
        sections_[i].first->setChecked(saved.sections[i]);
    }
}
void ReviewPage::rememberAnalysisReadingState() { analysisReadingState_ = captureReadingState(); }
void ReviewPage::restoreAnalysisReadingState(bool returnedToOrigin) {
    analysisReadingGeneration_.reset();
    if (!returnedToOrigin) {
        analysisReadingState_.reset();
        return;
    }
    analysisReadingGeneration_ = readingRestoreGeneration_;
    finishAnalysisReturn();
}
void ReviewPage::finishAnalysisReturn() {
    if (!analysisReadingGeneration_ || !analysisReadingState_) {
        return;
    }
    const auto generation = *analysisReadingGeneration_;
    if (generation != readingRestoreGeneration_ || !matchesReadingState(*analysisReadingState_)) {
        analysisReadingGeneration_.reset();
        analysisReadingState_.reset();
        return;
    }
    if (!isVisible() || !analyze_->isEnabled()) {
        return;
    }
    auto saved = std::move(*analysisReadingState_);
    analysisReadingState_.reset();
    analysisReadingGeneration_.reset();
    if (!controller_.exploration().sameAnalysisOrigin()) {
        return;
    }
    restoreReadingSections(saved);
    analyze_->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, this, [this, saved = std::move(saved), generation] {
        if (!isVisible() || generation != readingRestoreGeneration_ ||
            !matchesReadingState(saved) || !controller_.exploration().sameAnalysisOrigin()) {
            return;
        }
        for (const auto& [bar, value] : saved.scrollBars) {
            bar->setValue(value);
        }
    });
}
void ReviewPage::captureRetryReadingState() {
    auto saved = captureReadingState();
    if (!saved) {
        return;
    }
    ++readingRestoreGeneration_;
    retryFocusPending_ = true;
    retryFocusGeneration_ = readingRestoreGeneration_;
    retryBoardFocused_ = false;
    retryReadingState_ = std::move(saved);
}
void ReviewPage::restoreRetryReadingState() {
    auto saved = std::move(*retryReadingState_);
    retryReadingState_.reset();
    if (!matchesReadingState(saved)) {
        ++readingRestoreGeneration_;
        return;
    }
    restoreReadingSections(saved);
    // Wait for the restored content's layout before applying its scroll positions.
    const auto generation = readingRestoreGeneration_;
    QTimer::singleShot(0, this, [this, saved = std::move(saved), generation] {
        if (generation != readingRestoreGeneration_ || controller_.exploration().active() ||
            !matchesReadingState(saved)) {
            return;
        }
        for (const auto& [bar, value] : saved.scrollBars) {
            if (bar) {
                bar->setValue(value);
            }
        }
    });
}
void ReviewPage::restoreRetryFocus() {
    if (retryFocusGeneration_ != readingRestoreGeneration_) {
        retryFocusPending_ = false;
    }
    if (!controller_.exploration().active() && retryFocusPending_ && isVisible() &&
        window()->isActiveWindow() && tryAgain_->isVisible() && tryAgain_->isEnabled()) {
        retryFocusPending_ = false;
        tryAgain_->setFocus(Qt::OtherFocusReason);
    }
}
} // namespace chess_review::ui
