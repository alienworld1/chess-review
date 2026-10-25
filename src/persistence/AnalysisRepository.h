#pragma once
#include "engine/AnalysisCache.h"
#include <QMetaType>
#include <cstdint>
#include <optional>
#include <variant>

namespace chess_review::persistence {
enum class CacheErrorCode { InvalidInput, Storage, Busy, Closing, Unavailable, Conflict };
struct CacheError {
    CacheErrorCode code;
    QString message; // Bounded safe diagnostic; no engine text, SQL, or native errors.
};
struct AnalysisStored {
    std::int64_t id;
    bool duplicate = false;
};
struct CacheLookup {
    std::optional<engine::AnalysisResult> hit;
    int discardedRecords = 0; // Invalid cache rows removed during this lookup.
};
using StoreAnalysisResult = std::variant<AnalysisStored, CacheError>;
using FindAnalysisResult = std::variant<CacheLookup, CacheError>;
} // namespace chess_review::persistence
Q_DECLARE_METATYPE(chess_review::persistence::StoreAnalysisResult)
Q_DECLARE_METATYPE(chess_review::persistence::FindAnalysisResult)
