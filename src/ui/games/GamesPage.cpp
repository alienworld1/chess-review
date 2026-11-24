#include "ui/games/GamesPage.h"
#include "ui/OpeningText.h"
#include "ui/Theme.h"
#include "ui/chess/ChessBoardWidget.h"
#include "ui/games/ImportReportDialog.h"
#include <QAbstractTableModel>
#include <QAction>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QGridLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace chess_review::ui {
namespace {
QString completionText(app::ImportBatchStatus status, bool hasErrors) {
    switch (status) {
    case app::ImportBatchStatus::Completed:
        return hasErrors ? QObject::tr("Import completed with errors")
                         : QObject::tr("Import completed");
    case app::ImportBatchStatus::Cancelled:
        return QObject::tr("Import cancelled");
    case app::ImportBatchStatus::Failed:
        return QObject::tr("Import stopped after a failure");
    case app::ImportBatchStatus::GameLimitReached:
        return QObject::tr("Import stopped: batch game limit reached");
    case app::ImportBatchStatus::Idle:
    case app::ImportBatchStatus::Running:
        return {};
    }
    return {};
}
bool supportedDrop(const QMimeData& mime) {
    if (!mime.hasUrls()) {
        return false;
    }
    return app::ImportController::acceptsFiles(mime.urls());
}
void configureTable(QTableView& table) {
    table.setSelectionBehavior(QAbstractItemView::SelectRows);
    table.setSelectionMode(QAbstractItemView::SingleSelection);
    table.setEditTriggers(QAbstractItemView::NoEditTriggers);
    table.setWordWrap(false);
    table.setAlternatingRowColors(true);
    table.verticalHeader()->hide();
    table.horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table.horizontalHeader()->setStretchLastSection(true);
}
class PaletteRefreshFilter final : public QObject {
  public:
    PaletteRefreshFilter(std::function<void()> callback, QObject* parent)
        : QObject(parent), callback_(std::move(callback)) {}

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::PaletteChange) {
            callback_();
        }
        return false;
    }

  private:
    std::function<void()> callback_;
};

class GamePlayerSwatch final : public QWidget {
  public:
    GamePlayerSwatch(bool white, QWidget* parent) : QWidget(parent), white_(white) {
        setFixedSize(10, 10);
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
    bool white_;
};

bool isMeaningfulTag(const QString& value) {
    const auto trimmed = value.trimmed();
    return !trimmed.isEmpty() && trimmed != QLatin1String("?") && trimmed != QLatin1String("-") &&
           trimmed != QLatin1String("????.??.??");
}

QLabel* detailValueLabel(const QString& text, QWidget* parent, const char* role = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    if (role) {
        setTextRole(*label, role);
    }
    return label;
}

void addDetailRow(QGridLayout& grid, QWidget* parent, const QString& labelText,
                  const QString& valueText, const char* valueRole = nullptr,
                  const QString& toolTip = {}) {
    const int row = grid.rowCount();
    auto* label = new QLabel(labelText, parent);
    label->setTextFormat(Qt::PlainText);
    setTextRole(*label, "caption");
    auto* value = detailValueLabel(valueText, parent, valueRole);
    if (!toolTip.isEmpty()) {
        value->setToolTip(QStringLiteral("<qt>%1</qt>").arg(toolTip.toHtmlEscaped()));
    }
    grid.addWidget(label, row, 0, Qt::AlignLeft | Qt::AlignTop);
    grid.addWidget(value, row, 1);
}
} // namespace

