#include "ui/home/HomePage.h"
#include "app/EngineSettingsController.h"
#include "app/GameLibraryController.h"
#include "app/ImportController.h"
#include "app/PuzzleController.h"
#include "app/ReviewGameController.h"
#include "app/ReviewWorkspaceController.h"
#include "ui/EngineText.h"
#include "ui/Theme.h"
#include <QAction>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace chess_review::ui {
namespace {
class SectionIcon final : public QWidget {
  public:
    SectionIcon(QString name, QWidget* parent) : QWidget(parent), name_(std::move(name)) {
        setFixedSize(20, 20);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const auto color = palette().color(QPalette::PlaceholderText);
        themedIcon(name_, color, color).paint(&painter, rect());
    }

  private:
    QString name_;
};

class RecentGameDelegate final : public QStyledItemDelegate {
  public:
    explicit RecentGameDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const auto text = index.data(Qt::DisplayRole).toString();
        auto split = text.indexOf(u'\n');
        if (split < 0) {
            split = text.indexOf(QChar::LineSeparator);
        }
        const QString title = split >= 0 ? text.left(split) : text;
        const QString subtitle = split >= 0 ? text.mid(split + 1) : QString{};
        opt.text.clear();
        opt.features &= ~QStyleOptionViewItem::HasDisplay;
        if (opt.widget) {
            opt.widget->style()->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter,
                                               opt.widget);
        }
        const QRect bounds = opt.rect.adjusted(12, 4, -12, -4);
        painter->save();
        QFont titleFont = opt.font;
        titleFont.setWeight(QFont::DemiBold);
        QFont subFont = opt.font;
        if (subFont.pointSizeF() > 0) {
            subFont.setPointSizeF(subFont.pointSizeF() * 0.92);
        }
        const QFontMetrics subMetrics(subFont);
        const int subWidth =
            subtitle.isEmpty()
                ? 0
                : std::min(bounds.width() / 2, subMetrics.horizontalAdvance(subtitle) + 14);
        const QRect titleRect(bounds.left(), bounds.top(), bounds.width() - subWidth,
                              bounds.height());
        const QFontMetrics titleMetrics(titleFont);
        painter->setFont(titleFont);
        painter->setPen(opt.state.testFlag(QStyle::State_Selected)
                            ? opt.palette.color(QPalette::HighlightedText)
                            : opt.palette.color(QPalette::Text));
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          titleMetrics.elidedText(title, Qt::ElideRight, titleRect.width()));
        if (!subtitle.isEmpty()) {
            const QRect subRect(titleRect.right(), bounds.top(), subWidth, bounds.height());
            painter->setFont(subFont);
            painter->setPen(opt.palette.color(QPalette::PlaceholderText));
            painter->drawText(subRect, Qt::AlignRight | Qt::AlignVCenter,
                              subMetrics.elidedText(subtitle, Qt::ElideRight, subRect.width()));
        }
        if (opt.state.testFlag(QStyle::State_HasFocus) && opt.widget) {
            QStyleOptionFocusRect focusOpt;
            focusOpt.QStyleOption::operator=(opt);
            focusOpt.rect = opt.rect;
            opt.widget->style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focusOpt, painter,
                                               opt.widget);
        }
        painter->restore();
    }
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        return {QStyledItemDelegate::sizeHint(option, index).width(),
                std::max(36, option.fontMetrics.height() + 16)};
    }
};

QLabel* textLabel(const QString& text, QWidget* parent, const char* role = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    if (role) {
        setTextRole(*label, role);
    }
    return label;
}

QString gameTitle(const persistence::GameSummary& game) {
    const auto white = game.white.simplified();
    const auto black = game.black.simplified();
    return HomePage::tr("%1 vs %2")
        .arg(white.isEmpty() ? HomePage::tr("White") : white,
             black.isEmpty() ? HomePage::tr("Black") : black);
}

class ImportDropButton final : public QToolButton {
  public:
    ImportDropButton(app::ImportController& imports, QWidget* parent)
        : QToolButton(parent), imports_(imports) {
        setAcceptDrops(true);
    }

  protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        if (imports_.available() && event->possibleActions().testFlag(Qt::CopyAction) &&
            event->mimeData()->hasUrls() &&
            app::ImportController::acceptsFiles(event->mimeData()->urls())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
        }
    }
    void dropEvent(QDropEvent* event) override {
        if (imports_.available() && event->possibleActions().testFlag(Qt::CopyAction) &&
            event->mimeData()->hasUrls() && imports_.importFiles(event->mimeData()->urls())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
        }
    }

  private:
    app::ImportController& imports_;
};

class RecentList final : public QListWidget {
  public:
    explicit RecentList(QWidget* parent) : QListWidget(parent) {
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setUniformItemSizes(true);
        setAlternatingRowColors(true);
        setItemDelegate(new RecentGameDelegate(this));
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        connect(model(), &QAbstractItemModel::modelReset, this, [this] { updateGeometry(); });
        connect(model(), &QAbstractItemModel::rowsInserted, this, [this] { updateGeometry(); });
    }

    QSize sizeHint() const override {
        const int height = count() > 0 ? count() * sizeHintForRow(0) : 0;
        return {0, height + 2 * frameWidth()};
    }
};

QHBoxLayout* informationRow(QWidget* section, const QString& iconName, const QString& heading,
                            QLabel*& detail) {
    auto* row = new QHBoxLayout(section);
    row->setContentsMargins(0, 6, 0, 6);
    row->setSpacing(12);
    if (!iconName.isEmpty()) {
        row->addWidget(new SectionIcon(iconName, section), 0, Qt::AlignTop);
    }
    auto* description = new QVBoxLayout;
    description->setSpacing(2);
    description->addWidget(textLabel(heading, section, "strong"));
    detail = textLabel({}, section, "secondary");
    description->addWidget(detail);
    row->addLayout(description, 1);
    return row;
}
} // namespace

