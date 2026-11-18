#pragma once

#include "persistence/GameRepository.h"
#include <QCoreApplication>

namespace chess_review::ui {
inline QString openingText(const std::optional<persistence::OpeningResolution>& opening) {
    if (!opening || !opening->match) {
        return {};
    }
    return QCoreApplication::translate("OpeningText", "%1 (%2)")
        .arg(QString::fromUtf8(opening->match->name), QString::fromLatin1(opening->match->eco));
}
} // namespace chess_review::ui
