#include "anoa_browser.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QWebEngineCookieStore>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>

AnoaBrowser::AnoaBrowser(const Config &config, QWidget *parent)
    : QWebEngineView(parent)
    , m_config(config)
    , m_profile(nullptr)
{
    // QTWEBENGINE_CHROMIUM_FLAGS must be set before WebEngine initializes its
    // profile/page. Setting it here, before creating QWebEngineProfile and
    // calling setPage(), ensures Chromium picks up the remote-debugging port.
    // Chromium DevTools runs on port+1; our HTTP/WS proxy layer listens on port.
    QByteArray flags = "--remote-debugging-port=" + QByteArray::number(m_config.port + 1);
    if (m_config.noSandbox)
        flags += " --no-sandbox";
    if (m_config.headless)
        flags += " --disable-gpu";
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);

    if (m_config.headless)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    m_profile = QWebEngineProfile::defaultProfile();
    setPage(new QWebEnginePage(m_profile, this));
}

void AnoaBrowser::init()
{
    resize(m_config.width, m_config.height);
    // show() is required in both headed and headless (offscreen) mode: without it
    // the widget has no backing surface and QWebEngineView reports a 0×0 viewport.
    // With QPA_PLATFORM=offscreen the call creates an invisible surface, not a window.
    show();
    // Navigating to about:blank ensures the renderer process is started and the
    // page registers as a DevTools target in /json/list so CDP clients can attach.
    load(QUrl(QStringLiteral("about:blank")));
}

void AnoaBrowser::loadExtensions(const QStringList &paths)
{
    for (const QString &path : paths) {
        if (!QDir(path).exists()) {
            qWarning("Extension path does not exist, skipping: %s", qPrintable(path));
            continue;
        }
        const QString manifestPath = path + "/manifest.json";
        if (!QFile::exists(manifestPath)) {
            qWarning("No manifest.json found in extension path, skipping: %s", qPrintable(path));
            continue;
        }
        QFile f(manifestPath);
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning("Cannot read manifest.json, skipping: %s", qPrintable(path));
            continue;
        }
        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
        if (doc.isNull()) {
            qWarning("Invalid manifest.json (%s), skipping: %s",
                     qPrintable(parseErr.errorString()), qPrintable(path));
            continue;
        }
        const QJsonObject manifest = doc.object();
        const int manifestVersion = manifest["manifest_version"].toInt();
        if (manifestVersion == 3) {
            qWarning("Manifest v3 not supported in Qt6 WebEngine, skipping: %s", qPrintable(path));
            continue;
        }

        // Inject content scripts via QWebEngineScript (manifest v1/v2 only)
        const QJsonArray contentScripts = manifest["content_scripts"].toArray();
        for (const auto &csVal : contentScripts) {
            const QJsonObject cs = csVal.toObject();
            const QJsonArray jsFiles = cs["js"].toArray();
            for (const auto &jsVal : jsFiles) {
                const QString jsPath = path + "/" + jsVal.toString();
                QFile jsFile(jsPath);
                if (!jsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    qWarning("Cannot read content script, skipping: %s", qPrintable(jsPath));
                    continue;
                }
                QWebEngineScript script;
                script.setName(jsPath);
                script.setSourceCode(QString::fromUtf8(jsFile.readAll()));
                script.setInjectionPoint(QWebEngineScript::DocumentReady);
                script.setWorldId(QWebEngineScript::MainWorld);
                m_profile->scripts()->insert(script);
            }
        }
    }
}

void AnoaBrowser::setupNamedProfile(const QString &name, const QString &baseDir)
{
    if (m_profile != QWebEngineProfile::defaultProfile())
        m_profile->deleteLater();

    m_profile = new QWebEngineProfile(name, this);
    m_profile->setPersistentStoragePath(QDir(baseDir).filePath(name));
    m_profile->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);

    auto *oldPage = page();
    setPage(new QWebEnginePage(m_profile, this));
    oldPage->deleteLater();
}

QList<QNetworkCookie> AnoaBrowser::getCookies(const QUrl &origin)
{
    Q_UNUSED(origin)
    QList<QNetworkCookie> cookies;
    auto *store = m_profile->cookieStore();

    QEventLoop loop;
    auto conn = connect(store, &QWebEngineCookieStore::cookieAdded,
                        [&cookies](const QNetworkCookie &cookie) {
                            cookies.append(cookie);
                        });
    QTimer::singleShot(500, &loop, &QEventLoop::quit);
    store->loadAllCookies();
    loop.exec();
    disconnect(conn);
    return cookies;
}

void AnoaBrowser::setCookie(const QNetworkCookie &cookie, const QUrl &origin)
{
    m_profile->cookieStore()->setCookie(cookie, origin);
}

void AnoaBrowser::clearStorage(const QUrl &origin)
{
    Q_UNUSED(origin)
    m_profile->cookieStore()->deleteAllCookies();
    m_profile->clearAllVisitedLinks();
    page()->triggerAction(QWebEnginePage::Stop);
}
