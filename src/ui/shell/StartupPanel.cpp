#include "ui/shell/StartupPanel.h"
#include "diagnostics/Logger.h"
#include "ui/shell/DiagnosticsDialog.h"
#include <QApplication>
#include <QClipboard>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace chess_review::ui {

StartupPanel::StartupPanel(QWidget* parent) : QWidget(parent) {
    auto* content = this;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
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
    summary_->hide();
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    notice_ = new QLabel(content);
    notice_->setObjectName(QStringLiteral("loggingNotice"));
    notice_->setTextFormat(Qt::PlainText);
    notice_->setWordWrap(true);
    notice_->hide();
    configurationNotice_ = new QLabel(content);
    configurationNotice_->setObjectName(QStringLiteral("configurationNotice"));
    configurationNotice_->setTextFormat(Qt::PlainText);
    configurationNotice_->setWordWrap(true);
    configurationNotice_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                                  Qt::TextSelectableByKeyboard);
    configurationNotice_->hide();
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
    progress_ = new QProgressBar(content);
    progress_->setRange(0, 0);
    progress_->setAccessibleName(QStringLiteral("Database initialization"));
    auto* exit = new QPushButton(QStringLiteral("E&xit"), content);
    connect(exit, &QPushButton::clicked, this, &StartupPanel::closeRequested);
    exit_ = exit;
    exit_->hide();
    layout->addWidget(status_);
    layout->addWidget(progress_);
    layout->addWidget(notice_);
    layout->addWidget(configurationNotice_);
    layout->addWidget(summary_);
    layout->addWidget(reveal_, 0, Qt::AlignLeft);
    layout->addWidget(diagnostic_);
    layout->addWidget(copy_, 0, Qt::AlignLeft);
    layout->addWidget(exit, 0, Qt::AlignLeft);
}

void StartupPanel::showProgress(const QString& message) { status_->setText(message); }

void StartupPanel::showReady() {
    status_->setText(QStringLiteral("Ready"));
    progress_->hide();
    status_->hide();
    setVisible(loggingNoticeShown_ || !configurationReport_.isEmpty());
}

void StartupPanel::showFailure(const app::Diagnostic& diagnostic) {
    show();
    summary_->show();
    failure_ = diagnostic;
    status_->show();
    exit_->show();
    status_->setText(tr("Could not initialize local storage"));
    summary_->setText(diagnostic.summary + QStringLiteral("\n\n") + diagnostic.nextSteps);
    diagnostic_->setPlainText(diagnostic.sensitiveDetails);
    reveal_->setChecked(false);
    diagnostic_->hide();
    reveal_->show();
    copy_->show();
    progress_->hide();
}

QString StartupPanel::safeReport() const {
    return configurationReport_ + (failure_.code.isEmpty() ? QString() : failure_.safeReport()) +
           (logger_ ? logger_->report() : tr("Local logging is not configured.\n"));
}

void StartupPanel::setDiagnostics(diagnostics::Logger& logger) {
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
            show();
        }
    };
    connect(&logger, &diagnostics::Logger::healthChanged, this, updateHealth);
    updateHealth();
}

void StartupPanel::showDiagnostics() {
    if (auto* existing =
            window()->findChild<DiagnosticsDialog*>(QStringLiteral("diagnosticsDialog"))) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto* dialog = new DiagnosticsDialog(
        logger_,
        [source = QPointer<StartupPanel>(this)] {
            return source ? source->safeReport()
                          : tr("Startup diagnostics are no longer available.\n");
        },
        window());
    dialog->show();
}

void StartupPanel::showClosing() {
    show();
    progress_->setAccessibleName(tr("Application shutdown"));
    status_->setText(tr("Closing…"));
    status_->show();
    progress_->show();
    exit_->setEnabled(false);
}

void StartupPanel::showConfigurationFailure(const QString& safeDetails) {
    const auto summary =
        tr("Review configuration is invalid. Reviews are unavailable. "
           "Correct config.toml in the application configuration directory and restart. "
           "Import and manual analysis remain available.");
    configurationReport_ = summary + u'\n' + safeDetails;
    configurationNotice_->setText(summary + u'\n' + safeDetails.section(u'\n', 0, 0) + u'\n' +
                                  tr("See Help → Diagnostics for all configuration details."));
    configurationNotice_->setAccessibleName(configurationNotice_->text());
    configurationNotice_->show();
    show();
}
} // namespace chess_review::ui
