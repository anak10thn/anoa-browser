#include "agent/agent_cli.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include "agent/agent_help.h"
#include "agent/agent_script.h"
#include "agent/agent_skill.h"
#include "cdp/cdp_client.h"
#include "config/config.h"

namespace {

QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}
QTextStream &err()
{
    static QTextStream s(stderr);
    return s;
}

// Exit codes. Distinct on purpose: an agent retrying a flaky click should not
// retry "nothing is listening", and a shell script should be able to tell the
// two apart without parsing prose.
enum ExitCode { Ok = 0, Failed = 1, Usage = 2, NoBrowser = 3 };

int fail(const QString &message, int code = Failed)
{
    err() << "anoa: " << message << Qt::endl;
    return code;
}

// ── A synchronous view of an asynchronous client ────────────────────────────
//
// CdpClient is built for the viewer, where nothing may block the frame loop.
// A command-line invocation is the opposite: it does one thing and exits, and
// the only sane shape for that is a blocking call. Each call runs a nested
// event loop, which is safe here precisely because there is no other work in
// this process to starve.
class Session
{
public:
    bool attach(const QString &host, int port, const QString &token, int timeoutMs)
    {
        // Constructed here, not as a plain member: the bearer token is a
        // constructor argument and is not known until the config is parsed.
        m_client = std::make_unique<CdpClient>(token);
        m_client->setRequestTimeout(timeoutMs);
        m_client->setExitOnDiscoveryFailure(false);

        QEventLoop loop;
        bool ok = false;
        QObject::connect(m_client.get(), &CdpClient::connected, &loop, [&]() {
            ok = true;
            loop.quit();
        });
        QObject::connect(m_client.get(), &CdpClient::discoveryFailed, &loop,
                         [&](const QString &why) {
                             m_why = why;
                             loop.quit();
                         });
        // One attempt only. The viewer retries forever because a session is
        // worth reconnecting; a command has a caller waiting on it.
        QObject::connect(m_client.get(), &CdpClient::retryScheduled, &loop, [&](int, int) {
            if (m_why.isEmpty())
                m_why = QStringLiteral("no answer from %1:%2").arg(host).arg(port);
            loop.quit();
        });
        QTimer::singleShot(timeoutMs, &loop, [&]() {
            if (m_why.isEmpty())
                m_why = QStringLiteral("timed out attaching to %1:%2").arg(host).arg(port);
            loop.quit();
        });

        m_client->connectToEndpoint(
            QUrl(QStringLiteral("http://%1:%2").arg(host).arg(port)));
        if (!m_client->isConnected())
            loop.exec();
        return ok || m_client->isConnected();
    }

    QString why() const { return m_why; }

    CdpResult call(const QString &method, const QJsonObject &params = QJsonObject())
    {
        QEventLoop loop;
        CdpResult result;
        m_client->send(method, params, [&](const CdpResult &r) {
            result = r;
            loop.quit();
        });
        loop.exec();
        return result;
    }

    // Runtime.evaluate with the helper script guaranteed to be installed.
    // Installing on every call rather than once is deliberate: the page may
    // have navigated since the last command, and this process has no way to
    // know that without asking. The script returns early when it is already
    // there, so the cost is a property read.
    QJsonValue evaluate(const QString &expression, QString *error)
    {
        if (!m_installed) {
            QJsonObject boot;
            boot[QStringLiteral("expression")] = agentScript();
            boot[QStringLiteral("returnByValue")] = true;
            const CdpResult r = call(QStringLiteral("Runtime.evaluate"), boot);
            if (!r.ok) {
                if (error)
                    *error = r.errorMessage;
                return QJsonValue();
            }
            m_installed = true;
        }

        QJsonObject params;
        params[QStringLiteral("expression")] = expression;
        params[QStringLiteral("returnByValue")] = true;
        params[QStringLiteral("awaitPromise")] = true;
        const CdpResult r = call(QStringLiteral("Runtime.evaluate"), params);
        if (!r.ok) {
            if (error)
                *error = r.errorMessage;
            return QJsonValue();
        }
        // A thrown exception is a result, not a transport error, so it arrives
        // in the payload and has to be dug out or it reads as success.
        const QJsonObject details =
            r.result.value(QStringLiteral("exceptionDetails")).toObject();
        if (!details.isEmpty()) {
            if (error) {
                const QJsonObject ex = details.value(QStringLiteral("exception")).toObject();
                *error = ex.value(QStringLiteral("description")).toString(
                    details.value(QStringLiteral("text")).toString(
                        QStringLiteral("evaluation failed")));
            }
            return QJsonValue();
        }
        return r.result.value(QStringLiteral("result")).toObject().value(QStringLiteral("value"));
    }

