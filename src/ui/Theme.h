#pragma once
#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <optional>

class QAbstractButton;
class QFrame;
class QPainter;
class QRectF;
class QWidget;
namespace chess_review::review {
enum class Classification;
}

namespace chess_review::ui {
void installTheme();
void setThemeMode(Qt::ColorScheme scheme);
bool darkTheme();

// Accessible foreground for classification text on neutral surfaces.
QColor classificationColor(std::optional<review::Classification> classification);
// Vivid fill for small classification badges (DESIGN.md §3.2).
QColor classificationFill(review::Classification classification);
void paintClassificationBadge(QPainter& painter, const QRectF& bounds,
                              review::Classification classification);
QPixmap classificationPixmap(review::Classification classification, int size, qreal ratio);

// Typography roles: title, heading, metric, lesson, strong, secondary, caption, badge.
void setTextRole(QWidget& widget, const char* role);
void setPrimaryAction(QAbstractButton& button);
// Borderless tertiary command; still shows hover, pressed, focus and disabled states.
void setQuietAction(QAbstractButton& button);
// Tighter button padding for dense workstation strips.
void setCompactAction(QAbstractButton& button);
// A grouped content pane on the root canvas (surface.panel with a subtle border).
void setPanelSurface(QWidget& widget);
// A recessed container for callouts, stat tiles, hints and inline notices.
void setSubtleSurface(QWidget& widget);
// Borderless table/editor embedded inside another surface.
void setFlatSurface(QWidget& widget);
QFrame* makeSeparator(QWidget* parent);

// Bundled interface icon tinted from the active theme; follows palette changes.
QIcon themedIcon(const QString& name, const QColor& color, const QColor& disabled);
void setIcon(QAbstractButton& button, const QString& name);
// Icon-only command: keeps its label as tooltip and accessible name.
void setIconOnly(QAbstractButton& button, const QString& name);
} // namespace chess_review::ui
