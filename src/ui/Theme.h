#pragma once
#include <QColor>
#include <QStyle>
#include <optional>

class QAbstractButton;
class QWidget;
namespace chess_review::review {
enum class Classification;
}

namespace chess_review::ui {
// One application theme, following the system palette including runtime changes.
void installTheme();
QColor classificationColor(std::optional<review::Classification> classification);
void setTextRole(QWidget& widget, const char* role);
void setPrimaryAction(QAbstractButton& button);
void setNavigationIcon(QAbstractButton& button, QStyle::StandardPixmap icon);
} // namespace chess_review::ui