    CdpClient &client() { return *m_client; }

private:
    // Owned, and created by attach() rather than by the constructor. Held as a
    // pointer and not a reference-to-member: attach() replaces it, and a
    // reference bound to the old object would dangle the moment it did.
    std::unique_ptr<CdpClient> m_client;
    QString m_why;
    bool m_installed = false;
};

// ── argument helpers ────────────────────────────────────────────────────────

bool takeFlag(QStringList &args, const QString &name)
{
    const int i = args.indexOf(name);
    if (i < 0)
        return false;
    args.removeAt(i);
    return true;
}

QString takeOption(QStringList &args, const QString &name, const QString &fallback = QString())
{
    const int i = args.indexOf(name);
    if (i < 0 || i + 1 >= args.size())
        return fallback;
    const QString value = args.at(i + 1);
    args.removeAt(i + 1);
    args.removeAt(i);
    return value;
}

QString jsString(const QString &s)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact))
        .mid(1)
        .chopped(1); // ["..."] -> "..."
}

void printJson(const QJsonValue &v)
{
    const QJsonDocument doc = v.isArray() ? QJsonDocument(v.toArray())
                                          : QJsonDocument(v.toObject());
    out() << QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed() << Qt::endl;
}

// The default, human/agent readable rendering of a snapshot. One element per
// line so an agent can grep it, and refs first because they are what the next
// command needs.
void printSnapshot(const QJsonObject &snap)
{
    out() << snap.value(QStringLiteral("title")).toString() << Qt::endl;
    out() << snap.value(QStringLiteral("url")).toString() << Qt::endl;

    const QJsonArray headings = snap.value(QStringLiteral("headings")).toArray();
    if (!headings.isEmpty()) {
        out() << Qt::endl;
        for (const QJsonValue &h : headings) {
            const QJsonObject o = h.toObject();
            out() << QString(2 * o.value(QStringLiteral("level")).toInt(1), QLatin1Char(' '))
                  << o.value(QStringLiteral("text")).toString() << Qt::endl;
        }
    }

    const QJsonArray els = snap.value(QStringLiteral("elements")).toArray();
    out() << Qt::endl << els.size() << " interactive element"
          << (els.size() == 1 ? "" : "s") << Qt::endl;
    for (const QJsonValue &e : els) {
        const QJsonObject o = e.toObject();
        QString line = QStringLiteral("  %1  %2")
                           .arg(o.value(QStringLiteral("ref")).toString(), -5)
                           .arg(o.value(QStringLiteral("role")).toString(), -9);
        const QString name = o.value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            line += QStringLiteral("  ") + name;
        const QJsonArray st = o.value(QStringLiteral("state")).toArray();
        if (!st.isEmpty()) {
            QStringList bits;
            for (const QJsonValue &s : st)
                bits << s.toString();
            line += QStringLiteral("  [") + bits.join(QStringLiteral(" ")) + QStringLiteral("]");
        }
        out() << line << Qt::endl;
    }
}

// ── commands ────────────────────────────────────────────────────────────────

int cmdOpen(Session &s, QStringList args, bool json)
{
    if (args.isEmpty())
        return fail(QStringLiteral("open needs a url — try: anoa open example.com"), Usage);
    QString url = args.first();
    if (!url.contains(QStringLiteral("://")) && !url.startsWith(QStringLiteral("about:")))
        url = QStringLiteral("https://") + url;

    QJsonObject p;
    p[QStringLiteral("url")] = url;
    const CdpResult r = s.call(QStringLiteral("Page.navigate"), p);
    if (!r.ok)
        return fail(QStringLiteral("navigate failed: %1").arg(r.errorMessage));

    // Page.navigate returns as soon as the navigation is committed, which is
    // before there is anything to snapshot. Poll rather than subscribe to
    // Page.loadEventFired: this process attached a moment ago and may well
    // have missed the event already.
    QElapsedTimer clock;
    clock.start();
    QString e;
    while (clock.elapsed() < 15000) {
        const QJsonValue v = s.evaluate(QStringLiteral("document.readyState"), &e);
        if (v.toString() == QStringLiteral("complete"))
            break;
        QEventLoop wait;
        QTimer::singleShot(100, &wait, &QEventLoop::quit);
        wait.exec();
    }

    const QJsonValue info = s.evaluate(QStringLiteral("__anoa.info()"), &e);
    if (json) {
        printJson(info);
    } else {
        const QJsonObject o = info.toObject();
        out() << o.value(QStringLiteral("title")).toString() << Qt::endl
              << o.value(QStringLiteral("url")).toString() << Qt::endl;
    }
    return Ok;
}

