#include "TestEnvironment.h"
#include "app/StartupCoordinator.h"
#include "review/Classification.h"
#include "ui/Theme.h"
#include "ui/chess/ChessBoardWidget.h"
#include "ui/shell/MainWindow.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QSvgRenderer>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <array>
#include <cmath>

using namespace chess_review;

namespace {
double linearChannel(int component) {
    const double srgb = static_cast<double>(component) / 255.0;
    return srgb <= 0.03928 ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QColor& color) {
    return 0.2126 * linearChannel(color.red()) + 0.7152 * linearChannel(color.green()) +
           0.0722 * linearChannel(color.blue());
}

double contrastRatio(const QColor& first, const QColor& second) {
    const double l1 = relativeLuminance(first);
    const double l2 = relativeLuminance(second);
    return (std::max(l1, l2) + 0.05) / (std::min(l1, l2) + 0.05);
}

bool isInternalQtWidget(const QWidget& widget) {
    if (widget.objectName().startsWith(QStringLiteral("qt_"))) {
        return true;
    }
    const auto* parent = widget.parentWidget();
    if (!parent) {
        return false;
    }
    return qobject_cast<const QComboBox*>(parent) != nullptr ||
           qobject_cast<const QAbstractSpinBox*>(parent) != nullptr ||
           qobject_cast<const QLineEdit*>(parent) != nullptr ||
           qobject_cast<const QTabBar*>(parent) != nullptr;
}

class ApplicationFixture final {
  public:
    explicit ApplicationFixture(const QString& rootPath) : window(navigation, startup.analysis()) {
        window.installEngines(startup.engines());
        window.installSettings(startup.reviewConfiguration());
        window.installEngineAnalysis(startup.engineAnalysis());
        window.installImports(startup.localImports(), startup.library());
        window.installReview(startup.reviewGame());
        window.installPuzzles(startup.puzzles(), startup.puzzleGeneration());
        QObject::connect(&startup, &app::StartupCoordinator::ready, &window,
                         &ui::MainWindow::showReady);
        QObject::connect(&window, &ui::MainWindow::closeRequested, &startup,
                         &app::StartupCoordinator::shutdown);

        QSignalSpy ready(&startup, &app::StartupCoordinator::ready);
        QSignalSpy failed(&startup, &app::StartupCoordinator::failed);
        startup.start(rootPath + QStringLiteral("/library.sqlite3"),
                      rootPath + QStringLiteral("/ui-state.ini"),
                      rootPath + QStringLiteral("/config.toml"));
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&startup, &app::StartupCoordinator::ready, &loop, &QEventLoop::quit);
        QObject::connect(&startup, &app::StartupCoordinator::failed, &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        if (ready.isEmpty() && failed.isEmpty()) {
            timeout.start(5000);
            loop.exec();
        }
        if (ready.count() != 1 || !failed.isEmpty()) {
            qFatal("Accessibility application fixture failed to start");
        }
    }

    ~ApplicationFixture() {
        QEventLoop loop;
        bool stopped = false;
        QObject::connect(&startup, &app::StartupCoordinator::finished, &loop, [&] {
            stopped = true;
            loop.quit();
        });
        QTimer::singleShot(6000, &loop, &QEventLoop::quit);
        startup.shutdown();
        if (startup.state() != app::Lifecycle::Stopped) {
            loop.exec();
        } else {
            stopped = true;
        }
        if (!stopped) {
            qFatal("Accessibility application fixture did not close");
        }
        window.completeShutdown();
    }

    app::StartupCoordinator startup;
    app::NavigationController navigation;
    ui::MainWindow window;
};

QWidget* buddyFor(const QWidget& root, const QWidget* target) {
    for (auto* label : root.findChildren<QLabel*>()) {
        if (label->buddy() == target && !label->text().trimmed().isEmpty()) {
            return label;
        }
    }
    return nullptr;
}

void clickKey(QWidget* widget, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QTest::keyPress(widget, key, modifiers);
    QTest::keyRelease(widget, key, modifiers);
}

QDialog* findVisibleNoticeDialog(const ui::MainWindow& window, const QString& title) {
    for (auto* dialog : window.findChildren<QDialog*>()) {
        if (dialog->isVisible() && dialog->windowTitle() == title) {
            return dialog;
        }
    }
    return nullptr;
}
} // namespace

class AccessibilityTest final : public QObject {
    Q_OBJECT

  private slots:
    void init();
    void svgPieceProvenanceAndAttribution();
    void lightAndDarkThemeContrast();
    void allPagesAccessibleNamesAndIconButtons();
    void keyboardOnlyNavigationAndDialogs();
};

