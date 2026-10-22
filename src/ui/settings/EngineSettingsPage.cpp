#include "ui/settings/EngineSettingsPage.h"
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStringView>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace chess_review::ui {
namespace {
QLabel* plainLabel(QWidget* parent, const QString& text = {}) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    // Unbroken engine text must not impose a minimum page width. Full metadata
    // remains available in the scrollable, copyable detail panes.
    auto policy = label->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Ignored);
    label->setSizePolicy(policy);
    return label;
}
QString presented(const QString& text) {
    QString result;
    for (qsizetype index = 0; index < text.size();) {
        const auto character = text[index];
        const bool pair = character.isHighSurrogate() && index + 1 < text.size() &&
                          text[index + 1].isLowSurrogate();
        const auto code = pair ? QChar::surrogateToUcs4(character, text[index + 1])
                               : static_cast<char32_t>(character.unicode());
        const auto category = QChar::category(code);
        if (category == QChar::Other_Control || category == QChar::Other_Format ||
            category == QChar::Other_Surrogate || category == QChar::Separator_Line ||
            category == QChar::Separator_Paragraph) {
            result += (pair ? QStringLiteral("\\U%1") : QStringLiteral("\\u%1"))
                          .arg(static_cast<uint>(code), pair ? 8 : 4, 16, QLatin1Char('0'));
        } else {
            result += QStringView(text).sliced(index, pair ? 2 : 1);
        }
        index += pair ? 2 : 1;
    }
    return result;
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
    profiles_->setSelectionMode(QAbstractItemView::SingleSelection);
    profiles_->setWordWrap(false);
    profiles_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    profiles_->horizontalHeader()->setStretchLastSection(true);
    profiles_->setColumnWidth(0, 200);
    profiles_->setColumnWidth(1, 300);
    profiles_->setMinimumHeight(150);
    layout->addWidget(profiles_);
    details_ = new QPlainTextEdit(this);
    details_->setObjectName(QStringLiteral("engineProfileDetails"));
    details_->setAccessibleName(tr("Selected engine details"));
    details_->setReadOnly(true);
    details_->setPlaceholderText(
        tr("Select an engine to inspect and copy its full name and path."));
    detailsTabs_ = new QTabWidget(this);
    detailsTabs_->setMaximumHeight(170);
    detailsTabs_->addTab(details_, tr("Saved profile"));
    layout->addWidget(detailsTabs_);
    auto* connectionActions = new QHBoxLayout;
    connect_ = new QPushButton(tr("&Connect"), this);
    connect_->setObjectName(QStringLiteral("connectEngine"));
    disconnect_ = new QPushButton(tr("&Disconnect"), this);
    disconnect_->setObjectName(QStringLiteral("disconnectEngine"));
    connectionActions->addWidget(connect_);
    connectionActions->addWidget(disconnect_);
    connectionActions->addStretch();
    layout->addLayout(connectionActions);
    runtimeStatus_ = plainLabel(this);
    runtimeStatus_->setObjectName(QStringLiteral("engineRuntimeStatus"));
    layout->addWidget(runtimeStatus_);
    auto* liveDetails = new QWidget(detailsTabs_);
    auto* liveLayout = new QVBoxLayout(liveDetails);
    options_ = new QComboBox(this);
    options_->setObjectName(QStringLiteral("engineRuntimeOptions"));
    options_->setAccessibleName(tr("Advertised UCI options (read only)"));
    options_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    options_->setMinimumContentsLength(20);
    liveLayout->addWidget(options_);
    optionDetails_ = new QPlainTextEdit(this);
    optionDetails_->setObjectName(QStringLiteral("engineRuntimeOptionDetails"));
    optionDetails_->setAccessibleName(tr("Full UCI option declaration (read only)"));
    optionDetails_->setReadOnly(true);
    liveLayout->addWidget(optionDetails_);
    detailsTabs_->addTab(liveDetails, tr("Live options (read only)"));
    connect(options_, &QComboBox::currentIndexChanged, this, &EngineSettingsPage::updateOption);
    connect(connect_, &QPushButton::clicked, this, [this] {
        if (const auto* profile = selectedProfile()) {
            controller_.connectEngine(profile->id);
        }
    });
    connect(disconnect_, &QPushButton::clicked, &controller_,
            &app::EngineSettingsController::disconnectEngine);
    connect(profiles_, &QTableWidget::itemSelectionChanged, this,
            &EngineSettingsPage::updateDetails);
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
    path_ = new QPlainTextEdit(this);
    path_->setObjectName(QStringLiteral("engineSelectedPath"));
    path_->setAccessibleName(tr("Resolved executable path"));
    path_->setReadOnly(true);
    path_->setMaximumHeight(60);
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
        if (picking_) {
            return;
        }
        // Nonblocking native picker; cancellation never launches software.
        if (!picker_) {
            picker_ = new QFileDialog(this, tr("Choose UCI engine executable"));
            picker_->setFileMode(QFileDialog::ExistingFile);
            connect(picker_, &QFileDialog::fileSelected, &controller_,
                    &app::EngineSettingsController::selectExecutable);
            connect(picker_, &QFileDialog::finished, this, [this] {
                picking_ = false;
                updateState();
            });
        }
        picking_ = true;
        picker_->open();
        updateState();
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
                      state.registration == EngineRegistrationState::Closing ||
                      engine::runtimeActive(state.runtime.phase);
    browse_->setEnabled(state.ready && !busy && !picking_ &&
                        state.registration != EngineRegistrationState::Inspecting);
    if (!state.ready && picking_) {
        picker_->reject();
    }
    add_->setEnabled(state.ready && !busy &&
                     state.registration == EngineRegistrationState::Selected);
    cancel_->setEnabled(state.ready && state.registration != EngineRegistrationState::Idle &&
                        state.registration != EngineRegistrationState::Saving);
    const auto path = presented(state.selectedPath);
    if (path_->toPlainText() != path) {
        path_->setPlainText(path);
    }
    path_->setVisible(!path.isEmpty());
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
    updateRuntime();
    if (profilesRevision_ == state.profilesRevision) {
        return;
    }
    profilesRevision_ = state.profilesRevision;
    const auto* selectedValue = selectedProfile();
    const auto selected = selectedValue ? std::optional(selectedValue->id) : std::nullopt;
    const QSignalBlocker blocker(profiles_);
    profiles_->clearContents();
    profiles_->clearSelection();
    profiles_->setCurrentCell(-1, -1);
    profiles_->setRowCount(static_cast<int>(state.profiles.size()));
    int row = 0;
    for (const auto& profile : state.profiles) {
        const QStringList values{
            clipped(presented(profile.displayName)),
            clipped(presented(profile.engine.executablePath)),
            profile.isDefault ? tr("Yes") : QString(),
            profile.engine.lastValidatedAt.toUTC().toString(Qt::ISODateWithMs)};
        for (int column = 0; column < values.size(); ++column) {
            profiles_->setItem(row, column, new QTableWidgetItem(values[column]));
        }
        profiles_->item(row, 0)->setData(Qt::UserRole, static_cast<qlonglong>(profile.id));
        if (selected && *selected == profile.id) {
            profiles_->selectRow(row);
        }
        ++row;
    }
    updateDetails();
}
void EngineSettingsPage::updateDetails() {
    updateRuntime();
    const auto* profile = selectedProfile();
    if (!profile) {
        details_->clear();
        return;
    }
    details_->setPlainText(
        tr("Engine: %1\nExecutable: %2")
            .arg(presented(profile->displayName), presented(profile->engine.executablePath)));
}
const engine::EngineProfile* EngineSettingsPage::selectedProfile() const {
    const auto rows = profiles_->selectionModel()->selectedRows();
    if (rows.size() != 1) {
        return nullptr;
    }
    const auto* item = profiles_->item(rows.front().row(), 0);
    if (!item) {
        return nullptr;
    }
    const auto id = item->data(Qt::UserRole).toLongLong();
    const auto& profiles = controller_.state().profiles;
    const auto found =
        std::find_if(profiles.begin(), profiles.end(),
                     [id](const engine::EngineProfile& value) { return value.id == id; });
    return found == profiles.end() ? nullptr : &*found;
}
void EngineSettingsPage::updateRuntime() {
    const auto& state = controller_.state();
    using engine::RuntimePhase;
    const auto phase = state.runtime.phase;
    connect_->setEnabled(state.ready && !engine::runtimeActive(phase) &&
                         state.registration != app::EngineRegistrationState::Inspecting &&
                         state.registration != app::EngineRegistrationState::Validating &&
                         state.registration != app::EngineRegistrationState::Saving &&
                         selectedProfile() != nullptr);
    disconnect_->setEnabled(state.ready && engine::runtimeActive(phase) &&
                            phase != RuntimePhase::Disconnecting);
    QString status;
    switch (phase) {
    case RuntimePhase::Disconnected:
        status = tr("Disconnected");
        break;
    case RuntimePhase::CheckingExecutable:
        status = tr("Checking executable…");
        break;
    case RuntimePhase::Starting:
        status = tr("Starting…");
        break;
    case RuntimePhase::Handshaking:
        status = tr("Handshaking…");
        break;
    case RuntimePhase::Ready:
        status = tr("Ready — handshake completed; no analysis requested.");
        break;
    case RuntimePhase::Disconnecting:
        status = tr("Disconnecting…");
        break;
    case RuntimePhase::Failed:
        status = tr("Failed");
        break;
    }
    if (!state.connectedDisplayName.isEmpty()) {
        status.prepend(presented(clipped(state.connectedDisplayName)) + QStringLiteral(": "));
    }
    if (state.runtime.error) {
        status += QStringLiteral("\n") + state.runtime.error->message;
    }
    if (state.runtime.discovery) {
        const auto& value = *state.runtime.discovery;
        const auto yesNo = [](bool supported) { return supported ? tr("Yes") : tr("No"); };
        status += tr("\nAdvertised support — MultiPV: %1; WDL: %2; Threads: %3; Hash: %4")
                      .arg(yesNo(value.capabilities.supportsMultiPv),
                           yesNo(value.capabilities.supportsWdl),
                           yesNo(value.capabilities.supportsThreads),
                           yesNo(value.capabilities.supportsHash));
    }
    runtimeStatus_->setText(status);
    detailsTabs_->setTabEnabled(1, state.runtime.discovery.has_value());
    if (discovery_ == state.runtime.discovery) {
        return;
    }
    discovery_ = state.runtime.discovery;
    detailsTabs_->setCurrentIndex(discovery_ ? 1 : 0);
    const QSignalBlocker blocker(options_);
    options_->clear();
    if (discovery_) {
        for (const auto& option : discovery_->capabilities.options) {
            options_->addItem(clipped(presented(option.name)));
        }
    }
    options_->setEnabled(options_->count() > 0);
    options_->setVisible(options_->count() > 0);
    updateOption();
}
void EngineSettingsPage::updateOption() {
    const auto index = options_->currentIndex();
    if (!discovery_) {
        optionDetails_->setPlainText(tr("Connect an engine to inspect its advertised options."));
        return;
    }
    const auto identity =
        tr("Reported name: %1\nAuthor: %2\nExecutable: %3")
            .arg(presented(discovery_->reportedName),
                 presented(discovery_->reportedAuthor.value_or(tr("Not provided"))),
                 presented(discovery_->executablePath));
    if (index < 0) {
        optionDetails_->setPlainText(identity + tr("\nNo UCI options advertised."));
        return;
    }
    const auto& value = discovery_->capabilities.options[index];
    QString type;
    switch (value.type) {
    case engine::OptionType::Check:
        type = QStringLiteral("check");
        break;
    case engine::OptionType::Spin:
        type = QStringLiteral("spin");
        break;
    case engine::OptionType::Combo:
        type = QStringLiteral("combo");
        break;
    case engine::OptionType::Button:
        type = QStringLiteral("button");
        break;
    case engine::OptionType::String:
        type = QStringLiteral("string");
        break;
    }
    QString text = identity + QStringLiteral("\n\n") +
                   tr("Name: %1\nType: %2\nDefault: %3")
                       .arg(presented(value.name), type, presented(value.defaultValue));
    if (value.minimum && value.maximum) {
        text += tr("\nMinimum: %1\nMaximum: %2").arg(*value.minimum).arg(*value.maximum);
    }
    if (!value.choices.isEmpty()) {
        QStringList choices;
        for (const auto& choice : value.choices) {
            choices.append(presented(choice));
        }
        text += tr("\nChoices:\n%1").arg(choices.join(u'\n'));
    }
    optionDetails_->setPlainText(text);
}
} // namespace chess_review::ui