HomePage::HomePage(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("homePage"));
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(16, 10, 16, 10);
    auto* content = new QWidget(this);
    content->setMaximumWidth(800);
    outer->addWidget(content, 1);
    auto* column = new QVBoxLayout(content);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(10);

    auto* intro = new QFrame(content);
    intro->setObjectName(QStringLiteral("homeIntro"));
    intro->setFrameShape(QFrame::NoFrame);
    setPanelSurface(*intro);
    auto* hero = new QHBoxLayout(intro);
    hero->setContentsMargins(20, 14, 20, 16);
    hero->setSpacing(16);
    auto* introText = new QVBoxLayout;
    introText->setSpacing(8);
    introText->addWidget(textLabel(tr("Review your games"), intro, "title"));
    entryHint_ = textLabel(tr("Import a game to review your moves."), intro, "secondary");
    introText->addWidget(entryHint_);
    entry_ = new QVBoxLayout;
    introText->addLayout(entry_);
    hero->addLayout(introText, 1);
    emptyArt_ = new QLabel(intro);
    emptyArt_->setPixmap(QIcon(QStringLiteral(":/pieces/Chess_nlt45.svg"))
                             .pixmap(QSize(52, 52), devicePixelRatioF()));
    emptyArt_->hide();
    hero->addWidget(emptyArt_, 0, Qt::AlignVCenter);
    column->addWidget(intro);

    auto* resumeFrame = new QFrame(content);
    resumeFrame->setObjectName(QStringLiteral("homeContinue"));
    resumeFrame->setFrameShape(QFrame::NoFrame);
    setPanelSurface(*resumeFrame);
    resume_ = resumeFrame;
    auto* resumeLayout =
        informationRow(resumeFrame, QStringLiteral("play"), tr("Continue"), resumeDetail_);
    resumeLayout->setContentsMargins(14, 10, 14, 10);
    resumeAction_ = new QPushButton(resume_);
    resumeAction_->setObjectName(QStringLiteral("homeResumeReview"));
    setIcon(*resumeAction_, QStringLiteral("play"));
    resumeLayout->addWidget(resumeAction_, 0, Qt::AlignVCenter);
    column->addWidget(resume_);
    resume_->hide();
    recovery_ = new QVBoxLayout;
    column->addLayout(recovery_);

    recentSection_ = new QWidget(content);
    auto* recentLayout = new QVBoxLayout(recentSection_);
    recentLayout->setContentsMargins(0, 4, 0, 4);
    recentLayout->setSpacing(6);
    auto* recentHeader = new QHBoxLayout;
    recentHeader->setSpacing(8);
    recentHeader->addWidget(textLabel(tr("Recent games"), recentSection_, "heading"), 1);
    auto* games = new QPushButton(tr("Open Games"), recentSection_);
    setQuietAction(*games);
    setIcon(*games, QStringLiteral("chevron-right"));
    connect(games, &QPushButton::clicked, this, &HomePage::gamesRequested);
    openRecent_ = new QPushButton(tr("Open selected game"), recentSection_);
    openRecent_->setObjectName(QStringLiteral("homeOpenRecent"));
    setIcon(*openRecent_, QStringLiteral("play"));
    retryRecent_ = new QPushButton(tr("Retry loading games"), recentSection_);
    setIcon(*retryRecent_, QStringLiteral("refresh"));
    recentHeader->addWidget(retryRecent_);
    recentHeader->addWidget(openRecent_);
    recentHeader->addWidget(games);
    recentLayout->addLayout(recentHeader);
    recentStatus_ = textLabel({}, recentSection_, "secondary");
    recentStatus_->setObjectName(QStringLiteral("homeRecentStatus"));
    recentLayout->addWidget(recentStatus_);
    recent_ = new RecentList(recentSection_);
    recent_->setObjectName(QStringLiteral("homeRecentGames"));
    recent_->setAccessibleName(tr("Recent games"));
    recentLayout->addWidget(recent_);
    setTabOrder(recent_, openRecent_);
    column->addWidget(recentSection_);
    recentSection_->hide();

    auto* supporting = new QWidget(content);
    supporting->setObjectName(QStringLiteral("homeLibraryStatus"));
    auto* supportingLayout = new QVBoxLayout(supporting);
    supportingLayout->setContentsMargins(0, 4, 0, 0);
    supportingLayout->setSpacing(2);
    puzzleSection_ = new QWidget(supporting);
    auto* puzzleRow =
        informationRow(puzzleSection_, QStringLiteral("puzzle"), tr("Puzzles"), puzzleDetail_);
    puzzleDetail_->setObjectName(QStringLiteral("homePuzzleSummary"));
    auto* puzzles = new QPushButton(tr("Open Puzzles"), puzzleSection_);
    puzzles->setObjectName(QStringLiteral("homeOpenPuzzles"));
    setQuietAction(*puzzles);
    setIcon(*puzzles, QStringLiteral("chevron-right"));
    puzzleRow->addWidget(puzzles, 0, Qt::AlignVCenter);
    connect(puzzles, &QPushButton::clicked, this, &HomePage::puzzlesRequested);
    supportingLayout->addWidget(puzzleSection_);
    puzzleSection_->hide();
    engineSection_ = new QWidget(supporting);
    auto* engineRow = informationRow(engineSection_, QStringLiteral("engine"), tr("Review engine"),
                                     engineDetail_);
    engineDetail_->setObjectName(QStringLiteral("homeEngineStatus"));
    auto* settings = new QPushButton(tr("Engine Settings"), engineSection_);
    settings->setObjectName(QStringLiteral("homeEngineSettings"));
    setQuietAction(*settings);
    setIcon(*settings, QStringLiteral("chevron-right"));
    engineRow->addWidget(settings, 0, Qt::AlignVCenter);
    connect(settings, &QPushButton::clicked, this, &HomePage::settingsRequested);
    supportingLayout->addWidget(engineSection_);
    engineSection_->hide();
    column->addWidget(supporting);
    column->addStretch();
}

