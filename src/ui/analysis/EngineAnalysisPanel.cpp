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
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextDocument>
#include <QVBoxLayout>
#include <algorithm>

namespace chess_review::ui {
namespace {
class CandidateLineList final : public QListWidget {
  public:
    explicit CandidateLineList(QWidget* parent) : QListWidget(parent) {}

    QSize sizeHint() const override {
        const int rows = std::clamp(count(), 1, 3);
        const int rowHeight =
            count() > 0 && sizeHintForRow(0) > 0 ? sizeHintForRow(0) : fontMetrics().height() + 8;
        return {QListWidget::sizeHint().width(), std::clamp(rows * rowHeight + 2 * frameWidth() + 4,
                                                            minimumHeight(), maximumHeight())};
    }
};
} // namespace

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
    setToolTip(tr("White perspective: %1").arg(value));
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
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF bar = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath clip;
    clip.addRoundedRect(bar, 3.0, 3.0);
    painter.save();
    painter.setClipPath(clip);
    if (!whiteShare_) {
        painter.fillRect(bar, palette().color(QPalette::AlternateBase));
        painter.setPen(QPen(palette().color(QPalette::Mid), 1.5));
        painter.drawLine(QPointF(bar.left() + 5, bar.center().y()),
                         QPointF(bar.right() - 5, bar.center().y()));
    } else {
        const QColor light(QStringLiteral("#e8ece9"));
        const QColor dark(QStringLiteral("#2a3430"));
        const bool flipped = board_.state().blackAtBottom;
        painter.fillRect(bar, flipped ? dark : light);
        const double upperShare = flipped ? *whiteShare_ : 1.0 - *whiteShare_;
        painter.fillRect(QRectF(bar.left(), bar.top(), bar.width(), bar.height() * upperShare),
                         flipped ? light : dark);
        painter.setPen(QPen(QColor(128, 140, 134, 90), 1.0));
        painter.drawLine(QPointF(bar.left(), bar.center().y()),
                         QPointF(bar.right(), bar.center().y()));
    }
    painter.restore();
    painter.setPen(palette().color(QPalette::Mid));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(bar, 3.0, 3.0);
}

