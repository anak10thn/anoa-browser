#pragma once

#include <QList>
#include <QNetworkCookie>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QWidget>

#include "../config/config.h"
#include "tab_ids.h"

class QStackedLayout;
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineView;

// The browser: a container that owns one view per tab, not a view itself.
//
// The layout is a QStackedLayout with no margins and no spacing, so the active
// view fills the container exactly. That is load-bearing rather than cosmetic:
// HttpServer reports this widget's width()/height() as the coordinate space
// /render/click is measured in, and grab() must capture the page and nothing
// else. A margin here would silently shift every synthetic click.
class AnoaBrowser : public QWidget
{
    Q_OBJECT

public:
    explicit AnoaBrowser(const Config &config, QWidget *parent = nullptr);
    void init();

    void loadExtensions(const QStringList &paths);
    void setupNamedProfile(const QString &name, const QString &baseDir);
    QList<QNetworkCookie> getCookies(const QUrl &origin);
    void setCookie(const QNetworkCookie &cookie, const QUrl &origin);
    void clearStorage(const QUrl &origin);

    // ── the registry ────────────────────────────────────────────────────────
    QString newTab(const QUrl &url = QUrl());
    // Refuses to close the last tab: one process still means at least one page.
    bool closeTab(const QString &id);
    bool selectTab(const QString &id);
    QStringList tabIds() const;
    QString activeTabId() const;
    int tabCount() const;
    QWebEngineView *viewFor(const QString &id) const;
    QWebEnginePage *pageFor(const QString &id) const;
    QWebEngineView *activeView() const;

    // The engine's id for a tab, resolved separately because a page exists
    // before its DevTools target does.
    QString chromiumTargetIdFor(const QString &id) const;
    void setChromiumTargetIdFor(const QString &id, const QString &targetId);

    // ── the active tab, under the names call sites already use ──────────────
    // These keep working exactly as they did when this class was the view, so
    // no existing caller changes meaning: they all act on the active tab.
    QWebEnginePage *page() const;
    void load(const QUrl &url);
    void back();
    void forward();
    void reload();
    QUrl url() const;
    QString title() const;

    void sendClick(const QPoint &pos, Qt::MouseButton button);
    void sendScroll(const QPoint &pos, int angleDeltaY);
    void sendText(const QString &text);
    bool sendKey(const QString &keyName);

signals:
    void tabCreated(const QString &id);
    void tabClosed(const QString &id);
    void tabActivated(const QString &id);

    // BrowserWindow can no longer connect to one fixed view, so these carry
    // whichever tab is active and are re-emitted when the active tab changes.
    void activeUrlChanged(const QUrl &url);
    void activeTitleChanged(const QString &title);
    void activeLoadFinished(bool ok);

private:
    // Every tab is built here so they are identical: same settings, same
    // viewport size, same headless handling.
    QWebEngineView *createView(QWebEngineProfile *profile);
    int indexOf(const QString &id) const;

    struct Tab {
        QString id;
        QWebEngineView *view = nullptr;
        QWebEngineProfile *profile = nullptr;
        QString chromiumTargetId;
    };

    Config m_config;
    QWebEngineProfile *m_profile;
    QStackedLayout *m_stack;
    QList<Tab> m_tabs; // creation order
    QString m_activeTabId;
    TabIdMinter m_minter;
};
