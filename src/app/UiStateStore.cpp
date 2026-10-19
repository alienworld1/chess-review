#include "app/UiStateStore.h"
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QSettings>
#include <QThread>
#include <QTimer>

namespace chess_review::app {
namespace {
// This file contains only a small geometry blob and a destination identifier.
constexpr qint64 maxFileBytes = 64 * 1024;
bool validFile(const QString& filename) {
    if (!QDir::isAbsolutePath(filename) || filename.contains(QChar::Null)) {
        return false;
    }
    const QFileInfo file(filename);
    return !file.isSymLink() && (!file.exists() || (file.isFile() && file.size() <= maxFileBytes));
}
UiState readState(const QString& filename, bool& failed) {
    if (!validFile(filename)) {
        failed = true;
        return {};
    }
    if (!QFileInfo::exists(filename)) {
        return {};
    }
    QSettings settings(filename, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    UiState state;
    state.geometry = settings.value(QStringLiteral("shell/v1/geometry")).toByteArray();
    const auto maximized = settings.value(QStringLiteral("shell/v1/maximized"), false);
    const auto destination = destinationFromId(
        settings.value(QStringLiteral("shell/v1/destination"), QStringLiteral("home")).toString());
    const auto maximizedText = maximized.toString();
    if (settings.status() != QSettings::NoError || state.geometry.size() > 4096 || !destination ||
        (maximizedText != QStringLiteral("true") && maximizedText != QStringLiteral("false"))) {
        failed = true;
        return {};
    }
    state.maximized = maximized.toBool();
    state.destination = *destination;
    return state;
}
bool writeState(const QString& filename, const UiState& state) {
    if (state.geometry.size() > 4096 || destinationId(state.destination).isEmpty() ||
        !validFile(filename) || !QDir().mkpath(QFileInfo(filename).absolutePath())) {
        return false;
    }
    QSettings settings(filename, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    settings.setValue(QStringLiteral("shell/v1/geometry"), state.geometry);
    settings.setValue(QStringLiteral("shell/v1/maximized"), state.maximized);
    settings.setValue(QStringLiteral("shell/v1/destination"), destinationId(state.destination));
    settings.sync(); // Keep Qt's atomic replacement requirement enabled.
    return settings.status() == QSettings::NoError;
}
} // namespace
UiStateStore::UiStateStore(QObject* parent) : QObject(parent) {
    pool_.setMaxThreadCount(1);
    pool_.setExpiryTimeout(0);
}
UiStateStore::~UiStateStore() {
    Q_ASSERT_X(pool_.waitForDone(0), "UiStateStore", "Await closed() before destruction");
}
void UiStateStore::load(const QString& filename) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (started_ || closing_) {
        return;
    }
    started_ = true;
    filename_ = filename;
    pool_.start([this, filename] {
        bool failure = false;
        const auto state = readState(filename, failure);
        QMetaObject::invokeMethod(
            this,
            [this, state, failure] {
                if (failure) {
                    emit failed();
                }
                if (!closing_) {
                    emit loaded(state);
                }
            },
            Qt::QueuedConnection);
    });
}
void UiStateStore::saveAndClose(const std::optional<UiState>& state) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (closing_) {
        return;
    }
    closing_ = true;
    if (!started_) {
        QTimer::singleShot(0, this, &UiStateStore::closed);
        return;
    }
    pool_.start([this, state, filename = filename_] {
        const bool success = !state || writeState(filename, *state);
        QMetaObject::invokeMethod(
            this,
            [this, success] {
                if (!success) {
                    emit failed();
                }
                finishClose();
            },
            Qt::QueuedConnection);
    });
}
void UiStateStore::finishClose() {
    if (!pool_.waitForDone(0)) {
        QTimer::singleShot(5, this, &UiStateStore::finishClose);
        return;
    }
    emit closed();
}
} // namespace chess_review::app
