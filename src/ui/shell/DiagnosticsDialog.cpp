#include "ui/shell/DiagnosticsDialog.h"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <limits>
#include <utility>

namespace chess_review::ui {
DiagnosticsDialog::DiagnosticsDialog(diagnostics::Logger* logger,
                                     std::function<QString()> reportProvider, QWidget* parent)
    : QDialog(parent), logger_(logger), reportProvider_(std::move(reportProvider)) {
    auto* dialog = this;
    dialog->setObjectName(QStringLiteral("diagnosticsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Diagnostics"));
    dialog->resize(650, 450);
    auto* layout = new QVBoxLayout(dialog);
    auto* report = new QPlainTextEdit(dialog);
    report->setObjectName(QStringLiteral("safeDiagnosticsReport"));
    report->setReadOnly(true);
    report->setAccessibleName(tr("Redacted diagnostics report"));
    report->setPlainText(reportProvider_());
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
            [this, report] { report->setPlainText(reportProvider_()); });
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
}
} // namespace chess_review::ui
