#include "ui/analysis/EngineAnalysisPanel.h"
#include "ui/EngineText.h"
#include "ui/chess/ChessBoardWidget.h"
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

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
    engine_->setWordWrap(false);
    status_ = label(QStringLiteral("engineAnalysisStatus"));
    status_->setAccessibleName(tr("Engine status"));
    auto* buttons = new QHBoxLayout;
    start_ = new QPushButton(tr("Start"), this);
    start_->setObjectName(QStringLiteral("startEngineAnalysis"));
    start_->setToolTip(tr("Analyze this position continuously, following board moves until Stop."));
    stop_ = new QPushButton(tr("Stop"), this);
    stop_->setObjectName(QStringLiteral("stopEngineAnalysis"));
    auto* manage = new QPushButton(tr("Manage Engines…"), this);
    manage->setObjectName(QStringLiteral("manageAnalysisEngines"));
    buttons->addWidget(start_);
    buttons->addWidget(stop_);
    buttons->addWidget(manage);
    layout->addLayout(buttons);
    evaluation_ = label(QStringLiteral("engineEvaluation"));
    depth_ = label(QStringLiteral("engineDepth"));
    pv_ = new QPlainTextEdit(this);
    pv_->setObjectName(QStringLiteral("enginePv"));
    pv_->setAccessibleName(tr("Principal variation — read only"));
    pv_->setReadOnly(true);
    pv_->setMaximumHeight(110);
    pv_->setPlaceholderText(tr("No principal variation available"));
    layout->addWidget(pv_);
    error_ = label(QStringLiteral("engineAnalysisError"));
    connect(start_, &QPushButton::clicked, &controller_, &app::EngineAnalysisController::start);
    connect(stop_, &QPushButton::clicked, &controller_, &app::EngineAnalysisController::stop);
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
    engine_->setToolTip(presentedEngineText(clippedEngineText(state.engineName)));
    status_->setText(state.status);
    evaluation_->setText(
        tr("White perspective: %1")
            .arg(state.line.evaluation.isEmpty() ? tr("Unavailable") : state.line.evaluation));
    depth_->setText(state.line.depth ? tr("Depth: %1").arg(*state.line.depth)
                                     : tr("Depth: unavailable"));
    if (pv_->toPlainText() != state.line.pv) {
        pv_->setPlainText(state.line.pv);
    }
    error_->setText(state.error);
    error_->setVisible(!state.error.isEmpty());
    start_->setEnabled(state.canStart);
    stop_->setEnabled(state.canStop);
}
} // namespace chess_review::ui
