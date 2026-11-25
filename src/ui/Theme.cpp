#include "ui/Theme.h"
#include "review/Classification.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFile>
#include <QFont>
#include <QFrame>
#include <QIconEngine>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPointer>
#include <QScopedValueRollback>
#include <QString>
#include <QStyle>
#include <QStyleHints>
#include <QSvgRenderer>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <QWidget>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

static void initializeIconResources() { Q_INIT_RESOURCE(icons); }

namespace chess_review::ui {
namespace {
struct Palette {
    QColor root, panel, subtle, hover, selected, text, secondary, muted, border, strong, accent,
        accentHover, accentPressed, onAccent, focus;
};
Palette themePalette(bool dark) {
    if (dark) {
        return {QColor("#191f2a"), QColor("#222b38"), QColor("#293443"), QColor("#303d4d"),
                QColor("#284635"), QColor("#eef2f7"), QColor("#b2bfd0"), QColor("#a1b0c2"),
                QColor("#364255"), QColor("#596b80"), QColor("#4cad70"), QColor("#65c486"),
                QColor("#38985d"), QColor("#101d16"), QColor("#87b8ff")};
    }
    return {QColor("#f4f6f8"), QColor("#ffffff"), QColor("#edf1f5"), QColor("#e7edf1"),
            QColor("#dfeee4"), QColor("#182230"), QColor("#4c5869"), QColor("#5a6778"),
            QColor("#d8dee5"), QColor("#adb8c4"), QColor("#27834b"), QColor("#216e40"),
            QColor("#185a32"), QColor("#ffffff"), QColor("#2476ca")};
}

void applyTextFont(QWidget& widget) {
    const auto role = widget.property("textRole").toString();
    if (role.isEmpty()) {
        return;
    }
    auto font = widget.window()->font();
    if (role == QLatin1String("title") || role == QLatin1String("heading") ||
        role == QLatin1String("metric") || role == QLatin1String("badge") ||
        role == QLatin1String("strong")) {
        font.setWeight(QFont::DemiBold);
    }
    const double factor = role == QLatin1String("title")     ? 1.7
                          : role == QLatin1String("heading") ? 1.25
                          : role == QLatin1String("metric")  ? 2.0
                          : role == QLatin1String("lesson")  ? 1.1
                          : role == QLatin1String("caption") ? 0.92
                                                             : 1.0;
    if (font.pointSizeF() > 0) {
        font.setPointSizeF(font.pointSizeF() * factor);
    } else {
        font.setPixelSize(qRound(font.pixelSize() * factor));
    }
    widget.setFont(font);
}

double luminance(const QColor& color) {
    const auto channel = [](double value) {
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) +
           0.0722 * channel(color.blueF());
}
double contrast(const QColor& a, const QColor& b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

class SvgIconEngine final : public QIconEngine {
  public:
    SvgIconEngine(const QByteArray& svg, const QColor& normal, const QColor& disabled)
        : normal_(renderer(svg, normal)), disabled_(renderer(svg, disabled)) {}
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override {
        (mode == QIcon::Disabled ? disabled_ : normal_)->render(painter, rect);
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override {
        QPixmap pixmap(QSize(qRound(size.width() * scale), qRound(size.height() * scale)));
        pixmap.fill(Qt::transparent);
        {
            QPainter painter(&pixmap);
            paint(&painter, pixmap.rect(), mode, state);
        }
        pixmap.setDevicePixelRatio(scale);
        return pixmap;
    }
    QIconEngine* clone() const override { return new SvgIconEngine(*this); }
    QString key() const override { return QStringLiteral("chess-review-svg"); }

  private:
    static std::shared_ptr<QSvgRenderer> renderer(QByteArray svg, const QColor& color) {
        svg.replace("currentColor", color.name().toLatin1());
        return std::make_shared<QSvgRenderer>(svg);
    }
    std::shared_ptr<QSvgRenderer> normal_;
    std::shared_ptr<QSvgRenderer> disabled_;
};

void ensureIconResources() {
    static const bool initialized = [] {
        initializeIconResources();
        return true;
    }();
    static_cast<void>(initialized);
}

void applyButtonIcon(QAbstractButton& button) {
    const auto name = button.property("themeIcon").toString();
    if (name.isEmpty() || button.property("themeIconApplying").toBool()) {
        return;
    }
    button.setProperty("themeIconApplying", true);
    const auto colors = themePalette(darkTheme());
    const bool primary = button.property("primary").toBool();
    const bool muted = button.property("mutedUnchecked").toBool() && !button.isChecked();
    const QColor normal = primary ? colors.onAccent : (muted ? colors.secondary : colors.text);
    button.setIcon(themedIcon(name, normal, colors.secondary));
    button.setProperty("themeIconApplying", false);
}

class Theme final : public QObject {
  public:
    Theme()
        : QObject(qApp), systemDark_(qApp->palette().color(QPalette::Window).lightness() < 128) {
        ensureIconResources();
        QCoreApplication::setAttribute(Qt::AA_UseStyleSheetPropagationInWidgetStyles);
        auto font = qApp->font();
        if (font.pointSizeF() > 0 && font.pointSizeF() < 10.0) {
            font.setPointSizeF(10.0);
            qApp->setFont(font);
        }
        qApp->installEventFilter(this);
        connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this,
                [this](Qt::ColorScheme scheme) {
                    if (scheme != Qt::ColorScheme::Unknown) {
                        systemDark_ = scheme == Qt::ColorScheme::Dark;
                        apply();
                    }
                });
        apply();
    }

    void setMode(Qt::ColorScheme mode) {
        if (mode_ != mode) {
            mode_ = mode;
            apply();
        }
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange && !applying_) {
            const auto window = qApp->palette().color(QPalette::Window);
            if (window != appliedWindow_) {
                systemDark_ = window.lightness() < 128;
                apply();
            }
        }
        if (event->type() == QEvent::FontChange && !applyingFonts_) {
            auto* window = qobject_cast<QWidget*>(watched);
            if (window && window->isWindow()) {
                const QScopedValueRollback guard(applyingFonts_, true);
                for (auto* widget : QApplication::allWidgets()) {
                    if (widget->window() == window) {
                        applyTextFont(*widget);
                    }
                }
            }
        }
        if (event->type() == QEvent::PaletteChange) {
            if (auto* button = qobject_cast<QAbstractButton*>(watched)) {
                applyButtonIcon(*button);
            }
        }
        if (event->type() == QEvent::ActionChanged) {
            if (auto* button = qobject_cast<QAbstractButton*>(watched);
                button && button->property("themeIcon").isValid() &&
                !button->property("themeIconApplying").toBool()) {
                QTimer::singleShot(0, button, [guard = QPointer<QAbstractButton>(button)] {
                    if (guard) {
                        applyButtonIcon(*guard);
                    }
                });
            }
        }
        return false;
    }

  private:
    void apply() {
        const QScopedValueRollback guard(applying_, true);
        auto palette = qApp->palette();
        const bool dark =
            mode_ == Qt::ColorScheme::Unknown ? systemDark_ : mode_ == Qt::ColorScheme::Dark;
        const auto c = themePalette(dark);
        if (appliedWindow_ == c.root && palette.color(QPalette::Window) == c.root) {
            return;
        }
        appliedWindow_ = c.root;
        palette.setColor(QPalette::Window, c.root);
        palette.setColor(QPalette::Base, c.panel);
        palette.setColor(QPalette::AlternateBase, c.subtle);
        palette.setColor(QPalette::WindowText, c.text);
        palette.setColor(QPalette::Text, c.text);
        palette.setColor(QPalette::Button, c.panel);
        palette.setColor(QPalette::ButtonText, c.text);
        palette.setColor(QPalette::PlaceholderText, c.secondary);
        palette.setColor(QPalette::Highlight, c.selected);
        palette.setColor(QPalette::HighlightedText, c.text);
        palette.setColor(QPalette::Mid, c.border);
        palette.setColor(QPalette::Link, c.accentHover);
        palette.setColor(QPalette::ToolTipBase, c.panel);
        palette.setColor(QPalette::ToolTipText, c.text);
        for (const auto role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText}) {
            palette.setColor(QPalette::Disabled, role, c.secondary);
        }
        qApp->setPalette(palette);
        // Scoped patches only: shape and state colors. Fonts stay in code so enlarged
        // window fonts continue to propagate (QSS font rules would freeze them).
        auto styleSheet = QStringLiteral(R"(
QMainWindow, QDialog { background: %1; }
QWidget { selection-background-color: %9; selection-color: %4; }
QLabel { background: transparent; }
QLabel[textRole="secondary"], QLabel[textRole="caption"] { color: %5; }
QLabel[textRole="badge"] { padding: 3px 10px; border-radius: 11px; }
QPushButton, QToolButton { background: %2; color: %4; border: 1px solid %6;
    border-radius: 6px; padding: 6px 12px; }
QPushButton:hover, QToolButton:hover { background: %3; border-color: %11; }
QPushButton:pressed, QToolButton:pressed, QToolButton:checked, QPushButton:checked {
    background: %9; }
QPushButton:disabled, QToolButton:disabled { color: %5; background: %12; border-color: %6; }
QPushButton:focus, QToolButton:focus, QComboBox:focus, QLineEdit:focus, QSpinBox:focus {
    border: 2px solid %10; }
QPushButton[quiet="true"], QToolButton[quiet="true"] { background: transparent;
    border-color: transparent; }
QPushButton[quiet="true"]:hover, QToolButton[quiet="true"]:hover { background: %3; }
QPushButton[quiet="true"]:pressed, QToolButton[quiet="true"]:pressed,
QPushButton[quiet="true"]:checked, QToolButton[quiet="true"]:checked { background: %9; }
QPushButton[quiet="true"]:disabled, QToolButton[quiet="true"]:disabled {
    background: transparent; border-color: transparent; }
QPushButton[quiet="true"]:focus, QToolButton[quiet="true"]:focus { border: 2px solid %10; }
QPushButton[compact="true"], QToolButton[compact="true"] { padding: 4px 8px; }
QPushButton[primary="true"], QToolButton[primary="true"] {
    background: %7; color: %8; border-color: %7; }
QPushButton[primary="true"]:hover, QToolButton[primary="true"]:hover {
    background: %13; border-color: %13; }
QPushButton[primary="true"]:pressed, QToolButton[primary="true"]:pressed {
    background: %14; border-color: %14; }
QPushButton[primary="true"]:focus, QToolButton[primary="true"]:focus { border: 2px solid %10; }
QPushButton[primary="true"]:disabled, QToolButton[primary="true"]:disabled {
    background: %3; color: %5; border-color: %6; }
QToolButton[popupMode="2"]::menu-indicator { image: none; width: 0; }
QToolButton[popupMode="2"] { padding-right: 12px; }
QWidget[surface="panel"] { background: %2; border: 1px solid %6; border-radius: 10px; }
QWidget[surface="subtle"] { background: %12; border: 1px solid %6; border-radius: 8px; }
QFrame[separator="true"] { background: %6; border: none; min-height: 1px; max-height: 1px; }
QAbstractItemView[flatSurface="true"], QPlainTextEdit[flatSurface="true"],
QTextEdit[flatSurface="true"] { background: transparent; border: none; }
QWidget#homeLibraryStatus { border-top: 1px solid %6; }
QWidget#navigationRail { border-right: 1px solid %6; }
QWidget#navigationRail QToolButton { background: transparent; border: 1px solid transparent;
    border-radius: 8px; padding: 8px 6px; color: %5; }
QWidget#navigationRail QToolButton:hover { background: %3; color: %4; }
QWidget#navigationRail QToolButton:checked { background: %9; color: %4; }
QWidget#navigationRail QToolButton:focus { border: 2px solid %10; }
QComboBox, QLineEdit, QSpinBox { background: %2; color: %4; border: 1px solid %6;
    border-radius: 6px; padding: 5px 8px; }
QComboBox:hover, QLineEdit:hover, QSpinBox:hover { border-color: %11; }
QAbstractItemView, QPlainTextEdit, QTextEdit { background: %2; color: %4;
    border: 1px solid %6; border-radius: 6px; }
QAbstractItemView { alternate-background-color: %3; gridline-color: %6; }
QHeaderView { background: transparent; }
QHeaderView::section { background: %2; color: %5; border: none;
    border-bottom: 1px solid %6; padding: 6px 8px; }
QTableView::item { padding: 4px 8px; }
QTableView::item:selected, QListView::item:selected, QTreeView::item:selected {
    background: %9; color: %4; }
QSplitter::handle { background: transparent; }
QSplitter::handle:horizontal { width: 12px; }
QSplitter::handle:vertical { height: 8px; }
QProgressBar { border: none; border-radius: 2px; background: %3; max-height: 4px; }
QProgressBar::chunk { background: %7; border-radius: 2px; }
QScrollArea { border: none; background: transparent; }
QTabWidget::pane { border: none; border-top: 1px solid %6; }
QTabBar::tab { background: transparent; color: %5; border: none;
    border-bottom: 2px solid transparent; padding: 8px 14px; margin-right: 4px; }
QTabBar::tab:hover { color: %4; }
QTabBar::tab:selected { color: %4; border-bottom-color: %7; }
QGroupBox { border: 1px solid %6; border-radius: 8px; margin-top: 14px; padding: 12px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: %5; }
QToolTip { background: %2; color: %4; border: 1px solid %6; padding: 4px 6px; }
QMenu { background: %2; color: %4; border: 1px solid %6; padding: 4px; }
QMenu::item { padding: 6px 22px 6px 12px; border-radius: 4px; }
QMenu::item:selected { background: %3; }
QMenu::item:disabled { color: %5; }
QMenu::separator { height: 1px; background: %6; margin: 4px 6px; }
)")
                              .arg(c.root.name(), c.panel.name(), c.hover.name(), c.text.name(),
                                   c.secondary.name(), c.border.name(), c.accent.name(),
                                   c.onAccent.name(), c.selected.name())
                              .arg(c.focus.name(), c.strong.name(), c.subtle.name(),
                                   c.accentHover.name(), c.accentPressed.name());
        for (std::size_t index = 0; index < review::classificationCount; ++index) {
            const auto label = static_cast<review::Classification>(index);
            auto tint = classificationFill(label);
            styleSheet += QStringLiteral("\nQLabel[classification=\"%1\"] { color: %2; }"
                                         "\nQLabel[classification=\"%1\"][textRole=\"badge\"] {"
                                         " background: rgba(%3, %4, %5, %6); }")
                              .arg(static_cast<qulonglong>(index))
                              .arg(classificationColor(label).name())
                              .arg(tint.red())
                              .arg(tint.green())
                              .arg(tint.blue())
                              .arg(dark ? 56 : 41);
        }
        qApp->setStyleSheet(styleSheet);
    }
    bool applying_ = false;
    bool applyingFonts_ = false;
    bool systemDark_;
    QColor appliedWindow_;
    Qt::ColorScheme mode_ = Qt::ColorScheme::Unknown;
};

