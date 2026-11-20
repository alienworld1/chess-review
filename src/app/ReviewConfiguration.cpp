#include "app/ReviewConfiguration.h"
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <utility>

namespace chess_review::app {
namespace {
QString diagnosticReport(const config::Diagnostics& diagnostics) {
    QString report;
    for (const auto& diagnostic : diagnostics) {
        report += QString::fromStdString(diagnostic.field) + QStringLiteral(": ") +
                  QString::fromStdString(diagnostic.message);
        if (diagnostic.line) {
            report += QObject::tr(" (line %1, column %2)")
                          .arg(*diagnostic.line)
                          .arg(diagnostic.column.value_or(1));
        }
        report += u'\n';
    }
    return report.trimmed();
}
} // namespace

ReviewConfiguration::ReviewConfiguration(QObject* parent) : QObject(parent) {
    pool_.setMaxThreadCount(1);
}
ReviewConfiguration::~ReviewConfiguration() {
    Q_ASSERT_X(pool_.waitForDone(0), "ReviewConfiguration", "Await closed() before destruction");
}
void ReviewConfiguration::load(const std::optional<QString>& filename) {
    if (started_ || closing_) {
        return;
    }
    started_ = true;
    filename_ = filename;
    submit(Operation::Load, [filename] {
        return filename ? config::loadDocument(*filename)
                        : config::Document{config::parse({}), std::nullopt};
    });
}
void ReviewConfiguration::reload() {
    if (canEdit()) {
        submit(Operation::Load, [filename = *filename_] { return config::loadDocument(filename); });
    }
}
void ReviewConfiguration::saveBasic(const config::BasicPreferences& preferences) {
    if (canEdit() && source_ && configuration_) {
        submit(Operation::Save, [filename = *filename_, source = *source_, preferences] {
            return config::saveBasicPreferences(filename, source, preferences);
        });
    }
}
void ReviewConfiguration::validateDraft(const QByteArray& contents) {
    if (canEdit() && source_) {
        submit(Operation::Validate, [contents] {
            return config::Document{
                config::parse(std::string_view(contents.constData(),
                                               static_cast<std::size_t>(contents.size()))),
                std::nullopt};
        });
    }
}
void ReviewConfiguration::saveDraft(const QByteArray& baseline, const QByteArray& contents) {
    if (canEdit() && source_) {
        submit(Operation::Save, [filename = *filename_, baseline, contents] {
            return config::saveDocument(filename, baseline, contents);
        });
    }
}
void ReviewConfiguration::submit(Operation operation, std::function<config::Document()> work) {
    Q_ASSERT(QThread::currentThread() == thread());
    busy_ = true;
    operationReport_.clear();
    pool_.start([this, operation, work = std::move(work)] {
        config::Document document;
        try {
            document = work();
        } catch (...) {
            document.result =
                config::Diagnostics{{"configuration", "Configuration operation failed", {}, {}}};
        }
        QMetaObject::invokeMethod(
            this,
            [this, operation, document = std::move(document)]() mutable {
                busy_ = false;
                if (closing_) {
                    return;
                }
                if (const auto* errors = std::get_if<config::Diagnostics>(&document.result)) {
                    operationReport_ = diagnosticReport(*errors);
                    if (operation == Operation::Load) {
                        source_ = std::move(document.source);
                        diagnostics_ = *errors;
                        configuration_.reset();
                        state_ = ConfigurationState::Invalid;
                    }
                } else if (operation == Operation::Validate) {
                    operationReport_ = tr("TOML is valid.");
                } else {
                    source_ = std::move(document.source);
                    configuration_ =
                        std::get<std::shared_ptr<const config::ValidatedConfiguration>>(
                            std::move(document.result));
                    state_ = ConfigurationState::Valid;
                    diagnostics_.clear();
                    operationReport_ = operation == Operation::Save ? tr("Settings saved.")
                                                                    : tr("Configuration loaded.");
                }
                emit changed();
            },
            Qt::QueuedConnection);
    });
    if (state_ != ConfigurationState::Loading) {
        emit changed();
    }
}
void ReviewConfiguration::close() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (closing_) {
        return;
    }
    closing_ = true;
    QTimer::singleShot(0, this, &ReviewConfiguration::finishClose);
}
void ReviewConfiguration::finishClose() {
    if (!pool_.waitForDone(0)) {
        QTimer::singleShot(5, this, &ReviewConfiguration::finishClose);
        return;
    }
    emit closed();
}
QString ReviewConfiguration::safeReport() const { return diagnosticReport(diagnostics_); }
} // namespace chess_review::app
