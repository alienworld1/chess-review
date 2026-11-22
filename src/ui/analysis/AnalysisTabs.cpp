#include "ui/analysis/AnalysisPage.h"
#include <QAction>
#include <QKeySequence>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTabBar>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace chess_review::ui {
void AnalysisPage::installGameTabs(QAction& closeAction) {
    tabs_ = new QTabBar(this);
    tabs_->setObjectName(QStringLiteral("analysisGameTabs"));
    tabs_->setAccessibleName(tr("Open analysis games"));
    tabs_->setAutoHide(true);
    tabs_->setTabsClosable(true);
    tabs_->setExpanding(false);
    tabs_->setElideMode(Qt::ElideRight);
    tabs_->setUsesScrollButtons(true);
    qobject_cast<QVBoxLayout*>(layout())->insertWidget(0, tabs_);
    closeTab_ = &closeAction;
    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0) {
            selectGameTab(tabs_->tabData(index).toLongLong());
        }
    });
    connect(tabs_, &QTabBar::tabCloseRequested, this,
            [this](int index) { closeTab(tabs_->tabData(index).toLongLong()); });
    connect(closeTab_, &QAction::triggered, this, [this] { closeTab(controller_.activeTab()); });
    auto* nextTab = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Tab), this);
    nextTab->setContext(Qt::WidgetWithChildrenShortcut);
    connect(nextTab, &QShortcut::activated, this, [this] {
        if (!controller_.busy() && tabs_->count() > 1) {
            tabs_->setCurrentIndex((tabs_->currentIndex() + 1) % tabs_->count());
        }
    });
    refreshTabs();
}
void AnalysisPage::refreshTabs() {
    if (!tabs_) {
        return;
    }
    const auto tabs = controller_.tabs();
    const QSignalBlocker blocker(tabs_);
    if (!std::ranges::equal(tabs, displayedTabs_, {}, &app::AnalysisTab::id,
                            &app::AnalysisTab::id)) {
        while (tabs_->count() > 0) {
            tabs_->removeTab(tabs_->count() - 1);
        }
        for (const auto& tab : tabs) {
            const int index = tabs_->addTab(QString{});
            tabs_->setTabData(index, static_cast<qlonglong>(tab.id));
            if (tab.id == 0) {
                for (const auto side : {QTabBar::LeftSide, QTabBar::RightSide}) {
                    if (auto* button = tabs_->tabButton(index, side)) {
                        button->deleteLater();
                        tabs_->setTabButton(index, side, nullptr);
                    }
                }
            }
        }
    }
    if (tabs != displayedTabs_) {
        for (std::size_t index = 0; index < tabs.size(); ++index) {
            const auto& tab = tabs[index];
            const auto display = tab.dirty ? tr("%1 *").arg(tab.title) : tab.title;
            const auto tooltip = tab.dirty ? tr("%1 (unsaved edits)").arg(tab.title) : tab.title;
            tabs_->setTabText(static_cast<int>(index),
                              QString(display).replace(u'&', QStringLiteral("&&")));
            tabs_->setTabToolTip(static_cast<int>(index),
                                 QStringLiteral("<qt>%1</qt>").arg(tooltip.toHtmlEscaped()));
            tabs_->setAccessibleTabName(static_cast<int>(index), tooltip);
        }
        displayedTabs_ = tabs;
    }
    const auto active = std::ranges::find(tabs, controller_.activeTab(), &app::AnalysisTab::id);
    tabs_->setCurrentIndex(static_cast<int>(active - tabs.begin()));
    tabs_->setEnabled(!controller_.busy() && !tabAction_);
    closeTab_->setEnabled(isVisible() && tabs_->isEnabled() && controller_.activeTab() > 0);
}
void AnalysisPage::selectGameTab(persistence::GameId id) {
    if (!tabAction_ && !controller_.busy()) {
        controller_.selectTab(id);
    } else if (!tabAction_) {
        useTab(id, [] {});
    }
}
void AnalysisPage::closeTab(persistence::GameId id) {
    if (id > 0) {
        useTab(id, [this, id] {
            requestDiscard([this, id] {
                if (controller_.activeTab() == id) {
                    controller_.closeActiveGameTab();
                }
            });
        });
    }
}
void AnalysisPage::useTab(persistence::GameId id, std::function<void()> action) {
    if (tabAction_ || (id != 0 && !controller_.hasGameTab(id))) {
        return;
    }
    tabAction_ = TabAction{id, std::move(action)};
    finishTabAction();
}
void AnalysisPage::finishTabAction() {
    if (!tabAction_ || controller_.busy()) {
        return;
    }
    if (controller_.activeTab() == tabAction_->id) {
        auto action = std::exchange(tabAction_, std::nullopt);
        refreshTabs();
        action->action();
    } else if (!tabAction_->selecting) {
        tabAction_->selecting = true;
        controller_.selectTab(tabAction_->id);
    } else {
        tabAction_.reset();
        refreshTabs();
    }
}
void AnalysisPage::requestDiscardTabs(std::function<void()> onDiscard) {
    const auto original = controller_.activeTab();
    useTab(original, [this, original, onDiscard = std::move(onDiscard)]() mutable {
        std::vector<persistence::GameId> remaining;
        for (const auto& tab : controller_.tabs()) {
            if (tab.id != original && tab.dirty) {
                remaining.push_back(tab.id);
            }
        }
        remaining.push_back(original);
        confirmTabs(std::move(remaining), original, std::move(onDiscard));
    });
}
void AnalysisPage::confirmTabs(std::vector<persistence::GameId> remaining,
                               persistence::GameId original, std::function<void()> onDiscard) {
    if (remaining.empty()) {
        useTab(original, std::move(onDiscard));
        return;
    }
    const auto id = remaining.back();
    remaining.pop_back();
    useTab(id, [this, original, remaining = std::move(remaining),
                onDiscard = std::move(onDiscard)]() mutable {
        requestDiscard([this, original, remaining = std::move(remaining),
                        onDiscard = std::move(onDiscard)]() mutable {
            confirmTabs(std::move(remaining), original, std::move(onDiscard));
        });
    });
}
} // namespace chess_review::ui