class LibraryTableModel final : public QAbstractTableModel {
  public:
    static constexpr int openingColumn = 8;
    LibraryTableModel(app::GameLibraryController& controller, QObject* parent)
        : QAbstractTableModel(parent), controller_(controller) {}
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : openingColumn + 1;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 ||
            section > openingColumn) {
            return {};
        }
        const QStringList labels{tr("White"),         tr("Black"),        tr("Result"),
                                 tr("Played date"),   tr("Time control"), tr("Imported (UTC)"),
                                 tr("Review status"), tr("Accuracy"),     tr("Opening")};
        return labels.at(section);
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() ||
            index.column() < 0 || index.column() > openingColumn) {
            return {};
        }
        const auto& row = rows_[static_cast<std::size_t>(index.row())];
        if (index.column() == openingColumn) {
            const auto text = openingText(row.opening);
            if (role == Qt::ToolTipRole && !text.isEmpty()) {
                return QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped());
            }
            if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) {
                return text;
            }
            return {};
        }
        if (role == Qt::ToolTipRole) {
            if (index.column() == 6) {
                return controller_.reviewStatus(row.id).toHtmlEscaped();
            }
            if (index.column() == 7) {
                const auto accuracy = controller_.reviewAccuracy(row.id);
                if (accuracy == QStringLiteral("—")) {
                    return {};
                }
                const auto side = controller_.reviewedSide(row.id);
                const auto tip =
                    side ? tr("%1 (%2)").arg(accuracy, *side == chess::Color::White ? tr("White")
                                                                                    : tr("Black"))
                         : accuracy;
                return tip.toHtmlEscaped();
            }
            if (row.metadataError) {
                return tr("Game ID %1: %2")
                    .arg(row.id)
                    .arg(row.metadataError->message)
                    .toHtmlEscaped();
            }
            const auto value = data(index, Qt::DisplayRole).toString();
            return value.isEmpty()
                       ? QVariant{}
                       : QVariant{QStringLiteral("<qt>%1</qt>").arg(value.toHtmlEscaped())};
        }
        if (role != Qt::DisplayRole && role != Qt::AccessibleTextRole) {
            return {};
        }
        if (row.metadataError) {
            return index.column() == 0 ? tr("Unreadable metadata (ID %1)").arg(row.id) : QString{};
        }
        if (index.column() == 6) {
            return controller_.reviewStatus(row.id);
        }
        if (index.column() == 7) {
            const auto accuracy = controller_.reviewAccuracy(row.id);
            if (accuracy == QStringLiteral("—")) {
                return QString{};
            }
            const auto side = controller_.reviewedSide(row.id);
            return side ? tr("%1 (%2)").arg(accuracy,
                                            *side == chess::Color::White ? tr("W") : tr("B"))
                        : accuracy;
        }
        const QStringList values{row.white,    row.black,       row.result,
                                 row.playedAt, row.timeControl, row.importedAt};
        auto value = values.at(index.column());
        if (index.column() == 2 && value == u"*") {
            return tr("No result");
        }
        if ((index.column() == 3 || index.column() == 4) && !isMeaningfulTag(value)) {
            return QString{};
        }
        if (value.isEmpty()) {
            value = tr("Unknown");
        }
        if (row.metadataTruncated && index.column() == 0) {
            value += tr(" [truncated]");
        }
        return value;
    }
    std::optional<persistence::GameId> gameId(int row) const {
        if (row < 0 || row >= rowCount()) {
            return std::nullopt;
        }
        return rows_[static_cast<std::size_t>(row)].id;
    }
    bool reload() {
        if (version_ == controller_.rowsVersion()) {
            return false;
        }
        beginResetModel();
        version_ = controller_.rowsVersion();
        rows_ = controller_.rows();
        endResetModel();
        return true;
    }
    void updateReviews() {
        if (rowCount() > 0) {
            emit dataChanged(index(0, 6), index(rowCount() - 1, 7));
        }
    }
    const persistence::GameSummary* summary(int row) const {
        return gameId(row) ? &rows_[static_cast<std::size_t>(row)] : nullptr;
    }
    void updateOpening(persistence::GameId id, const persistence::OpeningResolution& opening) {
        const auto row = std::ranges::find(rows_, id, &persistence::GameSummary::id);
        if (row == rows_.end()) {
            return;
        }
        row->opening = opening;
        const auto cell = index(static_cast<int>(row - rows_.begin()), openingColumn);
        emit dataChanged(cell, cell, {Qt::DisplayRole, Qt::AccessibleTextRole, Qt::ToolTipRole});
    }

  private:
    app::GameLibraryController& controller_;
    std::vector<persistence::GameSummary> rows_;
    std::uint64_t version_ = 0;
};

