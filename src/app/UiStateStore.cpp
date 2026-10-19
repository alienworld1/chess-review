#include "app/UiStateStore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMetaObject>
#include <QSaveFile>
#include <QSettings>
#include <QTemporaryFile>
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
    if (file.isSymLink() || (file.exists() && (!file.isFile() || file.size() > maxFileBytes))) {
        return false;
    }
    if (file.exists()) {
        return true;
    }
    // Missing directories are normal first-run state; a file/dangling link in their
    // place is an invalid location and must not masquerade as absent preferences.
    QFileInfo ancestor(file.absolutePath());
    while (!ancestor.exists() && !ancestor.isSymLink()) {
        const auto parent = ancestor.absolutePath();
        if (parent == ancestor.absoluteFilePath()) {
            return false;
        }
        ancestor.setFile(parent);
    }
    return ancestor.isDir();
}
UiState readState(const QString& filename, std::optional<UiStateError>& error) {
    if (!validFile(filename)) {
        error = UiStateError::Invalid;
        return {};
    }
    if (!QFileInfo::exists(filename)) {
        return {};
    }
    QSettings settings(filename, QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    UiState state;
    const auto geometry = settings.value(QStringLiteral("shell/v1/geometry"), QByteArray{});
    state.geometry = geometry.toByteArray();
    const auto maximized = settings.value(QStringLiteral("shell/v1/maximized"), false);
    const auto destination = destinationFromId(
        settings.value(QStringLiteral("shell/v1/destination"), QStringLiteral("home")).toString());
    const auto maximizedText = maximized.toString();
    if (settings.status() == QSettings::AccessError) {
        error = UiStateError::Read;
        return {};
    }
    if (settings.status() != QSettings::NoError ||
        geometry.metaType().id() != QMetaType::QByteArray || state.geometry.size() > 4096 ||
        !destination ||
        (maximizedText != QStringLiteral("true") && maximizedText != QStringLiteral("false"))) {
        error = UiStateError::Invalid;
        return {};
    }
    state.maximized = maximized.toBool();
    state.destination = *destination;
    return state;
}
std::optional<UiStateError> writeState(const QString& filename, const UiState& state) {
    if (state.geometry.size() > 4096 || destinationId(state.destination).isEmpty() ||
        !validFile(filename)) {
        return UiStateError::Invalid;
    }
    if (!QDir().mkpath(QFileInfo(filename).absolutePath())) {
        return UiStateError::Write;
    }
    const auto lockPath = filename + QStringLiteral(".lock");
    const QFileInfo lockInfo(lockPath);
    const QFileInfo cleanupLock(lockPath + QStringLiteral(".rmlock"));
    // QLockFile reads metadata using unbounded readLine(); refuse unsafe/oversized input.
    if (lockInfo.isSymLink() ||
        (lockInfo.exists() && (!lockInfo.isFile() || lockInfo.size() > 16 * 1024))) {
        return UiStateError::Invalid;
    }
    if (cleanupLock.exists() || cleanupLock.isSymLink()) {
        return UiStateError::Busy;
    }
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        return lock.error() == QLockFile::LockFailedError ? UiStateError::Busy
                                                          : UiStateError::Write;
    }
    if (!validFile(filename)) {
        return UiStateError::Invalid;
    }
    // QSettings::sync() takes its own indefinite lock. Serialize into a unique staging
    // file instead, then atomically publish while holding the real QSettings lock.
    QTemporaryFile staging(QFileInfo(filename).absolutePath() +
                           QStringLiteral("/.ui-state-XXXXXX"));
    if (!staging.open()) {
        return UiStateError::Write;
    }
    if (QFileInfo::exists(filename)) {
        QFile original(filename);
        if (!original.open(QIODevice::ReadOnly)) {
            return UiStateError::Read;
        }
        const auto contents = original.read(maxFileBytes + 1);
        if (original.error() != QFileDevice::NoError) {
            return UiStateError::Read;
        }
        if (contents.size() > maxFileBytes) {
            return UiStateError::Invalid;
        }
        // Preserve unrecognized keys, including settings from a newer application.
        if (staging.write(contents) != contents.size()) {
            return UiStateError::Write;
        }
    }
    if (!staging.flush()) {
        return UiStateError::Write;
    }
    staging.close(); // Let Qt atomically replace this file on Windows too.
    {
        QSettings settings(staging.fileName(), QSettings::IniFormat);
        settings.setFallbacksEnabled(false);
        settings.setValue(QStringLiteral("shell/v1/geometry"), state.geometry);
        settings.setValue(QStringLiteral("shell/v1/maximized"), state.maximized);
        settings.setValue(QStringLiteral("shell/v1/destination"), destinationId(state.destination));
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            return settings.status() == QSettings::FormatError ? UiStateError::Invalid
                                                               : UiStateError::Write;
        }
    }
    QFile serialized(staging.fileName());
    if (!serialized.open(QIODevice::ReadOnly)) {
        return UiStateError::Write;
    }
    const auto contents = serialized.read(maxFileBytes + 1);
    if (serialized.error() != QFileDevice::NoError || contents.size() > maxFileBytes) {
        return UiStateError::Write;
    }
    QSaveFile output(filename);
    if (!output.open(QIODevice::WriteOnly) || output.write(contents) != contents.size() ||
        !output.commit()) {
        return UiStateError::Write;
    }
    return std::nullopt;
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
        std::optional<UiStateError> error;
        const auto state = readState(filename, error);
        QMetaObject::invokeMethod(
            this,
            [this, state, error] {
                if (error) {
                    emit failed(*error);
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
        const auto error = state ? writeState(filename, *state) : std::nullopt;
        QMetaObject::invokeMethod(
            this,
            [this, error] {
                if (error) {
                    emit failed(*error);
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
