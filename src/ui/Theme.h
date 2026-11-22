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
void installTheme();
void setThemeMode(Qt::ColorScheme scheme);
QColor classificationColor(std::optional<review::Classification> classification);
void setTextRole(QWidget& widget, const char* role);
void setPrimaryAction(QAbstractButton& button);
void setNavigationIcon(QAbstractButton& button, QStyle::StandardPixmap icon);
} // namespace chess_review::ui
