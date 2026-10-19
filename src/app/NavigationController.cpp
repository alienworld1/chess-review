#include "app/NavigationController.h"
#include <array>

namespace chess_review::app {
namespace {
constexpr std::array destinations{Destination::Home, Destination::Games, Destination::Puzzles,
                                  Destination::Analyze, Destination::Settings};
}
QString destinationId(Destination destination) {
    switch (destination) {
    case Destination::Home:
        return QStringLiteral("home");
    case Destination::Games:
        return QStringLiteral("games");
    case Destination::Puzzles:
        return QStringLiteral("puzzles");
    case Destination::Analyze:
        return QStringLiteral("analyze");
    case Destination::Settings:
        return QStringLiteral("settings");
    }
    return {};
}
std::optional<Destination> destinationFromId(const QString& id) {
    for (const auto destination : destinations) {
        if (destinationId(destination) == id) {
            return destination;
        }
    }
    return std::nullopt;
}
NavigationController::NavigationController(QObject* parent) : QObject(parent) {}
Destination NavigationController::current() const { return current_; }
void NavigationController::navigate(Destination destination) {
    if (current_ == destination || destinationId(destination).isEmpty()) {
        return;
    }
    current_ = destination;
    emit destinationChanged(destination);
}
} // namespace chess_review::app
