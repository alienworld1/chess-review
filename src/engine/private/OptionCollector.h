#pragma once
#include "engine/EngineProfile.h"
#include <set>

namespace chess_review::engine {
// Own the declaration order and its uniqueness index together for streamed/batch input.
class OptionCollector final {
  public:
    std::optional<Error> append(UciOption option);
    const Capabilities& capabilities() const { return capabilities_; }

  private:
    struct NameLess {
        bool operator()(const QString& left, const QString& right) const {
            return left.compare(right, Qt::CaseInsensitive) < 0;
        }
    };
    Capabilities capabilities_;
    std::set<QString, NameLess> names_;
};
} // namespace chess_review::engine