void HomePage::installImports(app::ImportController& imports, QAction& file, QAction& paste,
                              QAction& link) {
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    auto* fileButton = new ImportDropButton(imports, this);
    fileButton->setObjectName(QStringLiteral("homeImportButton"));
    fileButton->setDefaultAction(&file);
    fileButton->setText(tr("Review a game"));
    fileButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setPrimaryAction(*fileButton);
    setIcon(*fileButton, QStringLiteral("import"));
    connect(&file, &QAction::changed, fileButton,
            [fileButton] { fileButton->setText(tr("Review a game")); });
    buttons->addWidget(fileButton);
    auto* pasteButton = new QToolButton(this);
    pasteButton->setDefaultAction(&paste);
    pasteButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setIcon(*pasteButton, QStringLiteral("paste"));
    buttons->addWidget(pasteButton);
    auto* linkButton = new QToolButton(this);
    linkButton->setDefaultAction(&link);
    linkButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setIcon(*linkButton, QStringLiteral("link"));
    buttons->addWidget(linkButton);
    buttons->addStretch();
    entry_->addLayout(buttons);
    setTabOrder(fileButton, pasteButton);
    setTabOrder(pasteButton, linkButton);
    setTabOrder(linkButton, resumeAction_);
}

void HomePage::installLibrary(app::GameLibraryController& library) {
    library_ = &library;
    const auto open = [this] {
        if (const auto* item = recent_->currentItem(); item && openRecent_->isEnabled()) {
            emit recentGameRequested(item->data(Qt::UserRole).toLongLong());
        }
    };
    connect(openRecent_, &QPushButton::clicked, this, open);
    connect(recent_, &QListWidget::itemActivated, this, [open](QListWidgetItem*) { open(); });
    connect(recent_, &QListWidget::currentRowChanged, this, [this](int row) {
        openRecent_->setEnabled(row >= 0 &&
                                recent_->currentItem()->data(Qt::UserRole).toLongLong() > 0);
    });
    connect(retryRecent_, &QPushButton::clicked, &library, [&library] { library.refresh(); });
    connect(&library, &app::GameLibraryController::changed, this,
            [this, &library, version = std::optional<std::uint64_t>{}]() mutable {
                if (library.showingGames()) {
                    return;
                }
                if (library.ready() && !library.busy() && version != library.rowsVersion()) {
                    version = library.rowsVersion();
                    updateRecent(library);
                }
                const bool available =
                    library.ready() && !library.busy() && library.listError().isEmpty();
                const bool populated = recent_->count() > 0;
                const bool empty = available && !populated;
                recent_->setVisible(library.ready() && populated && library.listError().isEmpty());
                openRecent_->setVisible(!recent_->isHidden());
                openRecent_->setEnabled(available && recent_->currentItem() &&
                                        recent_->currentItem()->data(Qt::UserRole).toLongLong() >
                                            0);
                retryRecent_->setVisible(library.ready() && !library.listError().isEmpty());
                retryRecent_->setEnabled(!library.busy());
                recentStatus_->setVisible(!available || empty);
                recentStatus_->setText(!library.ready() ? tr("Waiting for the game library…")
                                       : library.busy() ? tr("Loading recent games…")
                                       : empty          ? tr("Imported games will appear here.")
                                                        : library.listError());
                emptyArt_->setVisible(empty);
                entryHint_->setText(
                    empty ? tr("Import a game to review key moments and find stronger moves.")
                          : tr("Open a PGN file or paste a game to review it."));
            });
    recentSection_->show();
    recent_->hide();
    openRecent_->hide();
    retryRecent_->hide();
    recentStatus_->setText(tr("Waiting for the game library…"));
}

void HomePage::updateRecent(app::GameLibraryController& library) {
    const auto* selected = recent_->currentItem();
    const auto previous = selected ? selected->data(Qt::UserRole) : QVariant{};
    const QSignalBlocker blocker(recent_);
    recent_->clear();
    for (const auto& game : library.rows()) {
        auto* item = new QListWidgetItem(recent_);
        if (game.metadataError) {
            item->setText(tr("Game #%1\nSome saved details could not be read").arg(game.id));
        } else {
            const auto result = game.result == QLatin1String("*") || game.result.isEmpty()
                                    ? tr("Result not recorded")
                                    : game.result;
            const auto played = game.playedAt.simplified();
            const auto details = played.isEmpty() ? result : tr("%1 · %2").arg(result, played);
            item->setText(tr("%1\n%2").arg(gameTitle(game), details));
        }
        item->setToolTip(
            QStringLiteral("<qt>%1</qt>")
                .arg(item->text().toHtmlEscaped().replace(u'\n', QStringLiteral("<br>"))));
        item->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(game.id));
        if (game.id <= 0) {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        } else if (item->data(Qt::UserRole) == previous || !recent_->currentItem()) {
            recent_->setCurrentItem(item);
        }
    }
    recent_->updateGeometry();
}

