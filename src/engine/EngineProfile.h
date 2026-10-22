#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>
#include <variant>

namespace chess_review::engine {
// Values are part of the version 1 persistence format.
enum class OptionType { Check = 0, Spin = 1, Combo = 2, Button = 3, String = 4 };
struct UciOption {
    QString name;
    OptionType type = OptionType::String;
    QString defaultValue;
    std::optional<qint64> minimum;
    std::optional<qint64> maximum;
    QStringList choices;
    bool operator==(const UciOption&) const = default;
};
struct Capabilities {
    QList<UciOption> options;
    bool supportsMultiPv = false;
    bool supportsWdl = false;
    bool supportsThreads = false;
    bool supportsHash = false;
    bool operator==(const Capabilities&) const = default;
};
struct ValidatedEngine {
    QString executablePath;
    QByteArray executableSha256;
    QString reportedName;
    std::optional<QString> reportedAuthor;
    std::optional<QString> detectedVersion;
    Capabilities capabilities;
    QDateTime lastValidatedAt;
    bool operator==(const ValidatedEngine&) const = default;
};
struct EngineProfile {
    std::int64_t id = 0;
    QString displayName;
    ValidatedEngine engine;
    bool isDefault = false;
    QDateTime createdAt;
    QMap<QString, QString> optionOverrides = {}; // Empty until option editing is implemented.
    bool operator==(const EngineProfile&) const = default;
};
enum class ErrorCode {
    Path,
    Read,
    Changed,
    Start,
    Timeout,
    Protocol,
    OutputLimit,
    Crash,
    Cleanup,
    Cancelled,
    Busy,
    Closing,
    Storage,
    InvalidRecord,
    Replacement,
    InvalidRequest
};
struct Error {
    ErrorCode code;
    QString message; // Static, actionable text; never native errors or raw engine output.
};
using ValidationResult = std::variant<ValidatedEngine, Error>;
using PathResult = std::variant<QString, Error>;
// Bounded UCI declarations and the versioned persistence codec use the same validation.
std::variant<UciOption, Error> parseOption(const QString& line);
std::optional<Error> appendOption(Capabilities& capabilities, UciOption option);
QByteArray encodeCapabilities(const Capabilities& capabilities);
std::optional<Capabilities> decodeCapabilities(const QByteArray& bytes);
bool validEngine(const ValidatedEngine& engine);
} // namespace chess_review::engine
Q_DECLARE_METATYPE(chess_review::engine::ValidationResult)
Q_DECLARE_METATYPE(chess_review::engine::PathResult)