int cmdSnapshot(Session &s, QStringList args, bool json)
{
    const bool interactive = takeFlag(args, QStringLiteral("-i"))
                             || takeFlag(args, QStringLiteral("--interactive"));
    QString e;
    const QJsonValue v = s.evaluate(
        QStringLiteral("__anoa.snapshot(%1)").arg(interactive ? "true" : "false"), &e);
    if (v.isNull() || v.isUndefined())
        return fail(e.isEmpty() ? QStringLiteral("snapshot failed") : e);
    if (json)
        printJson(v);
    else
        printSnapshot(v.toObject());
    return Ok;
}

int cmdClick(Session &s, QStringList args, bool json)
{
    if (args.isEmpty())
        return fail(QStringLiteral("click needs a ref or selector — try: anoa click @e2"), Usage);
    QString e;
    const QJsonValue v =
        s.evaluate(QStringLiteral("__anoa.clickPoint(%1)").arg(jsString(args.first())), &e);
    if (v.isNull() || v.isUndefined())
        return fail(e.isEmpty() ? QStringLiteral("click failed") : e);

    const QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("error"))) {
        const QString kind = o.value(QStringLiteral("error")).toString();
        if (kind == QStringLiteral("covered")) {
            const QJsonObject by = o.value(QStringLiteral("covered_by")).toObject();
            return fail(QStringLiteral("%1 is covered by <%2> %3 — dismiss it, then re-snapshot")
                            .arg(args.first(),
                                 by.value(QStringLiteral("tag")).toString(),
                                 by.value(QStringLiteral("name")).toString()));
        }
        return fail(kind);
    }

    // A real Input event, not element.click(): the synthetic one skips hit
    // testing, so it would happily "click" through an overlay the user can see.
    QJsonObject p;
    p[QStringLiteral("x")] = o.value(QStringLiteral("x")).toDouble();
    p[QStringLiteral("y")] = o.value(QStringLiteral("y")).toDouble();
    p[QStringLiteral("button")] = QStringLiteral("left");
    p[QStringLiteral("clickCount")] = 1;
    p[QStringLiteral("type")] = QStringLiteral("mousePressed");
    p[QStringLiteral("buttons")] = 1;
    CdpResult r = s.call(QStringLiteral("Input.dispatchMouseEvent"), p);
    if (r.ok) {
        p[QStringLiteral("type")] = QStringLiteral("mouseReleased");
        p[QStringLiteral("buttons")] = 0;
        r = s.call(QStringLiteral("Input.dispatchMouseEvent"), p);
    }
    if (!r.ok)
        return fail(QStringLiteral("click failed: %1").arg(r.errorMessage));

    if (json)
        printJson(o.value(QStringLiteral("el")));
    else
        out() << "clicked " << args.first() << Qt::endl;
    return Ok;
}

int cmdFill(Session &s, QStringList args, bool json)
{
    if (args.size() < 2)
        return fail(QStringLiteral("fill needs a target and a value — "
                                   "try: anoa fill @e3 \"text\""),
                    Usage);
    QString e;
    const QJsonValue v = s.evaluate(QStringLiteral("__anoa.fill(%1, %2)")
                                        .arg(jsString(args.at(0)), jsString(args.at(1))),
                                    &e);
    const QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("error")))
        return fail(o.value(QStringLiteral("error")).toString());
    if (v.isNull() || v.isUndefined())
        return fail(e.isEmpty() ? QStringLiteral("fill failed") : e);
    if (json)
        printJson(o.value(QStringLiteral("el")));
    else
        out() << "filled " << args.at(0) << Qt::endl;
    return Ok;
}

int cmdType(Session &s, QStringList args)
{
    if (args.isEmpty())
        return fail(QStringLiteral("type needs text"), Usage);
    QJsonObject p;
    p[QStringLiteral("text")] = args.join(QLatin1Char(' '));
    const CdpResult r = s.call(QStringLiteral("Input.insertText"), p);
    return r.ok ? Ok : fail(QStringLiteral("type failed: %1").arg(r.errorMessage));
}

