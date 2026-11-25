#include "ui/settings/EngineSettingsPage.h"
#include "ui/EngineText.h"
#include "ui/Theme.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
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
} // namespace
EngineSettingsPage::EngineSettingsPage(app::EngineSettingsController& controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    setObjectName(QStringLiteral("settingsPage"));
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(0, 0, 0, 0);
    page->setSpacing(10);
    auto* headerRow = new QHBoxLayout;
    auto* heading = plainLabel(this, tr("Settings"));
    setTextRole(*heading, "title");
    headerRow->addWidget(heading, 1);
    auto* appearanceLabel = new QLabel(tr("&Appearance"), this);
    setTextRole(*appearanceLabel, "secondary");
    appearance_ = new QComboBox(this);
    appearance_->setObjectName(QStringLiteral("appearanceMode"));
    appearance_->addItem(tr("System"), static_cast<int>(Qt::ColorScheme::Unknown));
    appearance_->addItem(tr("Light"), static_cast<int>(Qt::ColorScheme::Light));
    appearance_->addItem(tr("Dark"), static_cast<int>(Qt::ColorScheme::Dark));
    appearanceLabel->setBuddy(appearance_);
    headerRow->addWidget(appearanceLabel);
    headerRow->addWidget(appearance_);
    page->addLayout(headerRow);
    connect(appearance_, &QComboBox::currentIndexChanged, this, [this] {
        emit colorSchemeChanged(static_cast<Qt::ColorScheme>(appearance_->currentData().toInt()));
    });
    sections_ = new QTabWidget(this);
    sections_->setObjectName(QStringLiteral("settingsSections"));
    page->addWidget(sections_);
    auto* engines = new QScrollArea(sections_);
    engines->setWidgetResizable(true);
    engines->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(engines);
    content->setMaximumWidth(920);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);
    engines->setWidget(content);
    sections_->addTab(engines, tr("&Engines"));
    auto* engineTopRow = new QHBoxLayout;
    engineTopRow->setSpacing(12);
    auto* engineHint =
        plainLabel(content, tr("Add a trusted UCI engine installed on your computer. Its "
                               "executable stays where you installed it."));
    setTextRole(*engineHint, "secondary");
    engineTopRow->addWidget(engineHint, 1);
    detect_ = new QPushButton(tr("&Detect installed"), content);
    detect_->setObjectName(QStringLiteral("detectEngines"));
    setIcon(*detect_, QStringLiteral("search"));
    detect_->setToolTip(
        tr("Search standard folders for installed UCI chess engines without running them."));
    engineTopRow->addWidget(detect_, 0, Qt::AlignVCenter);
    browse_ = new QPushButton(tr("&Choose executable…"), content);
    browse_->setObjectName(QStringLiteral("chooseEngine"));
    setIcon(*browse_, QStringLiteral("plus"));
    browse_->setToolTip(tr("Connect or add one UCI engine at a time. Disconnect the current engine "
                           "before registering another."));
    engineTopRow->addWidget(browse_, 0, Qt::AlignVCenter);
    layout->addLayout(engineTopRow);
    detectBox_ = new QWidget(content);
    detectBox_->setObjectName(QStringLiteral("engineDetectionBox"));
    setSubtleSurface(*detectBox_);
    auto* detectLayout = new QVBoxLayout(detectBox_);
    detectLayout->setContentsMargins(12, 10, 12, 10);
    detectLayout->setSpacing(8);
    detectMessage_ = plainLabel(detectBox_);
    detectMessage_->setObjectName(QStringLiteral("engineDetectMessage"));
    detectMessage_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detectLayout->addWidget(detectMessage_);
    auto* detectRow = new QHBoxLayout;
    detectRow->setSpacing(8);
    detected_ = new QComboBox(detectBox_);
    detected_->setObjectName(QStringLiteral("detectedEngines"));
    detected_->setAccessibleName(tr("Detected UCI engines"));
    detected_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    detected_->setMinimumContentsLength(24);
    selectDetected_ = new QPushButton(tr("&Select"), detectBox_);
    selectDetected_->setObjectName(QStringLiteral("selectDetectedEngine"));
    setIcon(*selectDetected_, QStringLiteral("plus"));
    detectRow->addWidget(detected_, 1);
    detectRow->addWidget(selectDetected_);
    detectLayout->addLayout(detectRow);
    detectBox_->hide();
    layout->addWidget(detectBox_);
    profiles_ = new QTableWidget(0, 4, content);
    profiles_->setObjectName(QStringLiteral("engineProfiles"));
    profiles_->setAccessibleName(tr("Registered engines"));
    profiles_->setHorizontalHeaderLabels(
        {tr("Engine"), tr("Executable path"), tr("Default"), tr("Last validated (UTC)")});
    profiles_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    profiles_->setSelectionBehavior(QAbstractItemView::SelectRows);
    profiles_->setSelectionMode(QAbstractItemView::SingleSelection);
    profiles_->setWordWrap(false);
    profiles_->setShowGrid(false);
    profiles_->setAlternatingRowColors(true);
    profiles_->verticalHeader()->hide();
    profiles_->verticalHeader()->setDefaultSectionSize(36);
    profiles_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    profiles_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    profiles_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    profiles_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    profiles_->setColumnWidth(1, 300);
    profiles_->setMinimumHeight(126);
    profiles_->setMaximumHeight(230);
    layout->addWidget(profiles_);
    details_ = new QPlainTextEdit(this);
    details_->setObjectName(QStringLiteral("engineProfileDetails"));
    details_->setAccessibleName(tr("Selected engine details"));
    details_->setReadOnly(true);
    details_->setPlaceholderText(
        tr("Select an engine to inspect and copy its full name and path."));
    detailsTabs_ = new QTabWidget(this);
    detailsTabs_->addTab(details_, tr("Saved profile"));
    auto* technical = new QDialog(this);
    technical->setObjectName(QStringLiteral("engineTechnicalInspector"));
    technical->setWindowTitle(tr("Engine details"));
    technical->resize(640, 480);
    auto* technicalLayout = new QVBoxLayout(technical);
    technicalLayout->addWidget(detailsTabs_);
    inspector_ = new QPushButton(tr("Engine details…"), this);
    inspector_->setObjectName(QStringLiteral("engineInspectorButton"));
    setQuietAction(*inspector_);
    setIcon(*inspector_, QStringLiteral("info"));

    connect(inspector_, &QPushButton::clicked, technical, &QDialog::show);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, technical);
    technicalLayout->addWidget(close);
    connect(close, &QDialogButtonBox::rejected, technical, &QDialog::reject);
    auto* connectionActions = new QHBoxLayout;
    connect_ = new QPushButton(tr("&Connect"), this);
    connect_->setObjectName(QStringLiteral("connectEngine"));
    setIcon(*connect_, QStringLiteral("play"));
    disconnect_ = new QPushButton(tr("&Disconnect"), this);
    disconnect_->setObjectName(QStringLiteral("disconnectEngine"));
    setIcon(*disconnect_, QStringLiteral("stop"));
    connectionActions->addWidget(connect_);
    connectionActions->addWidget(disconnect_);
    test_ = new QPushButton(tr("&Test analysis"), this);
    test_->setObjectName(QStringLiteral("testEngine"));
    test_->setToolTip(tr("Run a short search of the starting position with the connected engine."));
    setIcon(*test_, QStringLiteral("analyze"));
    connectionActions->addWidget(test_);
    connectionActions->addStretch();
    layout->addLayout(connectionActions);
    runtimeStatus_ = plainLabel(this);
    runtimeStatus_->setObjectName(QStringLiteral("engineRuntimeStatus"));
    runtimeStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    setTextRole(*runtimeStatus_, "secondary");
    layout->addWidget(runtimeStatus_);
    testMessage_ = plainLabel(this);
    testMessage_->setObjectName(QStringLiteral("engineTestMessage"));
    testMessage_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(testMessage_);
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
    connect(test_, &QPushButton::clicked, this, [this] {
        if (const auto* profile = selectedProfile()) {
            controller_.testEngine(profile->id);
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
    layout->insertLayout(3, pagination);
    auto* profileActions = new QHBoxLayout;
    configure_ = new QPushButton(tr("&Options…"), this);
    configure_->setObjectName(QStringLiteral("editEngineOptions"));
    setIcon(*configure_, QStringLiteral("settings"));
    rename_ = new QPushButton(tr("&Rename…"), this);
    rename_->setObjectName(QStringLiteral("renameEngine"));
    setIcon(*rename_, QStringLiteral("note"));
    makeDefault_ = new QPushButton(tr("Set as &default"), this);
    makeDefault_->setObjectName(QStringLiteral("defaultEngine"));
    locate_ = new QPushButton(tr("&Locate executable…"), this);
    locate_->setObjectName(QStringLiteral("locateEngine"));
    locate_->setToolTip(tr("Select the moved executable file for this missing engine profile."));
    setIcon(*locate_, QStringLiteral("search"));
    remove_ = new QPushButton(tr("Remo&ve…"), this);
    remove_->setObjectName(QStringLiteral("removeEngine"));
    setQuietAction(*remove_);
    setIcon(*remove_, QStringLiteral("trash"));
    profileActions->addWidget(configure_);
    profileActions->addWidget(rename_);
    profileActions->addWidget(makeDefault_);
    profileActions->addWidget(locate_);
    profileActions->addWidget(remove_);
    profileActions->addWidget(inspector_);
    profileActions->addStretch();
    layout->insertLayout(4, profileActions);
    profileMessage_ = plainLabel(this);
    profileMessage_->setObjectName(QStringLiteral("engineProfileMessage"));
    profileMessage_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->insertWidget(5, profileMessage_);
    connect(configure_, &QPushButton::clicked, this, &EngineSettingsPage::showOptionEditor);
    connect(makeDefault_, &QPushButton::clicked, this, [this] {
        if (const auto* profile = selectedProfile()) {
            controller_.editProfile(profile->id, persistence::MakeDefaultEngine{});
        }
    });
    connect(rename_, &QPushButton::clicked, this, [this] {
        const auto* profile = selectedProfile();
        if (!profile) {
            return;
        }
        auto* dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(tr("Rename engine"));
        auto* form = new QFormLayout(dialog);
        auto* name = new QLineEdit(dialog);
        name->setMaxLength(120);
        name->setText(presentedEngineText(profile->displayName));
        name->selectAll();
        form->addRow(tr("&Display name"), name);
        auto* actions =
            new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
        auto* save = actions->button(QDialogButtonBox::Save);
        save->setEnabled(!name->text().trimmed().isEmpty());
        connect(name, &QLineEdit::textChanged, save,
                [save](const QString& text) { save->setEnabled(!text.trimmed().isEmpty()); });
        form->addRow(actions);
        connect(actions, &QDialogButtonBox::accepted, dialog,
                [this, dialog, name, id = profile->id] {
                    controller_.editProfile(id, persistence::RenameEngine{name->text()});
                    dialog->accept();
                });
        connect(actions, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        dialog->open();
    });
    connect(remove_, &QPushButton::clicked, this, [this] {
        const auto* profile = selectedProfile();
        if (!profile) {
            return;
        }
        auto* dialog = new QMessageBox(
            QMessageBox::Question, tr("Remove engine"),
            tr("Remove this engine from Settings? Its executable, games and saved reviews "
               "will remain on disk."),
            QMessageBox::Cancel, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setDefaultButton(QMessageBox::Cancel);
        auto* remove = dialog->addButton(tr("Remove engine"), QMessageBox::DestructiveRole);
        connect(remove, &QPushButton::clicked, dialog, [this, id = profile->id] {
            controller_.editProfile(id, persistence::RemoveEngine{});
        });
        dialog->open();
    });
    listError_ = plainLabel(this);
    setTextRole(*listError_, "secondary");
    layout->addWidget(listError_);
    auto* registrationBox = new QWidget(content);
    setSubtleSurface(*registrationBox);
    auto* regLayout = new QVBoxLayout(registrationBox);
    regLayout->setContentsMargins(12, 10, 12, 10);
    regLayout->setSpacing(8);
    path_ = new QPlainTextEdit(registrationBox);
    path_->setObjectName(QStringLiteral("engineSelectedPath"));
    path_->setAccessibleName(tr("Resolved executable path"));
    path_->setReadOnly(true);
    path_->setFrameShape(QFrame::NoFrame);
    setFlatSurface(*path_);
    path_->setMaximumHeight(52);
    regLayout->addWidget(path_);
    message_ = plainLabel(registrationBox);
    message_->setObjectName(QStringLiteral("engineRegistrationMessage"));
    message_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    regLayout->addWidget(message_);
    auto* actions = new QHBoxLayout;
    add_ = new QPushButton(tr("&Validate and add"), registrationBox);
    add_->setObjectName(QStringLiteral("registerEngine"));
    cancel_ = new QPushButton(tr("Cancel"), registrationBox);
    cancel_->setObjectName(QStringLiteral("cancelEngineRegistration"));
    setPrimaryAction(*add_);
    setIcon(*add_, QStringLiteral("plus"));
    setQuietAction(*cancel_);
    actions->addWidget(add_);
    actions->addWidget(cancel_);
    actions->addStretch();
    regLayout->addLayout(actions);
    layout->addWidget(registrationBox);
    layout->addStretch();
    connect(detect_, &QPushButton::clicked, &controller_,
            &app::EngineSettingsController::detectInstalledEngines);
    connect(selectDetected_, &QPushButton::clicked, this, [this] {
        const auto selectedPath = detected_->currentData().toString();
        if (!selectedPath.isEmpty()) {
            controller_.selectExecutable(selectedPath);
        }
    });
    connect(locate_, &QPushButton::clicked, this, [this] {
        const auto* profile = selectedProfile();
        if (locating_ || !profile || !controller_.canEditProfile()) {
            return;
        }
        locatingProfileId_ = profile->id;
        if (!locatePicker_) {
            locatePicker_ = new QFileDialog(this, tr("Locate moved UCI engine executable"));
            locatePicker_->setFileMode(QFileDialog::ExistingFile);
            connect(locatePicker_, &QFileDialog::fileSelected, this, [this](const QString& file) {
                if (locatingProfileId_ > 0) {
                    controller_.relocateProfile(locatingProfileId_, file);
                }
            });
            connect(locatePicker_, &QFileDialog::finished, this, [this] {
                locating_ = false;
                updateState();
            });
        }
        locating_ = true;
        locatePicker_->open();
        updateState();
    });
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
    connect(&controller_, &app::EngineSettingsController::activityChanged, this,
            &EngineSettingsPage::updateState);
    setVerbose(false);
    updateState();
}
void EngineSettingsPage::setVerbose(bool verbose) {
    verbose_ = verbose;

    profiles_->setColumnHidden(1, !verbose);
    profiles_->setColumnHidden(3, !verbose);
    updateRuntime();
}
void EngineSettingsPage::updateState() {
    const auto& state = controller_.state();
    using app::EngineRegistrationState;
    const bool busy = state.editing || state.registration == EngineRegistrationState::Validating ||
                      state.registration == EngineRegistrationState::Saving ||
                      state.registration == EngineRegistrationState::Closing ||
                      engine::runtimeActive(state.runtime.phase);
    detect_->setEnabled(state.ready && !state.detecting && !busy);
    browse_->setEnabled(state.ready && !busy && !picking_ && !locating_ &&
                        state.registration != EngineRegistrationState::Inspecting);
    if (!state.ready && picking_) {
        picker_->reject();
    }
    if (!state.ready && locating_ && locatePicker_) {
        locatePicker_->reject();
    }
    detectMessage_->setText(state.detectMessage);
    detectMessage_->setVisible(!state.detectMessage.isEmpty());
    if (shownDetected_ != state.detectedEngines) {
        shownDetected_ = state.detectedEngines;
        const QSignalBlocker detectBlock(detected_);
        detected_->clear();
        for (const auto& candidate : shownDetected_) {
            detected_->addItem(
                tr("%1 (%2)").arg(clippedEngineText(presentedEngineText(candidate.name)),
                                  clippedEngineText(presentedEngineText(candidate.path))),
                candidate.path);
        }
    }
    const bool hasDetected = !state.detectedEngines.isEmpty();
    detected_->setVisible(hasDetected);
    selectDetected_->setVisible(hasDetected);
    selectDetected_->setEnabled(state.ready && hasDetected && !busy && !picking_ && !locating_ &&
                                state.registration != EngineRegistrationState::Inspecting);
    detectBox_->setVisible(!state.detectMessage.isEmpty() || hasDetected);
    const bool adding = state.registration != EngineRegistrationState::Idle &&
                        state.registration != EngineRegistrationState::Closing;
    add_->setText(state.relocatingProfileId ? tr("&Validate and locate") : tr("&Validate and add"));
    add_->setVisible(adding);
    cancel_->setVisible(adding);
    add_->setEnabled(state.ready && !busy &&
                     state.registration == EngineRegistrationState::Selected);
    cancel_->setEnabled(state.ready && state.registration != EngineRegistrationState::Idle &&
                        state.registration != EngineRegistrationState::Saving);
    const auto path = presentedEngineText(state.selectedPath);
    if (path_->toPlainText() != path) {
        path_->setPlainText(path);
    }
    path_->setVisible(!path.isEmpty());
    message_->setText(state.message);
    message_->setVisible(!state.message.isEmpty());
    path_->parentWidget()->setVisible(adding || !path.isEmpty() || !state.message.isEmpty());
    profileMessage_->setText(state.profileMessage);
    profileMessage_->setVisible(!state.profileMessage.isEmpty());
    testMessage_->setText(state.testMessage);
    testMessage_->setVisible(!state.testMessage.isEmpty());
    listError_->setText(
        state.listError.isEmpty() && state.profiles.empty()
            ? (state.listing
                   ? tr("Loading registered engines…")
                   : tr("No engines registered. Choose an executable to add your first engine."))
            : state.listError);
    listError_->setVisible(!listError_->text().isEmpty());
    previous_->setEnabled(state.ready && !state.listing && state.offset > 0);
    next_->setEnabled(state.ready && !state.listing && state.hasNext);
    previous_->setVisible(state.offset > 0 || state.hasNext);
    next_->setVisible(state.offset > 0 || state.hasNext);
    refresh_->setEnabled(state.ready && !state.listing);
    refresh_->setVisible(!state.listError.isEmpty());
    updateRuntime();
    if (profilesRevision_ == state.profilesRevision) {
        updateMissingIndicators();
        return;
    }
    profilesRevision_ = state.profilesRevision;
    shownMissing_ = state.missingProfiles;
    const auto* selectedValue = selectedProfile();
    const auto selected = selectedValue ? std::optional(selectedValue->id) : std::nullopt;
    const QSignalBlocker blocker(profiles_);
    profiles_->clearContents();
    profiles_->clearSelection();
    profiles_->setCurrentCell(-1, -1);
    profiles_->setRowCount(static_cast<int>(state.profiles.size()));
    int row = 0;
    for (const auto& profile : state.profiles) {
        const bool missing = state.missingProfiles.contains(profile.id);
        const QStringList values{
            clippedEngineText(presentedEngineText(profile.displayName)) +
                (missing ? tr(" (Missing)") : QString{}),
            clippedEngineText(presentedEngineText(profile.engine.executablePath)),
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
void EngineSettingsPage::updateMissingIndicators() {
    const auto& state = controller_.state();
    if (shownMissing_ == state.missingProfiles) {
        return;
    }
    shownMissing_ = state.missingProfiles;
    const QSignalBlocker blocker(profiles_);
    for (int row = 0; row < profiles_->rowCount(); ++row) {
        auto* item = profiles_->item(row, 0);
        if (!item) {
            continue;
        }
        const auto id = item->data(Qt::UserRole).toLongLong();
        const auto found = std::ranges::find(state.profiles, id, &engine::EngineProfile::id);
        if (found == state.profiles.end()) {
            continue;
        }
        const bool missing = state.missingProfiles.contains(id);
        const auto text = clippedEngineText(presentedEngineText(found->displayName)) +
                          (missing ? tr(" (Missing)") : QString{});
        if (item->text() != text) {
            item->setText(text);
        }
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
    const auto& engine = profile->engine;
    auto text = tr("Profile: %1\nReported name: %2\nExecutable: %3")
                    .arg(presentedEngineText(profile->displayName),
                         presentedEngineText(engine.reportedName),
                         presentedEngineText(engine.executablePath));
    if (controller_.state().missingProfiles.contains(profile->id)) {
        text += tr("\nStatus: Executable missing at saved path");
    }
    if (engine.reportedAuthor) {
        text += tr("\nAuthor: %1").arg(presentedEngineText(*engine.reportedAuthor));
    }
    if (engine.detectedVersion) {
        text += tr("\nVersion: %1").arg(presentedEngineText(*engine.detectedVersion));
    }
    details_->setPlainText(text);
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
    const auto* profile = selectedProfile();
    const bool canEdit = profile && controller_.canEditProfile();
    const bool hasOptions = profile && std::ranges::any_of(profile->engine.capabilities.options,
                                                           engine::editableOption);
    configure_->setEnabled(canEdit && hasOptions);
    configure_->setToolTip(engine::runtimeActive(phase)
                               ? tr("Disconnect the engine before changing its options.")
                               : tr("Edit supported UCI options, including threads and hash."));
    rename_->setEnabled(canEdit);
    makeDefault_->setEnabled(canEdit && !profile->isDefault);
    const bool missingSelected = profile && state.missingProfiles.contains(profile->id);
    locate_->setVisible(missingSelected);
    locate_->setEnabled(canEdit && missingSelected && !locating_ && !picking_);
    remove_->setEnabled(canEdit);
    inspector_->setEnabled(profile || state.runtime.discovery);
    test_->setEnabled(profile && profile->id == state.runtime.profileId &&
                      phase == RuntimePhase::Ready && !state.analyzing && !state.testing);
    connect_->setEnabled(
        state.ready && !state.editing && !state.listing && !engine::runtimeActive(phase) &&
        state.registration != app::EngineRegistrationState::Inspecting &&
        state.registration != app::EngineRegistrationState::Validating &&
        state.registration != app::EngineRegistrationState::Saving && profile != nullptr);
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
        status = tr("Connecting…");
        break;
    case RuntimePhase::Ready:
        status = state.analyzing ? tr("Analyzing") : tr("Ready");
        break;
    case RuntimePhase::Disconnecting:
        status = tr("Disconnecting…");
        break;
    case RuntimePhase::Failed:
        status = tr("Failed");
        break;
    }
    if (!state.connectedDisplayName.isEmpty()) {
        status.prepend(presentedEngineText(clippedEngineText(state.connectedDisplayName)) +
                       QStringLiteral(": "));
    }
    if (state.runtime.error) {
        status += QStringLiteral("\n") + state.runtime.error->message;
    }
    if (state.runtime.phase == engine::RuntimePhase::Ready && state.runtime.workers > 1) {
        status += tr("\n%1 engine workers connected").arg(state.runtime.workers);
    }
    if (verbose_ && state.runtime.discovery) {
        const auto& value = *state.runtime.discovery;
        const auto yesNo = [](bool supported) { return supported ? tr("Yes") : tr("No"); };
        status += tr("\nCapabilities: MultiPV %1 · WDL %2 · Threads %3 · Hash %4")
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
            options_->addItem(clippedEngineText(presentedEngineText(option.name)));
        }
    }
    options_->setEnabled(options_->count() > 0);
    options_->setVisible(options_->count() > 0);
    updateOption();
}
void EngineSettingsPage::setColorScheme(Qt::ColorScheme scheme) {
    const QSignalBlocker blocker(appearance_);
    appearance_->setCurrentIndex(appearance_->findData(static_cast<int>(scheme)));
}
void EngineSettingsPage::showEngines() { sections_->setCurrentIndex(0); }
void EngineSettingsPage::showOptionEditor() {
    const auto* profile = selectedProfile();
    if (!profile || !controller_.canEditProfile()) {
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Engine options"));
    dialog->resize(505, 0);
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(plainLabel(
        dialog, tr("Settings apply when this engine next connects. One engine worker runs at a "
                   "time. More threads use more CPU, and a larger hash uses more memory.")));
    auto* form = new QFormLayout;
    layout->addLayout(form);
    auto* names = new QComboBox(dialog);
    names->setObjectName(QStringLiteral("engineOptionName"));
    names->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    names->setMinimumContentsLength(16);
    form->addRow(tr("&Option"), names);
    const auto options = profile->engine.capabilities.options;
    QList<qsizetype> indices;
    for (qsizetype i = 0; i < options.size(); ++i) {
        if (!engine::editableOption(options[i])) {
            continue;
        }
        const auto name = options[i].name.toCaseFolded();
        if (name == QStringLiteral("threads") || name == QStringLiteral("hash")) {
            indices.prepend(i);
        } else {
            indices.append(i);
        }
    }
    for (const auto index : indices) {
        const auto& name = options[index].name;
        const auto folded = name.toCaseFolded();
        names->addItem(folded == QStringLiteral("threads") ? tr("CPU threads")
                       : folded == QStringLiteral("hash")
                           ? tr("Hash memory (MiB)")
                           : clippedEngineText(presentedEngineText(name)),
                       index);
    }
    auto* scalar = new QLineEdit(dialog);
    scalar->setObjectName(QStringLiteral("engineOptionValue"));
    scalar->setMaxLength(1024);
    auto* choices = new QComboBox(dialog);
    choices->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    choices->setMinimumContentsLength(16);
    form->addRow(tr("&Value"), scalar);
    form->addRow(tr("&Choice"), choices);
    auto* limits = plainLabel(dialog);
    layout->addWidget(limits);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
    auto* reset = buttons->addButton(tr("Restore default"), QDialogButtonBox::ResetRole);
    auto* save = buttons->button(QDialogButtonBox::Save);
    layout->addWidget(buttons);
    const auto current = [names, options]() -> const engine::UciOption& {
        return options[names->currentData().toLongLong()];
    };
    const auto value = [scalar, choices] {
        return choices->isHidden() ? scalar->text() : choices->currentData().toString();
    };
    const auto validate = [current, value, save] {
        save->setEnabled(engine::normalizedOverride(current(), value()).has_value());
    };
    const auto update = [=, overrides = profile->optionOverrides] {
        const auto& option = current();
        const auto name = option.name.toCaseFolded();
        auto defaultValue = option.defaultValue;
        if (name == QStringLiteral("threads")) {
            defaultValue = QStringLiteral("1");
        }
        const auto saved = overrides.value(name, defaultValue);
        reset->setEnabled(overrides.contains(name));
        const bool hasChoices =
            option.type == engine::OptionType::Check || option.type == engine::OptionType::Combo;
        form->setRowVisible(scalar, !hasChoices);
        form->setRowVisible(choices, hasChoices);
        const QSignalBlocker blockChoices(choices);
        const QSignalBlocker blockScalar(scalar);
        choices->clear();
        if (option.type == engine::OptionType::Check) {
            choices->addItem(tr("On"), QStringLiteral("true"));
            choices->addItem(tr("Off"), QStringLiteral("false"));
        } else if (option.type == engine::OptionType::Combo) {
            for (const auto& choice : option.choices) {
                choices->addItem(clippedEngineText(presentedEngineText(choice)), choice);
            }
        }
        if (hasChoices) {
            choices->setCurrentIndex(choices->findData(saved));
        } else {
            scalar->setText(saved);
        }
        auto help = tr("Default: %1").arg(clippedEngineText(presentedEngineText(defaultValue)));
        if (option.minimum && option.maximum) {
            const auto minimum = name == QStringLiteral("threads") || name == QStringLiteral("hash")
                                     ? std::max<qint64>(1, *option.minimum)
                                     : *option.minimum;
            auto maximum = *option.maximum;
            if (name == QStringLiteral("threads")) {
                maximum = std::min(maximum, engine::maximumEngineThreads);
            } else if (name == QStringLiteral("hash")) {
                maximum = std::min(maximum, engine::maximumEngineHashMiB);
            }
            help += minimum <= maximum ? tr("  ·  Range: %1–%2").arg(minimum).arg(maximum)
                                       : tr("  ·  No supported values within the app limit");
        }
        limits->setText(help);
        validate();
    };
    connect(names, &QComboBox::currentIndexChanged, dialog, update);
    connect(choices, &QComboBox::currentIndexChanged, dialog, validate);
    connect(scalar, &QLineEdit::textChanged, dialog, validate);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog,
            [this, dialog, current, value, id = profile->id] {
                controller_.editProfile(id, persistence::SetEngineOption{current().name, value()});
                dialog->accept();
            });
    connect(reset, &QPushButton::clicked, dialog, [this, dialog, current, id = profile->id] {
        controller_.editProfile(id, persistence::SetEngineOption{current().name, std::nullopt});
        dialog->accept();
    });
    update();
    dialog->open();
}
void EngineSettingsPage::updateOption() {
    const auto index = options_->currentIndex();
    if (!discovery_) {
        optionDetails_->setPlainText(tr("Connect an engine to inspect its advertised options."));
        return;
    }
    const auto identity =
        tr("Reported name: %1\nAuthor: %2\nExecutable: %3")
            .arg(presentedEngineText(discovery_->reportedName),
                 presentedEngineText(discovery_->reportedAuthor.value_or(tr("Not provided"))),
                 presentedEngineText(discovery_->executablePath));
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
    QString text =
        identity + QStringLiteral("\n\n") +
        tr("Name: %1\nType: %2\nDefault: %3")
            .arg(presentedEngineText(value.name), type, presentedEngineText(value.defaultValue));
    if (value.minimum && value.maximum) {
        text += tr("\nMinimum: %1\nMaximum: %2").arg(*value.minimum).arg(*value.maximum);
    }
    if (!value.choices.isEmpty()) {
        QStringList choices;
        for (const auto& choice : value.choices) {
            choices.append(presentedEngineText(choice));
        }
        text += tr("\nChoices:\n%1").arg(choices.join(u'\n'));
    }
    optionDetails_->setPlainText(text);
}
} // namespace chess_review::ui
