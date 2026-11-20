#include "config/Configuration.h"
#include "puzzles/Puzzle.h"
#include "review/Intelligence.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QScopeGuard>
#include <algorithm>
#include <sstream>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <filesystem>
#include <system_error>
#endif
#include <cmath>
#include <initializer_list>
#include <toml++/toml.hpp>
#include <tuple>
#include <utility>

namespace chess_review::config {
namespace {
constexpr auto maxBytes = static_cast<std::size_t>(maximumConfigurationBytes);
class Reader {
  public:
    Diagnostics errors;
    void error(std::string field, std::string message, const toml::node* node = nullptr) {
        if (errors.size() >= 32) {
            return;
        }
        Diagnostic diagnostic{std::move(field), std::move(message), {}, {}};
        if (node) {
            diagnostic.line = node->source().begin.line;
            diagnostic.column = node->source().begin.column;
        }
        errors.push_back(std::move(diagnostic));
    }
    void keys(const toml::table& table, std::string_view path,
              std::initializer_list<std::string_view> allowed) {
        for (const auto& [key, node] : table) {
            bool found = false;
            for (const auto name : allowed) {
                if (key.str() == name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                error(std::string(path), "Unknown configuration key", &node);
            }
        }
    }
    const toml::table* table(const toml::table& parent, std::string_view key,
                             std::string_view path) {
        const auto* node = parent.get(key);
        if (!node) {
            return nullptr;
        }
        if (const auto* value = node->as_table()) {
            return value;
        }
        error(std::string(path), "Expected a table", node);
        return nullptr;
    }
    void integer(const toml::table& table, std::string_view key, const std::string& path,
                 std::uint64_t& target, std::uint64_t maximum) {
        const auto* node = table.get(key);
        if (!node) {
            return;
        }
        const auto* value = node->as_integer();
        if (!value || value->get() < 1 || static_cast<std::uint64_t>(value->get()) > maximum) {
            error(path, "Expected an integer in [1, " + std::to_string(maximum) + "]", node);
            return;
        }
        target = static_cast<std::uint64_t>(value->get());
    }
    void number(const toml::table& table, std::string_view key, const std::string& path,
                double& target, double minimum, double maximum, bool exclusive = false) {
        const auto* node = table.get(key);
        if (!node) {
            return;
        }
        const auto value = node->value<double>();
        if ((!node->is_integer() && !node->is_floating_point()) || !value ||
            !std::isfinite(*value) ||
            (exclusive ? (*value <= minimum || *value >= maximum)
                       : (*value < minimum || *value > maximum))) {
            error(path,
                  "Expected a finite number in " + std::string(exclusive ? "(" : "[") +
                      std::to_string(minimum) + ", " + std::to_string(maximum) +
                      (exclusive ? ")" : "]"),
                  node);
            return;
        }
        target = *value;
    }
};
QByteArray digest(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}
Identities identities(const review::Configuration& config, std::uint32_t version) {
    QByteArray classification;
    QDataStream c(&classification, QIODevice::WriteOnly);
    c.setVersion(QDataStream::Qt_6_0);
    c.setByteOrder(QDataStream::BigEndian);
    c.setFloatingPointPrecision(QDataStream::DoublePrecision);
    c << (version == 1 ? QStringLiteral("classification/v1") : QStringLiteral("classification/v2"))
      << config.classification.inaccuracyLoss << config.classification.mistakeLoss
      << config.classification.blunderLoss;
    if (version == 2) {
        c << config.classification.excellentLoss;
    }
    QByteArray conversion;
    QDataStream e(&conversion, QIODevice::WriteOnly);
    e.setVersion(QDataStream::Qt_6_0);
    e.setByteOrder(QDataStream::BigEndian);
    e.setFloatingPointPrecision(QDataStream::DoublePrecision);
    e << QStringLiteral("expected-score/v1") << config.conversion.fallbackScaleCp
      << config.conversion.preferWdl;
    QByteArray analysis;
    QDataStream a(&analysis, QIODevice::WriteOnly);
    a.setVersion(QDataStream::Qt_6_0);
    a.setByteOrder(QDataStream::BigEndian);
    a.setFloatingPointPrecision(QDataStream::DoublePrecision);
    a << QStringLiteral("analysis/v1") << static_cast<quint32>(config.quality)
      << config.conversion.preferWdl;
    for (const auto& budget : config.budgets) {
        a << static_cast<quint64>(budget.quickScanNodes)
          << static_cast<quint64>(budget.deepeningNodes)
          << static_cast<quint64>(budget.verificationNodes);
    }
    return {digest(classification), digest(conversion), digest(analysis)};
}
Result failure(const char* message) { return Diagnostics{{"configuration", message, {}, {}}}; }
} // namespace
Result parse(std::string_view contents) {
    if (contents.size() > maxBytes) {
        return failure("Configuration exceeds 64 KiB");
    }
    // toml++ bounds array/inline-table recursion, but not dotted-key table depth.
    // A conservative raw punctuation budget bounds that depth before constructing
    // an AST; counting dots in comments/strings too deliberately fails closed.
    if (std::count(contents.begin(), contents.end(), '.') > 256) {
        return failure(
            "Configuration exceeds the 256-dot complexity limit (including comments and strings)");
    }
    auto parsed = toml::parse(contents);
    if (!parsed) {
        const auto source = parsed.error().source();
        return Diagnostics{
            {"configuration", "Invalid TOML syntax", source.begin.line, source.begin.column}};
    }
    const auto& root = parsed.table();
    review::Configuration config;
    Reader reader;
    reader.keys(root, "configuration",
                {"version", "analysis", "classification", "review", "expectedScore", "refinement",
                 "special", "ui", "puzzles", "openings"});
    UiConfiguration ui;
    PuzzleConfiguration puzzles;
    if (const auto* table = reader.table(root, "puzzles", "puzzles")) {
        reader.keys(*table, "puzzles",
                    {"automatic", "maximumPlayerMoves", "maximumPositions", "maximumBudgetUnits",
                     "minimumMaterialGain", "immediateMaterialGain"});
        if (const auto* node = table->get("automatic")) {
            if (const auto* value = node->as_boolean()) {
                puzzles.automatic = value->get();
            } else {
                reader.error("puzzles.automatic", "Expected a boolean", node);
            }
        }
        for (const auto& [key, target, maximum] :
             {std::tuple{"maximumPlayerMoves", &puzzles.maximumPlayerMoves, 4},
              std::tuple{"maximumPositions", &puzzles.maximumPositions, 64},
              std::tuple{"maximumBudgetUnits", &puzzles.maximumBudgetUnits, 64},
              std::tuple{"minimumMaterialGain", &puzzles.minimumMaterialGain, 9},
              std::tuple{"immediateMaterialGain", &puzzles.immediateMaterialGain, 9}}) {
            auto number = static_cast<std::uint64_t>(*target);
            reader.integer(*table, key, std::string("puzzles.") + key, number,
                           static_cast<std::uint64_t>(maximum));
            *target = static_cast<int>(number);
        }
        if (puzzles.maximumBudgetUnits < 6 || puzzles.minimumMaterialGain < 2 ||
            puzzles.immediateMaterialGain < puzzles.minimumMaterialGain) {
            reader.error("puzzles",
                         "Puzzle budgets require at least six units; material thresholds must "
                         "satisfy 2 <= minimum <= immediate",
                         table);
        }
    }
    if (const auto* table = reader.table(root, "ui", "ui")) {
        reader.keys(*table, "ui", {"verbose"});
        if (const auto* node = table->get("verbose")) {
            if (const auto* value = node->as_boolean()) {
                ui.verbose = value->get();
            } else {
                reader.error("ui.verbose", "Expected a boolean", node);
            }
        }
    }
    std::uint64_t version = 1;
    reader.integer(root, "version", "version", version, 1);
    if (const auto* analysis = reader.table(root, "analysis", "analysis")) {
        reader.keys(*analysis, "analysis", {"fast", "standard", "thorough", "liveMultiPv"});
        if (analysis->contains("liveMultiPv")) {
            std::uint64_t width = 3;
            reader.integer(*analysis, "liveMultiPv", "analysis.liveMultiPv", width, 256);
            ui.liveAnalysisLines = static_cast<int>(width);
        }
        const std::array<std::string_view, 3> names{"fast", "standard", "thorough"};
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto path = "analysis." + std::string(names[i]);
            const auto* table = reader.table(*analysis, names[i], path);
            if (!table) {
                continue;
            }
            reader.keys(*table, path, {"quickScanNodes", "deepeningNodes", "verificationNodes"});
            auto& budget = config.budgets[i];
            reader.integer(*table, "quickScanNodes", path + ".quickScanNodes",
                           budget.quickScanNodes, 1000000000000ULL);
            reader.integer(*table, "deepeningNodes", path + ".deepeningNodes",
                           budget.deepeningNodes, 1000000000000ULL);
            reader.integer(*table, "verificationNodes", path + ".verificationNodes",
                           budget.verificationNodes, 1000000000000ULL);
            if (budget.quickScanNodes > budget.deepeningNodes ||
                budget.deepeningNodes > budget.verificationNodes) {
                reader.error(path, "Budgets must satisfy quick scan <= deepening <= verification",
                             table);
            }
        }
    }
    if (const auto* table = reader.table(root, "openings", "openings")) {
        reader.keys(*table, "openings", {"enabled", "maximumPly", "minimumReferences"});
        auto& book = config.openings;
        if (const auto* node = table->get("enabled")) {
            if (const auto* value = node->as_boolean()) {
                book.enabled = value->get();
            } else {
                reader.error("openings.enabled", "Expected a boolean", node);
            }
        }
        auto ply = static_cast<std::uint64_t>(book.maximumPly);
        auto references = static_cast<std::uint64_t>(book.minimumReferences);
        reader.integer(*table, "maximumPly", "openings.maximumPly", ply, openings::maxBookPly);
        reader.integer(*table, "minimumReferences", "openings.minimumReferences", references,
                       openings::maxBookSupport);
        book.maximumPly = static_cast<int>(ply);
        book.minimumReferences = static_cast<int>(references);
    }
    if (const auto* table = reader.table(root, "refinement", "refinement")) {
        reader.keys(*table, "refinement", {"boundaryMargin", "maximumDrift", "multiPv"});
        reader.number(*table, "boundaryMargin", "refinement.boundaryMargin",
                      config.refinement.boundaryMargin, 0, 1);
        reader.number(*table, "maximumDrift", "refinement.maximumDrift",
                      config.refinement.maximumDrift, 0, 1);
        std::uint64_t width = 3;
        reader.integer(*table, "multiPv", "refinement.multiPv", width, 256);
        config.refinement.multiPv = static_cast<int>(width);
    }
    if (const auto* table = reader.table(root, "special", "special")) {
        reader.keys(*table, "special",
                    {"maximumLoss", "maximumLossDrift", "alternativeGap", "missLoss", "viableScore",
                     "alreadyWinningScore", "margin", "sacrificeValue"});
        auto& c = config.special;
        reader.number(*table, "maximumLoss", "special.maximumLoss", c.maximumLoss, 0, 1);
        reader.number(*table, "maximumLossDrift", "special.maximumLossDrift", c.maximumLossDrift, 0,
                      1);
        reader.number(*table, "alternativeGap", "special.alternativeGap", c.alternativeGap, 0, 1);
        reader.number(*table, "missLoss", "special.missLoss", c.missLoss, 0, 1);
        reader.number(*table, "viableScore", "special.viableScore", c.viableScore, 0, 1);
        reader.number(*table, "alreadyWinningScore", "special.alreadyWinningScore",
                      c.alreadyWinningScore, 0, 1);
        reader.number(*table, "margin", "special.margin", c.margin, 0, 1);
        std::uint64_t sacrifice = 2;
        reader.integer(*table, "sacrificeValue", "special.sacrificeValue", sacrifice, 9);
        c.sacrificeValue = static_cast<int>(sacrifice);
    }
    if (const auto* table = reader.table(root, "classification", "classification")) {
        reader.keys(*table, "classification",
                    {"inaccuracyLoss", "mistakeLoss", "blunderLoss", "excellentLoss"});
        auto& c = config.classification;
        reader.number(*table, "excellentLoss", "classification.excellentLoss", c.excellentLoss, 0,
                      1, true);
        reader.number(*table, "inaccuracyLoss", "classification.inaccuracyLoss", c.inaccuracyLoss,
                      0, 1, true);
        reader.number(*table, "mistakeLoss", "classification.mistakeLoss", c.mistakeLoss, 0, 1,
                      true);
        reader.number(*table, "blunderLoss", "classification.blunderLoss", c.blunderLoss, 0, 1,
                      true);
        if (c.inaccuracyLoss >= c.mistakeLoss || c.mistakeLoss >= c.blunderLoss) {
            reader.error("classification", "Loss thresholds must be strictly increasing", table);
        }
    }
    if (const auto* table = reader.table(root, "expectedScore", "expectedScore")) {
        reader.keys(*table, "expectedScore", {"fallbackScaleCp"});
        reader.number(*table, "fallbackScaleCp", "expectedScore.fallbackScaleCp",
                      config.conversion.fallbackScaleCp, 1, 10000);
    }
    if (const auto* table = reader.table(root, "review", "review")) {
        reader.keys(*table, "review", {"quality", "preferWdl"});
        if (const auto* node = table->get("quality")) {
            const auto quality = node->value<std::string>();
            if (quality == "fast") {
                config.quality = review::Quality::Fast;
            } else if (quality == "standard") {
                config.quality = review::Quality::Standard;
            } else if (quality == "thorough") {
                config.quality = review::Quality::Thorough;
            } else {
                reader.error("review.quality", "Expected fast, standard or thorough", node);
            }
        }
        if (const auto* node = table->get("preferWdl")) {
            if (const auto* boolean = node->as_boolean()) {
                config.conversion.preferWdl = boolean->get();
            } else {
                reader.error("review.preferWdl", "Expected a boolean", node);
            }
        }
    }
    if (!reader.errors.empty()) {
        return reader.errors;
    }
    auto result = validate(config);
    if (const auto* valid = std::get_if<std::shared_ptr<const ValidatedConfiguration>>(&result)) {
        auto withUi = **valid;
        withUi.ui = ui;
        withUi.puzzles = puzzles;
        return std::make_shared<const ValidatedConfiguration>(std::move(withUi));
    }
    return result;
}
Result validate(const review::Configuration& values, std::uint32_t classificationIdentityVersion) {
    const auto& c = values.classification;
    const auto& e = values.conversion;
    const auto& r = values.refinement;
    if (!openings::validBookConfig(values.openings)) {
        return failure("Invalid opening-book configuration values");
    }
    if (!review::validSpecialConfig(values.special)) {
        return failure("Invalid special-label configuration values");
    }
    if (!std::isfinite(r.boundaryMargin) || r.boundaryMargin < 0 || r.boundaryMargin > 1 ||
        !std::isfinite(r.maximumDrift) || r.maximumDrift < 0 || r.maximumDrift > 1 ||
        r.multiPv < 1 || r.multiPv > 256) {
        return failure("Invalid refinement configuration values");
    }
    if ((classificationIdentityVersion != 1 && classificationIdentityVersion != 2) ||
        (classificationIdentityVersion == 1 && c.excellentLoss != 0.01) ||
        !std::isfinite(c.excellentLoss) || c.excellentLoss <= 0 || c.excellentLoss >= 1 ||
        values.version != 1 ||
        (values.quality != review::Quality::Fast && values.quality != review::Quality::Standard &&
         values.quality != review::Quality::Thorough) ||
        !std::isfinite(c.inaccuracyLoss) || !std::isfinite(c.mistakeLoss) ||
        !std::isfinite(c.blunderLoss) || c.inaccuracyLoss <= 0 ||
        c.inaccuracyLoss >= c.mistakeLoss || c.mistakeLoss >= c.blunderLoss || c.blunderLoss >= 1 ||
        !std::isfinite(e.fallbackScaleCp) || e.fallbackScaleCp < 1 || e.fallbackScaleCp > 10000) {
        return failure("Invalid review configuration values");
    }
    for (const auto& b : values.budgets) {
        if (b.quickScanNodes < 1 || b.quickScanNodes > b.deepeningNodes ||
            b.deepeningNodes > b.verificationNodes || b.verificationNodes > 1000000000000ULL) {
            return failure("Invalid analysis budgets");
        }
    }
    return std::make_shared<const ValidatedConfiguration>(
        ValidatedConfiguration{values,
                               identities(values, classificationIdentityVersion),
                               classificationIdentityVersion,
                               {},
                               {}});
}
static std::variant<QByteArray, Diagnostics> readBytes(const QString& filename) {
    const auto error = [](const char* message) {
        return Diagnostics{{"configuration", message, {}, {}}};
    };
    if (!QDir::isAbsolutePath(filename) || filename.contains(QChar::Null) ||
        !filename.isValidUtf16()) {
        return error("Configuration location is unavailable");
    }
    const auto missing = [&]() -> std::variant<QByteArray, Diagnostics> {
        QFileInfo ancestor(QFileInfo(filename).absolutePath());
        while (!ancestor.exists() && !ancestor.isSymLink()) {
            const auto parent = ancestor.absolutePath();
            if (parent == ancestor.absoluteFilePath()) {
                return error("Configuration location is unavailable");
            }
            ancestor.setFile(parent);
        }
        if (!ancestor.isDir() || !ancestor.isReadable()) {
            return error("Configuration location is unavailable");
        }
        return QByteArray{};
    };
    QFile file;
#ifdef Q_OS_UNIX
    // Open the actual target without following a leaf symlink or blocking on a FIFO,
    // then validate the descriptor. A path-only stat followed by QFile::open races
    // with replacements, and QFileInfo::exists conflates EACCES with absence.
    const auto native = QFile::encodeName(filename);
    const int descriptor =
        ::open(native.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        if (errno == ENOENT) {
            return missing();
        }
        if (errno == EACCES || errno == EPERM) {
            return error("Configuration could not be read; check file and directory permissions");
        }
        if (errno == ELOOP) {
            return error("Configuration cannot be a symbolic link");
        }
        return error("Configuration could not be opened safely");
    }
    auto closeDescriptor = qScopeGuard([descriptor] { ::close(descriptor); });
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        return error("Configuration must be a regular file");
    }
    if (status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) > maxBytes) {
        return error("Configuration exceeds 64 KiB");
    }
    if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        return error("Configuration could not be read");
    }
    closeDescriptor.dismiss(); // QFile now owns the descriptor, including error paths below.