int cmdPress(Session &s, QStringList args)
{
    if (args.isEmpty())
        return fail(QStringLiteral("press needs a key — e.g. Enter, Tab, ArrowDown"), Usage);
    const QString key = args.first();
    // The few keys worth spelling out; anything else is passed through and
    // Chromium decides. Windows virtual key codes are what CDP wants.
    static const QHash<QString, int> codes{
        {QStringLiteral("Enter"), 13},     {QStringLiteral("Tab"), 9},
        {QStringLiteral("Escape"), 27},    {QStringLiteral("Backspace"), 8},
        {QStringLiteral("Delete"), 46},    {QStringLiteral("ArrowUp"), 38},
        {QStringLiteral("ArrowDown"), 40}, {QStringLiteral("ArrowLeft"), 37},
        {QStringLiteral("ArrowRight"), 39}, {QStringLiteral("Home"), 36},
        {QStringLiteral("End"), 35},       {QStringLiteral("PageUp"), 33},
        {QStringLiteral("PageDown"), 34},
    };
    QJsonObject p;
    p[QStringLiteral("key")] = key;
    if (codes.contains(key)) {
        p[QStringLiteral("windowsVirtualKeyCode")] = codes.value(key);
        p[QStringLiteral("nativeVirtualKeyCode")] = codes.value(key);
    }
    p[QStringLiteral("type")] = QStringLiteral("rawKeyDown");
    CdpResult r = s.call(QStringLiteral("Input.dispatchKeyEvent"), p);
    if (r.ok) {
        p[QStringLiteral("type")] = QStringLiteral("keyUp");
        r = s.call(QStringLiteral("Input.dispatchKeyEvent"), p);
    }
    return r.ok ? Ok : fail(QStringLiteral("press failed: %1").arg(r.errorMessage));
}

int cmdGet(Session &s, QStringList args, bool json)
{
    if (args.isEmpty())
        return fail(QStringLiteral("get needs a property: text, html, value or attr"), Usage);
    const QString what = args.takeFirst();
    QString e;
    QJsonValue v;
    if (what == QStringLiteral("attr")) {
        if (args.size() < 2)
            return fail(QStringLiteral("get attr needs a target and an attribute name"), Usage);
        v = s.evaluate(QStringLiteral("__anoa.attr(%1, %2)")
                           .arg(jsString(args.at(0)), jsString(args.at(1))),
                       &e);
    } else {
        const QString target = args.isEmpty() ? QString() : args.first();
        v = s.evaluate(QStringLiteral("__anoa.get(%1, %2)")
                           .arg(jsString(what), target.isEmpty() ? QStringLiteral("null")
                                                                 : jsString(target)),
                       &e);
    }
    const QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("error")))
        return fail(o.value(QStringLiteral("error")).toString());
    if (v.isNull() || v.isUndefined())
        return fail(e.isEmpty() ? QStringLiteral("get failed") : e);
    if (json)
        printJson(o);
    else
        out() << o.value(QStringLiteral("value")).toVariant().toString() << Qt::endl;
    return Ok;
}

int cmdEval(Session &s, QStringList args, bool json)
{
    if (args.isEmpty())
        return fail(QStringLiteral("eval needs an expression"), Usage);
    QString e;
    const QJsonValue v = s.evaluate(args.join(QLatin1Char(' ')), &e);
    if (!e.isEmpty())
        return fail(e);
    if (json || v.isObject() || v.isArray())
        printJson(v.isObject() || v.isArray() ? v : QJsonValue(QJsonObject{{"value", v}}));
    else
        out() << v.toVariant().toString() << Qt::endl;
    return Ok;
}

