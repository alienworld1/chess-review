#include "ui/settings/EngineSettingsPage.h"
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace chess_review::ui {
namespace {
QLabel* plainLabel(QWidget* parent, const QString& text = {}) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}
QString clipped(const QString& text) {
    if (text.size() <= 256) {
        return text;
    }
    const qsizetype length = text.at(255).isHighSurrogate() ? 255 : 256;
    return text.left(length) + QChar(0x2026);
}
} // namespace
EngineSettingsPage::EngineSettingsPage(app::EngineSettingsController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setObjectName(QStringLiteral("settingsPage"));
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(plainLabel(this, tr("Settings — Engines")));
    layout->addWidget(
        plainLabel(this, tr("Add a UCI engine installed on your computer. Executables stay in "
                            "their current location. Saved validation times describe past checks; "
                            "engines do not run when this page opens.")));
    profiles_ = new QTableWidget(0, 4, this);
    profiles_->setObjectName(QStringLiteral("engineProfiles"));
    profiles_->setAccessibleName(tr("Registered engines"));
    profiles_->setHorizontalHeaderLabels(
        {tr("Engine"), tr("Executable path"), tr("Default"), tr("Last validated (UTC)")});
    profiles_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    profiles_->setSelectionBehavior(QAbstractItemView::SelectRows);
    profiles_->setWordWrap(false);
    profiles_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    profiles_->horizontalHeader()->setStretchLastSection(true);
    profiles_->setColumnWidth(0, 200);
    profiles_->setColumnWidth(1, 300);
    profiles_->setMinimumHeight(180);
    layout->addWidget(profiles_);
    auto* pagination = new QHBoxLayout;
    previous_ = new QPushButton(tr("Previous"), this);
    next_ = new QPushButton(tr("Next"), this);
    refresh_ = new QPushButton(tr("Refresh"), this);
    pagination->addWidget(previous_);
    pagination->addWidget(next_);
    pagination->addWidget(refresh_);
    pagination->addStretch();
    layout->addLayout(pagination);
    listError_ = plainLabel(this);
    layout->addWidget(listError_);
    browse_ = new QPushButton(tr("Choose &Executable…"), this);
    browse_->setObjectName(QStringLiteral("chooseEngine"));
    layout->addWidget(browse_, 0, Qt::AlignLeft);
    path_ = plainLabel(this);
    path_->setObjectName(QStringLiteral("engineSelectedPath"));
    path_->setAccessibleName(tr("Resolved executable path"));
    path_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(path_);
    message_ = plainLabel(this);
    message_->setObjectName(QStringLiteral("engineRegistrationMessage"));
    layout->addWidget(message_);
    auto* actions = new QHBoxLayout;
    add_ = new QPushButton(tr("&Validate and Add"), this);
    add_->setObjectName(QStringLiteral("registerEngine"));
    cancel_ = new QPushButton(tr("Cancel"), this);
    cancel_->setObjectName(QStringLiteral("cancelEngineRegistration"));
    actions->addWidget(add_);
    actions->addWidget(cancel_);
    actions->addStretch();
    layout->addLayout(actions);
    layout->addStretch();
    connect(browse_, &QPushButton::clicked, this, [this] {
        // Nonblocking native picker; cancellation never launches software.
        auto* picker = new QFileDialog(this, tr("Choose UCI engine executable"));
        picker->setAttribute(Qt::WA_DeleteOnClose);
        picker->setFileMode(QFileDialog::ExistingFile);
        connect(picker, &QFileDialog::fileSelected, &controller_,
                &app::EngineSettingsController::selectExecutable);
        picker->open();
    });
    connect(add_, &QPushButton::clicked, &controller_,
            &app::EngineSettingsController::registerSelected);
    connect(cancel_, &QPushButton::clicked, &controller_, &app::EngineSettingsController::cancel);
    connect(previous_, &QPushButton::clicked, this, [this] {
        controller_.refresh(std::max<std::int64_t>(0, controller_.state().offset - 50));
    });
    connect(next_, &QPushButton::clicked, this,
            [this] { controller_.refresh(controller_.state().offset + 50); });
    connect(refresh_, &QPushButton::clicked, this,
            [this] { controller_.refresh(controller_.state().offset); });
    connect(&controller_, &app::EngineSettingsController::changed, this,
            &EngineSettingsPage::updateState);
    updateState();
}
void EngineSettingsPage::updateState() {
    const auto& state = controller_.state();
    using app::EngineRegistrationState;
    const bool busy = state.registration == EngineRegistrationState::Validating ||
                      state.registration == EngineRegistrationState::Saving ||
                      state.registration == EngineRegistrationState::Closing;
    browse_->setEnabled(state.ready && !busy);
    add_->setEnabled(state.ready && state.registration == EngineRegistrationState::Selected);
    cancel_->setEnabled(state.ready && state.registration != EngineRegistrationState::Idle &&
                        state.registration != EngineRegistrationState::Saving);
    path_->setText(state.selectedPath.isEmpty() ? tr("No executable selected.")
                                                : state.selectedPath);
    message_->setText(state.message);
    listError_->setText(
        state.listError.isEmpty() && state.profiles.empty()
            ? (state.listing
                   ? tr("Loading registered engines…")
                   : tr("No engines registered. Choose an executable to add your first engine."))
            : state.listError);
    previous_->setEnabled(state.ready && !state.listing && state.offset > 0);
    next_->setEnabled(state.ready && !state.listing && state.hasNext);
    refresh_->setEnabled(state.ready && !state.listing);
    profiles_->setRowCount(static_cast<int>(state.profiles.size()));
    int row = 0;
    for (const auto& profile : state.profiles) {
        const QStringList values{
            clipped(profile.displayName), clipped(profile.engine.executablePath),
            profile.isDefault ? tr("Yes") : QString(),
            profile.engine.lastValidatedAt.toUTC().toString(Qt::ISODateWithMs)};
        for (int column = 0; column < values.size(); ++column) {
            profiles_->setItem(row, column, new QTableWidgetItem(values[column]));
        }
        ++row;
    }
}
} // namespace chess_review::ui
