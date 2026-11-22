#include "ui/analysis/EngineAnalysisPanel.h"
#include "ui/EngineText.h"
#include "ui/Theme.h"
#include "ui/chess/ChessBoardWidget.h"
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextDocument>
#include <QVBoxLayout>
#include <algorithm>

namespace chess_review::ui {
EvaluationBarWidget::EvaluationBarWidget(ChessBoardWidget& board, QWidget* parent)
    : QWidget(parent), board_(board) {
    setObjectName(QStringLiteral("evaluationBar"));
    setAccessibleName(tr("Evaluation bar — White perspective"));
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    board_.installEventFilter(this);
    setEvaluation({});
}
QSize EvaluationBarWidget::sizeHint() const { return {22, 248}; }
bool EvaluationBarWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == &board_ && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        alignWithBoard();
    }
    return QWidget::eventFilter(watched, event);
}
void EvaluationBarWidget::alignWithBoard() {
    if (board_.parentWidget() != parentWidget()) {
        return;
    }
    const auto square = board_.boardRect();
    const auto topLeft = board_.mapTo(parentWidget(), square.topLeft().toPoint());
    setGeometry(topLeft.x() - sizeHint().width() - 6, topLeft.y(), sizeHint().width(),
                static_cast<int>(square.height()));
    update();
}
void EvaluationBarWidget::setEvaluation(const app::EngineLinePresentation& line) {
    alignWithBoard();
    whiteShare_ = line.whiteShare;
    const auto value = line.evaluation.isEmpty() ? tr("Unavailable") : line.evaluation;
    setToolTip(tr("White perspective: %1\nVisual scale: −10 to +10 pawns; mate fills the bar.\n"
                  "The bar is unavailable for missing or bounded scores.")
                   .arg(value));
    setAccessibleDescription(tr("White perspective: %1").arg(value));
    update();
}
void EvaluationBarWidget::setExpectedScore(std::optional<double> whiteShare,
                                           const QString& description) {
    alignWithBoard();
    whiteShare_ = whiteShare;
    setToolTip(description);
    setAccessibleName(tr("Evaluation bar — White expected score"));
    setAccessibleDescription(description);
    update();
}
void EvaluationBarWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const QRectF bar = QRectF(rect()).adjusted(0, 0, -1, -1);
    if (!whiteShare_) {
        painter.fillRect(bar, QBrush(palette().color(QPalette::Mid), Qt::Dense4Pattern));
    } else {
        const QColor light(QStringLiteral("#e6e9e7"));
        const QColor dark(QStringLiteral("#27312e"));
        const bool flipped = board_.state().blackAtBottom;
        painter.fillRect(bar, flipped ? dark : light);
        const double upperShare = flipped ? *whiteShare_ : 1.0 - *whiteShare_;
        painter.fillRect(QRectF(bar.left(), bar.top(), bar.width(), bar.height() * upperShare),
                         flipped ? light : dark);
    }
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawRect(bar);
}