Theme& applicationTheme() {
    static QPointer<Theme> theme;
    if (!theme) {
        theme = new Theme;
    }
    return *theme;
}

QPainterPath starPath(const QRectF& box) {
    QPainterPath path;
    const auto center = box.center();
    const double outer = box.width() / 2;
    const double inner = outer * 0.45;
    for (int i = 0; i < 10; ++i) {
        const double radius = i % 2 == 0 ? outer : inner;
        const double angle = -std::numbers::pi / 2 + i * std::numbers::pi / 5;
        const QPointF point(center.x() + radius * std::cos(angle),
                            center.y() + radius * std::sin(angle));
        if (i == 0) {
            path.moveTo(point);
        } else {
            path.lineTo(point);
        }
    }
    path.closeSubpath();
    return path;
}
} // namespace

bool darkTheme() { return qApp->palette().color(QPalette::Window).lightness() < 128; }

QColor classificationColor(std::optional<review::Classification> classification) {
    const auto palette = qApp->palette();
    if (!classification) {
        return palette.color(QPalette::Mid);
    }
    const bool dark = darkTheme();
    switch (*classification) {
    case review::Classification::Brilliant:
        return QColor(dark ? "#50d2e5" : "#117484");
    case review::Classification::Great:
        return QColor(dark ? "#79b4f4" : "#286caf");
    case review::Classification::Miss:
        return QColor(dark ? "#ef93ab" : "#b04566");
    case review::Classification::Best:
        return QColor(dark ? "#65c486" : "#227843");
    case review::Classification::Excellent:
        return QColor(dark ? "#62bac8" : "#1b7180");
    case review::Classification::Good:
        return QColor(dark ? "#8fcb8a" : "#3f7a3a");
    case review::Classification::Book:
        return QColor(dark ? "#a8bdbb" : "#56706e");
    case review::Classification::Inaccuracy:
        return QColor(dark ? "#f0c46c" : "#8a5a06");
    case review::Classification::Mistake:
        return QColor(dark ? "#f2a65e" : "#a6510e");
    case review::Classification::Blunder:
        return QColor(dark ? "#f07780" : "#bd3340");
    }
    return palette.color(QPalette::Mid);
}

