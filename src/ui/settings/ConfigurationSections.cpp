#include "app/ReviewConfiguration.h"
#include "ui/settings/EngineSettingsPage.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextDocument>
#include <QVBoxLayout>
#include <array>
#include <memory>

static void initializeSettingsResources() { Q_INIT_RESOURCE(settings_reference); }

namespace chess_review::ui {
namespace {
QLabel* text(QWidget* parent, const QString& value = {}) {
    auto* label = new QLabel(value, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}
} // namespace

void EngineSettingsPage::installConfiguration(app::ReviewConfiguration& configuration) {
    auto* scroll = new QScrollArea(sections_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* basic = new QWidget(scroll);
    auto* basicLayout = new QVBoxLayout(basic);
    scroll->setWidget(basic);
    sections_->addTab(scroll, tr("&Review preferences"));

    auto* review = new QGroupBox(tr("New reviews"), basic);
    auto* reviewForm = new QFormLayout(review);
    reviewForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    reviewForm->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    auto* quality = new QComboBox(review);
    quality->setObjectName(QStringLiteral("defaultReviewQuality"));
    quality->addItems({tr("Fast"), tr("Standard"), tr("Thorough")});
    reviewForm->addRow(tr("&Default quality"), quality);
    auto* description = text(review);
    reviewForm->addRow(description);
    auto* lines = new QSpinBox(review);
    lines->setRange(1, 256);
    lines->setToolTip(tr("Maximum candidate moves per deeper review search. The engine may "
                         "support fewer. Higher values spread the search across more moves."));
    reviewForm->addRow(tr("Candidate &lines"), lines);
    auto* book = new QCheckBox(tr("Use local opening references"), review);
    auto* wdl = new QCheckBox(tr("Prefer engine win/draw/loss estimates when available"), review);
    auto* bothSides = new QCheckBox(tr("Include key moments from both players"), review);
    bothSides->setObjectName(QStringLiteral("reviewBothSides"));
    reviewForm->addRow(bothSides);
    reviewForm->addRow(book);
    reviewForm->addRow(wdl);
    basicLayout->addWidget(review);

    auto* classification = new QGroupBox(tr("Move classifications"), basic);
    auto* thresholdsForm = new QFormLayout(classification);
    thresholdsForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    thresholdsForm->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    thresholdsForm->addRow(
        text(classification, tr("Enter expected-score loss from 0 to 1. For example, 0.05 "
                                "means a loss of 5 percentage points.")));
    auto* inaccuracy = new QLineEdit(classification);
    auto* mistake = new QLineEdit(classification);
    auto* blunder = new QLineEdit(classification);
    const std::array thresholds{inaccuracy, mistake, blunder};
    for (auto* field : thresholds) {
        field->setMaxLength(32);
        field->setMaximumWidth(180);
    }
    inaccuracy->setObjectName(QStringLiteral("inaccuracyLoss"));
    mistake->setObjectName(QStringLiteral("mistakeLoss"));
    blunder->setObjectName(QStringLiteral("blunderLoss"));
    thresholdsForm->addRow(tr("&Inaccuracy starts at"), inaccuracy);
    thresholdsForm->addRow(tr("&Mistake starts at"), mistake);
    thresholdsForm->addRow(tr("&Blunder starts at"), blunder);
    auto* standardThresholds = new QPushButton(tr("Use standard thresholds"), classification);
    thresholdsForm->addRow(standardThresholds);
    basicLayout->addWidget(classification);

    auto* preferences = new QGroupBox(tr("Application"), basic);
    auto* preferencesLayout = new QVBoxLayout(preferences);
    auto* liveLines = new QSpinBox(preferences);
    liveLines->setRange(0, 256);
    liveLines->setSpecialValueText(tr("Use last choice"));
    liveLines->setToolTip(tr("Default candidate lines in Analyze. You can change the number of "
                             "lines while analyzing a position."));
    auto* analysisForm = new QFormLayout;
    analysisForm->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    analysisForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    analysisForm->addRow(tr("Default &analysis lines"), liveLines);
    preferencesLayout->addLayout(analysisForm);
    auto* puzzles =
        new QCheckBox(tr("Generate puzzles automatically after completed reviews"), preferences);
    puzzles->setObjectName(QStringLiteral("automaticPuzzles"));
    auto* verbose = new QCheckBox(tr("Show technical details"), preferences);
    preferencesLayout->addWidget(puzzles);
    preferencesLayout->addWidget(verbose);
    basicLayout->addWidget(preferences);
    auto* basicHelp = text(basic, tr("Use Advanced to customize preset budgets or classification "
                                     "thresholds. Saving here may reformat config.toml."));
    basicLayout->addWidget(basicHelp);
    auto* basicStatus = text(basic);
    basicStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    basicLayout->addWidget(basicStatus);
    auto* basicActions = new QHBoxLayout;
    auto* apply = new QPushButton(tr("&Save preferences"), basic);
    apply->setObjectName(QStringLiteral("saveReviewPreferences"));
    auto* reset = new QPushButton(tr("Discard changes"), basic);
    basicActions->addWidget(apply);
    basicActions->addWidget(reset);
    basicActions->addStretch();
    basicLayout->addLayout(basicActions);
    basicLayout->addStretch();

    auto* advanced = new QWidget(sections_);
    auto* advancedLayout = new QVBoxLayout(advanced);
    sections_->addTab(advanced, tr("&Advanced"));
    advancedLayout->addWidget(
        text(advanced, tr("Edit config.toml and validate it before saving. Review settings are "
                          "captured when a review starts.")));
    auto* location = new QLineEdit(advanced);
    location->setReadOnly(true);
    location->setAccessibleName(tr("Configuration file location"));
    advancedLayout->addWidget(location);
    auto* editor = new QPlainTextEdit(advanced);
    editor->setObjectName(QStringLiteral("advancedToml"));
    editor->setAccessibleName(tr("TOML configuration editor"));
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor->setPlaceholderText(
        tr("An empty file uses defaults. For example:\n\n[review]\nquality = \"standard\""));
    advancedLayout->addWidget(editor, 1);
    auto* buttons = new QHBoxLayout;
    auto* reload = new QPushButton(tr("&Reload from disk"), advanced);
    auto* validate = new QPushButton(tr("&Validate TOML"), advanced);
    validate->setObjectName(QStringLiteral("validateToml"));
    auto* save = new QPushButton(tr("&Save TOML"), advanced);
    save->setObjectName(QStringLiteral("saveToml"));
    buttons->addWidget(reload);
    buttons->addWidget(validate);
    buttons->addWidget(save);
    buttons->addStretch();
    auto* reference = new QPushButton(tr("Configuration &reference…"), advanced);
    buttons->addWidget(reference);
    advancedLayout->addLayout(buttons);
    auto* report = new QPlainTextEdit(advanced);
    report->setObjectName(QStringLiteral("tomlDiagnostics"));
    report->setReadOnly(true);
    report->setAccessibleName(tr("Configuration diagnostics"));
    report->setMaximumHeight(96);
    advancedLayout->addWidget(report);

    connect(reference, &QPushButton::clicked, advanced, [advanced, editor] {
        initializeSettingsResources();
        QFile example(QStringLiteral(":/settings/config.example.toml"));
        auto* dialog = new QDialog(advanced);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(tr("Configuration reference"));
        dialog->resize(680, 500);
        auto* layout = new QVBoxLayout(dialog);
        auto* contents = new QPlainTextEdit(dialog);
        contents->setAccessibleName(tr("Example TOML configuration"));
        contents->setReadOnly(true);
        const auto source =
            example.open(QIODevice::ReadOnly) ? QString::fromUtf8(example.readAll()) : QString{};
        contents->setPlainText(source.isEmpty() ? tr("The example could not be opened.") : source);
        contents->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        layout->addWidget(contents);
        auto* actions = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        auto* use = actions->addButton(tr("Use example"), QDialogButtonBox::ActionRole);
        use->setEnabled(!source.isEmpty() && !editor->isReadOnly() &&
                        editor->document()->isEmpty());
        connect(actions, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        connect(use, &QPushButton::clicked, dialog, [editor, source, dialog] {
            if (!editor->isReadOnly() && editor->document()->isEmpty()) {
                editor->insertPlainText(source);
            }
            dialog->accept();
        });
        layout->addWidget(actions);
        dialog->open();
    });

    const auto formValues = [=, &configuration] {
        const auto snapshot = configuration.snapshot();
        const auto excellent = snapshot ? snapshot->values.classification.excellentLoss
                                        : review::ClassificationConfig{}.excellentLoss;
        return config::BasicPreferences{static_cast<review::Quality>(quality->currentIndex()),
                                        wdl->isChecked(),
                                        book->isChecked(),
                                        puzzles->isChecked(),
                                        verbose->isChecked(),
                                        lines->value(),
                                        liveLines->value() ? std::optional{liveLines->value()}
                                                           : std::nullopt,
                                        bothSides->isChecked(),
                                        {inaccuracy->text().toDouble(), mistake->text().toDouble(),
                                         blunder->text().toDouble(), excellent}};
    };
    const auto setThresholds = [=](const review::ClassificationConfig& value) {
        const auto display = [](double loss) {
            return QLocale::c().toString(loss, 'g', QLocale::FloatingPointShortest);
        };
        inaccuracy->setText(display(value.inaccuracyLoss));
        mistake->setText(display(value.mistakeLoss));
        blunder->setText(display(value.blunderLoss));
    };
    const auto resetForm = [=, &configuration] {
        if (const auto snapshot = configuration.snapshot()) {
            const auto value = config::basicPreferences(*snapshot);
            quality->setCurrentIndex(static_cast<int>(value.quality));
            wdl->setChecked(value.preferWdl);
            book->setChecked(value.openingBook);
            puzzles->setChecked(value.automaticPuzzles);
            verbose->setChecked(value.verbose);
            lines->setValue(value.reviewLines);
            liveLines->setValue(value.liveAnalysisLines.value_or(0));
            bothSides->setChecked(value.reviewBothSides);
            setThresholds(value.classification);
        }
    };
    const auto refreshActions = [=, &configuration] {
        const bool dirty = editor->document()->isModified();
        const bool writable = configuration.canEdit() && configuration.source().has_value();
        const auto snapshot = configuration.snapshot();
        const auto draft = formValues();
        const bool basicDirty = snapshot && draft != config::basicPreferences(*snapshot);
        const bool canEditBasic = writable && snapshot && !dirty;
        const auto& loss = draft.classification;
        const bool validThresholds = 0 < loss.inaccuracyLoss &&
                                     loss.inaccuracyLoss < loss.mistakeLoss &&
                                     loss.mistakeLoss < loss.blunderLoss && loss.blunderLoss < 1;
        review->setEnabled(canEditBasic);
        classification->setEnabled(canEditBasic);
        preferences->setEnabled(canEditBasic);
        reset->setEnabled(canEditBasic && basicDirty);
        apply->setEnabled(canEditBasic && basicDirty && validThresholds);
        basicStatus->setText(
            dirty ? tr("Save or discard the Advanced draft before editing these preferences.")
            : configuration.state() == app::ConfigurationState::Invalid
                ? tr("Correct the TOML in Advanced to edit these preferences.")
            : snapshot && !validThresholds
                ? tr("Enter three increasing loss thresholds, each greater than 0 and less than 1.")
                : configuration.operationReport());
        const bool withinLimit =
            editor->document()->characterCount() <= config::maximumConfigurationBytes + 1;
        editor->setReadOnly(!writable || basicDirty);
        reload->setEnabled(configuration.canEdit() && !basicDirty);
        reload->setText(dirty ? tr("Discard draft and &reload") : tr("&Reload from disk"));
        validate->setEnabled(writable && !basicDirty && withinLimit);
        save->setEnabled(writable && !basicDirty && withinLimit && dirty);
        if (!withinLimit) {
            report->setPlainText(tr("Configuration exceeds 64 KiB."));
        } else if (basicDirty) {
            report->setPlainText(tr("Save or discard changes in Review preferences to edit TOML."));
        } else if (configuration.busy()) {
            report->setPlainText(tr("Working…"));
        } else if (!configuration.filename()) {
            report->setPlainText(tr("The configuration file location is unavailable."));
        } else {
            const auto& message = configuration.operationReport();
            if (report->toPlainText() != message) {
                report->setPlainText(message);
            }
        }
    };
    connect(quality, &QComboBox::currentIndexChanged, description, [description](int index) {
        switch (static_cast<review::Quality>(index)) {
        case review::Quality::Fast:
            description->setText(
                tr("Scans the whole game and checks up to two key moves in depth."));
            break;
        case review::Quality::Standard:
            description->setText(
                tr("Scans the whole game and checks up to four key moves in depth."));
            break;
        case review::Quality::Thorough:
            description->setText(tr("Uses deeper searches and broader verification. Best when you "
                                    "can leave the review running longer."));
            break;
        }
    });
    connect(quality, &QComboBox::currentIndexChanged, basic, refreshActions);
    connect(lines, &QSpinBox::valueChanged, basic, refreshActions);
    connect(liveLines, &QSpinBox::valueChanged, basic, refreshActions);
    for (auto* box : {wdl, book, puzzles, verbose, bothSides}) {
        connect(box, &QCheckBox::toggled, basic, refreshActions);
    }
    for (auto* field : thresholds) {
        connect(field, &QLineEdit::textChanged, basic, refreshActions);
    }
    connect(standardThresholds, &QPushButton::clicked, basic,
            [setThresholds] { setThresholds(review::ClassificationConfig{}); });
    connect(editor->document(), &QTextDocument::modificationChanged, advanced, refreshActions);
    connect(editor, &QPlainTextEdit::textChanged, advanced, refreshActions);
    connect(reset, &QPushButton::clicked, basic, resetForm);
    connect(apply, &QPushButton::clicked, basic,
            [formValues, &configuration] { configuration.saveBasic(formValues()); });

    struct Draft {
        std::optional<QByteArray> baseline;
        std::optional<QByteArray> pendingSave;
        bool reloading = false;
    };
    auto draft = std::make_shared<Draft>();
    connect(validate, &QPushButton::clicked, advanced, [editor, &configuration] {
        configuration.validateDraft(editor->toPlainText().toUtf8());
    });
    connect(save, &QPushButton::clicked, advanced, [editor, draft, &configuration] {
        if (draft->baseline) {
            draft->pendingSave = editor->toPlainText().toUtf8();
            configuration.saveDraft(*draft->baseline, *draft->pendingSave);
        }
    });
    connect(reload, &QPushButton::clicked, advanced, [draft, &configuration] {
        draft->reloading = true;
        configuration.reload();
    });
    const auto update = [=, &configuration, last = configuration.snapshot()]() mutable {
        if (!configuration.busy()) {
            const auto& source = configuration.source();
            if (draft->reloading || (source && draft->pendingSave == source) ||
                (!editor->document()->isModified() && draft->baseline != source)) {
                const auto contents = QString::fromUtf8(source.value_or(QByteArray{}));
                if (editor->toPlainText() != contents) {
                    editor->setPlainText(contents);
                }
                editor->document()->setModified(false);
                draft->baseline = source;
            }
            draft->pendingSave.reset();
            draft->reloading = false;
            if (last != configuration.snapshot()) {
                last = configuration.snapshot();
                resetForm();
            }
        }
        location->setText(configuration.filename().value_or(QString{}));
        refreshActions();
    };
    connect(&configuration, &app::ReviewConfiguration::changed, advanced, update);
    resetForm();
    if (configuration.source()) {
        draft->baseline = configuration.source();
        editor->setPlainText(QString::fromUtf8(*draft->baseline));
    }
    location->setText(configuration.filename().value_or(QString{}));
    // Show the initial description even if Qt selected the first entry before wiring signals.
    if (!configuration.snapshot()) {
        quality->setCurrentIndex(static_cast<int>(review::Quality::Standard));
    }
    refreshActions();
}
} // namespace chess_review::ui
