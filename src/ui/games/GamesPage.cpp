#include "ui/games/GamesPage.h"
#include <QAbstractTableModel>
#include <QAction>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace chess_review::ui {
namespace {
QString fileState(app::ImportFileState state) {
    switch (state) {
    case app::ImportFileState::Pending:
        return QObject::tr("Waiting");
    case app::ImportFileState::Reading:
        return QObject::tr("Reading");
    case app::ImportFileState::Parsing:
        return QObject::tr("Importing");
    case app::ImportFileState::Completed:
        return QObject::tr("Completed");
    case app::ImportFileState::CompletedWithErrors:
        return QObject::tr("Completed with errors");
    case app::ImportFileState::Failed:
        return QObject::tr("Failed");
    case app::ImportFileState::Cancelled:
        return QObject::tr("Cancelled");
    case app::ImportFileState::Unprocessed:
        return QObject::tr("Not processed");
    }
    return {};
}
QString completionText(app::ImportBatchStatus status, bool hasErrors) {
    switch (status) {
    case app::ImportBatchStatus::Completed:
        return hasErrors ? QObject::tr("Import completed with errors.")
                         : QObject::tr("Import completed.");
    case app::ImportBatchStatus::Cancelled:
        return QObject::tr("Import cancelled.");
    case app::ImportBatchStatus::Failed:
        return QObject::tr("Import stopped after a failure.");
    case app::ImportBatchStatus::GameLimitReached:
        return QObject::tr("Import stopped: batch game limit reached.");
    case app::ImportBatchStatus::Idle:
    case app::ImportBatchStatus::Running:
        return {};
    }
    return {};
}
QString outcomeText(const app::ImportItem& item) {
    if (const auto* inserted = std::get_if<persistence::Inserted>(&item.outcome)) {
        return QObject::tr("Imported (ID %1)").arg(inserted->id);
    }
    if (const auto* duplicate = std::get_if<persistence::Duplicate>(&item.outcome)) {
        return QObject::tr("Duplicate (existing ID %1)").arg(duplicate->id);
    }
    if (const auto* failure = std::get_if<pgn::PgnFailure>(&item.outcome)) {
        return QString::fromStdString(failure->error.message);
    }
    return std::get<persistence::GameError>(item.outcome).message;
}
QString diagnosticText(const pgn::Diagnostic& diagnostic) {
    auto text = QObject::tr("%1 — line %2, byte column %3, byte offset %4")
                    .arg(QString::fromStdString(diagnostic.message))
                    .arg(diagnostic.location.line)
                    .arg(diagnostic.location.column)
                    .arg(diagnostic.location.offset);
    if (diagnostic.ply) {
        text += QObject::tr(", ply %1").arg(*diagnostic.ply);
    }
    return text;
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
} // namespace

class LibraryTableModel final : public QAbstractTableModel {
  public:
    LibraryTableModel(app::GameLibraryController& controller, QObject* parent)
        : QAbstractTableModel(parent), controller_(controller) {}
    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : 8;
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 ||
            section >= 8) {
            return {};
        }
        const QStringList labels{tr("White"),         tr("Black"),
                                 tr("Result"),        tr("Played date"),
                                 tr("Time control"),  tr("Imported (UTC)"),
                                 tr("Review status"), tr("Chosen-player accuracy")};
        return labels.at(section);
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() ||
            index.column() < 0 || index.column() >= 8) {
            return {};
        }
        const auto& row = rows_[static_cast<std::size_t>(index.row())];
        if (role == Qt::ToolTipRole) {
            if (index.column() == 6) {
                return controller_.reviewStatus(row.id).toHtmlEscaped();
            }
            if (index.column() == 7) {
                return controller_.reviewAccuracy(row.id).toHtmlEscaped();
            }
            if (row.metadataError) {
                return tr("Game ID %1: %2")
                    .arg(row.id)
                    .arg(row.metadataError->message)
                    .toHtmlEscaped();
            }
            return row.metadataTruncated
                       ? tr("Metadata is truncated; the complete game remains stored.")
                       : tr("Game ID %1").arg(row.id);
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
            return controller_.reviewAccuracy(row.id);
        }
        const QStringList values{row.white,    row.black,       row.result,
                                 row.playedAt, row.timeControl, row.importedAt};
        auto value = values.at(index.column());
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
    void reload() {
        beginResetModel();
        rows_ = controller_.rows();
        endResetModel();
    }

  private:
    app::GameLibraryController& controller_;
    std::vector<persistence::GameSummary> rows_;
};

