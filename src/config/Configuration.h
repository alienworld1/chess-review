#pragma once
#include "review/Configuration.h"
#include <QByteArray>
#include <QString>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
namespace chess_review::config {
struct Diagnostic {
    std::string field;   // Recognized schema path only; never user-supplied unknown key text.
    std::string message; // Static safe text; never parser excerpts or native errors.
    std::optional<std::uint32_t> line;
    std::optional<std::uint32_t> column;
};
using Diagnostics = std::vector<Diagnostic>;
struct Identities {
    QByteArray classification;
    QByteArray conversion;
    QByteArray analysis;
};
struct ValidatedConfiguration {
    review::Configuration values;
    Identities identities;
};
using Result = std::variant<std::shared_ptr<const ValidatedConfiguration>, Diagnostics>;
[[nodiscard]] Result parse(std::string_view contents);
// File I/O and parsing must run on a worker. Absence selects defaults; other errors fail closed.
[[nodiscard]] Result load(const QString& filename);
} // namespace chess_review::config