void HomePage::installPuzzles(app::PuzzleController& puzzles) {
    const auto update = [this, &puzzles] {
        const auto& summary = puzzles.savedSummary();
        if (!summary.error.isEmpty()) {
            puzzleDetail_->setText(tr("Could not load the saved puzzle count."));
        } else if (!summary.count) {
            puzzleDetail_->setText(tr("Loading saved puzzles…"));
        } else if (*summary.count == 0) {
            puzzleDetail_->setText(
                tr("No saved puzzles yet. Generate puzzles from a completed review."));
        } else if (summary.more) {
            puzzleDetail_->setText(tr("More than %1 saved puzzles").arg(*summary.count));
        } else {
            puzzleDetail_->setText(*summary.count == 1
                                       ? tr("1 saved puzzle")
                                       : tr("%L1 saved puzzles").arg(*summary.count));
        }
    };
    connect(&puzzles, &app::PuzzleController::savedSummaryChanged, this, update);
    puzzleSection_->show();
    update();
}

void HomePage::installEngines(app::EngineSettingsController& engines) {
    const auto update = [this, &engines] {
        const auto& state = engines.state();
        const auto phase = state.runtime.phase;
        const auto preferred = clippedEngineText(presentedEngineText(state.defaultDisplayName));
        const auto connected = clippedEngineText(presentedEngineText(state.connectedDisplayName));
        const bool usingDefault = state.defaultProfileId == state.runtime.profileId;
        QString status;
        if (phase == engine::RuntimePhase::Failed) {
            status = usingDefault
                         ? tr("Connection failed. Open Engine Settings for details.")
                         : tr("%1 failed. Open Engine Settings for details.").arg(connected);
        } else if (phase == engine::RuntimePhase::Ready) {
            status = usingDefault ? tr("Connected and ready")
                                  : tr("%1 is connected and ready").arg(connected);
        } else if (phase == engine::RuntimePhase::Disconnecting) {
            status = usingDefault ? tr("Disconnecting…") : tr("Disconnecting %1…").arg(connected);
        } else if (engine::runtimeActive(phase)) {
            status = usingDefault ? tr("Connecting…") : tr("Connecting %1…").arg(connected);
        } else if (!state.ready || (state.listing && !state.defaultProfileId)) {
            status = tr("Checking for a review engine…");
        } else if (!state.listError.isEmpty()) {
            status = tr("Could not load registered engines. Open Engine Settings for details.");
        } else if (state.defaultProfileId &&
                   state.missingProfiles.contains(*state.defaultProfileId)) {
            status = tr("Executable missing · Open Engine Settings to locate or replace");
        } else if (state.defaultProfileId) {
            status = tr("Disconnected · Starts automatically");
        } else {
            status = tr("Add an engine in Settings to analyze games.");
        }
        engineDetail_->setText(
            state.defaultProfileId ? tr("Default: %1 · %2").arg(preferred, status) : status);
    };
    connect(&engines, &app::EngineSettingsController::changed, this, update);
    engineSection_->show();
    update();
}

void HomePage::installReview(app::ReviewGameController& review) {
    const auto showTask = [&review] {
        const auto& task = review.taskState();
        return task.active || (task.gameId > 0 && !task.error.isEmpty());
    };
    const auto update = [this, &review, showTask] {
        const auto& task = review.taskState();
        const auto& selected = review.state();
        if (showTask()) {
            const auto status =
                task.active ? tr("Review in progress") : tr("Review needs attention");
            auto detail =
                task.game.isEmpty() ? status : tr("%1 · %2").arg(status, task.game.simplified());
            if (task.active && task.run && task.run->totalPositions > 0) {
                detail = tr("%1 · %2 of %3 positions saved")
                             .arg(detail)
                             .arg(task.run->completedPositions)
                             .arg(task.run->totalPositions);
            }
            resumeDetail_->setText(detail);
            resumeAction_->setText(tr("Return to review"));
            resume_->show();
        } else if (selected.run || !selected.game.isEmpty()) {
            resumeDetail_->setText(selected.game.simplified());
            const bool complete =
                selected.run && selected.run->status == review::JobStatus::Complete;
            resumeAction_->setText(complete       ? tr("Open saved review")
                                   : selected.run ? tr("Open unfinished review")
                                                  : tr("Return to game setup"));
            resume_->show();
        } else {
            resume_->hide();
        }
    };
    connect(resumeAction_, &QPushButton::clicked, this, [this, &review, showTask] {
        if (showTask()) {
            review.workspace().returnToTask();
        } else {
            emit reviewRequested();
        }
    });
    connect(&review, &app::ReviewGameController::changed, this, update);
    connect(&review, &app::ReviewGameController::taskChanged, this, update);
    update();
    installRecovery(review);
}