class ImportTableModel final : public QAbstractTableModel {
  public:
    using RowKey = std::pair<std::size_t, std::optional<std::size_t>>;
    ImportTableModel(app::ImportController& controller, QObject* parent)
        : QAbstractTableModel(parent), controller_(controller) {}
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
        const QStringList labels{tr("Source"), tr("Game"), tr("Outcome"), tr("Warnings")};
        return labels.at(section);
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount() ||
            index.column() < 0 || index.column() >= 4 ||
            (role != Qt::DisplayRole && role != Qt::AccessibleTextRole)) {
            return {};
        }
        const auto [fileIndex, gameIndex] = rows_[static_cast<std::size_t>(index.row())];
        const auto& file = files_[fileIndex];
        const auto* item = gameIndex ? &file.items[*gameIndex] : nullptr;
        switch (index.column()) {
        case 0:
            return file.name;
        case 1:
            return item ? QString::number(item->ordinal) : QString{};
        case 2:
            return item                     ? outcomeText(*item)
                   : file.message.isEmpty() ? fileState(file.state)
                                            : tr("%1: %2").arg(fileState(file.state), file.message);
        case 3:
            return item ? QString::number(item->warnings.size() + item->suppressedWarnings)
                        : QString{};
        default:
            return {};
        }
    }
    QString details(int row) const {
        if (row < 0 || row >= rowCount()) {
            return {};
        }
        const auto [fileIndex, gameIndex] = rows_[static_cast<std::size_t>(row)];
        const auto& file = files_[fileIndex];
        if (!gameIndex) {
            return file.name + QLatin1Char('\n') + fileState(file.state) + QLatin1Char('\n') +
                   file.message;
        }
        const auto& item = file.items[*gameIndex];
        auto text = tr("%1 — game %2\n%3\nSource byte range [%4, %5)")
                        .arg(file.name)
                        .arg(item.ordinal)
                        .arg(outcomeText(item))
                        .arg(item.range.begin)
                        .arg(item.range.end);
        if (const auto* failure = std::get_if<pgn::PgnFailure>(&item.outcome)) {
            text += QLatin1Char('\n') + diagnosticText(failure->error);
        }
        for (const auto& warning : item.warnings) {
            text += QLatin1Char('\n') + diagnosticText(warning);
        }
        if (item.suppressedWarnings > 0) {
            text += tr("\n%1 additional warnings suppressed.").arg(item.suppressedWarnings);
        }
        return text;
    }
    std::optional<RowKey> rowKey(int row) const {
        if (row < 0 || row >= rowCount()) {
            return std::nullopt;
        }
        return rows_[static_cast<std::size_t>(row)];
    }
    QModelIndex indexForKey(const RowKey& key) const {
        const auto found = std::find(rows_.begin(), rows_.end(), key);
        return found == rows_.end() ? QModelIndex{}
                                    : index(static_cast<int>(found - rows_.begin()), 0);
    }
    void reload() {
        beginResetModel();
        rows_.clear();
        files_ = controller_.files();
        for (std::size_t f = 0; f < files_.size(); ++f) {
            rows_.emplace_back(f, std::nullopt);
            for (std::size_t g = 0; g < files_[f].items.size(); ++g) {
                rows_.emplace_back(f, g);
            }
        }
        endResetModel();
    }

  private:
    app::ImportController& controller_;
    std::vector<RowKey> rows_;
    // Own the bounded presentation snapshot until beginResetModel(), including across batches.
    std::vector<app::ImportFileResult> files_;
};

