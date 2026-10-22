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
enum class OptionType { Check, Spin, Combo, Button, String };
struct UciOption {
    QString name;
    OptionType type = OptionType::String;
    QString defaultValue;
    std::optional<qint64> minimum;
    std::optional<qint64> maximum;
    QStringList choices;
};
struct Capabilities {
    QList<UciOption> options;
    bool supportsMultiPv = false;
    bool supportsWdl = false;
    bool supportsThreads = false;
    bool supportsHash = false;
};
struct ValidatedEngine {
    QString executablePath;
    QByteArray executableSha256;
    QString reportedName;
    std::optional<QString> reportedAuthor;
    std::optional<QString> detectedVersion;
    Capabilities capabilities;
    QDateTime lastValidatedAt;
};
struct EngineProfile {
    std::int64_t id = 0;
    QString displayName;
    ValidatedEngine engine;
    bool isDefault = false;
    QDateTime createdAt;
    QMap<QString, QString> optionOverrides = {}; // Empty until option editing is implemented.
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
    Replacement
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