QColor classificationFill(review::Classification classification) {
    switch (classification) {
    case review::Classification::Brilliant:
        return QColor("#26b9ce");
    case review::Classification::Great:
        return QColor("#418fe2");
    case review::Classification::Miss:
        return QColor("#d76f8c");
    case review::Classification::Best:
        return QColor("#3b9b57");
    case review::Classification::Excellent:
        return QColor("#258e9e");
    case review::Classification::Good:
        return QColor("#79a96b");
    case review::Classification::Book:
        return QColor("#728c8a");
    case review::Classification::Inaccuracy:
        return QColor("#e8bd37");
    case review::Classification::Mistake:
        return QColor("#e58639");
    case review::Classification::Blunder:
        return QColor("#dc515a");
    }
    return QColor("#728c8a");
}

void paintClassificationBadge(QPainter& painter, const QRectF& bounds,
                              review::Classification classification) {
    const double side = std::min(bounds.width(), bounds.height());
    QRectF circle(0, 0, side, side);
    circle.moveCenter(bounds.center());
    const auto fill = classificationFill(classification);
    const QColor light("#ffffff");
    const QColor dark("#1b2129");
    const auto ink = contrast(fill, light) >= contrast(fill, dark) ? light : dark;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawEllipse(circle);
    QPen stroke(ink, std::max(1.2, side * 0.12), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    const auto at = [&circle](double x, double y) {
        return QPointF(circle.left() + x * circle.width(), circle.top() + y * circle.height());
    };
    switch (classification) {
    case review::Classification::Best: {
        painter.setBrush(ink);
        painter.drawPath(
            starPath(circle.adjusted(side * 0.2, side * 0.18, -side * 0.2, -side * 0.22)));
        break;
    }
    case review::Classification::Good: {
        painter.setPen(stroke);
        painter.setBrush(Qt::NoBrush);
        const std::array points{at(0.28, 0.52), at(0.44, 0.67), at(0.73, 0.36)};
        painter.drawPolyline(points.data(), static_cast<int>(points.size()));
        break;
    }
    case review::Classification::Excellent: {
        painter.setPen(stroke);
        painter.drawLine(at(0.5, 0.28), at(0.5, 0.72));
        painter.drawLine(at(0.28, 0.5), at(0.72, 0.5));
        break;
    }
    case review::Classification::Miss: {
        painter.setPen(stroke);
        painter.drawLine(at(0.33, 0.33), at(0.67, 0.67));
        painter.drawLine(at(0.67, 0.33), at(0.33, 0.67));
        break;
    }
    case review::Classification::Book: {
        stroke.setWidthF(std::max(1.0, side * 0.09));
        painter.setPen(stroke);
        painter.setBrush(Qt::NoBrush);
        QPainterPath book;
        book.moveTo(at(0.5, 0.36));
        book.cubicTo(at(0.42, 0.3), at(0.32, 0.3), at(0.24, 0.33));
        book.lineTo(at(0.24, 0.69));
        book.cubicTo(at(0.32, 0.66), at(0.42, 0.66), at(0.5, 0.72));
        book.cubicTo(at(0.58, 0.66), at(0.68, 0.66), at(0.76, 0.69));
        book.lineTo(at(0.76, 0.33));
        book.cubicTo(at(0.68, 0.3), at(0.58, 0.3), at(0.5, 0.36));
        book.lineTo(at(0.5, 0.72));
        painter.drawPath(book);
        break;
    }
    default: {
        const QString glyph = classification == review::Classification::Brilliant    ? "!!"
                              : classification == review::Classification::Great      ? "!"
                              : classification == review::Classification::Inaccuracy ? "?!"
                              : classification == review::Classification::Mistake    ? "?"
                                                                                     : "??";
        auto font = painter.font();
        font.setBold(true);
        font.setPixelSize(std::max(6, qRound(side * (glyph.size() > 1 ? 0.5 : 0.62))));
        font.setLetterSpacing(QFont::PercentageSpacing, 90);
        painter.setFont(font);
        painter.setPen(ink);
        painter.drawText(circle, Qt::AlignCenter, glyph);
        break;
    }
    }
    painter.restore();
}

QPixmap classificationPixmap(review::Classification classification, int size, qreal ratio) {
    QPixmap pixmap(QSize(qRound(size * ratio), qRound(size * ratio)));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    paintClassificationBadge(painter, QRectF(0, 0, size, size), classification);
    return pixmap;
}

void installTheme() { applicationTheme(); }
void setThemeMode(Qt::ColorScheme scheme) { applicationTheme().setMode(scheme); }
void setTextRole(QWidget& widget, const char* role) {
    widget.setProperty("textRole", QString::fromLatin1(role));
    applyTextFont(widget);
}
void setPrimaryAction(QAbstractButton& button) {
    button.setProperty("primary", true);
    setTextRole(button, "strong");
    applyButtonIcon(button);
}
void setQuietAction(QAbstractButton& button) { button.setProperty("quiet", true); }
void setCompactAction(QAbstractButton& button) { button.setProperty("compact", true); }
void setPanelSurface(QWidget& widget) {
    widget.setProperty("surface", QStringLiteral("panel"));
    widget.setAttribute(Qt::WA_StyledBackground);
}
void setSubtleSurface(QWidget& widget) {
    widget.setProperty("surface", QStringLiteral("subtle"));
    widget.setAttribute(Qt::WA_StyledBackground);
}
void setFlatSurface(QWidget& widget) { widget.setProperty("flatSurface", true); }
QFrame* makeSeparator(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setFrameShape(QFrame::NoFrame);
    line->setProperty("separator", true);
    line->setFixedHeight(1);
    return line;
}
QIcon themedIcon(const QString& name, const QColor& color, const QColor& disabled) {
    ensureIconResources();
    QFile file(QStringLiteral(":/icons/%1.svg").arg(name));
    if (!file.open(QIODevice::ReadOnly)) {
        Q_ASSERT_X(false, "themedIcon", "missing bundled icon");
        return {};
    }
    return QIcon(new SvgIconEngine(file.readAll(), color, disabled));
}
void setIcon(QAbstractButton& button, const QString& name) {
    button.setProperty("themeIcon", name);
    if (!button.property("keepIconSize").toBool()) {
        button.setIconSize(QSize(16, 16));
    }
    if (auto* tool = qobject_cast<QToolButton*>(&button);
        tool && tool->defaultAction() && !tool->property("themeIconActionWired").toBool()) {
        tool->setProperty("themeIconActionWired", true);
        QObject::connect(tool->defaultAction(), &QAction::changed, tool,
                         [tool] { applyButtonIcon(*tool); });
        QObject::connect(tool, &QAbstractButton::toggled, tool, [tool] { applyButtonIcon(*tool); });
    }
    applyButtonIcon(button);
}
void setIconOnly(QAbstractButton& button, const QString& name) {
    const auto label = button.text().remove(QLatin1Char('&'));
    if (button.accessibleName().isEmpty()) {
        button.setAccessibleName(label);
    }
    if (button.toolTip().isEmpty()) {
        button.setToolTip(label);
    }
    button.setText({});
    setIcon(button, name);
}
} // namespace chess_review::ui