GamesPage::GamesPage(app::ImportController& imports, app::GameLibraryController& library,
                     QAction& fileAction, QAction& pasteAction, QWidget* parent)
    : QWidget(parent), imports_(imports), library_(library) {
    setObjectName(QStringLiteral("gamesPage"));
    setAcceptDrops(true);
    auto* layout = new QVBoxLayout(this);
    auto* heading = new QLabel(tr("Games"), this);
    layout->addWidget(heading);
    auto* toolbar = new QHBoxLayout;
    for (auto* action : {&fileAction, &pasteAction}) {
        auto* button = new QToolButton(this);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        toolbar->addWidget(button);
    }
    refresh_ = new QPushButton(tr("Refresh"), this);
    refresh_->setObjectName(QStringLiteral("refreshGames"));
    toolbar->addWidget(refresh_);
    open_ = new QPushButton(tr("Open in Analyze"), this);
    open_->setObjectName(QStringLiteral("openGameButton"));
    open_->setToolTip(tr("Select a saved game, then open its moves and variations."));
    toolbar->addWidget(open_);
    review_ = new QPushButton(tr("Review…"), this);
    review_->setObjectName(QStringLiteral("reviewGameButton"));
    toolbar->addWidget(review_);
    connect(review_, &QPushButton::clicked, this, [this] {
        if (const auto id = libraryModel_->gameId(games_->currentIndex().row());
            id && review_->isEnabled()) {
            emit reviewRequested(*id);
        }
    });
    connect(open_, &QPushButton::clicked, this, &GamesPage::requestOpen);
    toolbar->addStretch();
    layout->addLayout(toolbar);
    libraryStatus_ = new QLabel(this);
    libraryStatus_->setTextFormat(Qt::PlainText);
    libraryStatus_->setWordWrap(true);
    libraryStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                            Qt::TextSelectableByKeyboard);
    libraryStatus_->setObjectName(QStringLiteral("libraryStatus"));
    layout->addWidget(libraryStatus_);
    libraryModel_ = new LibraryTableModel(library, this);
    games_ = new QTableView(this);
    games_->setObjectName(QStringLiteral("gamesTable"));
    games_->setAccessibleName(tr("Saved games"));
    games_->setModel(libraryModel_);
    configureTable(*games_);
    connect(games_, &QTableView::activated, this, &GamesPage::requestOpen);
    connect(games_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            &GamesPage::updateOpenAction);
    games_->setColumnWidth(0, 130);
    games_->setColumnWidth(1, 130);
    games_->setColumnWidth(2, 60);
    games_->setColumnWidth(6, 300);
    games_->setColumnWidth(7, 270);
    // Keep review results visible before secondary metadata on laptop displays.
    games_->horizontalHeader()->moveSection(games_->horizontalHeader()->visualIndex(6), 3);
    games_->horizontalHeader()->moveSection(games_->horizontalHeader()->visualIndex(7), 4);
    games_->setColumnWidth(5, 200);
    layout->addWidget(games_, 3);
    auto* paging = new QHBoxLayout;
    previous_ = new QPushButton(tr("Previous page"), this);
    next_ = new QPushButton(tr("Next page"), this);
    previous_->setObjectName(QStringLiteral("previousGames"));
    next_->setObjectName(QStringLiteral("nextGames"));
    paging->addWidget(previous_);
    paging->addWidget(next_);
    paging->addStretch();
    layout->addLayout(paging);
    auto* progress = new QHBoxLayout;
    importStatus_ = new QLabel(this);
    importStatus_->setTextFormat(Qt::PlainText);
    importStatus_->setWordWrap(true);
    importStatus_->setObjectName(QStringLiteral("importStatus"));
    importStatus_->setAccessibleName(tr("Import progress"));
    importStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                           Qt::TextSelectableByKeyboard);
    progress->addWidget(importStatus_, 1);
    cancel_ = new QPushButton(tr("Cancel import"), this);
    cancel_->setObjectName(QStringLiteral("cancelImport"));
    progress->addWidget(cancel_);
    layout->addLayout(progress);
    results_ = new QTableView(this);
    results_->setObjectName(QStringLiteral("importResults"));
    results_->setAccessibleName(tr("Latest import results"));
    importModel_ = new ImportTableModel(imports, this);
    results_->setModel(importModel_);
    configureTable(*results_);
    results_->setColumnWidth(0, 180);
    results_->setColumnWidth(1, 60);
    results_->setColumnWidth(2, 350);
    layout->addWidget(results_, 2);
    details_ = new QPlainTextEdit(this);
    details_->setObjectName(QStringLiteral("importDetails"));
    details_->setAccessibleName(tr("Selected import result details"));
    details_->setReadOnly(true);
    details_->setMaximumHeight(140);
    layout->addWidget(details_, 1);
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
        results_->setCurrentIndex(QModelIndex{});
        results_->scrollToTop();
        results_->horizontalScrollBar()->setValue(0);
        detailText_.clear();
        details_->clear();
        updateImport();
    });
    connect(&imports_, &app::ImportController::finished, this, &GamesPage::updateImport);
    connect(results_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            &GamesPage::showResultDetails);
    connect(&library_, &app::GameLibraryController::changed, this, &GamesPage::updateLibrary);
    connect(refresh_, &QPushButton::clicked, this, [this] { library_.refresh(); });
    connect(previous_, &QPushButton::clicked, &library_, &app::GameLibraryController::previous);
    connect(next_, &QPushButton::clicked, &library_, &app::GameLibraryController::next);
    connect(cancel_, &QPushButton::clicked, &imports_, &app::ImportController::cancel);
    updateLibrary();
    updateImport();
}
void GamesPage::requestOpen() {
    const auto id = libraryModel_->gameId(games_->currentIndex().row());
    if (library_.ready() && !library_.opening() && !library_.busy() && id) {
        emit openRequested(*id);
    }
}
void GamesPage::updateOpenAction() {
    review_->setEnabled(library_.ready() && !library_.busy() &&
                        libraryModel_->gameId(games_->currentIndex().row()).has_value());
    open_->setEnabled(library_.ready() && !library_.opening() && !library_.busy() &&
                      libraryModel_->gameId(games_->currentIndex().row()).has_value());
}
void GamesPage::updateLibrary() {
    const auto selected = libraryModel_->gameId(games_->currentIndex().row());
    libraryModel_->reload();
    if (selected) {
        for (int row = 0; row < libraryModel_->rowCount(); ++row) {
            if (libraryModel_->gameId(row) == selected) {
                games_->setCurrentIndex(libraryModel_->index(row, 0));
                break;
            }
        }
    }
    updateOpenAction();
    libraryStatus_->setText(library_.opening()            ? tr("Opening saved game…")
                            : !library_.ready()           ? tr("Local storage is unavailable.")
                            : library_.busy()             ? tr("Loading games…")
                            : !library_.error().isEmpty() ? library_.error()
                            : library_.rows().empty()
                                ? tr("No games yet. Import PGN files or paste PGN text.")
                                : tr("Page %1 — newest imports first").arg(library_.page() + 1));
    refresh_->setEnabled(library_.ready() && !library_.busy());
    previous_->setEnabled(library_.ready() && !library_.busy() && library_.page() > 0);
    next_->setEnabled(library_.ready() && !library_.busy() && library_.hasNext());
}
void GamesPage::updateImport() {
    const auto selected = importModel_->rowKey(results_->currentIndex().row());
    const auto top = importModel_->rowKey(results_->rowAt(0));
    const auto horizontal = results_->horizontalScrollBar()->value();
    {
        const QSignalBlocker selectionSignals(results_->selectionModel());
        importModel_->reload();
        if (selected) {
            results_->setCurrentIndex(importModel_->indexForKey(*selected));
        } else if (importModel_->rowCount() > 0) {
            results_->setCurrentIndex(importModel_->index(0, 0));
        }
        if (top) {
            results_->scrollTo(importModel_->indexForKey(*top), QAbstractItemView::PositionAtTop);
        }
        results_->horizontalScrollBar()->setValue(horizontal);
    }
    showResultDetails();
    const bool hasResults = !imports_.files().empty();
    results_->setVisible(hasResults);
    details_->setVisible(hasResults);
    cancel_->setVisible(imports_.busy());
    cancel_->setEnabled(imports_.busy() && !imports_.cancelling());
    auto status = completionText(imports_.status(), imports_.failures() > 0);
    if (imports_.busy()) {
        status = imports_.cancelling() ? tr("Cancelling; any submitted save will finish…")
                                       : tr("Importing file %1 of %2, game %3…")
                                             .arg(imports_.currentFile() + 1)
                                             .arg(imports_.files().size())
                                             .arg(imports_.currentGame());
    }
    if (imports_.busy() && imports_.currentFile() < imports_.files().size()) {
        status += QLatin1Char(' ') + imports_.files()[imports_.currentFile()].name.left(256);
    }
    if (hasResults) {
        status += tr(" %1 imported, %2 duplicates, %3 failures, %4 warnings.")
                      .arg(imports_.inserted())
                      .arg(imports_.duplicates())
                      .arg(imports_.failures())
                      .arg(imports_.warnings());
    } else {
        status = tr("Drop local .pgn files here to import. UTF-8 only; up to 32 files and 64 MiB "
                    "per batch.");
    }
    importStatus_->setText(status);
}
void GamesPage::showResultDetails() {
    const auto text = importModel_->details(results_->currentIndex().row());
    if (text != detailText_) {
        detailText_ = text;
        details_->setPlainText(text);
    }
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