int cmdWait(Session &s, QStringList args)
{
    const QString selector = takeOption(args, QStringLiteral("--selector"));
    const QString msText = takeOption(args, QStringLiteral("--ms"));
    const bool load = takeFlag(args, QStringLiteral("--load")) || (selector.isEmpty() && msText.isEmpty());
    const int budget = takeOption(args, QStringLiteral("--timeout"), QStringLiteral("15000")).toInt();

    if (!msText.isEmpty()) {
        QEventLoop loop;
        QTimer::singleShot(msText.toInt(), &loop, &QEventLoop::quit);
        loop.exec();
        return Ok;
    }

    QElapsedTimer clock;
    clock.start();
    QString e;
    while (clock.elapsed() < budget) {
        if (load) {
            if (s.evaluate(QStringLiteral("document.readyState"), &e).toString()
                == QStringLiteral("complete"))
                return Ok;
        } else {
            const QJsonValue v =
                s.evaluate(QStringLiteral("__anoa.exists(%1)").arg(jsString(selector)), &e);
            if (v.toObject().value(QStringLiteral("found")).toBool())
                return Ok;
        }
        QEventLoop wait;
        QTimer::singleShot(100, &wait, &QEventLoop::quit);
        wait.exec();
    }
    return fail(load ? QStringLiteral("page did not finish loading in %1ms").arg(budget)
                     : QStringLiteral("%1 did not appear in %2ms").arg(selector).arg(budget));
}

int cmdScroll(Session &s, QStringList args)
{
    int dy = 400, dx = 0;
    if (takeFlag(args, QStringLiteral("--up")))
        dy = -400;
    const QString by = takeOption(args, QStringLiteral("--by"));
    if (!by.isEmpty())
        dy = by.toInt();
    if (takeFlag(args, QStringLiteral("--top")))
        dy = -1000000;
    if (takeFlag(args, QStringLiteral("--bottom")))
        dy = 1000000;
    QString e;
    s.evaluate(QStringLiteral("__anoa.scroll(%1, %2)").arg(dx).arg(dy), &e);
    return e.isEmpty() ? Ok : fail(e);
}

int cmdHistory(Session &s, const QString &verb)
{
    if (verb == QStringLiteral("reload")) {
        const CdpResult r = s.call(QStringLiteral("Page.reload"));
        return r.ok ? Ok : fail(QStringLiteral("reload failed: %1").arg(r.errorMessage));
    }
    const CdpResult hist = s.call(QStringLiteral("Page.getNavigationHistory"));
    if (!hist.ok)
        return fail(QStringLiteral("history unavailable: %1").arg(hist.errorMessage));
    const QJsonArray entries = hist.result.value(QStringLiteral("entries")).toArray();
    const int current = hist.result.value(QStringLiteral("currentIndex")).toInt(-1);
    const int target = current + (verb == QStringLiteral("back") ? -1 : 1);
    if (current < 0 || target < 0 || target >= entries.size())
        return fail(QStringLiteral("no %1 entry").arg(verb));
    QJsonObject p;
    p[QStringLiteral("entryId")] =
        entries.at(target).toObject().value(QStringLiteral("id")).toInt();
    const CdpResult r = s.call(QStringLiteral("Page.navigateToHistoryEntry"), p);
    return r.ok ? Ok : fail(QStringLiteral("%1 failed: %2").arg(verb, r.errorMessage));
}

int writeDecoded(const QString &path, const QString &base64, const char *what)
{
    const QByteArray bytes = QByteArray::fromBase64(base64.toLatin1());
    if (bytes.isEmpty())
        return fail(QStringLiteral("%1 came back empty").arg(QLatin1String(what)));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("cannot write %1: %2").arg(path, f.errorString()));
    f.write(bytes);
    f.close();
    out() << path << " (" << bytes.size() << " bytes)" << Qt::endl;
    return Ok;
}

int cmdScreenshot(Session &s, QStringList args)
{
    const QString path = args.isEmpty() ? QStringLiteral("screenshot.png") : args.first();
    QJsonObject p;
    p[QStringLiteral("format")] = QStringLiteral("png");
    const CdpResult r = s.call(QStringLiteral("Page.captureScreenshot"), p);
    if (!r.ok)
        return fail(QStringLiteral("screenshot failed: %1").arg(r.errorMessage));
    return writeDecoded(path, r.result.value(QStringLiteral("data")).toString(), "screenshot");
}

int cmdPdf(Session &s, QStringList args)
{
    const QString path = args.isEmpty() ? QStringLiteral("page.pdf") : args.first();
    const CdpResult r = s.call(QStringLiteral("Page.printToPDF"));
    if (!r.ok)
        return fail(QStringLiteral("pdf failed: %1").arg(r.errorMessage));
    return writeDecoded(path, r.result.value(QStringLiteral("data")).toString(), "pdf");
}

