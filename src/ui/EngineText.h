#pragma once
#include <QString>

namespace chess_review::ui {
// Escape invisible engine-supplied characters and bound compact identity labels.
QString presentedEngineText(const QString& text);
QString clippedEngineText(const QString& text);
} // namespace chess_review::ui
