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
// Bounded UI preferences only; no moves, annotations, or product records.
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
bool validBoardPreferences(const BoardPreferences& preferences) {
    if (preferences.inputMode != BoardInputMode::Drag &&
        preferences.inputMode != BoardInputMode::Click) {
        return false;
    }
    if (preferences.analysisSplit < 1 || preferences.analysisSplit > 99) {
        return false;
    }
    for (const auto& color : preferences.annotationColors) {
        if (color.size() != 7 || color.front() != QLatin1Char('#')) {
            return false;
        }
        for (qsizetype i = 1; i < color.size(); ++i) {
            const auto c = color[i].toLower();
            if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                  (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) {
                return false;
            }
        }
    }
    return true;
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
    const auto mode =
        settings.value(QStringLiteral("board/v1/input"), QStringLiteral("drag")).toString();
    const auto coordinates =
        settings.value(QStringLiteral("board/v1/coordinates"), true).toString();
    const auto animation = settings.value(QStringLiteral("board/v1/animation"), true).toString();
    const auto orientation =
        settings.value(QStringLiteral("board/v1/blackAtBottom"), false).toString();
    bool splitValid = false;
    state.board.analysisSplit =
        settings.value(QStringLiteral("analysis/v1/split"), 60).toInt(&splitValid);
    state.board.blackAtBottom = orientation == QStringLiteral("true");
    state.board.inputMode =
        mode == QStringLiteral("click") ? BoardInputMode::Click : BoardInputMode::Drag;
    state.board.coordinates = coordinates == QStringLiteral("true");
    state.board.animation = animation == QStringLiteral("true");
    for (std::size_t i = 0; i < state.board.annotationColors.size(); ++i) {
        state.board.annotationColors[i] =
            settings
                .value(QStringLiteral("board/v1/color%1").arg(i), state.board.annotationColors[i])
                .toString();
    }
    if ((mode != QStringLiteral("drag") && mode != QStringLiteral("click")) ||
        (coordinates != QStringLiteral("true") && coordinates != QStringLiteral("false")) ||
        (animation != QStringLiteral("true") && animation != QStringLiteral("false")) ||
        (orientation != QStringLiteral("true") && orientation != QStringLiteral("false")) ||
        !splitValid || !validBoardPreferences(state.board)) {
        error = UiStateError::Invalid;
        state.board = {};
    }
    if (settings.contains(QStringLiteral("workspace/v1/gameId"))) {
        bool validId = false;
        bool validNode = false;
        const auto id = settings.value(QStringLiteral("workspace/v1/gameId")).toLongLong(&validId);
        const auto nodeText = settings.value(QStringLiteral("workspace/v1/node")).toString();
        const auto node = nodeText.toULongLong(&validNode);
        if (!validId || id <= 0 || !validNode || nodeText.startsWith(u'-')) {
            error = UiStateError::Invalid;
        } else {
            state.game = GameResume{id, node};
        }
    }
    state.maximized = maximized.toBool();
    state.destination = *destination;
    return state;
}
std::optional<UiStateError> writeState(const QString& filename, const UiState& state) {
    if (state.geometry.size() > 4096 || destinationId(state.destination).isEmpty() ||
        !validFile(filename) || !validBoardPreferences(state.board) ||
        (state.game && state.game->id <= 0)) {
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
        settings.setValue(QStringLiteral("board/v1/input"),
                          state.board.inputMode == BoardInputMode::Drag ? QStringLiteral("drag")
                                                                        : QStringLiteral("click"));
        settings.setValue(QStringLiteral("board/v1/coordinates"), state.board.coordinates);
        settings.setValue(QStringLiteral("board/v1/animation"), state.board.animation);
        settings.setValue(QStringLiteral("board/v1/blackAtBottom"), state.board.blackAtBottom);
        settings.setValue(QStringLiteral("analysis/v1/split"), state.board.analysisSplit);
        settings.remove(QStringLiteral("workspace/v1"));
        if (state.game) {
            settings.setValue(QStringLiteral("workspace/v1/gameId"),
                              QVariant::fromValue<qlonglong>(state.game->id));
            settings.setValue(QStringLiteral("workspace/v1/node"),
                              QVariant::fromValue<qulonglong>(state.game->node));
        }
        for (std::size_t i = 0; i < state.board.annotationColors.size(); ++i) {
            settings.setValue(QStringLiteral("board/v1/color%1").arg(i),
                              state.board.annotationColors[i]);
        }
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