GamesPage::GamesPage(app::ImportController& imports, app::GameLibraryController& library,
                     QAction& fileAction, QAction& pasteAction, QAction& linkAction,
                     QWidget* parent)
    : QWidget(parent), imports_(imports), library_(library) {
    setObjectName(QStringLiteral("gamesPage"));
    setAcceptDrops(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* headerRow = new QHBoxLayout;
    headerRow->setSpacing(8);
    auto* heading = new QLabel(tr("Games"), this);
    setTextRole(*heading, "title");
    headerRow->addWidget(heading);
    headerRow->addStretch();
    refresh_ = new QPushButton(tr("Refresh"), this);
    refresh_->setObjectName(QStringLiteral("refreshGames"));
    setQuietAction(*refresh_);
    setIcon(*refresh_, QStringLiteral("refresh"));
    headerRow->addWidget(refresh_);
    auto* statistics = new QPushButton(tr("Library Statistics…"), this);
    statistics->setObjectName(QStringLiteral("libraryStatisticsButton"));
    setQuietAction(*statistics);
    setIcon(*statistics, QStringLiteral("summary"));
    connect(statistics, &QPushButton::clicked, this, &GamesPage::showStatistics);
    headerRow->addWidget(statistics);
    layout->addLayout(headerRow);

    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    const std::array importIcons{"import", "paste", "link"};
    const std::array importActions{&fileAction, &pasteAction, &linkAction};
    for (std::size_t i = 0; i < importActions.size(); ++i) {
        auto* button = new QToolButton(this);
        button->setDefaultAction(importActions[i]);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        setIcon(*button, QString::fromLatin1(importIcons[i]));
        toolbar->addWidget(button);
    }
    toolbar->addStretch();
    layout->addLayout(toolbar);

    auto* searchBar = new QHBoxLayout;
    searchBar->setSpacing(8);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("gameSearch"));
    search_->setAccessibleName(tr("Search players"));
    search_->setPlaceholderText(tr("Search White or Black players"));
    search_->setClearButtonEnabled(true);
    search_->setAcceptDrops(false);
    search_->setMaxLength(persistence::GameLibraryQuery::maxSearchLength);
    auto* searchIcon = new QAction(search_);
    const auto updateSearchIcon = [searchIcon, this] {
        const auto color = palette().color(QPalette::PlaceholderText);
        searchIcon->setIcon(themedIcon(QStringLiteral("search"), color, color));
    };
    updateSearchIcon();
    search_->addAction(searchIcon, QLineEdit::LeadingPosition);
    search_->installEventFilter(new PaletteRefreshFilter(updateSearchIcon, search_));
    searchBar->addWidget(search_, 1);
    toggleFilters_ = new QPushButton(tr("Filters"), this);
    toggleFilters_->setObjectName(QStringLiteral("toggleGameFilters"));
    toggleFilters_->setCheckable(true);
    setIcon(*toggleFilters_, QStringLiteral("filter"));
    searchBar->addWidget(toggleFilters_);
    togglePreview_ = new QPushButton(tr("Details"), this);
    togglePreview_->setObjectName(QStringLiteral("toggleGamePreview"));
    togglePreview_->setCheckable(true);
    setIcon(*togglePreview_, QStringLiteral("board"));
    searchBar->addWidget(togglePreview_);
    layout->addLayout(searchBar);
    auto* find = new QShortcut(QKeySequence::Find, this);
    find->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QShortcut::activated, search_, [this] {
        search_->setFocus();
        search_->selectAll();
    });
    auto* filters = new QWidget(this);
    auto* filterBar = new QHBoxLayout(filters);
    filterBar->setContentsMargins(0, 0, 0, 0);
    resultFilter_ = new QComboBox(filters);
    resultFilter_->setObjectName(QStringLiteral("gameResultFilter"));
    resultFilter_->setAccessibleName(tr("Result filter"));
    resultFilter_->addItems(
        {tr("Any result"), tr("White won"), tr("Black won"), tr("Draw"), tr("No result")});
    filterBar->addWidget(resultFilter_, 1);
    reviewFilter_ = new QComboBox(filters);
    reviewFilter_->setObjectName(QStringLiteral("gameReviewFilter"));
    reviewFilter_->setAccessibleName(tr("Saved review filter"));
    reviewFilter_->addItems(
        {tr("Any review status"), tr("Has saved review"), tr("No saved review")});
    filterBar->addWidget(reviewFilter_, 1);
    sort_ = new QComboBox(filters);
    sort_->setObjectName(QStringLiteral("gameSort"));
    sort_->setAccessibleName(tr("Sort games"));
    sort_->addItems({tr("Newest imports"), tr("Oldest imports"), tr("Latest played"),
                     tr("White player A–Z"), tr("Black player A–Z")});
    filterBar->addWidget(sort_, 1);
    clearFilters_ = new QPushButton(tr("Reset"), filters);
    clearFilters_->setToolTip(tr("Clear player search and filters, and restore the default sort"));
    filterBar->addWidget(clearFilters_);
    filters->hide();
    layout->addWidget(filters);
    connect(toggleFilters_, &QPushButton::toggled, filters, &QWidget::setVisible);
    searchTimer_ = new QTimer(this);
    searchTimer_->setSingleShot(true);
    searchTimer_->setInterval(180);
    connect(searchTimer_, &QTimer::timeout, this, &GamesPage::applyQuery);
    connect(search_, &QLineEdit::textChanged, this, [this] {
        searchTimer_->start();
        updateOpenAction();
    });
    connect(search_, &QLineEdit::returnPressed, this, &GamesPage::applyQuery);
    for (auto* combo : {resultFilter_, reviewFilter_, sort_}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(12);
        connect(combo, &QComboBox::currentIndexChanged, this, &GamesPage::applyQuery);
    }
    connect(clearFilters_, &QPushButton::clicked, this, [this] {
        const QSignalBlocker result(resultFilter_);
        const QSignalBlocker review(reviewFilter_);
        const QSignalBlocker sort(sort_);
        search_->clear();
        resultFilter_->setCurrentIndex(0);
        reviewFilter_->setCurrentIndex(0);
        sort_->setCurrentIndex(0);
        applyQuery();
    });
    connect(togglePreview_, &QPushButton::clicked, this, [this](bool visible) {
        previewPreference_ = visible;
        updatePreviewVisibility();
    });

    review_ = new QPushButton(tr("Review…"), this);
    review_->setObjectName(QStringLiteral("reviewGameButton"));
    setPrimaryAction(*review_);
    setIcon(*review_, QStringLiteral("play"));
    open_ = new QPushButton(tr("Open in Analyze"), this);
    open_->setObjectName(QStringLiteral("openGameButton"));
    open_->setToolTip(tr("Open the selected game in Analyze."));
    setIcon(*open_, QStringLiteral("analyze"));
    remove_ = new QPushButton(tr("Delete…"), this);
    remove_->setObjectName(QStringLiteral("deleteGameButton"));
    setQuietAction(*remove_);
    setIcon(*remove_, QStringLiteral("trash"));
    connect(remove_, &QPushButton::clicked, this, &GamesPage::confirmDeletion);
    connect(review_, &QPushButton::clicked, this, [this] {
        if (const auto id = libraryModel_->gameId(games_->currentIndex().row());
            id && review_->isEnabled()) {
            emit reviewRequested(*id);
        }
    });
    connect(open_, &QPushButton::clicked, this, &GamesPage::requestOpen);
    libraryStatus_ = new QLabel(this);
    libraryStatus_->setTextFormat(Qt::PlainText);
    libraryStatus_->setWordWrap(true);
    libraryStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                            Qt::TextSelectableByKeyboard);
    libraryStatus_->setObjectName(QStringLiteral("libraryStatus"));
    setTextRole(*libraryStatus_, "caption");
    layout->addWidget(libraryStatus_);
    deletionStatus_ = new QLabel(this);
    deletionStatus_->setObjectName(QStringLiteral("gameDeletionStatus"));
    deletionStatus_->setTextFormat(Qt::PlainText);
    deletionStatus_->setWordWrap(true);
    deletionStatus_->hide();
    layout->addWidget(deletionStatus_);
    auto* content = new QHBoxLayout;
    layout->addLayout(content, 1);
    libraryModel_ = new LibraryTableModel(library, this);
    games_ = new QTableView(this);
    games_->setObjectName(QStringLiteral("gamesTable"));
    games_->setAccessibleName(tr("Saved games"));
    games_->setModel(libraryModel_);
    configureTable(*games_);
    games_->horizontalHeader()->setStretchLastSection(false);
    games_->horizontalHeader()->setMinimumSectionSize(60);
    games_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    games_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    connect(games_, &QTableView::doubleClicked, this, &GamesPage::requestOpen);
    auto* openSelected = new QShortcut(games_);
    openSelected->setKeys({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)});
    openSelected->setContext(Qt::WidgetWithChildrenShortcut);
    connect(openSelected, &QShortcut::activated, this, &GamesPage::requestOpen);
    connect(games_->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this] {
        selectedGame_ = libraryModel_->gameId(games_->currentIndex().row());
        updateOpenAction();
        updatePreviewVisibility();
    });
    games_->setColumnWidth(2, 86);
    games_->setColumnWidth(3, 94);
    games_->setColumnWidth(6, 110);
    games_->setColumnHidden(5, true);
    games_->setColumnHidden(4, true);
    games_->setShowGrid(false);
    games_->verticalHeader()->setDefaultSectionSize(38);
    games_->setColumnWidth(7, 116);
    // Keep review results visible before secondary metadata on laptop displays.
    games_->horizontalHeader()->moveSection(games_->horizontalHeader()->visualIndex(6), 3);
    games_->horizontalHeader()->moveSection(games_->horizontalHeader()->visualIndex(7), 4);
    games_->setColumnWidth(LibraryTableModel::openingColumn, 165);
    games_->setColumnWidth(5, 200);
    games_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(games_, &QTableView::customContextMenuRequested, this, [this](const QPoint& point) {
        const auto index = games_->indexAt(point);
        if (!index.isValid() && games_->viewport()->rect().contains(point)) {
            return;
        }
        if (index.isValid()) {
            games_->setCurrentIndex(index);
        }
        auto* menu = new QMenu(games_);
        connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
        for (auto* button : {open_, review_, remove_}) {
            auto* action = menu->addAction(button->text(), button, &QPushButton::click);
            action->setEnabled(button->isEnabled());
        }
        menu->popup(games_->viewport()->mapToGlobal(point));
    });
    content->addWidget(games_, 1);
    previewPane_ = new QWidget(this);
    previewPane_->setObjectName(QStringLiteral("selectedGamePreview"));
    previewPane_->setFixedWidth(296);
    setPanelSurface(*previewPane_);
    auto* previewLayout = new QVBoxLayout(previewPane_);
    previewLayout->setContentsMargins(14, 14, 14, 14);
    previewLayout->setSpacing(10);
    auto* previewHeader = new QHBoxLayout;
    previewHeader->setSpacing(8);
    previewHeading_ = new QLabel(tr("Game details"), previewPane_);
    setTextRole(*previewHeading_, "heading");
    previewHeader->addWidget(previewHeading_, 1);
    previewStatus_ = new QLabel(previewPane_);
    previewStatus_->setTextFormat(Qt::PlainText);
    setTextRole(*previewStatus_, "caption");
    previewHeader->addWidget(previewStatus_);
    previewLayout->addLayout(previewHeader);
    board_ = new ChessBoardWidget(previewPane_);
    board_->setObjectName(QStringLiteral("gamePreviewBoard"));
    board_->setAccessibleName(tr("Final position"));
    board_->setFocusPolicy(Qt::NoFocus);
    board_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    board_->setFixedSize(184, 184);
    previewLayout->addWidget(board_, 0, Qt::AlignHCenter);
    metadata_ = new QScrollArea(previewPane_);
    metadata_->setObjectName(QStringLiteral("gameMetadata"));
    metadata_->setAccessibleName(tr("Selected game details"));
    metadata_->setWidgetResizable(true);
    metadata_->setFrameShape(QFrame::NoFrame);
    metadata_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    metadata_->viewport()->setAutoFillBackground(false);
    setFlatSurface(*metadata_);
    metadata_->setMinimumHeight(80);
    previewLayout->addWidget(metadata_, 1);
    content->addWidget(previewPane_);
    previous_ = new QPushButton(tr("Previous"), this);
    next_ = new QPushButton(tr("Next"), this);
    previous_->setObjectName(QStringLiteral("previousGames"));
    next_->setObjectName(QStringLiteral("nextGames"));
    setQuietAction(*previous_);
    setQuietAction(*next_);
    setIcon(*previous_, QStringLiteral("chevron-left"));
    setIcon(*next_, QStringLiteral("chevron-right"));
    auto* footer = new QHBoxLayout;
    footer->setSpacing(8);
    footer->addWidget(review_);
    footer->addWidget(open_);
    footer->addWidget(remove_);
    footer->addStretch();
    footer->addWidget(previous_);
    footer->addWidget(next_);
    layout->addLayout(footer);
    auto* importBar = new QWidget(this);
    importBar->setObjectName(QStringLiteral("importProgressBar"));
    setSubtleSurface(*importBar);
    auto* progress = new QHBoxLayout(importBar);
    progress->setContentsMargins(12, 8, 12, 8);
    progress->setSpacing(8);
    importStatus_ = new QLabel(importBar);
    importStatus_->setTextFormat(Qt::PlainText);
    importStatus_->setWordWrap(true);
    importStatus_->setObjectName(QStringLiteral("importStatus"));
    importStatus_->setAccessibleName(tr("Import progress"));
    importStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                           Qt::TextSelectableByKeyboard);
    progress->addWidget(importStatus_, 1);
    cancel_ = new QPushButton(tr("Cancel import"), importBar);
    cancel_->setObjectName(QStringLiteral("cancelImport"));
    setIcon(*cancel_, QStringLiteral("stop"));
    progress->addWidget(cancel_);
    report_ = new QPushButton(tr("Import report"), importBar);
    report_->setObjectName(QStringLiteral("importReportButton"));
    setQuietAction(*report_);
    setIcon(*report_, QStringLiteral("info"));
    progress->addWidget(report_);
    layout->insertWidget(1, importBar);
    importBar->hide();
    setTabOrder(refresh_, importStatus_);
    setTabOrder(importStatus_, cancel_);
    setTabOrder(cancel_, report_);
    setTabOrder(report_, search_);
    setTabOrder(games_, review_);
    setTabOrder(review_, open_);
    setTabOrder(open_, remove_);
    setTabOrder(remove_, previous_);
    setTabOrder(previous_, next_);
    reportDialog_ = new ImportReportDialog(imports, this);
    connect(report_, &QPushButton::clicked, reportDialog_, &QDialog::show);
    connect(reportDialog_, &ImportReportDialog::openRequested, this, &GamesPage::openRequested);
    connect(reportDialog_, &ImportReportDialog::reviewRequested, this, &GamesPage::reviewRequested);
    updateTimer_ = new QTimer(this);
    updateTimer_->setSingleShot(true);
    updateTimer_->setInterval(100);
    connect(updateTimer_, &QTimer::timeout, this, &GamesPage::updateImport);
    connect(&imports_, &app::ImportController::changed, this, [this] {
        if (!updateTimer_->isActive()) {
            updateTimer_->start();
        }
    });
    connect(&imports_, &app::ImportController::submitted, this, [this] {
        reportDialog_->resetBatch();
        updateImport();
    });
    connect(&imports_, &app::ImportController::finished, this, &GamesPage::updateImport);
    connect(&library_, &app::GameLibraryController::changed, this, &GamesPage::updateLibrary);
    connect(&library_, &app::GameLibraryController::openingChanged, libraryModel_,
            &LibraryTableModel::updateOpening);
    connect(&library_, &app::GameLibraryController::reviewsChanged, libraryModel_,
            &LibraryTableModel::updateReviews);
    connect(&library_, &app::GameLibraryController::reviewsChanged, this,
            &GamesPage::updatePreview);
    connect(&library_, &app::GameLibraryController::previewChanged, this,
            &GamesPage::updatePreview);
    connect(&library_, &app::GameLibraryController::deletionFailed, this,
            [this](const QString& message) {
                deletionStatus_->setText(message);
                deletionStatus_->show();
            });
    connect(&library_, &app::GameLibraryController::removed, this, [this](persistence::GameId id) {
        reportDialog_->gameRemoved(id);
        deletionStatus_->setText(tr("Game deleted from your library."));
        deletionStatus_->show();
    });
    connect(refresh_, &QPushButton::clicked, this, [this] { library_.refresh(); });
    connect(previous_, &QPushButton::clicked, &library_, &app::GameLibraryController::previous);
    connect(next_, &QPushButton::clicked, &library_, &app::GameLibraryController::next);
    connect(cancel_, &QPushButton::clicked, &imports_, &app::ImportController::cancel);
    updateLibrary();
    updateImport();
}
void GamesPage::requestOpen() {
    if (selectedGame_ && open_->isEnabled()) {
        emit openRequested(*selectedGame_);
    }
}
void GamesPage::updateOpenAction() {
    const auto* game = libraryModel_->summary(games_->currentIndex().row());
    const bool selected = game && game->id > 0 && library_.ready() && !library_.busy() &&
                          !library_.opening() && !library_.deleting() && !searchTimer_->isActive();
    review_->setEnabled(selected && !game->metadataError);
    open_->setEnabled(selected && !game->metadataError);
    remove_->setEnabled(selected && !imports_.busy());
}
void GamesPage::updateLibrary() {
    {
        const QSignalBlocker selection(games_->selectionModel());
        const auto selected = selectedGame_;
        if (library_.showingGames() && !library_.busy() && libraryModel_->reload()) {
            if (selected) {
                for (int row = 0; row < libraryModel_->rowCount(); ++row) {
                    if (libraryModel_->gameId(row) == selected) {
                        games_->setCurrentIndex(libraryModel_->index(row, 0));
                        break;
                    }
                }
            }
            selectedGame_ = libraryModel_->gameId(games_->currentIndex().row());
        }
    }
    updateOpenAction();
    updatePreviewVisibility();
    reportDialog_->setActionsAvailable(library_.ready() && !library_.opening() &&
                                       !library_.deleting());
    libraryStatus_->setText(
        library_.opening()            ? tr("Opening saved game…")
        : !library_.ready()           ? tr("Local storage is unavailable.")
        : library_.deleting()         ? tr("Deleting saved game…")
        : library_.busy()             ? tr("Loading games…")
        : !library_.error().isEmpty() ? library_.error()
        : library_.rows().size() == 1 ? tr("Page %1 · 1 game").arg(library_.page() + 1)
        : !library_.rows().empty()
            ? tr("Page %1 · %2 games").arg(library_.page() + 1).arg(library_.rows().size())
        : library_.query().filtered() ? tr("No games match your search or filters.")
                                      : tr("Import a game or drop PGN files here to get started."));
    refresh_->setEnabled(library_.ready() && !library_.busy());
    previous_->setEnabled(library_.ready() && !library_.busy() && library_.page() > 0);
    next_->setEnabled(library_.ready() && !library_.busy() && library_.hasNext());
}
void GamesPage::applyQuery() {
    searchTimer_->stop();
    deletionStatus_->hide();
    const auto activeFilters =
        (resultFilter_->currentIndex() != 0 ? 1 : 0) + (reviewFilter_->currentIndex() != 0 ? 1 : 0);
    toggleFilters_->setText(activeFilters ? tr("Filters (%1)").arg(activeFilters) : tr("Filters"));
    library_.setQuery({search_->text(),
                       static_cast<persistence::GameResultFilter>(resultFilter_->currentIndex()),
                       static_cast<persistence::GameReviewFilter>(reviewFilter_->currentIndex()),
                       static_cast<persistence::GameSort>(sort_->currentIndex())});
    updateOpenAction();
}
void GamesPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updatePreviewVisibility();
}
void GamesPage::updatePreviewVisibility() {
    const bool expanded = previewPreference_.value_or(width() >= 960);
    togglePreview_->setChecked(expanded);
    previewPane_->setVisible(expanded);
    library_.selectPreview(expanded ? selectedGame_ : std::nullopt);
    if (!expanded || !selectedGame_) {
        updatePreview();
    }
}
void GamesPage::updatePreview() {
    const auto& preview = library_.preview();
    const bool show = !previewPane_->isHidden() && preview && preview->summary.id == selectedGame_;
    board_->setVisible(show);
    metadata_->setVisible(show);
    if (!show) {
        displayedPreview_.reset();
        displayedMetadataSignature_.clear();
        previewHeading_->setText(tr("Game details"));
        previewStatus_->setText(!library_.previewError().isEmpty() ? library_.previewError()
                                : library_.previewLoading()        ? tr("Loading game details…")
                                                            : tr("Select a game to view details."));
        previewStatus_->setVisible(true);
        return;
    }
    const auto& game = preview->summary;
    const bool gameChanged = displayedPreview_ != game.id;
    if (gameChanged) {
        displayedPreview_ = game.id;
        app::BoardViewState board;
        board.pieces = preview->board.pieces;
        board.sideToMove = preview->board.sideToMove;
        board.lastMove = preview->lastMove;
        previewHeading_->setText(preview->plies > 0 ? tr("Final position")
                                                    : tr("Starting position"));
        board.positionDescription = previewHeading_->text();
        board_->setState(board);
        const auto fullMoves = (preview->plies + 1) / 2;
        previewStatus_->setText(
            preview->plies > 0 ? (fullMoves == 1 ? tr("1 move") : tr("%1 moves").arg(fullMoves))
                               : QString{});
        previewStatus_->setVisible(!previewStatus_->text().isEmpty());
    }
    const auto whiteName = isMeaningfulTag(game.white) ? game.white : tr("White");
    const auto blackName = isMeaningfulTag(game.black) ? game.black : tr("Black");
    const auto resultText = game.result == u"*" ? tr("No result") : game.result;
    const auto reviewStatus = library_.reviewStatus(game.id);
    const auto reviewedSide = library_.reviewedSide(game.id);
    const auto rawAccuracy = library_.reviewAccuracy(game.id);
    const auto accuracyText =
        reviewedSide && rawAccuracy != QStringLiteral("—")
            ? tr("%1 (%2)").arg(rawAccuracy,
                                *reviewedSide == chess::Color::White ? tr("White") : tr("Black"))
            : QString{};
    const auto resolvedOpening = openingText(game.opening);
    const auto imported = QDateTime::fromString(game.importedAt, Qt::ISODateWithMs).toLocalTime();
    const auto importedText =
        imported.isValid() ? QLocale().toString(imported, QLocale::ShortFormat) : QString{};
    const auto& source = preview->source;
    const auto sourceText = source.type == u"paste"       ? tr("Pasted PGN")
                            : source.type == u"file"      ? tr("Local file")
                            : source.type == u"lichess"   ? QStringLiteral("Lichess")
                            : source.type == u"chess.com" ? QStringLiteral("Chess.com")
                                                          : source.type;
    const QUrl uri(source.uri);
    const auto sourceLocation =
        source.uri.isEmpty()
            ? QString{}
            : (source.type == u"file" && uri.isLocalFile() ? uri.toLocalFile() : source.uri);
    const QString signature =
        QStringList{QString::number(game.id),
                    whiteName,
                    blackName,
                    resultText,
                    reviewStatus,
                    accuracyText,
                    resolvedOpening,
                    preview->openingTag,
                    preview->ecoTag,
                    game.playedAt,
                    game.timeControl,
                    preview->event,
                    preview->site,
                    preview->round,
                    sourceText,
                    sourceLocation,
                    importedText,
                    game.metadataTruncated ? QStringLiteral("1") : QStringLiteral("0"),
                    verbose_ ? QStringLiteral("v") : QStringLiteral("c")}
            .join(QChar(0x1f));
    if (displayedMetadataSignature_ != signature) {
        const int scroll = gameChanged ? 0 : metadata_->verticalScrollBar()->value();
        displayedMetadataSignature_ = signature;
        auto* body = new QWidget(metadata_);
        body->setAutoFillBackground(false);
        auto* bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(0, 2, 4, 2);
        bodyLayout->setSpacing(10);

        auto* playersLayout = new QVBoxLayout;
        playersLayout->setSpacing(4);
        const auto addPlayerRow = [body, playersLayout](bool white, const QString& name) {
            auto* row = new QHBoxLayout;
            row->setSpacing(8);
            row->addWidget(new GamePlayerSwatch(white, body), 0, Qt::AlignVCenter);
            row->addWidget(detailValueLabel(name, body, "strong"), 1);
            playersLayout->addLayout(row);
        };
        addPlayerRow(true, whiteName);
        addPlayerRow(false, blackName);
        if (isMeaningfulTag(resultText)) {
            auto* resultRow = new QHBoxLayout;
            resultRow->setSpacing(8);
            auto* resultLabel = new QLabel(tr("Result"), body);
            setTextRole(*resultLabel, "caption");
            resultRow->addWidget(resultLabel);
            resultRow->addWidget(detailValueLabel(resultText, body, "strong"), 1);
            playersLayout->addLayout(resultRow);
        }
        bodyLayout->addLayout(playersLayout);

        auto* reviewBox = new QWidget(body);
        setSubtleSurface(*reviewBox);
        auto* reviewGrid = new QGridLayout(reviewBox);
        reviewGrid->setContentsMargins(10, 8, 10, 8);
        reviewGrid->setHorizontalSpacing(10);
        reviewGrid->setVerticalSpacing(4);
        reviewGrid->setColumnStretch(1, 1);
        addDetailRow(*reviewGrid, reviewBox, tr("Review"), reviewStatus);
        if (!accuracyText.isEmpty()) {
            addDetailRow(*reviewGrid, reviewBox, tr("Accuracy"), accuracyText, "strong");
        }
        bodyLayout->addWidget(reviewBox);

        auto* infoWidget = new QWidget(body);
        infoWidget->setAutoFillBackground(false);
        auto* infoGrid = new QGridLayout(infoWidget);
        infoGrid->setContentsMargins(0, 0, 0, 0);
        infoGrid->setHorizontalSpacing(10);
        infoGrid->setVerticalSpacing(4);
        infoGrid->setColumnStretch(1, 1);
        if (!resolvedOpening.isEmpty()) {
            addDetailRow(*infoGrid, infoWidget, tr("Opening"), resolvedOpening);
            if (verbose_) {
                if (isMeaningfulTag(preview->openingTag) &&
                    preview->openingTag != resolvedOpening) {
                    addDetailRow(*infoGrid, infoWidget, tr("PGN opening"), preview->openingTag);
                }
                if (isMeaningfulTag(preview->ecoTag) &&
                    !resolvedOpening.contains(preview->ecoTag)) {
                    addDetailRow(*infoGrid, infoWidget, tr("PGN ECO"), preview->ecoTag);
                }
            }
        } else {
            if (isMeaningfulTag(preview->openingTag)) {
                addDetailRow(*infoGrid, infoWidget, tr("Opening"), preview->openingTag);
            }
            if (isMeaningfulTag(preview->ecoTag) &&
                (!isMeaningfulTag(preview->openingTag) || verbose_)) {
                addDetailRow(*infoGrid, infoWidget, tr("ECO"), preview->ecoTag);
            }
        }
        if (isMeaningfulTag(game.playedAt)) {
            addDetailRow(*infoGrid, infoWidget, tr("Played"), game.playedAt);
        }
        if (isMeaningfulTag(game.timeControl)) {
            addDetailRow(*infoGrid, infoWidget, tr("Time"), game.timeControl);
        }
        if (isMeaningfulTag(preview->event)) {
            addDetailRow(*infoGrid, infoWidget, tr("Event"), preview->event);
        }
        if (isMeaningfulTag(preview->site)) {
            addDetailRow(*infoGrid, infoWidget, tr("Site"), preview->site);
        }
        if (isMeaningfulTag(preview->round)) {
            addDetailRow(*infoGrid, infoWidget, tr("Round"), preview->round);
        }
        if (infoGrid->rowCount() > 0) {
            bodyLayout->addWidget(makeSeparator(body));
            bodyLayout->addWidget(infoWidget);
        } else {
            delete infoWidget;
        }

        auto* sourceWidget = new QWidget(body);
        sourceWidget->setAutoFillBackground(false);
        auto* sourceGrid = new QGridLayout(sourceWidget);
        sourceGrid->setContentsMargins(0, 0, 0, 0);
        sourceGrid->setHorizontalSpacing(10);
        sourceGrid->setVerticalSpacing(4);
        sourceGrid->setColumnStretch(1, 1);
        if (!sourceText.isEmpty()) {
            addDetailRow(*sourceGrid, sourceWidget, tr("Source"), sourceText, "secondary",
                         sourceLocation);
        }
        if (verbose_ && !sourceLocation.isEmpty()) {
            addDetailRow(*sourceGrid, sourceWidget, tr("Location"), sourceLocation, "secondary");
        }
        if (!importedText.isEmpty()) {
            addDetailRow(*sourceGrid, sourceWidget, tr("Imported"), importedText, "secondary");
        }
        if (sourceGrid->rowCount() > 0) {
            bodyLayout->addWidget(makeSeparator(body));
            bodyLayout->addWidget(sourceWidget);
        } else {
            delete sourceWidget;
        }

        if (game.metadataTruncated) {
            bodyLayout->addWidget(detailValueLabel(tr("Long fields are shortened in this preview."),
                                                   body, "caption"));
        }
        bodyLayout->addStretch();
        metadata_->setWidget(body);
        metadata_->verticalScrollBar()->setValue(scroll);
    }
    previewPane_->layout()->activate();
}
void GamesPage::showStatistics() {
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Library Statistics"));
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);
    auto* status = new QLabel(dialog);
    status->setObjectName(QStringLiteral("libraryStatisticsStatus"));
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* refresh = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    const auto load = [this, status, refresh] {
        refresh->setEnabled(false);
        status->setText(tr("Loading library statistics…"));
        library_.loadStatistics();
    };
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(refresh, &QPushButton::clicked, dialog, load);
    connect(&library_, &app::GameLibraryController::statisticsLoaded, dialog,
            [status, refresh](const persistence::LibraryStatisticsResult& result) {
                refresh->setEnabled(true);
                if (const auto* counts = std::get_if<persistence::LibraryStatistics>(&result)) {
                    status->setText(tr("Saved games: %L1\nGames with a completed review: %L2\n"
                                       "Completed reviews: %L3\nSaved puzzles: %L4")
                                        .arg(counts->games)
                                        .arg(counts->reviewedGames)
                                        .arg(counts->completedReviews)
                                        .arg(counts->puzzles));
                } else {
                    status->setText(std::get<persistence::GameError>(result).message);
                }
            });
    dialog->resize(360, dialog->sizeHint().height());
    dialog->open();
    load();
}