void HomePage::installRecovery(app::ReviewGameController& review) {
    auto* recovery = new QWidget(this);
    recovery->setObjectName(QStringLiteral("reviewRecovery"));
    auto* recoveryLayout = new QVBoxLayout(recovery);
    recoveryLayout->setContentsMargins(0, 0, 0, 0);
    auto* showRecovery = new QPushButton(tr("Unfinished reviews"), recovery);
    showRecovery->setObjectName(QStringLiteral("showInterruptedReviews"));
    showRecovery->setCheckable(true);
    recoveryLayout->addWidget(showRecovery, 0, Qt::AlignLeft);
    auto* recoveryDetails = new QWidget(recovery);
    recoveryDetails->setObjectName(QStringLiteral("interruptedReviewList"));
    auto* detailsLayout = new QVBoxLayout(recoveryDetails);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    auto* recoveryRows = new QListWidget(recoveryDetails);
    recoveryRows->setObjectName(QStringLiteral("interruptedReviewRows"));
    recoveryRows->setAccessibleName(tr("Interrupted reviews"));
    recoveryRows->setSelectionMode(QAbstractItemView::SingleSelection);
    recoveryRows->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    recoveryRows->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    recoveryRows->setMinimumHeight(100);
    recoveryRows->setMaximumHeight(240);
    detailsLayout->addWidget(recoveryRows);
    auto* openRecovery = new QPushButton(tr("Open selected review"), recoveryDetails);
    openRecovery->setObjectName(QStringLiteral("openInterruptedReview"));
    openRecovery->setEnabled(false);
    detailsLayout->addWidget(openRecovery, 0, Qt::AlignLeft);
    const auto openSelected = [this, recoveryRows, &review] {
        const auto* item = recoveryRows->currentItem();
        if (review.workspace().recoveryBusy() || !item || !item->data(Qt::UserRole + 2).toBool()) {
            return;
        }
        review.workspace().selectGame(item->data(Qt::UserRole + 1).toLongLong(),
                                      item->data(Qt::UserRole).toLongLong());
        emit reviewRequested();
    };
    connect(openRecovery, &QPushButton::clicked, this, openSelected);
    connect(recoveryRows, &QListWidget::itemActivated, this,
            [openSelected](QListWidgetItem*) { openSelected(); });
    connect(recoveryRows, &QListWidget::currentRowChanged, this,
            [recoveryRows, openRecovery, &review] {
                const auto* item = recoveryRows->currentItem();
                openRecovery->setEnabled(!review.workspace().recoveryBusy() && item &&
                                         item->data(Qt::UserRole + 2).toBool());
            });
    recoveryLayout->addWidget(recoveryDetails);
    recoveryDetails->hide();
    connect(showRecovery, &QPushButton::toggled, recoveryDetails, &QWidget::setVisible);
    connect(showRecovery, &QPushButton::toggled, this, [showRecovery](bool shown) {
        showRecovery->setText(shown ? tr("Hide unfinished reviews") : tr("Unfinished reviews"));
    });
    auto* recoveryError = new QLabel(recovery);
    recoveryError->setWordWrap(true);
    recoveryError->setTextFormat(Qt::PlainText);
    recoveryLayout->addWidget(recoveryError);
    auto* controls = new QHBoxLayout;
    auto* earlier = new QPushButton(tr("Previous"), recovery);
    auto* later = new QPushButton(tr("Next"), recovery);
    auto* refresh = new QPushButton(tr("Refresh"), recovery);
    controls->addWidget(earlier);
    controls->addWidget(later);
    controls->addWidget(refresh);
    controls->addStretch();
    detailsLayout->addLayout(controls);
    recovery_->addWidget(recovery);
    const auto updateRecovery = [this, recovery, recoveryRows, recoveryError, showRecovery, earlier,
                                 later, refresh, openRecovery, &review] {
        auto& workspace = review.workspace();
        const auto* selected = recoveryRows->currentItem();
        const auto previous = selected ? selected->data(Qt::UserRole) : QVariant{};
        const QSignalBlocker blocker(recoveryRows);
        // Retain the items/focus during the initial busy notification.
        if (showRecovery->isChecked() && !workspace.recoveryBusy()) {
            recoveryRows->clear();
            for (const auto& run : workspace.interrupted()) {
                QString title = tr("Saved game #%1").arg(run.gameId);
                if (library_ && !library_->showingGames()) {
                    const auto& gameRows = library_->rows();
                    const auto game = std::find_if(
                        gameRows.begin(), gameRows.end(),
                        [&run](const auto& candidate) { return candidate.id == run.gameId; });
                    if (game != gameRows.end() && !game->metadataError) {
                        title = gameTitle(*game);
                    }
                }
                auto* item = new QListWidgetItem(
                    run.metadataError
                        ? tr("%1\nSaved review could not be read.").arg(title)
                        : tr("%1 (%2)\n%3 · %4/%5 positions")
                              .arg(title,
                                   run.selectedSide == chess::Color::White ? tr("White")
                                                                           : tr("Black"),
                                   QLocale().toString(run.createdAt.toLocalTime(),
                                                      QLocale::ShortFormat))
                              .arg(run.completedPositions)
                              .arg(run.totalPositions),
                    recoveryRows);
                item->setData(Qt::UserRole, QVariant::fromValue(static_cast<qint64>(run.id)));
                item->setData(Qt::UserRole + 1,
                              QVariant::fromValue(static_cast<qint64>(run.gameId)));
                item->setData(Qt::UserRole + 2, run.id > 0 && run.gameId > 0);
                item->setToolTip(
                    QStringLiteral("<qt>%1</qt>")
                        .arg((run.metadataError
                                  ? QObject::tr(
                                        "The saved data is preserved. Open for details, or use "
                                        "Games to inspect another run or start a new review.")
                                  : item->text())
                                 .toHtmlEscaped()
                                 .replace(u'\n', QStringLiteral("<br>"))));
                if (item->data(Qt::UserRole) == previous) {
                    recoveryRows->setCurrentItem(item);
                }
            }
            if (!recoveryRows->currentItem() && recoveryRows->count() > 0) {
                recoveryRows->setCurrentRow(0);
            }
        }
        const auto* current = recoveryRows->currentItem();
        openRecovery->setEnabled(!workspace.recoveryBusy() && current &&
                                 current->data(Qt::UserRole + 2).toBool());
        recovery->setVisible(!workspace.interrupted().empty() ||
                             !workspace.recoveryError().isEmpty());
        recoveryError->setText(workspace.recoveryError());
        recoveryError->setVisible(!workspace.recoveryError().isEmpty());
        earlier->setEnabled(!workspace.recoveryBusy() && workspace.recoveryPage() > 0);
        later->setEnabled(!workspace.recoveryBusy() && workspace.recoveryHasNext());
        refresh->setEnabled(!workspace.recoveryBusy());
    };
    connect(showRecovery, &QPushButton::toggled, this, updateRecovery);
    connect(&review.workspace(), &app::ReviewWorkspaceController::recoveryChanged, this,
            updateRecovery);
    if (library_) {
        connect(library_, &app::GameLibraryController::changed, this, updateRecovery);
    }
    connect(earlier, &QPushButton::clicked, this, [&review] {
        auto& w = review.workspace();
        w.refreshRecovery(w.recoveryPage() - 1);
    });
    connect(later, &QPushButton::clicked, this, [&review] {
        auto& w = review.workspace();
        w.refreshRecovery(w.recoveryPage() + 1);
    });
    connect(refresh, &QPushButton::clicked, this,
            [&review] { review.workspace().refreshRecovery(); });
    updateRecovery();
}
} // namespace chess_review::ui
