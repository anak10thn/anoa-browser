#include "anoa_browser.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
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
    // Chromium 111+ rejects DevTools WebSocket connections whose Origin header
    // is not allowlisted. Remote CDP clients (tunnels, reverse proxies, browser
    // frontends) connect from arbitrary origins, so allow all; access control
    // is enforced by our own auth-token proxy layer instead.
    flags += " --remote-allow-origins=*";
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

// Synthetic input must go to the render widget (focusProxy), not the
// QWebEngineView itself — events posted to the view are not forwarded
// to Chromium. postEvent (not sendEvent) keeps this callable from HTTP
// handler code without re-entering the widget stack synchronously.
static QWidget *inputTarget(QWebEngineView *view)
{
    QWidget *proxy = view->focusProxy();
    return proxy ? proxy : view;
}

void AnoaBrowser::sendClick(const QPoint &pos, Qt::MouseButton button)
{
    QWidget *target = inputTarget(this);
    const QPointF posF(pos);
    const QPointF globalF(target->mapToGlobal(pos));
    // Chromium's click-count logic compares event timestamps; leaving them at 0
    // makes every synthetic click look simultaneous (double/triple-click runs).
    const auto stamp = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
    // Leading move puts the pointer at the click position so hover/hit-testing
    // state matches a real interaction before the press arrives.
    auto *move = new QMouseEvent(QEvent::MouseMove, posF, posF, globalF,
                                 Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    move->setTimestamp(stamp);
    auto *press = new QMouseEvent(QEvent::MouseButtonPress, posF, posF, globalF,
                                  button, button, Qt::NoModifier);
    press->setTimestamp(stamp + 1);
    auto *release = new QMouseEvent(QEvent::MouseButtonRelease, posF, posF, globalF,
                                    button, Qt::NoButton, Qt::NoModifier);
    release->setTimestamp(stamp + 50);
    QCoreApplication::postEvent(target, move);
    QCoreApplication::postEvent(target, press);
    QCoreApplication::postEvent(target, release);
}

void AnoaBrowser::sendScroll(const QPoint &pos, int angleDeltaY)
{
    QWidget *target = inputTarget(this);
    const QPointF posF(pos);
    const QPointF globalF(target->mapToGlobal(pos));
    // Null pixelDelta = classic notched wheel; angleDelta is in 1/8 degree,
    // one wheel notch = 120.
    auto *wheel = new QWheelEvent(posF, globalF, QPoint(), QPoint(0, angleDeltaY),
                                  Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    wheel->setTimestamp(static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()));
    QCoreApplication::postEvent(target, wheel);
}

void AnoaBrowser::sendText(const QString &text)
{
    QWidget *target = inputTarget(this);
    for (const QChar &ch : text) {
        // key = 0 (unknown) + non-empty text: Chromium takes the character from
        // the text payload, which handles any unicode without a key-code table.
        QCoreApplication::postEvent(target,
            new QKeyEvent(QEvent::KeyPress, 0, Qt::NoModifier, QString(ch)));
        QCoreApplication::postEvent(target,
            new QKeyEvent(QEvent::KeyRelease, 0, Qt::NoModifier, QString(ch)));
    }
}

bool AnoaBrowser::sendKey(const QString &keyName)
{
    struct NamedKey {
        const char *name;
        Qt::Key key;
        const char *text; // control chars Chromium expects alongside the key
    };
    static const NamedKey keys[] = {
        {"enter", Qt::Key_Return, "\r"},
        {"tab", Qt::Key_Tab, "\t"},
        {"backspace", Qt::Key_Backspace, ""},
        {"delete", Qt::Key_Delete, ""},
        {"escape", Qt::Key_Escape, ""},
        {"space", Qt::Key_Space, " "},
        {"up", Qt::Key_Up, ""},
        {"down", Qt::Key_Down, ""},
        {"left", Qt::Key_Left, ""},
        {"right", Qt::Key_Right, ""},
        {"home", Qt::Key_Home, ""},
        {"end", Qt::Key_End, ""},
        {"pageup", Qt::Key_PageUp, ""},
        {"pagedown", Qt::Key_PageDown, ""},
    };

    const QString wanted = keyName.toLower();
    for (const NamedKey &k : keys) {
        if (wanted == QLatin1String(k.name)) {
            QWidget *target = inputTarget(this);
            QCoreApplication::postEvent(target,
                new QKeyEvent(QEvent::KeyPress, k.key, Qt::NoModifier,
                              QString::fromLatin1(k.text)));
            QCoreApplication::postEvent(target,
                new QKeyEvent(QEvent::KeyRelease, k.key, Qt::NoModifier,
                              QString::fromLatin1(k.text)));
            return true;
        }
    }
    return false;
}
