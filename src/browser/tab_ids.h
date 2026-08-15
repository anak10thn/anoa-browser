#pragma once

#include <QJsonArray>
#include <QList>
#include <QString>

// Tab identity, and the discovery document that publishes it.
//
// Qt6::Core only, deliberately: this is the part of the tab feature that can be
// reached by the unit suite, which builds no WebEngine at all. Nothing here may
// grow a QtGui, QtNetwork, QtWebSockets or WebEngine include.

// Mints the short ids an agent holds on to: t1, t2, t3 ...
//
// The counter only ever goes up. A closed tab's id is never handed out again,
// because an agent may have captured it in an earlier command and would
// otherwise find itself driving a different page than the one it named.
class TabIdMinter
{
public:
    QString next();

private:
    quint64 m_counter = 0;
};

// Exactly ^t[1-9][0-9]*$ — so junk is rejected at the CLI and HTTP edges rather
// than reaching the registry. Leading zeros are invalid because "t01" and "t1"
// would otherwise be two spellings of one tab.
bool isValidTabId(const QString &id);

// One tab, as the discovery document needs to see it.
//
// tabId is ours and stable; chromiumTargetId is the engine's and changes when a
// page is recreated, which is the whole reason the two are kept apart.
struct TabTargetInfo {
    QString tabId;
    QString chromiumTargetId;
    QString title;
    QString url;
    QString browserContextId;
    bool active = false;
};

// Builds the /json/list array: one entry per tab, in the order given.
//
// The URLs point at the CDP proxy (port + 2) rather than Chromium's own
// debugging port (port + 1), because the proxy is the endpoint that answers the
// commands anoa handles itself. Tabs whose chromiumTargetId is still empty are
// omitted — a page exists before its DevTools target does, and advertising one
// that cannot be attached to is worse than not advertising it yet.
QJsonArray buildTargetList(const QList<TabTargetInfo> &tabs,
                           const QString &host,
                           quint16 proxyPort);
