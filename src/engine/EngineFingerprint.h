#pragma once

#include "engine/EngineProfile.h"
#include <QByteArray>
#include <QString>
#include <cstdint>
#include <optional>
#include <vector>

namespace chess_review::engine {
struct EffectiveOption {
    QString name; // Case-folded UCI name; entries are sorted by name.
    OptionType type = OptionType::String;
    QString value;
    bool operator==(const EffectiveOption&) const = default;
};
enum class ResourceVerification : std::uint32_t { ExternalResourcesUnverified = 0 };
// Captured configuration identity only. Equality never authorizes cache reuse.
struct EngineFingerprint {
    std::uint32_t schemaVersion = 1;
    std::uint32_t adapterPolicyVersion = 1;
    QByteArray executableSha256;
    QString reportedName;
    std::optional<QString> reportedAuthor;
    std::vector<EffectiveOption> options;
    ResourceVerification resources = ResourceVerification::ExternalResourcesUnverified;
    QByteArray digest;
    bool operator==(const EngineFingerprint&) const = default;
};
} // namespace chess_review::engine