int cmdStatus(Session &s, bool json)
{
    QString e;
    const QJsonValue v = s.evaluate(QStringLiteral("__anoa.info()"), &e);
    if (v.isNull() || v.isUndefined())
        return fail(e.isEmpty() ? QStringLiteral("status failed") : e);
    if (json) {
        printJson(v);
    } else {
        const QJsonObject o = v.toObject();
        const QJsonObject size = o.value(QStringLiteral("size")).toObject();
        out() << "attached  " << s.client().description() << Qt::endl
              << "title     " << o.value(QStringLiteral("title")).toString() << Qt::endl
              << "url       " << o.value(QStringLiteral("url")).toString() << Qt::endl
              << "viewport  " << size.value(QStringLiteral("w")).toInt() << "x"
              << size.value(QStringLiteral("h")).toInt() << Qt::endl
              << "state     " << o.value(QStringLiteral("ready")).toString() << Qt::endl;
    }
    return Ok;
}

} // namespace

bool isAgentCommand(const QString &verb)
{
    static const QStringList verbs{
        QStringLiteral("open"),   QStringLiteral("goto"),     QStringLiteral("snapshot"),
        QStringLiteral("click"),  QStringLiteral("fill"),     QStringLiteral("type"),
        QStringLiteral("press"),  QStringLiteral("get"),      QStringLiteral("eval"),
        QStringLiteral("wait"),   QStringLiteral("scroll"),   QStringLiteral("back"),
        QStringLiteral("forward"), QStringLiteral("reload"),  QStringLiteral("screenshot"),
        QStringLiteral("pdf"),    QStringLiteral("status"),   QStringLiteral("skills"),
        QStringLiteral("help"),
    };
    return verbs.contains(verb);
}

int runAgentCommand(const Config &config, const QString &verb, const QStringList &rawArgs)
{
    QStringList args = rawArgs;
    const bool json = takeFlag(args, QStringLiteral("--json"));

    // `skills` never touches the browser: an agent asks for it before it has
    // started one, which is the whole point of a discovery stub.
    if (verb == QStringLiteral("skills"))
        return runSkillsCommand(args);

    // Where to attach. Read from the command's own arguments rather than from
    // Config, because the verb took the rest of the line with it and
    // QCommandLineParser never saw these. Config supplies the defaults.
    const QString host =
        takeOption(args, QStringLiteral("--host"), config.termHost);
    bool portOk = true;
    const int port =
        takeOption(args, QStringLiteral("--port"), QString::number(config.port)).toInt(&portOk);
    const QString token =
        takeOption(args, QStringLiteral("--token"),
                   takeOption(args, QStringLiteral("--auth-token"), config.authToken));
    if (!portOk || port < 1 || port > 65535)
        return fail(QStringLiteral("--port must be 1-65535"), Usage);

    Session session;
    if (!session.attach(host, port, token, 10000)) {
        err() << "anoa: no browser on " << host << ":" << port;
        if (!session.why().isEmpty())
            err() << " (" << session.why() << ")";
        err() << Qt::endl
              << "      start one first:  anoa --headless --port " << port << Qt::endl;
        return NoBrowser;
    }

    if (verb == QStringLiteral("open") || verb == QStringLiteral("goto"))
        return cmdOpen(session, args, json);
    if (verb == QStringLiteral("snapshot"))
        return cmdSnapshot(session, args, json);
    if (verb == QStringLiteral("click"))
        return cmdClick(session, args, json);
    if (verb == QStringLiteral("fill"))
        return cmdFill(session, args, json);
    if (verb == QStringLiteral("type"))
        return cmdType(session, args);
    if (verb == QStringLiteral("press"))
        return cmdPress(session, args);
    if (verb == QStringLiteral("get"))
        return cmdGet(session, args, json);
    if (verb == QStringLiteral("eval"))
        return cmdEval(session, args, json);
    if (verb == QStringLiteral("wait"))
        return cmdWait(session, args);
    if (verb == QStringLiteral("scroll"))
        return cmdScroll(session, args);
    if (verb == QStringLiteral("back") || verb == QStringLiteral("forward")
        || verb == QStringLiteral("reload"))
        return cmdHistory(session, verb);
    if (verb == QStringLiteral("screenshot"))
        return cmdScreenshot(session, args);
    if (verb == QStringLiteral("pdf"))
        return cmdPdf(session, args);
    if (verb == QStringLiteral("status"))
        return cmdStatus(session, json);

    return fail(QStringLiteral("unknown command: %1").arg(verb), Usage);
}
