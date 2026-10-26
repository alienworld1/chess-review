#include "app/ReviewConfiguration.h"
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <utility>
namespace chess_review::app {
ReviewConfiguration::ReviewConfiguration(QObject* parent) : QObject(parent) {
    pool_.setMaxThreadCount(1);
}
ReviewConfiguration::~ReviewConfiguration() {
    Q_ASSERT_X(pool_.waitForDone(0), "ReviewConfiguration", "Await closed() before destruction");
}
void ReviewConfiguration::load(const std::optional<QString>& filename) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (started_ || closing_) {
        return;
    }
    started_ = true;
    pool_.start([this, filename] {
        config::Result result;
        try {
            result = filename ? config::load(*filename) : config::parse({});
        } catch (...) {
            result = config::Diagnostics{{"configuration", "Configuration loading failed", {}, {}}};
        }
        QMetaObject::invokeMethod(
            this,
            [this, result = std::move(result)] {
                if (closing_) {
                    return;
                }
                if (const auto* valid =
                        std::get_if<std::shared_ptr<const config::ValidatedConfiguration>>(
                            &result)) {
                    configuration_ = *valid;
                    state_ = ConfigurationState::Valid;
                } else {
                    diagnostics_ = std::get<config::Diagnostics>(result);
                    state_ = ConfigurationState::Invalid;
                }
                emit changed();
            },
            Qt::QueuedConnection);
    });
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
QString ReviewConfiguration::safeReport() const {
    QString report;
    for (const auto& diagnostic : diagnostics_) {
        report += QString::fromStdString(diagnostic.field) + QStringLiteral(": ") +
                  QString::fromStdString(diagnostic.message);
        if (diagnostic.line) {
            report += QStringLiteral(" (line %1, column %2)")
                          .arg(*diagnostic.line)
                          .arg(diagnostic.column.value_or(1));
        }
        report += u'\n';
    }
    return report;
}
} // namespace chess_review::app