EngineAnalysisPanel::EngineAnalysisPanel(app::EngineAnalysisController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setObjectName(QStringLiteral("engineAnalysisPanel"));
    setAccessibleName(tr("Live engine analysis"));
    setPanelSurface(*this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 6, 10, 6);
    layout->setSpacing(4);
    const auto makeLabel = [this](const QString& name) {
        auto* result = new QLabel(this);
        result->setObjectName(name);
        result->setTextFormat(Qt::PlainText);
        result->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        result->setWordWrap(true);
        result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        return result;
    };
    engine_ = makeLabel(QStringLiteral("analysisEngineName"));
    setTextRole(*engine_, "heading");
    engine_->setWordWrap(false);
    status_ = makeLabel(QStringLiteral("engineAnalysisStatus"));
    setTextRole(*status_, "caption");
    const auto button = [this](const QString& text, const char* name,
                               const QString& iconName = {}) {
        auto* result = new QPushButton(text, this);
        result->setObjectName(QString::fromLatin1(name));
        setCompactAction(*result);
        if (!iconName.isEmpty()) {
            result->setProperty("keepIconSize", true);
            result->setIconSize(QSize(14, 14));
            setIcon(*result, iconName);
        }
        return result;
    };
    auto* header = new QHBoxLayout;
    header->setSpacing(6);
    auto* headerText = new QVBoxLayout;
    headerText->setSpacing(1);
    headerText->addWidget(engine_);
    headerText->addWidget(status_);
    header->addLayout(headerText, 1);
    auto* widthLabel = new QLabel(tr("&Lines"), this);
    setTextRole(*widthLabel, "caption");
    width_ = new QSpinBox(this);
    width_->setObjectName(QStringLiteral("analysisMultiPv"));
    width_->setKeyboardTracking(false);
    widthLabel->setBuddy(width_);
    header->addWidget(widthLabel);
    header->addWidget(width_);
    auto* manage = button(tr("Engines…"), "manageAnalysisEngines", QStringLiteral("engine"));
    setQuietAction(*manage);
    header->addWidget(manage);
    layout->addLayout(header);

    auto* commands = new QHBoxLayout;
    commands->setSpacing(4);
    start_ = button(tr("Start"), "startEngineAnalysis", QStringLiteral("play"));
    start_->setToolTip(tr("Follow the current board with continuous analysis."));
    pause_ = button(tr("Pause"), "pauseEngineAnalysis", QStringLiteral("pause"));
    pause_->setToolTip(tr("Pause engine work until you choose Resume."));
    stop_ = button(tr("Stop"), "stopEngineAnalysis", QStringLiteral("stop"));
    deepen_ = button(tr("Deepen"), "deepenEngineAnalysis", QStringLiteral("deepen"));
    deepen_->setToolTip(tr("Run a depth-limited search on the current position."));
    setPrimaryAction(*start_);
    commands->addWidget(start_);
    commands->addWidget(pause_);
    commands->addWidget(stop_);
    commands->addWidget(deepen_);
    commands->addStretch();
    layout->addLayout(commands);

    evaluation_ = makeLabel(QStringLiteral("engineEvaluation"));
    evaluation_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    evaluation_->setAlignment(Qt::AlignCenter);
    evaluation_->setMinimumWidth(64);
    evaluation_->setContentsMargins(6, 3, 6, 3);
    setSubtleSurface(*evaluation_);
    setTextRole(*evaluation_, "strong");
    evaluation_->setAccessibleName(tr("Evaluation from White's perspective"));
    pv_ = new CandidateLineList(this);
    pv_->setObjectName(QStringLiteral("enginePv"));
    pv_->setAccessibleName(tr("Engine candidate lines"));
    pv_->setAccessibleDescription(tr("Select a line to preview it on the board."));
    pv_->setMinimumHeight(38);
    pv_->setMaximumHeight(112);
    pv_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    pv_->setTextElideMode(Qt::ElideRight);
    pv_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pv_->setFrameShape(QFrame::NoFrame);
    pv_->setAlternatingRowColors(true);
    setFlatSurface(*pv_);
    auto* candidate = new QHBoxLayout;
    candidate->setSpacing(8);
    candidate->addWidget(evaluation_, 0, Qt::AlignTop);
    candidate->addWidget(pv_, 1);
    layout->addLayout(candidate);

    previewControls_ = new QWidget(this);
    setSubtleSurface(*previewControls_);
    auto* previewLayout = new QVBoxLayout(previewControls_);
    previewLayout->setContentsMargins(10, 8, 10, 8);
    previewLayout->setSpacing(6);
    previewStatus_ = new QLabel(previewControls_);
    previewStatus_->setTextFormat(Qt::PlainText);
    previewStatus_->setWordWrap(true);
    setTextRole(*previewStatus_, "caption");
    previewLayout->addWidget(previewStatus_);
    auto* previewButtons = new QHBoxLayout;
    previewButtons->setSpacing(6);
    previous_ = button(tr("Previous"), "previousEnginePreview", QStringLiteral("chevron-left"));
    next_ = button(tr("Next"), "nextEnginePreview", QStringLiteral("chevron-right"));
    add_ = button(tr("Add Line"), "addEngineVariation", QStringLiteral("plus"));
    setPrimaryAction(*add_);
    auto* dismiss = button(tr("Exit Preview"), "dismissEngineVariation", QStringLiteral("close"));
    setQuietAction(*dismiss);
    previewButtons->addWidget(previous_);
    previewButtons->addWidget(next_);
    previewButtons->addStretch();
    previewButtons->addWidget(add_);
    previewButtons->addWidget(dismiss);
    previewLayout->addLayout(previewButtons);
    layout->addWidget(previewControls_);

    auto* details = button(tr("Engine details"), "engineMetricsToggle", QStringLiteral("info"));
    details->setCheckable(true);
    setQuietAction(*details);
    layout->addWidget(details, 0, Qt::AlignLeft);
    depth_ = makeLabel(QStringLiteral("engineDepth"));
    setTextRole(*depth_, "caption");
    layout->addWidget(depth_);
    depth_->hide();
    connect(details, &QPushButton::toggled, depth_, &QWidget::setVisible);
    error_ = makeLabel(QStringLiteral("engineAnalysisError"));
    layout->addWidget(error_);
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
    status_->setText(detail.isEmpty()
                         ? state.status
                         : state.status + tr(" · ") + locale.createSeparatedList(detail));
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
        pv_->updateGeometry();
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
            tr("Preview move %1 of %2: %3").arg(ply).arg(size).arg(preview->pv.left(4096));
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
