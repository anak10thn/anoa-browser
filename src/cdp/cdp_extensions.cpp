#include "cdp/cdp_extensions.h"

#include <QJsonDocument>
#include <QJsonObject>

static QString stubResult(const QJsonObject &cmd)
{
    QJsonObject resp;
    resp[QStringLiteral("id")] = cmd.value(QStringLiteral("id")).toInt();
    resp[QStringLiteral("result")] = QJsonObject();
    return QJsonDocument(resp).toJson(QJsonDocument::Compact);
}

QString CdpExtensions::processCommand(const QJsonObject &cmd, QWebEnginePage *page)
{
    const QString method = cmd.value(QStringLiteral("method")).toString();
    const int dotPos = method.indexOf(QLatin1Char('.'));
    if (dotPos < 0)
        return QString();

    const QString domain = method.left(dotPos);

    if (domain == QLatin1String("Profiler"))
        return handleProfiler(cmd);
    if (domain == QLatin1String("HeapProfiler"))
        return handleHeapProfiler(cmd);
    if (domain == QLatin1String("Security"))
        return handleSecurity(cmd, page);

    return QString();
}

QString CdpExtensions::handleProfiler(const QJsonObject &cmd)
{
    // All Profiler commands are stubbed — Qt has no direct V8 profiler API.
    return stubResult(cmd);
}

QString CdpExtensions::handleHeapProfiler(const QJsonObject &cmd)
{
    // All HeapProfiler commands are stubbed — no Qt API maps to these.
    return stubResult(cmd);
}

QString CdpExtensions::handleSecurity(const QJsonObject &cmd, QWebEnginePage *page)
{
    Q_UNUSED(page)
    // No direct QWebEngineProfile API for certificate error ignoring; return stub.
    return stubResult(cmd);
}
