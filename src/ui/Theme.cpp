#include "ui/Theme.h"
#include "review/Classification.h"
#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QObject>
#include <QPainter>
#include <QPalette>
#include <QScopedValueRollback>
#include <QString>
#include <QVariant>
#include <QWidget>

namespace chess_review::ui {
namespace {
void applyTextFont(QWidget& widget) {
    const auto role = widget.property("textRole").toString();
    if (role.isEmpty()) {
        return;
    }
    auto font = widget.window()->font();
    if (role == QLatin1String("title") || role == QLatin1String("heading") ||
        role == QLatin1String("metric") || role == QLatin1String("badge")) {
        font.setWeight(QFont::DemiBold);
    }
    const double factor = role == QLatin1String("title")     ? 1.7
                          : role == QLatin1String("heading") ? 1.35
                          : role == QLatin1String("metric")  ? 2.25
                          : role == QLatin1String("lesson")  ? 1.125
                                                             : 1.0;
    if (font.pointSizeF() > 0) {
        font.setPointSizeF(font.pointSizeF() * factor);
    } else {
        font.setPixelSize(qRound(font.pixelSize() * factor));
    }
    widget.setFont(font);
}

class Theme final : public QObject {
  public:
    explicit Theme(QObject* parent) : QObject(parent) {
        qApp->installEventFilter(this);
        apply();
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::ApplicationPaletteChange && !applying_) {
            apply();
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
            if (auto* button = qobject_cast<QAbstractButton*>(watched);
                button && button->property("navigationIcon").isValid()) {
                setNavigationIcon(*button, static_cast<QStyle::StandardPixmap>(
                                               button->property("navigationIcon").toInt()));
            }
        }
        return false;
    }

