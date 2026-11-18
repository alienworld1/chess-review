#pragma once

namespace chess_review::openings {

inline constexpr int bookPolicyVersion = 1;
inline constexpr int maxBookPly = 64;
inline constexpr int maxBookSupport = 10000;

struct BookConfig {
    bool enabled = true;
    int maximumPly = 24;
    int minimumReferences = 2;
    bool operator==(const BookConfig&) const = default;
};

[[nodiscard]] constexpr bool validBookConfig(const BookConfig& config) {
    return config.maximumPly >= 1 && config.maximumPly <= maxBookPly &&
           config.minimumReferences >= 1 && config.minimumReferences <= maxBookSupport;
}

} // namespace chess_review::openings
