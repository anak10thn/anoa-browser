#include "cdp/cdp_extensions.h"
#include "pdf/pdf_handler.h"

#include <QJsonDocument>
#include <QJsonObject>

static QString stubResult(const QJsonObject &cmd)
{
    QJsonObject resp;
    resp[QStringLiteral("id")] = cmd.value(QStringLiteral("id")).toInt();
    resp[QStringLiteral("result")] = QJsonObject();
    return QJsonDocument(resp).toJson(QJsonDocument::Compact);
}

QString CdpExtensions::processCommand(const QJsonObject &cmd, QWebEnginePage *page,
                                      bool *deferred,
                                      const std::function<void(const QString &)> &sendLater)
{
    // No handler defers yet; task-008 is the first. Cleared up front so a
    // caller never reads a stale value from its own stack.
    if (deferred)
        *deferred = false;
    Q_UNUSED(sendLater)

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
    if (domain == QLatin1String("Browser"))
        return handleBrowser(cmd);
    if (domain == QLatin1String("Target"))
        return handleTarget(cmd);
    if (method == QLatin1String("Page.printToPDF")) {
        // Only use Qt's PdfHandler when we have a page reference.
        // Without a page (e.g. in the proxy path), pass through to Chromium which
        // handles Page.printToPDF natively (Chrome 96+).
        if (page) {
            PdfHandler handler(page);
            return handler.handlePrintToPdf(cmd);
        }
        return QString(); // pass through to Chromium
    }

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

QJsonObject CdpExtensions::rewritePassthrough(const QJsonObject &cmd)
{
    const QString method = cmd.value(QStringLiteral("method")).toString();
    // Strip synthetic browserContextId (__anoa_default__) inserted by handleTarget
    // so Chromium uses its default context instead of an unknown context ID.
    if (method == QLatin1String("Target.createTarget")) {
        const QJsonObject params = cmd.value(QStringLiteral("params")).toObject();
        const QString ctxId = params.value(QStringLiteral("browserContextId")).toString();
        if (ctxId == QLatin1String("__anoa_default__")) {
            QJsonObject modified = cmd;
            QJsonObject p = params;
            p.remove(QStringLiteral("browserContextId"));
            modified[QStringLiteral("params")] = p;
            return modified;
        }
    }
    return QJsonObject(); // no rewrite needed
}

QString CdpExtensions::handleBrowser(const QJsonObject &cmd)
{
    // Only stub Browser commands that QtWebEngine Chromium rejects with
    // "Browser context management is not supported". Pass everything else
    // (e.g. Browser.getVersion) through to Chromium.
    const QString method = cmd.value(QStringLiteral("method")).toString();
    if (method == QLatin1String("Browser.setDownloadBehavior")
            || method == QLatin1String("Browser.getWindowForTarget")
            || method == QLatin1String("Browser.setWindowBounds")
            || method == QLatin1String("Browser.grantPermissions")
            || method == QLatin1String("Browser.resetPermissions")) {
        return stubResult(cmd);
    }
    return QString(); // pass through
}

QString CdpExtensions::handleTarget(const QJsonObject &cmd)
{
    const QString method = cmd.value(QStringLiteral("method")).toString();
    // QtWebEngine Chromium does not support multiple browser contexts (incognito).
    // Return a synthetic context ID so clients like Playwright can proceed;
    // rewritePassthrough() strips this ID before commands reach Chromium.
    if (method == QLatin1String("Target.createBrowserContext")) {
        QJsonObject result;
        result[QStringLiteral("browserContextId")] = QStringLiteral("__anoa_default__");
        QJsonObject resp;
        resp[QStringLiteral("id")] = cmd.value(QStringLiteral("id")).toInt();
        resp[QStringLiteral("result")] = result;
        return QJsonDocument(resp).toJson(QJsonDocument::Compact);
    }
    if (method == QLatin1String("Target.disposeBrowserContext")) {
        return stubResult(cmd);
    }
    return QString(); // pass through
}