void AccessibilityTest::init() {
    QTest::failOnWarning(QRegularExpression(
        QStringLiteral("^(?!This plugin does not support propagateSizeHints\\(\\)$).*")));
}

void AccessibilityTest::svgPieceProvenanceAndAttribution() {
    app::NavigationController navigation;
    ui::MainWindow window(navigation);
    ui::ChessBoardWidget board;
    Q_UNUSED(window);
    Q_UNUSED(board);

    QFile license(QStringLiteral(":/pieces/LICENSE.txt"));
    QVERIFY(license.open(QIODevice::ReadOnly));
    const QString licenseText = QString::fromUtf8(license.readAll());
    QVERIFY(licenseText.contains(QStringLiteral("Colin M. L. Burnett")));
    QVERIFY(licenseText.contains(QStringLiteral("BSD")));

    QFile provenanceFile(QStringLiteral(":/pieces/provenance.json"));
    QVERIFY(provenanceFile.open(QIODevice::ReadOnly));
    QJsonParseError parseError{};
    const auto document = QJsonDocument::fromJson(provenanceFile.readAll(), &parseError);
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QVERIFY(document.isArray());
    const auto entries = document.array();
    QCOMPARE(entries.size(), 12);

    QFile noticesFile(QStringLiteral(":/notices/THIRD_PARTY_NOTICES.md"));
    QVERIFY(noticesFile.open(QIODevice::ReadOnly));
    const QString noticesText = QString::fromUtf8(noticesFile.readAll());
    QVERIFY(!noticesText.isEmpty());
    QVERIFY(!noticesText.contains(QChar(0x2014)));
    QVERIFY(noticesText.contains(QStringLiteral("Colin M. L. Burnett")));
    QVERIFY(noticesText.contains(QStringLiteral("MIT")));
    QVERIFY(noticesText.contains(QStringLiteral("CC0 1.0")));
    QVERIFY(noticesText.contains(QStringLiteral("LGPLv3")));

    for (const auto& entryValue : entries) {
        QVERIFY(entryValue.isObject());
        const auto entry = entryValue.toObject();
        const QString fileName = entry.value(QStringLiteral("file")).toString();
        const QString expectedSha = entry.value(QStringLiteral("sha256")).toString();
        QVERIFY(fileName.startsWith(QStringLiteral("Chess_")) &&
                fileName.endsWith(QStringLiteral(".svg")));
        QVERIFY2(noticesText.contains(fileName), qPrintable(fileName));

        QFile svgFile(QStringLiteral(":/pieces/") + fileName);
        QVERIFY2(svgFile.open(QIODevice::ReadOnly), qPrintable(fileName));
        const QByteArray bytes = svgFile.readAll();
        const QString actualSha = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        QCOMPARE(actualSha.toLower(), expectedSha.toLower());

        QSvgRenderer renderer(bytes);
        QVERIFY2(renderer.isValid(), qPrintable(fileName));
        QVERIFY(!renderer.defaultSize().isEmpty());
    }
}

