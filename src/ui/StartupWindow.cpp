#include "ui/StartupWindow.h"

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
    progress_ = new QProgressBar(content);
    progress_->setRange(0, 0);
    progress_->setAccessibleName(QStringLiteral("Database initialization"));
    auto* exit = new QPushButton(QStringLiteral("E&xit"), content);
    connect(exit, &QPushButton::clicked, this, &QWidget::close);
    layout->addWidget(status_);
    layout->addWidget(progress_);
    layout->addWidget(diagnostic_);
    layout->addWidget(exit, 0, Qt::AlignLeft);
    layout->addStretch();
    setCentralWidget(content);
}

void StartupWindow::showProgress(const QString& message) { status_->setText(message); }

void StartupWindow::showReady() {
    status_->setText(QStringLiteral("Ready"));
    progress_->hide();
}

void StartupWindow::showFailure(const QString& diagnostic) {
    status_->setText(QStringLiteral("Could not initialize local storage"));
    diagnostic_->setText(diagnostic);
    progress_->hide();
}

void StartupWindow::closeEvent(QCloseEvent* event) {
    emit closeRequested();
    QMainWindow::closeEvent(event);
}

} // namespace chess_review::ui
