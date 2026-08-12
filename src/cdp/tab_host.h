#pragma once

#include <functional>

#include <QString>
#include <QStringList>
#include <QUrl>

// What the Target domain needs from whoever owns the tabs.
//
// A pure interface rather than an AnoaBrowser include: src/cdp knows nothing
// about src/browser, and everything on the other side of this seam is a
// QWebEngine type the CDP layer has no business naming.
class TabHost
{
public:
    virtual ~TabHost() = default;

    virtual QString newTab(const QUrl &url, const QString &profileName, bool isolated) = 0;
    virtual bool closeTab(const QString &tabId) = 0;
    virtual bool selectTab(const QString &tabId) = 0;

    virtual QStringList tabIds() const = 0;
    virtual QString activeTabId() const = 0;

    // Our id and the engine's, which are not the same thing and must not be
    // confused: a Chromium target id changes when a page is recreated.
    virtual QString targetIdFor(const QString &tabId) const = 0;
    virtual QString tabIdForTargetId(const QString &targetId) const = 0;

    virtual QString titleFor(const QString &tabId) const = 0;
    virtual QString urlFor(const QString &tabId) const = 0;
    virtual QString browserContextIdFor(const QString &tabId) const = 0;
    // A context id this registry minted, and a tab created inside it. Returns
    // empty for an id we never issued, so the caller can say so rather than
    // quietly opening the tab somewhere else.
    virtual bool knowsBrowserContext(const QString &contextId) const = 0;
    virtual QString newTabInBrowserContext(const QUrl &url, const QString &contextId) = 0;

    // Calls back once the tab has a Chromium target id — immediately if it
    // already has one. This is what makes Target.createTarget answerable at
    // all: the page exists before its target does.
    virtual void whenTargetResolved(const QString &tabId,
                                    std::function<void(const QString &targetId)> cb) = 0;
};
