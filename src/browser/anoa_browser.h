#pragma once

#include <QHash>
#include <QList>
#include <QNetworkCookie>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QWidget>

#include "../config/config.h"
#include "../cdp/tab_host.h"
#include "tab_ids.h"

class QNetworkAccessManager;
class QResizeEvent;
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
class AnoaBrowser : public QWidget, public TabHost
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
    // TabHost. Neither profile argument given means the shared default, so a
    // login in one tab is a login everywhere — today's behaviour.
    QString newTab(const QUrl &url = QUrl(), const QString &profileName = QString(),
                   bool isolated = false) override;
    // Refuses to close the last tab: one process still means at least one page.
    bool closeTab(const QString &id) override;
    bool selectTab(const QString &id) override;
    QStringList tabIds() const override;
    QString activeTabId() const override;
    QString targetIdFor(const QString &tabId) const override;
    QString tabIdForTargetId(const QString &targetId) const override;
    QString titleFor(const QString &tabId) const override;
    QString urlFor(const QString &tabId) const override;
    QString browserContextIdFor(const QString &tabId) const override;
    bool knowsBrowserContext(const QString &contextId) const override;
    QString newTabInBrowserContext(const QUrl &url, const QString &contextId) override;
    void whenTargetResolved(const QString &tabId,
                            std::function<void(const QString &targetId)> cb) override;
    int tabCount() const;
    QWebEngineView *viewFor(const QString &id) const;
    QWebEnginePage *pageFor(const QString &id) const;
    QWebEngineView *activeView() const;
    // The view a request means: the named tab, or the active one when unnamed.
    QWebEngineView *viewForOrActive(const QString &tabId) const;

    // The engine's id for a tab: what a CDP client dials, and what /json/list
    // and Target.* have to report. Qt exposes no such API on QWebEnginePage, so
    // it is discovered from Chromium's own discovery endpoint and cached. Empty
    // until resolution succeeds — a page exists a beat before its target does.
    QString chromiumTargetId(const QString &tabId) const;

    // What a test needs to see about profiles, without handing out the objects.
    QString profileNameFor(const QString &tabId) const;
    bool tabsShareProfile(const QString &a, const QString &b) const;

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

    // An empty tabId means the active tab, which is what every caller that
    // has never heard of tabs passes.
    void sendClick(const QPoint &pos, Qt::MouseButton button, const QString &tabId = QString());
    void sendScroll(const QPoint &pos, int angleDeltaY, const QString &tabId = QString());
    void sendText(const QString &text, const QString &tabId = QString());
    bool sendKey(const QString &keyName, const QString &tabId = QString());

signals:
    void tabCreated(const QString &id);
    void tabClosed(const QString &id);
    void tabActivated(const QString &id);
    void tabTargetResolved(const QString &tabId, const QString &targetId);

    // BrowserWindow can no longer connect to one fixed view, so these carry
    // whichever tab is active and are re-emitted when the active tab changes.
    void activeUrlChanged(const QUrl &url);
    void activeTitleChanged(const QString &title);
    void activeLoadFinished(bool ok);

protected:
    // Every tab is sized like the container, not just the visible one.
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Tab {
        QString id;
        QWebEngineView *view = nullptr;
        QWebEngineProfile *profile = nullptr;
        QString profileName; // empty = the shared default
        QString chromiumTargetId;
    };

    // Every tab is built here so they are identical: same settings, same
    // viewport size, same headless handling.
    QWebEngineView *createView(QWebEngineProfile *profile);
    int indexOf(const QString &id) const;
    // One object per name. Two Qt profiles over one on-disk path is a
    // corruption risk, not a duplicate, so a name is looked up before it is
    // created.
    QWebEngineProfile *profileFor(const QString &name, bool isolated);
    // The half of tab creation both entry points share, once the profile is
    // decided.
    QString finishNewTab(Tab &tab, const QUrl &url);
    void releaseProfile(QWebEngineProfile *profile);
    // Asynchronous by construction: a seam is crossed by signals, never by a
    // blocking call, and the answer does not exist yet when the tab is created.
    void resolveTargetId(const QString &tabId, int attempt);

    Config m_config;
    QWebEngineProfile *m_profile;
    QStackedLayout *m_stack;
    QNetworkAccessManager *m_nam;
    QList<Tab> m_tabs; // creation order
    // Named profiles, and how many tabs still hold each one. A profile has to
    // outlive every page using it, so it is destroyed only when its last tab
    // goes — and never for the default, which lives as long as the process.
    QHash<QString, QWebEngineProfile *> m_profilesByName;
    QHash<QWebEngineProfile *, int> m_profileUsers;
    // A CDP browser context is a profile as a client sees it. Minted per
    // profile OBJECT, so two tabs sharing a profile report one id and an
    // isolated tab reports its own.
    QHash<QWebEngineProfile *, QString> m_contextIds;
    int m_nextContextId = 0;
    QString m_activeTabId;
    TabIdMinter m_minter;
};