void AccessibilityTest::lightAndDarkThemeContrast() {
    const auto restore = qScopeGuard([] { ui::setThemeMode(Qt::ColorScheme::Unknown); });

    constexpr std::array classifications{
        review::Classification::Brilliant, review::Classification::Great,
        review::Classification::Miss,      review::Classification::Best,
        review::Classification::Excellent, review::Classification::Good,
        review::Classification::Book,      review::Classification::Inaccuracy,
        review::Classification::Mistake,   review::Classification::Blunder};

    for (const auto scheme : {Qt::ColorScheme::Light, Qt::ColorScheme::Dark}) {
        ui::setThemeMode(scheme);
        const auto palette = qApp->palette();
        const std::array surfaces{QPalette::Window, QPalette::Base, QPalette::AlternateBase,
                                  QPalette::Highlight};
        const std::array foregrounds{QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
                                     QPalette::HighlightedText, QPalette::PlaceholderText};
        for (const auto bgRole : surfaces) {
            const auto bg = palette.color(bgRole);
            for (const auto fgRole : foregrounds) {
                const auto fg = palette.color(fgRole);
                const double ratio = contrastRatio(fg, bg);
                QVERIFY2(ratio >= 4.5, qPrintable(QStringLiteral("scheme=%1 fg=%2 bg=%3 ratio=%4")
                                                      .arg(static_cast<int>(scheme))
                                                      .arg(fgRole)
                                                      .arg(bgRole)
                                                      .arg(ratio, 0, 'f', 2)));
            }
        }
        for (const auto bgRole : {QPalette::Window, QPalette::Base, QPalette::AlternateBase}) {
            const double linkRatio =
                contrastRatio(palette.color(QPalette::Link), palette.color(bgRole));
            QVERIFY2(linkRatio >= 4.5,
                     qPrintable(QStringLiteral("link contrast=%1").arg(linkRatio, 0, 'f', 2)));
        }
        const double tipRatio = contrastRatio(palette.color(QPalette::ToolTipText),
                                              palette.color(QPalette::ToolTipBase));
        QVERIFY(tipRatio >= 4.5);

        for (const auto classification : classifications) {
            const auto fg = ui::classificationColor(classification);
            for (const auto bgRole : {QPalette::Window, QPalette::Base, QPalette::AlternateBase}) {
                const double ratio = contrastRatio(fg, palette.color(bgRole));
                QVERIFY2(ratio >= 4.5,
                         qPrintable(QStringLiteral("scheme=%1 class=%2 bg=%3 ratio=%4")
                                        .arg(static_cast<int>(scheme))
                                        .arg(static_cast<int>(classification))
                                        .arg(bgRole)
                                        .arg(ratio, 0, 'f', 2)));
            }
            const auto fill = ui::classificationFill(classification);
            const double badgeContrast =
                std::max(contrastRatio(fill, QColor(QStringLiteral("#ffffff"))),
                         contrastRatio(fill, QColor(QStringLiteral("#1b2129"))));
            QVERIFY2(badgeContrast >= 3.0,
                     qPrintable(QStringLiteral("badge contrast=%1").arg(badgeContrast, 0, 'f', 2)));
        }
    }
}

void AccessibilityTest::allPagesAccessibleNamesAndIconButtons() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    ApplicationFixture fixture(root.path());
    fixture.window.resize(1360, 850);
    fixture.window.show();
    QCoreApplication::processEvents();

    auto* pages = fixture.window.findChild<QStackedWidget*>(QStringLiteral("destinationPages"));
    QVERIFY(pages);

    const auto auditCurrentPage = [&fixture] {
        QCoreApplication::processEvents();
        for (auto* widget : fixture.window.findChildren<QWidget*>()) {
            if (!widget->isVisible() || !widget->isEnabled() || isInternalQtWidget(*widget)) {
                continue;
            }
            const bool isInteractive = qobject_cast<QAbstractButton*>(widget) != nullptr ||
                                       qobject_cast<QComboBox*>(widget) != nullptr ||
                                       qobject_cast<QSpinBox*>(widget) != nullptr ||
                                       qobject_cast<QLineEdit*>(widget) != nullptr ||
                                       qobject_cast<QPlainTextEdit*>(widget) != nullptr ||
                                       qobject_cast<QTableView*>(widget) != nullptr ||
                                       qobject_cast<QListView*>(widget) != nullptr;
            if (!isInteractive) {
                continue;
            }
            const QString accessible = widget->accessibleName().trimmed();
            if (const auto* button = qobject_cast<const QAbstractButton*>(widget)) {
                const QString cleanedText = button->text().remove(u'&').trimmed();
                if (cleanedText.isEmpty()) {
                    QVERIFY2(
                        !accessible.isEmpty(),
                        qPrintable(QStringLiteral("Icon-only button missing accessibleName: %1")
                                       .arg(button->objectName())));
                    QVERIFY2(!button->toolTip().trimmed().isEmpty(),
                             qPrintable(QStringLiteral("Icon-only button missing toolTip: %1")
                                            .arg(button->objectName())));
                }
                QVERIFY2(
                    !accessible.isEmpty() || !cleanedText.isEmpty(),
                    qPrintable(QStringLiteral("Unlabeled button: %1").arg(button->objectName())));
                continue;
            }
            const bool hasLabel =
                !accessible.isEmpty() || buddyFor(fixture.window, widget) != nullptr;
            QVERIFY2(hasLabel,
                     qPrintable(QStringLiteral("Control missing label/accessibleName: %1 (%2)")
                                    .arg(widget->objectName(),
                                         QString::fromLatin1(widget->metaObject()->className()))));
        }
    };

    const std::array destinations{"homeAction", "gamesAction", "puzzlesAction", "analyzeAction",
                                  "settingsAction"};
    for (const auto* actionName : destinations) {
        auto* action = fixture.window.findChild<QAction*>(QString::fromLatin1(actionName));
        QVERIFY(action);
        action->trigger();
        if (QString::fromLatin1(actionName) == QStringLiteral("settingsAction")) {
            auto* tabs = fixture.window.findChild<QTabWidget*>(QStringLiteral("settingsSections"));
            QVERIFY(tabs);
            for (int i = 0; i < tabs->count(); ++i) {
                tabs->setCurrentIndex(i);
                auditCurrentPage();
            }
            tabs->setCurrentIndex(0);
        } else {
            auditCurrentPage();
        }
    }
}

