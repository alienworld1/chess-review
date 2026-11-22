#include "ui/games/ImportReportDialog.h"
#include <QAbstractTableModel>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTableView>
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
    case app::ImportFileState::Downloading:
        return QObject::tr("Downloading");
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
QString outcomeText(const app::ImportItem& item) {
    if (const auto* inserted = std::get_if<persistence::Inserted>(&item.outcome)) {
        Q_UNUSED(inserted);
        return QObject::tr("Imported");
    }
    if (const auto* duplicate = std::get_if<persistence::Duplicate>(&item.outcome)) {
        Q_UNUSED(duplicate);
        return QObject::tr("Already in your library");
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
} // namespace

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
            return item ? (item->title.isEmpty() ? tr("Game %1").arg(item->ordinal)
                                                 : tr("%1. %2").arg(item->ordinal).arg(item->title))
                        : QString{};
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
    QString details(int row, bool verbose) const {
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
        auto text = tr("%1 — game %2").arg(file.name).arg(item.ordinal);
        if (!item.title.isEmpty()) {
            text += u'\n' + item.title;
        }
        if (verbose) {
            text += tr("\nSource byte range [%1, %2)").arg(item.range.begin).arg(item.range.end);
        }
        const auto diagnostic = [verbose](const pgn::Diagnostic& value) {
            return verbose ? diagnosticText(value)
                           : tr("Line %1: %2")
                                 .arg(value.location.line)
                                 .arg(QString::fromStdString(value.message));
        };
        if (const auto* failure = std::get_if<pgn::PgnFailure>(&item.outcome)) {
            text += QLatin1Char('\n') + diagnostic(failure->error);
        } else {
            text += QLatin1Char('\n') + outcomeText(item);
        }
        for (const auto& warning : item.warnings) {
            text += QLatin1Char('\n') + diagnostic(warning);
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
    std::optional<persistence::GameId> gameId(int row) const {
        const auto key = rowKey(row);
        if (!key || !key->second) {
            return std::nullopt;
        }
        const auto& outcome = files_[key->first].items[*key->second].outcome;
        if (const auto* added = std::get_if<persistence::Inserted>(&outcome)) {
            return added->id;
        }
        if (const auto* duplicate = std::get_if<persistence::Duplicate>(&outcome)) {
            return duplicate->id;
        }
        return std::nullopt;
    }
    void resetBatch() {
        beginResetModel();
        rows_.clear();
        files_.clear();
        const auto& files = controller_.files();
        for (std::size_t f = 0; f < files.size(); ++f) {
            files_.push_back({files[f].name, files[f].state, files[f].message, {}});
            rows_.emplace_back(f, std::nullopt);
        }
        endResetModel();
        sync();
    }
    void sync() {
        const auto& files = controller_.files();
        if (files.size() != files_.size()) {
            resetBatch();
            return;
        }
        int row = 0;
        for (std::size_t f = 0; f < files_.size(); ++f) {
            auto& old = files_[f];
            const auto& current = files[f];
            if (old.state != current.state || old.message != current.message) {
                old.state = current.state;
                old.message = current.message;
                emit dataChanged(index(row, 2), index(row, 2));
            }
            row += 1 + static_cast<int>(old.items.size());
            if (old.items.size() == current.items.size()) {
                continue;
            }
            const auto count = static_cast<int>(current.items.size() - old.items.size());
            beginInsertRows({}, row, row + count - 1);
            for (auto g = old.items.size(); g < current.items.size(); ++g) {
                rows_.insert(rows_.begin() + row, {f, g});
                ++row;
                old.items.push_back(current.items[g]);
            }
            endInsertRows();
        }
    }

  private:
    app::ImportController& controller_;
    std::vector<RowKey> rows_;
    // Own the bounded presentation snapshot until beginResetModel(), including across batches.
    std::vector<app::ImportFileResult> files_;
};

ImportReportDialog::ImportReportDialog(app::ImportController& imports, QWidget* parent)
    : QDialog(parent), imports_(imports) {
    setObjectName(QStringLiteral("importReportDialog"));
    setWindowTitle(tr("Import report"));
    resize(800, 480);
    auto* layout = new QVBoxLayout(this);
    results_ = new QTableView(this);
    results_->setObjectName(QStringLiteral("importResults"));
    results_->setAccessibleName(tr("Latest import results"));
    model_ = new ImportTableModel(imports, this);
    results_->setModel(model_);
    results_->setSelectionBehavior(QAbstractItemView::SelectRows);
    results_->setSelectionMode(QAbstractItemView::SingleSelection);
    results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results_->setWordWrap(false);
    results_->setAlternatingRowColors(true);
    results_->verticalHeader()->hide();
    results_->horizontalHeader()->setStretchLastSection(true);
    results_->setColumnWidth(0, 180);
    results_->setColumnWidth(1, 250);
    results_->setColumnWidth(2, 240);
    layout->addWidget(results_, 2);
    details_ = new QPlainTextEdit(this);
    details_->setObjectName(QStringLiteral("importDetails"));
    details_->setAccessibleName(tr("Selected import result details"));
    details_->setReadOnly(true);
    details_->setMaximumHeight(140);
    layout->addWidget(details_, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    open_ = buttons->addButton(tr("Open in Analyze"), QDialogButtonBox::ActionRole);
    review_ = buttons->addButton(tr("Review…"), QDialogButtonBox::ActionRole);
    open_->setObjectName(QStringLiteral("openImportedGame"));
    review_->setObjectName(QStringLiteral("reviewGameFromImportReport"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(open_, &QPushButton::clicked, this, [this] { activateGame(false); });
    connect(review_, &QPushButton::clicked, this, [this] { activateGame(true); });
    connect(results_, &QTableView::activated, this, [this] { activateGame(false); });
    connect(results_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            &ImportReportDialog::showResultDetails);
    results_->setFocus();
    showResultDetails();
}

void ImportReportDialog::resetBatch() {
    removed_.clear();
    model_->resetBatch();
    results_->scrollToTop();
    results_->horizontalScrollBar()->setValue(0);
}
void ImportReportDialog::sync() {
    model_->sync();
    if (!results_->currentIndex().isValid() && model_->rowCount() > 0) {
        results_->setCurrentIndex(model_->index(0, 0));
    }
    showResultDetails();
}
void ImportReportDialog::setVerbose(bool verbose) {
    verbose_ = verbose;
    showResultDetails();
}
void ImportReportDialog::setActionsAvailable(bool available) {
    if (actionsAvailable_ != available) {
        actionsAvailable_ = available;
        showResultDetails();
    }
}
void ImportReportDialog::gameRemoved(persistence::GameId id) {
    for (const auto& file : imports_.files()) {
        if (std::ranges::any_of(file.items, [id](const app::ImportItem& item) {
                const auto* added = std::get_if<persistence::Inserted>(&item.outcome);
                const auto* duplicate = std::get_if<persistence::Duplicate>(&item.outcome);
                return (added && added->id == id) || (duplicate && duplicate->id == id);
            })) {
            removed_.insert(id);
            showResultDetails();
            return;
        }
    }
}
void ImportReportDialog::activateGame(bool review) {
    const auto id = model_->gameId(results_->currentIndex().row());
    if (!id || removed_.contains(*id) || !actionsAvailable_) {
        return;
    }
    hide();
    if (review) {
        emit reviewRequested(*id);
    } else {
        emit openRequested(*id);
    }
}
void ImportReportDialog::showResultDetails() {
    const auto row = results_->currentIndex().row();
    const auto id = model_->gameId(row);
    const bool removed = id && removed_.contains(*id);
    open_->setEnabled(id && !removed && actionsAvailable_);
    review_->setEnabled(open_->isEnabled());
    auto text = model_->details(row, verbose_);
    if (removed) {
        text += tr("\nThis game has since been deleted from your library.");
    }
    if (text != detailText_) {
        detailText_ = text;
        details_->setPlainText(text);
    }
}
} // namespace chess_review::ui
