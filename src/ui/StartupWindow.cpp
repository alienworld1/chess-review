#include "ui/StartupWindow.h"
#include "diagnostics/Logger.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QUrl>

#include <QCloseEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <limits>

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
    diagnostic_ = new QPlainTextEdit(content);
    diagnostic_->setObjectName(QStringLiteral("startupDiagnostic"));
    diagnostic_->setReadOnly(true);
    diagnostic_->setAccessibleName(tr("Private technical details"));
    diagnostic_->setMaximumHeight(200);
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
    connect(reveal_, &QPushButton::toggled, this, [this](bool checked) {
        diagnostic_->setVisible(checked);
        reveal_->setText(checked ? tr("Hide technical &details")
                                 : tr("Show technical &details (may contain private paths)"));
    });
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
    diagnostic_->setPlainText(diagnostic.sensitiveDetails);
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
    connect(&logger, &QObject::destroyed, this, [this, bound = &logger] {
        if (logger_ == bound) {
            logger_ = nullptr;
        }
    });
    const auto updateHealth = [this] {
        if (!loggingNoticeShown_ && logger_ &&
            logger_->failure() != diagnostics::LogFailure::None) {
            loggingNoticeShown_ = true;
            const auto diagnostic = app::loggingDiagnostic(logger_->failure());
            notice_->setText(diagnostic.summary + QStringLiteral("\n") + diagnostic.nextSteps);
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
    copy->setObjectName(QStringLiteral("copyDiagnosticsSnapshot"));
    connect(copy, &QPushButton::clicked, dialog,
            [report] { QApplication::clipboard()->setText(report->toPlainText()); });
    layout->addWidget(copy);
    auto* folder = new QPushButton(tr("Open &log folder"), dialog);
    folder->setObjectName(QStringLiteral("openLogFolder"));
    folder->setEnabled(logger_ && logger_->health() == diagnostics::Health::Active);
    auto* folderError = new QLabel(dialog);
    folderError->setTextFormat(Qt::PlainText);
    folderError->setWordWrap(true);
    folderError->hide();
    connect(folder, &QPushButton::clicked, dialog, [this, folderError] {
        if (!logger_ || logger_->health() != diagnostics::Health::Active ||
            !QDesktopServices::openUrl(QUrl::fromLocalFile(logger_->logDirectory()))) {
            folderError->setText(tr("The log folder could not be opened. Check that a desktop file "
                                    "manager is available."));
            folderError->show();
        } else {
            folderError->hide();
        }
    });
    layout->addWidget(folder);
    layout->addWidget(folderError);
    auto* revealLocation = new QPushButton(tr("Show private log &location"), dialog);
    revealLocation->setObjectName(QStringLiteral("revealLogLocation"));
    revealLocation->setCheckable(true);
    auto* location = new QLineEdit(dialog);
    location->setObjectName(QStringLiteral("privateLogLocation"));
    location->setReadOnly(true);
    location->setMaxLength(std::numeric_limits<int>::max());
    location->setAccessibleName(tr("Private attempted log folder"));
    location->hide();
    connect(revealLocation, &QPushButton::toggled, dialog,
            [this, location, revealLocation](bool checked) {
                if (checked) {
                    const auto path = logger_ ? logger_->logDirectory() : QString();
                    location->setText(path.isEmpty() ? tr("No valid log folder was resolved.")
                                                     : path);
                    location->setCursorPosition(0);
                }
                location->setVisible(checked);
                revealLocation->setText(checked ? tr("Hide private log &location")
                                                : tr("Show private log &location"));
            });
    layout->addWidget(revealLocation);
    layout->addWidget(location);
    // A stable snapshot lets users read/select a report without live log events
    // resetting their selection or scrolling. Copy exports exactly this snapshot.
    auto* refresh = new QPushButton(tr("&Refresh diagnostics"), dialog);
    refresh->setObjectName(QStringLiteral("refreshDiagnostics"));
    connect(refresh, &QPushButton::clicked, dialog,
            [this, report] { report->setPlainText(safeReport()); });
    layout->addWidget(refresh);
    if (logger_) {
        connect(logger_, &diagnostics::Logger::healthChanged, dialog, [this, folder] {
            folder->setEnabled(logger_ && logger_->health() == diagnostics::Health::Active);
        });
        connect(logger_, &QObject::destroyed, dialog, [this, folder] {
            folder->setEnabled(logger_ && logger_->health() == diagnostics::Health::Active);
        });
    }
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