EngineAnalysisPanel::EngineAnalysisPanel(app::EngineAnalysisController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setObjectName(QStringLiteral("engineAnalysisPanel"));
    setAccessibleName(tr("Live engine analysis"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    const auto label = [this, layout](const QString& name) {
        auto* result = new QLabel(this);
        result->setObjectName(name);
        result->setTextFormat(Qt::PlainText);
        result->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        result->setWordWrap(true);
        result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(result);
        return result;
    };
    engine_ = label(QStringLiteral("analysisEngineName"));
    setTextRole(*engine_, "heading");
    engine_->setWordWrap(false);
    status_ = label(QStringLiteral("engineAnalysisStatus"));
    auto* commands = new QGridLayout;
    const auto button = [this](const QString& text, const char* name) {
        auto* result = new QPushButton(text, this);
        result->setObjectName(QString::fromLatin1(name));
        return result;
    };
    start_ = button(tr("Start"), "startEngineAnalysis");
    start_->setToolTip(tr("Follow the current board with continuous analysis."));
    pause_ = button(tr("Pause"), "pauseEngineAnalysis");
    pause_->setToolTip(tr("Pause engine work until you choose Resume."));
    stop_ = button(tr("Stop"), "stopEngineAnalysis");
    deepen_ = button(tr("Deepen"), "deepenEngineAnalysis");
    deepen_->setToolTip(tr("Run a depth-limited search on the current position."));
    auto* manage = button(tr("Engines…"), "manageAnalysisEngines");
    setPrimaryAction(*start_);
    commands->addWidget(start_, 0, 0);
    commands->addWidget(pause_, 0, 1);
    commands->addWidget(stop_, 0, 2);
    commands->addWidget(deepen_, 0, 3);
    commands->addWidget(manage, 1, 0, 1, 2);
    auto* widthLabel = new QLabel(tr("&Lines"), this);
    width_ = new QSpinBox(this);
    width_->setObjectName(QStringLiteral("analysisMultiPv"));
    width_->setKeyboardTracking(false);
    widthLabel->setBuddy(width_);
    commands->addWidget(widthLabel, 1, 2, Qt::AlignRight);
    commands->addWidget(width_, 1, 3);
    layout->addLayout(commands);
    evaluation_ = label(QStringLiteral("engineEvaluation"));
    evaluation_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    evaluation_->setAccessibleName(tr("Evaluation from White's perspective"));
    layout->removeWidget(evaluation_);
    depth_ = label(QStringLiteral("engineDepth"));
    setTextRole(*depth_, "secondary");
    auto* details = button(tr("Engine details"), "engineMetricsToggle");
    details->setCheckable(true);
    layout->addWidget(details, 0, Qt::AlignLeft);
    depth_->hide();
    connect(details, &QPushButton::toggled, depth_, &QWidget::setVisible);
    pv_ = new QListWidget(this);
    pv_->setObjectName(QStringLiteral("enginePv"));
    pv_->setAccessibleName(tr("Engine candidate lines"));
    pv_->setAccessibleDescription(tr("Select a line to preview it on the board."));
    pv_->setMinimumHeight(70);
    pv_->setMaximumHeight(112);
    pv_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    pv_->setTextElideMode(Qt::ElideRight);
    pv_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* candidate = new QHBoxLayout;
    candidate->addWidget(evaluation_, 0, Qt::AlignTop);
    candidate->addWidget(pv_, 1);
    layout->insertLayout(layout->indexOf(depth_), candidate);
    previewControls_ = new QWidget(this);
    auto* previewLayout = new QVBoxLayout(previewControls_);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewStatus_ = new QLabel(previewControls_);
    previewStatus_->setTextFormat(Qt::PlainText);
    previewStatus_->setWordWrap(true);
    previewLayout->addWidget(previewStatus_);
    auto* previewButtons = new QGridLayout;
    previous_ = button(tr("Previous"), "previousEnginePreview");
    next_ = button(tr("Next"), "nextEnginePreview");
    add_ = button(tr("Add Line"), "addEngineVariation");
    auto* dismiss = button(tr("Exit Preview"), "dismissEngineVariation");
    previewButtons->addWidget(previous_, 0, 0);
    previewButtons->addWidget(next_, 0, 1);
    previewButtons->addWidget(add_, 1, 0);
    previewButtons->addWidget(dismiss, 1, 1);
    previewLayout->addLayout(previewButtons);
    layout->insertWidget(layout->indexOf(depth_), previewControls_);
    error_ = label(QStringLiteral("engineAnalysisError"));
    connect(start_, &QPushButton::clicked, &controller_, &app::EngineAnalysisController::start);
    connect(pause_, &QPushButton::clicked, &controller_, &app::EngineAnalysisController::pause);
    connect(stop_, &QPushButton::clicked, &controller_, &app::EngineAnalysisController::stop);
    connect(deepen_, &QPushButton::clicked, &controller_, &app::EngineAnalysisController::deepen);
    connect(width_, &QSpinBox::valueChanged, &controller_,
            &app::EngineAnalysisController::setMultiPv);
    const auto preview = [this](const QListWidgetItem* item) {
        controller_.previewLine(item->data(Qt::UserRole).toInt());
    };
    connect(pv_, &QListWidget::itemClicked, this, preview);
    connect(pv_, &QListWidget::itemActivated, this, preview);
    connect(previous_, &QPushButton::clicked, this, [this] { controller_.stepPreview(-1); });
    connect(next_, &QPushButton::clicked, this, [this] { controller_.stepPreview(1); });
    connect(add_, &QPushButton::clicked, &controller_, &app::EngineAnalysisController::addPreview);
    connect(dismiss, &QPushButton::clicked, &controller_,
            &app::EngineAnalysisController::dismissPreview);
    connect(manage, &QPushButton::clicked, this, &EngineAnalysisPanel::manageEnginesRequested);
    connect(&controller_, &app::EngineAnalysisController::changed, this,
            &EngineAnalysisPanel::refresh);
    refresh();
}
void EngineAnalysisPanel::refresh() {
    const auto& state = controller_.state();
    engine_->setText(state.engineName.isEmpty()
                         ? tr("Engine analysis")
                         : presentedEngineText(clippedEngineText(state.engineName)));
    engine_->setToolTip(
        Qt::convertFromPlainText(presentedEngineText(clippedEngineText(state.engineName))));
    const auto depth = state.line.depth ? tr("Depth %1").arg(*state.line.depth) : QString{};
    const auto target =
        state.targetDepth ? tr("Target depth %1").arg(*state.targetDepth) : QString{};
    QStringList detail;
    if (state.savedEvidence) {
        detail.append(state.line.depth
                          ? tr("Saved review analysis at depth %1").arg(*state.line.depth)
                          : tr("Saved review analysis"));
    } else if (!depth.isEmpty()) {
        detail.append(depth);
    }
    if (!target.isEmpty() && state.status != tr("Deepening to depth %1").arg(*state.targetDepth)) {
        detail.append(target);
    }
    const QLocale locale;
    status_->setText(detail.isEmpty() ? state.status
                                      : state.status + u'\n' + locale.createSeparatedList(detail));
    evaluation_->setText(
        tr("White\n%1")
            .arg(state.line.evaluation.isEmpty() ? tr("Unavailable") : state.line.evaluation));
    evaluation_->setToolTip(tr("Evaluation from White's perspective"));
    QStringList metrics;
    if (state.statistics.nodes) {
        metrics.append(
            tr("%1 nodes").arg(locale.toString(static_cast<qulonglong>(*state.statistics.nodes))));
    }
    if (state.statistics.nps) {
        metrics.append(
            tr("%1 nodes/s").arg(locale.toString(static_cast<qulonglong>(*state.statistics.nps))));
    }
    if (state.statistics.timeMs) {
        metrics.append(
            tr("%1 ms").arg(locale.toString(static_cast<qulonglong>(*state.statistics.timeMs))));
    }
    depth_->setText(metrics.empty() ? tr("No search metrics available")
                                    : locale.createSeparatedList(metrics));

    if (renderedCandidates_ != state.candidatesRevision) {
        renderedCandidates_ = state.candidatesRevision;
        const auto count = static_cast<int>(state.candidates.size());
        while (pv_->count() > std::max(1, count)) {
            delete pv_->takeItem(pv_->count() - 1);
        }
        while (pv_->count() < std::max(1, count)) {
            pv_->addItem(QString{});
        }
        if (count == 0) {
            auto* item = pv_->item(0);
            item->setFlags(Qt::NoItemFlags);
            item->setText(tr("No engine lines yet"));
            item->setToolTip({});
            item->setData(Qt::UserRole, {});
            item->setFont(pv_->font());
        }
        for (int i = 0; i < count; ++i) {
            const auto& line = state.candidates[static_cast<std::size_t>(i)];
            auto* item = pv_->item(i);
            auto font = pv_->font();
            font.setBold(line.rank == 1);
            const auto label = line.rank == 1 ? tr("Best") : QString::number(line.rank);
            item->setText(tr("%1   %2   %3").arg(label, line.evaluation, line.pv.left(4096)));
            item->setToolTip(
                tr("%1\n%2")
                    .arg(line.depth ? tr("Depth %1").arg(*line.depth) : tr("Depth unavailable"),
                         line.pv.left(4096))
                    .toHtmlEscaped());
            item->setFont(font);
            item->setFlags(line.moves.empty() ? Qt::ItemIsEnabled
                                              : Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            item->setData(Qt::UserRole, line.rank);
        }
    }

    const QSignalBlocker block(width_);
    if (width_->maximum() != state.availableLines) {
        width_->setRange(1, state.availableLines);
    }
    const auto effectiveCount = std::min(state.requestedLines, state.availableLines);
    if (width_->value() != effectiveCount) {
        width_->setValue(effectiveCount);
    }
    width_->setEnabled(state.availableLines > 1);
    width_->setToolTip(state.requestedLines > state.availableLines
                           ? tr("%1 preferred. Up to %2 available for this engine and position.")
                                 .arg(state.requestedLines)
                                 .arg(state.availableLines)
                           : tr("Number of candidate lines to analyze."));
    error_->setText(state.error);
    error_->setVisible(!state.error.isEmpty());
    start_->setText(state.paused ? tr("Resume") : tr("Start"));
    start_->setEnabled(state.canStart);
    pause_->setEnabled(state.canPause);
    stop_->setEnabled(state.canStop);
    deepen_->setEnabled(state.canDeepen);
    const auto& preview = controller_.preview();
    previewControls_->setVisible(preview.has_value());
    if (preview) {
        const auto ply = controller_.previewPly();
        const auto size = static_cast<int>(preview->moves.size());
        const auto previewText =
            tr("Preview at ply %1 of %2 (%3)").arg(ply).arg(size).arg(preview->pv.left(4096));
        previewStatus_->setText(
            QFontMetrics(previewStatus_->font())
                .elidedText(previewText, Qt::ElideRight, std::max(pv_->width(), 160)));
        previewStatus_->setToolTip(previewText.toHtmlEscaped());
        previous_->setEnabled(ply > 0);
        next_->setEnabled(ply < size);
        add_->setEnabled(controller_.previewBoard().has_value());
    }
}
} // namespace chess_review::ui