void GamesPage::confirmDeletion() {
    const auto* game = libraryModel_->summary(games_->currentIndex().row());
    if (!game || !remove_->isEnabled()) {
        return;
    }
    const auto id = game->id;
    auto* dialog = new QMessageBox(QMessageBox::Warning, tr("Delete saved game?"), {},
                                   QMessageBox::Cancel, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("confirmGameDeletion"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setText(game->metadataError
                        ? tr("Delete saved game %1?").arg(id)
                        : tr("Delete %1 vs %2?")
                              .arg(game->white.isEmpty() ? tr("White") : game->white,
                                   game->black.isEmpty() ? tr("Black") : game->black));
    dialog->setInformativeText(tr("This permanently removes the local game and its reviews. "
                                  "Saved puzzles and the original PGN file are kept."));
    auto* confirm = dialog->addButton(tr("Delete game"), QMessageBox::DestructiveRole);
    confirm->setObjectName(QStringLiteral("confirmDeleteGame"));
    dialog->setDefaultButton(QMessageBox::Cancel);
    dialog->setEscapeButton(QMessageBox::Cancel);
    connect(dialog, &QMessageBox::buttonClicked, this,
            [this, confirm, id](QAbstractButton* button) {
                if (button == confirm && !imports_.busy()) {
                    deletionStatus_->hide();
                    library_.remove(id);
                }
            });
    dialog->open();
}
void GamesPage::updateImport() {
    reportDialog_->sync();
    const bool hasResults = !imports_.files().empty();
    importStatus_->parentWidget()->setVisible(hasResults);
    importStatus_->setVisible(hasResults);
    report_->setVisible(hasResults);
    cancel_->setVisible(imports_.busy());
    cancel_->setEnabled(imports_.busy() && !imports_.cancelling());
    updateOpenAction();
    auto status = completionText(imports_.status(), imports_.failures() > 0);
    if (hasResults && imports_.files().front().remote) {
        const auto& source = imports_.files().front();
        if (imports_.busy() && !imports_.cancelling()) {
            importStatus_->setText(source.state == app::ImportFileState::Downloading
                                       ? tr("Downloading game…")
                                       : tr("Validating and saving game…"));
            return;
        }
        if (imports_.status() == app::ImportBatchStatus::Failed) {
            if (!source.message.isEmpty()) {
                status = source.message;
            } else if (!source.items.empty()) {
                const auto& outcome = source.items.front().outcome;
                if (const auto* failure = std::get_if<pgn::PgnFailure>(&outcome)) {
                    status = tr("Game PGN could not be imported. %1")
                                 .arg(QString::fromStdString(failure->error.message));
                } else if (const auto* error = std::get_if<persistence::GameError>(&outcome)) {
                    status = error->message;
                }
            }
            importStatus_->setText(status);
            return;
        }
    }
    if (imports_.busy()) {
        status = tr("Stopping import…");
        if (!imports_.cancelling() && imports_.currentFile() < imports_.files().size()) {
            const auto& source = imports_.files()[imports_.currentFile()];
            status = tr("%1 · Source %2 of %3")
                         .arg(source.state == app::ImportFileState::Reading ? tr("Reading PGN")
                                                                            : tr("Importing PGN"))
                         .arg(imports_.currentFile() + 1)
                         .arg(imports_.files().size());
            if (imports_.currentGame()) {
                status += tr(" · Game %1").arg(imports_.currentGame());
            }
        }
    }
    if (hasResults) {
        status += imports_.inserted() == 1 ? tr(" · 1 game added")
                                           : tr(" · %1 games added").arg(imports_.inserted());
        if (imports_.duplicates()) {
            status += tr(" · %1 already saved").arg(imports_.duplicates());
        }
        if (imports_.failures()) {
            status += tr(" · %1 failed").arg(imports_.failures());
        }
        if (imports_.warnings()) {
            status += imports_.warnings() == 1 ? tr(" · 1 warning")
                                               : tr(" · %1 warnings").arg(imports_.warnings());
        }
    }
    importStatus_->setText(status);
}
void GamesPage::setVerbose(bool verbose) {
    verbose_ = verbose;
    reportDialog_->setVerbose(verbose);
    updatePreview();
}

void GamesPage::setBoardDisplayOptions(bool coordinates, bool animation) {
    board_->setDisplayOptions(coordinates, animation);
}
void GamesPage::dragEnterEvent(QDragEnterEvent* event) {
    if (imports_.available() && event->possibleActions().testFlag(Qt::CopyAction) &&
        supportedDrop(*event->mimeData())) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
}
void GamesPage::dropEvent(QDropEvent* event) {
    if (imports_.available() && event->possibleActions().testFlag(Qt::CopyAction) &&
        supportedDrop(*event->mimeData()) && imports_.importFiles(event->mimeData()->urls())) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
}
} // namespace chess_review::ui