#else
    std::error_code pathError;
    const auto status = std::filesystem::symlink_status(
        std::filesystem::path(filename.toStdU16String()), pathError);
    if (pathError) {
        return pathError == std::errc::no_such_file_or_directory
                   ? missing()
                   : error("Configuration could not be inspected");
    }
    if (status.type() == std::filesystem::file_type::not_found) {
        return missing();
    }
    if (QFileInfo(filename).isSymLink() || !std::filesystem::is_regular_file(status)) {
        return error("Configuration must be a regular file");
    }
    file.setFileName(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        return error("Configuration could not be read");
    }
#endif
    const auto bytes = file.read(static_cast<qint64>(maxBytes + 1));
    if (file.error() != QFileDevice::NoError) {
        return error("Configuration could not be read");
    }
    if (bytes.size() > maximumConfigurationBytes) {
        return error("Configuration exceeds 64 KiB");
    }
    if (!bytes.isValidUtf8() || bytes.contains('\0')) {
        return error("Configuration must be UTF-8 text");
    }
    return bytes;
}
void applyPuzzleLimits(puzzles::Policy& policy, const PuzzleConfiguration& c) {
    policy.maximumPlayerMoves = c.maximumPlayerMoves;
    policy.maximumPositions = c.maximumPositions;
    policy.maximumBudgetUnits = c.maximumBudgetUnits;
    policy.minimumMaterialGain = c.minimumMaterialGain;
    policy.immediateMaterialGain = c.immediateMaterialGain;
}
Document loadDocument(const QString& filename) {
    auto read = readBytes(filename);
    if (auto* errors = std::get_if<Diagnostics>(&read)) {
        return {std::move(*errors), std::nullopt};
    }
    auto contents = std::get<QByteArray>(std::move(read));
    auto result =
        parse(std::string_view(contents.constData(), static_cast<std::size_t>(contents.size())));
    return {std::move(result), std::move(contents)};
}
Result load(const QString& filename) { return loadDocument(filename).result; }
Document saveDocument(const QString& filename, const QByteArray& expected, QByteArray contents) {
    auto validated =
        parse(std::string_view(contents.constData(), static_cast<std::size_t>(contents.size())));
    if (std::holds_alternative<Diagnostics>(validated)) {
        return {std::move(validated), std::nullopt};
    }
    if (!QDir::isAbsolutePath(filename) || filename.contains(QChar::Null) ||
        !filename.isValidUtf16() || !QDir().mkpath(QFileInfo(filename).absolutePath()) ||
        QFileInfo(filename).isSymLink()) {
        return {failure("Configuration location is unavailable"), std::nullopt};
    }
    QLockFile lock(filename + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) {
        return {failure("Configuration is being saved elsewhere or cannot be locked"),
                std::nullopt};
    }
    QSaveFile file(filename);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(contents) != contents.size()) {
        return {failure("Configuration could not be saved atomically"), std::nullopt};
    }
    const auto current = readBytes(filename);
    const auto* bytes = std::get_if<QByteArray>(&current);
    if (!bytes || *bytes != expected) {
        return {
            failure("Configuration changed on disk or could not be read; reload it before saving"),
            std::nullopt};
    }
    if (!file.commit()) {
        return {failure("Configuration could not be saved atomically"), std::nullopt};
    }
    return {std::move(validated), std::move(contents)};
}
BasicPreferences basicPreferences(const ValidatedConfiguration& c) {
    return {c.values.quality,
            c.values.conversion.preferWdl,
            c.values.openings.enabled,
            c.puzzles.automatic,
            c.ui.verbose,
            c.values.refinement.multiPv,
            c.ui.liveAnalysisLines};
}
Document saveBasicPreferences(const QString& filename, const QByteArray& expected,
                              const BasicPreferences& preferences) {
    const auto original =
        parse(std::string_view(expected.constData(), static_cast<std::size_t>(expected.size())));
    const auto* baseline = std::get_if<std::shared_ptr<const ValidatedConfiguration>>(&original);
    if (!baseline) {
        return {original, std::nullopt};
    }
    if (preferences == basicPreferences(**baseline)) {
        return {original, expected};
    }
    auto parsed = toml::parse(
        std::string_view(expected.constData(), static_cast<std::size_t>(expected.size())));
    if (!parsed) {
        return {failure("Configuration could not be parsed for updating"), std::nullopt};
    }
    auto& root = parsed.table();
    const auto update = [&root](const char* group, const char* key, auto value) {
        if (!root.contains(group)) {
            root.insert(group, toml::table{});
        }
        root.get_as<toml::table>(group)->insert_or_assign(key, value);
    };
    switch (preferences.quality) {
    case review::Quality::Fast:
        update("review", "quality", "fast");
        break;
    case review::Quality::Standard:
        update("review", "quality", "standard");
        break;
    case review::Quality::Thorough:
        update("review", "quality", "thorough");
        break;
    }
    update("review", "preferWdl", preferences.preferWdl);
    update("openings", "enabled", preferences.openingBook);
    update("puzzles", "automatic", preferences.automaticPuzzles);
    update("ui", "verbose", preferences.verbose);
    update("refinement", "multiPv", preferences.reviewLines);
    if (preferences.liveAnalysisLines) {
        update("analysis", "liveMultiPv", *preferences.liveAnalysisLines);
    } else if (auto* analysis = root.get_as<toml::table>("analysis")) {
        analysis->erase("liveMultiPv");
    }
    std::ostringstream formatted;
    formatted << root << '\n';
    return saveDocument(filename, expected, QByteArray::fromStdString(formatted.str()));
}
Result setAutomaticPuzzles(const QString& filename, bool enabled) {
    auto document = loadDocument(filename);
    const auto* baseline =
        std::get_if<std::shared_ptr<const ValidatedConfiguration>>(&document.result);
    if (!baseline || !document.source) {
        return document.result;
    }
    auto preferences = basicPreferences(**baseline);
    preferences.automaticPuzzles = enabled;
    return saveBasicPreferences(filename, *document.source, preferences).result;
}
} // namespace chess_review::config
