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
namespace chess_review::puzzles {
struct Policy;
}
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
// Presentation preferences do not participate in persisted review/cache identities.
struct UiConfiguration {
    bool verbose = false;
    std::optional<int> liveAnalysisLines;
    bool reviewBothSides = false;
};
struct PuzzleConfiguration {
    bool automatic = true;
    int maximumPlayerMoves = 4;
    int maximumPositions = 64;
    int maximumBudgetUnits = 64;
    int minimumMaterialGain = 2;
    int immediateMaterialGain = 5;
};
void applyPuzzleLimits(puzzles::Policy& policy, const PuzzleConfiguration& configuration);
struct ValidatedConfiguration {
    review::Configuration values;
    Identities identities;
    // Version 1 is retained only for decoding/resuming released settings.
    std::uint32_t classificationIdentityVersion = 2;
    UiConfiguration ui;
    PuzzleConfiguration puzzles;
};
using Result = std::variant<std::shared_ptr<const ValidatedConfiguration>, Diagnostics>;
[[nodiscard]] Result validate(const review::Configuration& values,
                              std::uint32_t classificationIdentityVersion = 2);
[[nodiscard]] Result parse(std::string_view contents);
constexpr qsizetype maximumConfigurationBytes = 64 * 1024;
struct Document {
    Result result;
    std::optional<QByteArray> source; // Includes readable TOML that failed schema validation.
};
struct BasicPreferences {
    review::Quality quality;
    bool preferWdl;
    bool openingBook;
    bool automaticPuzzles;
    bool verbose;
    int reviewLines;
    std::optional<int> liveAnalysisLines;
    bool reviewBothSides;
    review::ClassificationConfig classification;
    bool operator==(const BasicPreferences&) const = default;
};
BasicPreferences basicPreferences(const ValidatedConfiguration& configuration);
[[nodiscard]] Document loadDocument(const QString& filename);
[[nodiscard]] Document saveDocument(const QString& filename, const QByteArray& expected,
                                    QByteArray contents);
[[nodiscard]] Document saveBasicPreferences(const QString& filename, const QByteArray& expected,
                                            const BasicPreferences& preferences);
// File I/O and parsing must run on a worker. Absence selects defaults; other errors fail closed.
[[nodiscard]] Result load(const QString& filename);
// Safe bounded read, validation, and atomic replacement; call only on a worker.
[[nodiscard]] Result setAutomaticPuzzles(const QString& filename, bool enabled);
} // namespace chess_review::config