void AccessibilityTest::keyboardOnlyNavigationAndDialogs() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    ApplicationFixture fixture(root.path());
    fixture.window.resize(1360, 850);
    fixture.window.show();
    QVERIFY(QTest::qWaitForWindowActive(&fixture.window));

    const std::array<std::pair<Qt::Key, app::Destination>, 5> shortcuts{{
        {Qt::Key_2, app::Destination::Games},
        {Qt::Key_3, app::Destination::Puzzles},
        {Qt::Key_4, app::Destination::Analyze},
        {Qt::Key_5, app::Destination::Settings},
        {Qt::Key_1, app::Destination::Home},
    }};
    for (const auto& [key, expected] : shortcuts) {
        clickKey(&fixture.window, key, Qt::ControlModifier);
        QTRY_COMPARE(fixture.navigation.current(), expected);
    }

    clickKey(&fixture.window, Qt::Key_2, Qt::ControlModifier);
    QTRY_COMPARE(fixture.navigation.current(), app::Destination::Games);
    auto* gamesTable = fixture.window.findChild<QTableView*>(QStringLiteral("gamesTable"));
    QVERIFY(gamesTable);
    gamesTable->setFocus();
    QCoreApplication::processEvents();
    clickKey(gamesTable, Qt::Key_F, Qt::ControlModifier);
    auto* gameSearch = fixture.window.findChild<QLineEdit*>(QStringLiteral("gameSearch"));
    QVERIFY(gameSearch);
    QTRY_VERIFY(gameSearch->hasFocus());

    for (const auto& [key, destination] : shortcuts) {
        clickKey(&fixture.window, key, Qt::ControlModifier);
        QTRY_COMPARE(fixture.navigation.current(), destination);
        auto* sidebarHome = fixture.window.findChild<QToolButton*>(QStringLiteral("homeButton"));
        QVERIFY(sidebarHome);
        sidebarHome->setFocus();
        QCoreApplication::processEvents();
        QSet<QWidget*> visited;
        for (int step = 0; step < 10; ++step) {
            QWidget* current = QApplication::focusWidget();
            clickKey(current ? current : &fixture.window, Qt::Key_Tab);
            QCoreApplication::processEvents();
            if (auto* focused = QApplication::focusWidget()) {
                visited.insert(focused);
            }
        }
        QVERIFY2(visited.size() >= 3,
                 qPrintable(QStringLiteral("Destination %1 visited only %2 focus stops")
                                .arg(static_cast<int>(destination))
                                .arg(visited.size())));
    }

    auto* artworkAction =
        fixture.window.findChild<QAction*>(QStringLiteral("artworkLicenseAction"));
    QVERIFY(artworkAction);
    artworkAction->trigger();
    QTRY_VERIFY(findVisibleNoticeDialog(fixture.window, tr("Chess piece artwork license")) !=
                nullptr);
    auto* artworkDialog =
        findVisibleNoticeDialog(fixture.window, tr("Chess piece artwork license"));
    auto* artworkText = artworkDialog->findChild<QPlainTextEdit*>();
    QVERIFY(artworkText && artworkText->toPlainText().contains(QStringLiteral("Burnett")));
    clickKey(artworkDialog, Qt::Key_Escape);
    QTRY_VERIFY(findVisibleNoticeDialog(fixture.window, tr("Chess piece artwork license")) ==
                nullptr);

    auto* noticesAction =
        fixture.window.findChild<QAction*>(QStringLiteral("thirdPartyNoticesAction"));
    QVERIFY(noticesAction);
    noticesAction->trigger();
    QTRY_VERIFY(findVisibleNoticeDialog(fixture.window, tr("Third-party notices")) != nullptr);
    auto* noticesDialog = findVisibleNoticeDialog(fixture.window, tr("Third-party notices"));
    auto* noticesText = noticesDialog->findChild<QPlainTextEdit*>();
    QVERIFY(noticesText && noticesText->toPlainText().contains(QStringLiteral("Cburnett")));
    clickKey(noticesDialog, Qt::Key_Escape);
    QTRY_VERIFY(findVisibleNoticeDialog(fixture.window, tr("Third-party notices")) == nullptr);
}

int main(int argc, char* argv[]) {
    const TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("chess-review"));
    application.setQuitOnLastWindowClosed(false);
    AccessibilityTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "AccessibilityTest.moc"
