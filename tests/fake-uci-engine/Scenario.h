#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <optional>

namespace fake_uci {
inline constexpr qsizetype kMaxBytes = 1024 * 1024;
inline constexpr qsizetype kMaxLineBytes = 64 * 1024;
inline constexpr qsizetype kMaxActions = 4096;

enum class ActionType { Write, CancelPending, Exit, Crash, Hang };
struct Action {
    ActionType type{};
    int afterMs{};
    bool stderrStream{};
    QByteArray data;
};
struct Step {
    QByteArray expect;
    QVector<Action> actions;
};
struct Scenario {
    QVector<Step> steps;
};
// UCI command whitespace is ASCII space/tab; output bytes are never normalized.
QByteArray normalizedCommand(const QByteArray& command);
std::optional<Scenario> readScenario(const QString& path, QString& error);
} // namespace fake_uci
