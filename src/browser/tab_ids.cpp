#include "browser/tab_ids.h"

#include <QJsonObject>
#include <QRegularExpression>

QString TabIdMinter::next()
{
    return QStringLiteral("t") + QString::number(++m_counter);
}

bool isValidTabId(const QString &id)
{
    // Anchored at both ends: a partial match would accept "t1junk".
    static const QRegularExpression re(QStringLiteral("\\At[1-9][0-9]*\\z"));
    return re.match(id).hasMatch();
}

bool isValidTabName(const QString &name)
{
    static const QRegularExpression re(
        QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9_-]{0,31}\\z"));
    if (!re.match(name).hasMatch())
        return false;
    // A name that reads like an id would make --tab ambiguous.
    return !isValidTabId(name);
}

QJsonArray buildTargetList(const QList<TabTargetInfo> &tabs,
                           const QString &host,
                           quint16 proxyPort)
{
    QJsonArray out;
    const QString authority = host + QStringLiteral(":") + QString::number(proxyPort);

    for (const TabTargetInfo &tab : tabs) {
        if (tab.chromiumTargetId.isEmpty())
            continue;

        const QString wsPath =
            QStringLiteral("/devtools/page/") + tab.chromiumTargetId;

        QJsonObject entry;
        // "description" carries no information for a page target, but Chromium
        // emits it and this document has to keep looking like the one clients
        // already read.
        entry[QStringLiteral("description")] = QString();
        entry[QStringLiteral("devtoolsFrontendUrl")] =
            QStringLiteral("/devtools/inspector.html?ws=") + authority + wsPath;
        entry[QStringLiteral("id")] = tab.chromiumTargetId;
        entry[QStringLiteral("title")] = tab.title;
        entry[QStringLiteral("type")] = QStringLiteral("page");
        entry[QStringLiteral("url")] = tab.url;
        entry[QStringLiteral("webSocketDebuggerUrl")] =
            QStringLiteral("ws://") + authority + wsPath;
        // The two anoa-only keys. A CDP client that does not know them ignores
        // them; `anoa --tab` is the reason they are here.
        entry[QStringLiteral("anoaTabId")] = tab.tabId;
        entry[QStringLiteral("anoaActive")] = tab.active;
        // Only when there is one: an empty key would read as "named nothing"
        // rather than "unnamed".
        if (!tab.tabName.isEmpty())
            entry[QStringLiteral("anoaTabName")] = tab.tabName;

        out.append(entry);
    }

    return out;
}
