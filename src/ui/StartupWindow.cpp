#include "ui/StartupWindow.h"
#include "diagnostics/Logger.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QTimer>
#include <QUrl>

#include <QCloseEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

namespace chess_review::ui {

StartupWindow::StartupWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("chess-review"));
    resize(1000, 700);

    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 24, 24, 24);
    status_ = new QLabel(QStringLiteral("Opening local database…"), content);
    status_->setObjectName(QStringLiteral("startupStatus"));
    status_->setTextFormat(Qt::PlainText);
    diagnostic_ = new QLabel(content);
    diagnostic_->setObjectName(QStringLiteral("startupDiagnostic"));
    diagnostic_->setTextFormat(Qt::PlainText);
    diagnostic_->setWordWrap(true);
    diagnostic_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    diagnostic_->hide();
    summary_ = new QLabel(content);
    summary_->setObjectName(QStringLiteral("failureSummary"));
    summary_->setTextFormat(Qt::PlainText);
    summary_->setWordWrap(true);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    notice_ = new QLabel(content);
    notice_->setObjectName(QStringLiteral("loggingNotice"));
    notice_->setTextFormat(Qt::PlainText);
    notice_->setWordWrap(true);
    notice_->hide();
    reveal_ = new QPushButton(tr("Show technical &details (may contain private paths)"), content);
    reveal_->setObjectName(QStringLiteral("revealDetails"));
    reveal_->setCheckable(true);
    reveal_->hide();
    connect(reveal_, &QPushButton::toggled, diagnostic_, &QWidget::setVisible);
    copy_ = new QPushButton(tr("&Copy diagnostics"), content);
    copy_->setObjectName(QStringLiteral("copyDiagnostics"));
    copy_->hide();
    connect(copy_, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(safeReport()); });
    auto* help = menuBar()->addMenu(tr("&Help"));
    auto* action = help->addAction(tr("&Diagnostics…"));
    action->setObjectName(QStringLiteral("diagnosticsAction"));
    connect(action, &QAction::triggered, this, &StartupWindow::showDiagnostics);
    progress_ = new QProgressBar(content);
    progress_->setRange(0, 0);
    progress_->setAccessibleName(QStringLiteral("Database initialization"));
    auto* exit = new QPushButton(QStringLiteral("E&xit"), content);
    connect(exit, &QPushButton::clicked, this, &QWidget::close);
    layout->addWidget(status_);
    layout->addWidget(progress_);
    layout->addWidget(notice_);
    layout->addWidget(summary_);
    layout->addWidget(reveal_, 0, Qt::AlignLeft);
    layout->addWidget(diagnostic_);
    layout->addWidget(copy_, 0, Qt::AlignLeft);
    layout->addWidget(exit, 0, Qt::AlignLeft);
    layout->addStretch();
    setCentralWidget(content);
}

void StartupWindow::showProgress(const QString& message) { status_->setText(message); }

void StartupWindow::showReady() {
    status_->setText(QStringLiteral("Ready"));
    progress_->hide();
}

void StartupWindow::showFailure(const app::Diagnostic& diagnostic) {
    failure_ = diagnostic;
    status_->setText(tr("Could not initialize local storage"));
    summary_->setText(diagnostic.summary + QStringLiteral("\n\n") + diagnostic.nextSteps);
    diagnostic_->setText(diagnostic.sensitiveDetails);
    reveal_->setChecked(false);
    diagnostic_->hide();
    reveal_->show();
    copy_->show();
    progress_->hide();
}

QString StartupWindow::safeReport() const {
    return (failure_.code.isEmpty() ? QString() : failure_.safeReport()) +
           (logger_ ? logger_->report() : tr("Local logging is not configured.\n"));
}

void StartupWindow::setDiagnostics(diagnostics::Logger& logger) {
    logger_ = &logger;
    const auto updateHealth = [this] {
        if (!loggingNoticeShown_ && logger_->health() == diagnostics::Health::MemoryOnly) {
            loggingNoticeShown_ = true;
            notice_->setText(tr("Local logs could not be saved. You can continue using the app and "
                                "copy recent diagnostics from Help → Diagnostics."));
            notice_->show();
        }
    };
    connect(&logger, &diagnostics::Logger::healthChanged, this, updateHealth);
    updateHealth();
}

void StartupWindow::showDiagnostics() {
    // Reuse the existing modeless window, preserving keyboard focus and selection.
    if (auto* existing = findChild<QDialog*>(QStringLiteral("diagnosticsDialog"))) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("diagnosticsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Diagnostics"));
    dialog->resize(650, 450);
    auto* layout = new QVBoxLayout(dialog);
    auto* report = new QPlainTextEdit(dialog);
    report->setObjectName(QStringLiteral("safeDiagnosticsReport"));
    report->setReadOnly(true);
    report->setAccessibleName(tr("Redacted diagnostics report"));
    report->setPlainText(safeReport());
    layout->addWidget(report);
    auto* copy = new QPushButton(tr("&Copy diagnostics"), dialog);
    connect(copy, &QPushButton::clicked, dialog,
            [this] { QApplication::clipboard()->setText(safeReport()); });
    layout->addWidget(copy);
    auto* folder = new QPushButton(tr("Open &log folder"), dialog);
    folder->setEnabled(logger_ && logger_->health() == diagnostics::Health::Active);
    connect(folder, &QPushButton::clicked, dialog, [this, folder] {
        if (!logger_ || logger_->health() != diagnostics::Health::Active ||
            !QDesktopServices::openUrl(QUrl::fromLocalFile(logger_->logDirectory()))) {
            folder->setText(tr("Log folder could not be opened"));
        }
    });
    layout->addWidget(folder);
    // Refresh only on a bounded UI timer, never one queued update per log event.
    auto* refresh = new QTimer(dialog);
    connect(refresh, &QTimer::timeout, dialog, [this, report, folder] {
        const auto current = safeReport();
        if (report->toPlainText() != current) {
            report->setPlainText(current);
        }
        folder->setEnabled(logger_ && logger_->health() == diagnostics::Health::Active);
    });
    refresh->start(1000);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->show();
}

void StartupWindow::closeEvent(QCloseEvent* event) {
    emit closeRequested();
    QMainWindow::closeEvent(event);
}

} // namespace chess_review::ui
