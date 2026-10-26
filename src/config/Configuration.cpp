#include "config/Configuration.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <algorithm>
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
#include <utility>

namespace chess_review::config {
namespace {
constexpr std::size_t maxBytes = 64 * 1024;
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
Identities identities(const review::Configuration& config) {
    QByteArray classification;
    QDataStream c(&classification, QIODevice::WriteOnly);
    c.setVersion(QDataStream::Qt_6_0);
    c.setByteOrder(QDataStream::BigEndian);
    c.setFloatingPointPrecision(QDataStream::DoublePrecision);
    c << QStringLiteral("classification/v1") << config.classification.inaccuracyLoss
      << config.classification.mistakeLoss << config.classification.blunderLoss;
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
                {"version", "analysis", "classification", "review", "expectedScore"});
    std::uint64_t version = 1;
    reader.integer(root, "version", "version", version, 1);
    if (const auto* analysis = reader.table(root, "analysis", "analysis")) {
        reader.keys(*analysis, "analysis", {"fast", "standard", "thorough"});
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
    if (const auto* table = reader.table(root, "classification", "classification")) {
        reader.keys(*table, "classification", {"inaccuracyLoss", "mistakeLoss", "blunderLoss"});
        auto& c = config.classification;
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
    return std::make_shared<const ValidatedConfiguration>(
        ValidatedConfiguration{config, identities(config)});
}
Result load(const QString& filename) {
    if (!QDir::isAbsolutePath(filename) || filename.contains(QChar::Null) ||
        !filename.isValidUtf16()) {
        return failure("Configuration location is unavailable");
    }
    const auto missing = [&]() -> Result {
        QFileInfo ancestor(QFileInfo(filename).absolutePath());
        while (!ancestor.exists() && !ancestor.isSymLink()) {
            const auto parent = ancestor.absolutePath();
            if (parent == ancestor.absoluteFilePath()) {
                return failure("Configuration location is unavailable");
            }
            ancestor.setFile(parent);
        }
        if (!ancestor.isDir() || !ancestor.isReadable()) {
            return failure("Configuration location is unavailable");
        }
        return parse({});
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
            return failure("Configuration could not be read; check file and directory permissions");
        }
        if (errno == ELOOP) {
            return failure("Configuration cannot be a symbolic link");
        }
        return failure("Configuration could not be opened safely");
    }
    auto closeDescriptor = qScopeGuard([descriptor] { ::close(descriptor); });
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        return failure("Configuration must be a regular file");
    }
    if (status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) > maxBytes) {
        return failure("Configuration exceeds 64 KiB");
    }
    if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        return failure("Configuration could not be read");
    }
    closeDescriptor.dismiss(); // QFile now owns the descriptor, including error paths below.
#else
    std::error_code error;
    const auto status =
        std::filesystem::symlink_status(std::filesystem::path(filename.toStdU16String()), error);
    if (error) {
        return error == std::errc::no_such_file_or_directory
                   ? missing()
                   : failure("Configuration could not be inspected");
    }
    if (status.type() == std::filesystem::file_type::not_found) {
        return missing();
    }
    if (QFileInfo(filename).isSymLink() || !std::filesystem::is_regular_file(status)) {
        return failure("Configuration must be a regular file");
    }
    file.setFileName(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        return failure("Configuration could not be read");
    }
#endif
    const auto bytes = file.read(static_cast<qint64>(maxBytes + 1));
    if (file.error() != QFileDevice::NoError) {
        return failure("Configuration could not be read");
    }
    return parse(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
}
} // namespace chess_review::config