  private:
    void apply() {
        const QScopedValueRollback guard(applying_, true);
        auto palette = qApp->palette();
        const bool dark = palette.color(QPalette::Window).lightness() < 128;
        // Applying a style can itself post palette events. Rebuild only when the
        // color scheme changes, rather than feeding those events back into Qt.
        if (dark_ == dark) {
            return;
        }
        dark_ = dark;
        const auto root = dark ? "#191f2a" : "#f4f6f8";
        const auto panel = dark ? "#222b38" : "#ffffff";
        const auto subtle = dark ? "#293443" : "#edf1f5";
        const auto text = dark ? "#eef2f7" : "#182230";
        const auto secondary = dark ? "#a9b6c6" : "#586577";
        const auto border = dark ? "#364255" : "#d8dee5";
        const auto accent = dark ? "#65c486" : "#216e40";
        const auto onAccent = dark ? "#101d16" : "#ffffff";
        const auto selected = dark ? "#284635" : "#dfeee4";
        const auto focus = dark ? "#87b8ff" : "#2476ca";
        palette.setColor(QPalette::Window, QColor(root));
        palette.setColor(QPalette::Base, QColor(panel));
        palette.setColor(QPalette::AlternateBase, QColor(subtle));
        palette.setColor(QPalette::WindowText, QColor(text));
        palette.setColor(QPalette::Text, QColor(text));
        palette.setColor(QPalette::Button, QColor(panel));
        palette.setColor(QPalette::ButtonText, QColor(text));
        palette.setColor(QPalette::PlaceholderText, QColor(secondary));
        palette.setColor(QPalette::Highlight, QColor(selected));
        palette.setColor(QPalette::HighlightedText, QColor(text));
        palette.setColor(QPalette::Mid, QColor(border));
        for (const auto role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText}) {
            palette.setColor(QPalette::Disabled, role, QColor(secondary));
        }
        qApp->setPalette(palette);
        auto styleSheet =
            QStringLiteral(R"(
QMainWindow, QDialog { background: %1; }
QWidget { selection-background-color: %9; selection-color: %4; }
QLabel { background: transparent; }
QLabel[textRole="title"] { padding-bottom: 8px; }
QLabel[textRole="secondary"] { color: %5; }
QLabel[textRole="badge"] { padding: 5px 9px; border-radius: 4px; }
QPushButton, QToolButton { background: %2; color: %4; border: 1px solid %6;
    border-radius: 5px; padding: 7px 12px; }
QPushButton:hover, QToolButton:hover { background: %3; }
QPushButton:pressed, QToolButton:pressed { background: %9; }
QPushButton:disabled, QToolButton:disabled { color: %5; background: %1; }
QPushButton:focus, QToolButton:focus, QComboBox:focus, QLineEdit:focus {
    border: 2px solid %10; }
QPushButton[primary="true"], QToolButton[primary="true"] {
    background: %7; color: %8; border-color: %7; font-weight: 600; }
QPushButton[primary="true"]:disabled, QToolButton[primary="true"]:disabled {
    background: %3; color: %5; border-color: %6; }
QWidget#navigationRail { border-right: 1px solid %6; }
QWidget#navigationRail QToolButton { border: none; text-align: left; padding: 10px 14px; }
QWidget#navigationRail QToolButton:checked { background: %9; font-weight: 600; }
QComboBox, QLineEdit { background: %2; color: %4; border: 1px solid %6;
    border-radius: 4px; padding: 6px 8px; }
QAbstractItemView, QPlainTextEdit, QTextEdit { background: %2; color: %4;
    border: 1px solid %6; border-radius: 4px; }
QAbstractItemView { alternate-background-color: %3; gridline-color: %6; }
QHeaderView::section { background: %3; color: %5; border: none;
    border-bottom: 1px solid %6; padding: 6px 8px; }
QTableView::item { padding: 5px 8px; }
QTableView::item:selected { background: %9; color: %4; }
QSplitter::handle { background: %1; }
QSplitter::handle:horizontal { width: 12px; }
QProgressBar { border: none; border-radius: 3px; background: %3; min-height: 5px; }
QProgressBar::chunk { background: %7; border-radius: 3px; }
QScrollArea { border: none; background: transparent; }
QTabWidget::pane { border: 1px solid %6; }
QTabBar::tab { padding: 7px 12px; }
QGroupBox { border: 1px solid %6; border-radius: 5px; margin-top: 12px; padding: 12px; }
)")
                .arg(QLatin1String(root), QLatin1String(panel), QLatin1String(subtle),
                     QLatin1String(text), QLatin1String(secondary), QLatin1String(border),
                     QLatin1String(accent), QLatin1String(onAccent), QLatin1String(selected),
                     QLatin1String(focus));
        for (std::size_t index = 0; index < review::classificationCount; ++index) {
            const auto label = static_cast<review::Classification>(index);
            styleSheet += QStringLiteral("\nQLabel[classification=\"%1\"] { color: %2; }")
                              .arg(static_cast<qulonglong>(index))
                              .arg(classificationColor(label).name());
        }
        qApp->setStyleSheet(styleSheet);
    }
    bool applying_ = false;
    bool applyingFonts_ = false;
    std::optional<bool> dark_;
};
} // namespace
QColor classificationColor(std::optional<review::Classification> classification) {
    const auto palette = qApp->palette();
    if (!classification) {
        return palette.color(QPalette::Mid);
    }
    const bool dark = palette.color(QPalette::Window).lightness() < 128;
    switch (*classification) {
    case review::Classification::Brilliant:
        return QColor(dark ? "#50d2e5" : "#157c8d");
    case review::Classification::Great:
        return QColor(dark ? "#79b4f4" : "#286caf");
    case review::Classification::Miss:
        return QColor(dark ? "#f2a65e" : "#b45d18");
    case review::Classification::Best:
        return QColor(dark ? "#65c486" : "#227843");
    case review::Classification::Excellent:
        return QColor(dark ? "#62bac8" : "#25808e");
    case review::Classification::Good:
        return QColor(dark ? "#65c486" : "#227843");
    case review::Classification::Inaccuracy:
        return QColor(dark ? "#f0c46c" : "#9d660b");
    case review::Classification::Mistake:
        return QColor(dark ? "#f2a65e" : "#b45d18");
    case review::Classification::Blunder:
        return QColor(dark ? "#f07780" : "#c53d49");
    }
    return palette.color(QPalette::Mid);
}

void installTheme() {
    if (!qApp->property("chessReviewTheme").toBool()) {
        QCoreApplication::setAttribute(Qt::AA_UseStyleSheetPropagationInWidgetStyles);
        qApp->setProperty("chessReviewTheme", true);
        auto font = qApp->font();
        if (font.pointSizeF() > 0 && font.pointSizeF() < 10.0) {
            font.setPointSizeF(10.0);
            qApp->setFont(font);
        }
        new Theme(qApp);
    }
}
void setTextRole(QWidget& widget, const char* role) {
    widget.setProperty("textRole", QString::fromLatin1(role));
    applyTextFont(widget);
}
void setPrimaryAction(QAbstractButton& button) { button.setProperty("primary", true); }
void setNavigationIcon(QAbstractButton& button, QStyle::StandardPixmap icon) {
    button.setProperty("navigationIcon", static_cast<int>(icon));
    QIcon tinted;
    for (const auto mode : {QIcon::Normal, QIcon::Disabled}) {
        const auto ratio = button.devicePixelRatioF();
        auto pixmap = button.style()
                          ->standardIcon(icon, nullptr, &button)
                          .pixmap(QSize(qRound(18 * ratio), qRound(18 * ratio)), mode);
        pixmap.setDevicePixelRatio(ratio);
        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(
            pixmap.rect(),
            button.palette().color(mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active,
                                   QPalette::ButtonText));
        painter.end();
        tinted.addPixmap(pixmap, mode);
    }
    button.setIcon(tinted);
}
} // namespace chess_review::ui
