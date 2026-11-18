#pragma once

#include "chess/Position.h"
#include <span>
#include <unordered_map>

namespace chess_review::openings {

struct Opening {
    std::string eco;
    std::string name;
    [[nodiscard]] std::string_view family() const;
    [[nodiscard]] std::string_view variation() const;
    bool operator==(const Opening&) const = default;
};

// Inputs use UTF-8. The I/O boundary validates encoding before calling this function.
[[nodiscard]] bool validOpening(std::string_view eco, std::string_view name);
inline constexpr std::size_t maxOpeningNameBytes = 256;

// Build-time conversion of the upstream PGN references to a lookup resource.
[[nodiscard]] std::optional<std::string>
compileReferences(std::span<const std::string_view> volumes);

class OpeningIndex {
  public:
    [[nodiscard]] static std::optional<OpeningIndex> build(std::string_view references);
    [[nodiscard]] std::optional<Opening> recognize(chess::Position root,
                                                   std::span<const chess::Move> moves) const;

  private:
    struct Reference {
        Opening opening;
        std::string moveOrder;
    };

    OpeningIndex() = default;
    std::unordered_map<std::string, std::vector<Reference>> positions_;
    std::size_t maxPly_ = 0;
};

} // namespace chess_review::openings
