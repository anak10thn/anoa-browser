#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QtTest/QtTest>

// Pull in the function under test directly.
// config.cpp is compiled into a static lib (anoa-config-lib) by the CMake target.
#include "config/config.h"

// 4x2 RGB PNG, row 0 = red/green/blue/white, row 1 = black/yellow/cyan/magenta.
// Embedded rather than generated so the test exercises decoding only — the CDP
// backend receives PNG bytes it did not produce.
static const char kTinyPngBase64[] =
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAACCAIAAADwyuo0AAAAFklEQVR42mP4z8DA"
    "AMZAwACmwWyG/wCJkAv1L5VDcAAAAABJRU5ErkJggg==";

class TestConfig : public QObject
{
    Q_OBJECT

private:
    // Result of running parseArgs in a subprocess (see the parse_args harness
    // mode below). parseArgs validates by calling ::exit(1), so it cannot be
    // driven in-process — and QCommandLineParser reads argv from the live
    // QCoreApplication, which this test binary already owns for QTest.
    struct ParseResult {
        int exitCode = -1;
        QJsonObject cfg;
    };

    static ParseResult runParseArgs(const QStringList &args)
    {
        QProcess proc;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("ANOA_TEST_HARNESS", "parse_args");
        proc.setProcessEnvironment(env);
        proc.start(QCoreApplication::applicationFilePath(), args);

        ParseResult r;
        if (!proc.waitForFinished(10000)) {
            proc.kill();
            proc.waitForFinished(2000);
            return r; // exitCode stays -1 → any QCOMPARE against 0 fails loudly
        }
        if (proc.exitStatus() != QProcess::NormalExit)
            return r;

        r.exitCode = proc.exitCode();
        r.cfg = QJsonDocument::fromJson(proc.readAllStandardOutput()).object();
        return r;
    }

    // Write content to a temp file and return its path.
    static QString writeTempJson(QTemporaryDir &dir, const QByteArray &content,
                                 const QString &name = "config.json")
    {
        QString path = dir.filePath(name);
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write(content);
        f.close();
        return path;
    }

private slots:
    // CFG-01: default port when JSON has no port key
    void testDefaultPort()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({})");
        Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.port, 9222);
    }

    // CFG-02: custom port from JSON
    void testCustomPort()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"port":8080})");
        Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.port, 8080);
    }

    // CFG-05: headless flag from JSON
    void testHeadlessFlag()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"headless":true})");
        Config cfg = loadConfigFile(path);
        QVERIFY(cfg.headless);
    }

    // CFG-06: no-sandbox flag from JSON
    void testNoSandboxFlag()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"noSandbox":true})");
        Config cfg = loadConfigFile(path);
        QVERIFY(cfg.noSandbox);
    }

    // CFG-07: profile name from JSON
    void testProfileName()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"profileName":"myprofile"})");
        Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.profileName, QString("myprofile"));
    }

    // CFG-08: profile directory from JSON
    void testProfileDir()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"profileDir":"/tmp/p"})");
        Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.profileDir, QString("/tmp/p"));
    }

    // CFG-09: auth token from JSON
    void testAuthToken()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"authToken":"abc123"})");
        Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.authToken, QString("abc123"));
    }

    // CFG-10: multiple extension paths from JSON
    void testMultipleExtensionPaths()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"extensionPaths":["/p/a","/p/b"]})");
        Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.extensionPaths.size(), 2);
        QCOMPARE(cfg.extensionPaths.at(0), QString("/p/a"));
        QCOMPARE(cfg.extensionPaths.at(1), QString("/p/b"));
    }

    // CFG-11: port read from JSON config file
    void testPortFromConfigFile()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, R"({"port":8081})");
        Config cfg = loadConfigFile(path);
        QCOMPARE(cfg.port, 8081);
    }

    // CFG-13: missing config file → process exits with code 1
    void testMissingConfigFileExits()
    {
        // Run a subprocess that calls loadConfigFile on a nonexistent path.
        // The subprocess is this test binary itself invoked with a special
        // environment variable that triggers the helper slot below.
        QProcess proc;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("ANOA_TEST_HARNESS", "missing_config");
        proc.setProcessEnvironment(env);
        proc.start(QCoreApplication::applicationFilePath(), {});
        proc.waitForFinished(5000);
        QCOMPARE(proc.exitCode(), 1);
    }

    // CFG-14: malformed JSON config → process exits with code 1
    void testMalformedJsonConfigExits()
    {
        QTemporaryDir dir;
        QString path = writeTempJson(dir, "{invalid json}");

        QProcess proc;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("ANOA_TEST_HARNESS", "bad_json");
        env.insert("ANOA_TEST_HARNESS_PATH", path);
        proc.setProcessEnvironment(env);
        proc.start(QCoreApplication::applicationFilePath(), {});
        proc.waitForFinished(5000);
        QCOMPARE(proc.exitCode(), 1);
    }

    // CFG-03 & CFG-04: invalid ports — tested via shell (port validation is in
    // parseArgs which requires the full binary CLI pipeline; see
    // tests/integration/port_layout.test.sh PORT-INVALID-* cases).

    // ── Terminal mode options (`anoa-browser terminal`) ────────────────────
    // These live on the shared parser, so an unregistered flag would make
    // QCommandLineParser::process() hard-exit with "Unknown option".

    // TERM-CFG-01: every terminal flag is accepted and lands in its own field
    void testTerminalOptionsParsed()
    {
        ParseResult r = runParseArgs({"--term-host", "10.0.0.5",
                                      "--term-port", "9333",
                                      "--term-token", "termsecret",
                                      "--fps", "60",
                                      "--gfx", "kitty",
                                      "--cdp", "ws://example.test:9222/devtools/page/ABC"});
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.cfg["termHost"].toString(), QString("10.0.0.5"));
        QCOMPARE(r.cfg["termPort"].toInt(), 9333);
        QCOMPARE(r.cfg["termToken"].toString(), QString("termsecret"));
        QCOMPARE(r.cfg["fps"].toInt(), 60);
        QCOMPARE(r.cfg["gfxMode"].toString(), QString("kitty"));
        QCOMPARE(r.cfg["cdpUrl"].toString(),
                 QString("ws://example.test:9222/devtools/page/ABC"));
    }

    // TERM-CFG-02: struct defaults stand in when the flags are omitted
    void testTerminalDefaults()
    {
        ParseResult r = runParseArgs({});
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.cfg["termHost"].toString(), QString("127.0.0.1"));
        QCOMPARE(r.cfg["termPort"].toInt(), 9222);
        QCOMPARE(r.cfg["termToken"].toString(), QString());
        QCOMPARE(r.cfg["fps"].toInt(), 30);
        QCOMPARE(r.cfg["gfxMode"].toString(), QString("auto"));
        QCOMPARE(r.cfg["cdpUrl"].toString(), QString());
    }

    // TERM-CFG-03: --port / --auth-token keep their browser meaning and are
    // NOT aliased onto the terminal connection fields
    void testBrowserOptionsAreNotAliasedToTerminalFields()
    {
        ParseResult r = runParseArgs({"--port", "8080", "--auth-token", "browsersecret"});
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.cfg["port"].toInt(), 8080);
        QCOMPARE(r.cfg["authToken"].toString(), QString("browsersecret"));
        QCOMPARE(r.cfg["termPort"].toInt(), 9222);
        QCOMPARE(r.cfg["termToken"].toString(), QString());
    }

    // TERM-CFG-04: ...and the reverse — terminal flags never touch the
    // browser's listen port or server token
    void testTerminalOptionsDoNotAffectBrowserFields()
    {
        ParseResult r = runParseArgs({"--term-port", "9333", "--term-token", "termsecret"});
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.cfg["port"].toInt(), 9222);
        QCOMPARE(r.cfg["authToken"].toString(), QString());
    }

    // TERM-CFG-05: --term-port 0 is rejected
    void testTermPortZeroRejected()
    {
        ParseResult r = runParseArgs({"--term-port", "0"});
        QVERIFY(r.exitCode != 0);
    }

    // TERM-CFG-06: --term-port above 65535 is rejected
    void testTermPortTooLargeRejected()
    {
        ParseResult r = runParseArgs({"--term-port", "70000"});
        QVERIFY(r.exitCode != 0);
    }

    // TERM-CFG-07: --fps 0 is rejected
    void testFpsZeroRejected()
    {
        ParseResult r = runParseArgs({"--fps", "0"});
        QVERIFY(r.exitCode != 0);
    }

    // TERM-CFG-08: an unknown --gfx backend is rejected
    void testUnknownGfxModeRejected()
    {
        ParseResult r = runParseArgs({"--gfx", "bogus"});
        QVERIFY(r.exitCode != 0);
    }

    // TERM-CFG-09: wss:// is rejected — Qt is not built with OpenSSL here
    void testCdpWssRejected()
    {
        ParseResult r = runParseArgs({"--cdp", "wss://example.test:9222"});
        QVERIFY(r.exitCode != 0);
    }

    // TERM-CFG-10: the ws:// target form is accepted verbatim
    void testCdpWsUrlAccepted()
    {
        ParseResult r = runParseArgs({"--cdp", "ws://example.test:9222/devtools/page/ABC"});
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.cfg["cdpUrl"].toString(),
                 QString("ws://example.test:9222/devtools/page/ABC"));
    }

    // TERM-CFG-11: the http:// discovery form is accepted verbatim
    void testCdpHttpUrlAccepted()
    {
        ParseResult r = runParseArgs({"--cdp", "http://example.test:9222"});
        QCOMPARE(r.exitCode, 0);
        QCOMPARE(r.cfg["cdpUrl"].toString(), QString("http://example.test:9222"));
    }

    // ── QCoreApplication assumption ───────────────────────────────────────
    // Terminal mode constructs QCoreApplication, not QApplication, so that it
    // works over SSH with no display. The plan flags "QImage decodes PNG
    // without a GUI application object" as asserted-but-unverified; this test
    // is the verification, and it runs on every CI platform. This binary's
    // main() deliberately uses a plain QCoreApplication for the same reason.
    // If it ever fails, the fallback is QApplication with QT_QPA_PLATFORM
    // forced to offscreen from the argv pre-scan in src/main.cpp.

    // TERM-GUI-01: no GUI application object is alive in this process
    void testApplicationObjectIsPlainQCoreApplication()
    {
        QVERIFY(QCoreApplication::instance() != nullptr);
        QCOMPARE(QString::fromLatin1(QCoreApplication::instance()->metaObject()->className()),
                 QString("QCoreApplication"));
    }

    // TERM-GUI-02: QImage decodes PNG bytes under that plain QCoreApplication
    void testQImageDecodesPngWithoutGuiApplication()
    {
        QVERIFY2(QImageReader::supportedImageFormats().contains(QByteArrayLiteral("png")),
                 "QtGui reports no PNG reader without a GUI application object");

        const QByteArray png = QByteArray::fromBase64(QByteArray(kTinyPngBase64));
        QVERIFY(!png.isEmpty());

        QImage img;
        QVERIFY2(img.loadFromData(png, "PNG"), "QImage::loadFromData failed on a valid PNG");
        QCOMPARE(img.width(), 4);
        QCOMPARE(img.height(), 2);
        QCOMPARE(img.pixelColor(0, 0), QColor(255, 0, 0));
        QCOMPARE(img.pixelColor(3, 1), QColor(255, 0, 255));
    }

    // TERM-GUI-03: the halfblock path's downscale + RGB888 conversion also
    // works headless — this is the http_server.cpp scaling recipe in miniature
    void testQImageScaleAndConvertWithoutGuiApplication()
    {
        QImage img;
        QVERIFY(img.loadFromData(QByteArray::fromBase64(QByteArray(kTinyPngBase64)), "PNG"));

        const QImage scaled = img.scaled(2, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QCOMPARE(scaled.size(), QSize(2, 1));

        const QImage rgb = scaled.convertToFormat(QImage::Format_RGB888);
        QCOMPARE(rgb.format(), QImage::Format_RGB888);
        QVERIFY(!rgb.isNull());
    }
};

// When invoked as a harness subprocess, run the specific exit-code scenario.
static bool runHarnessIfRequested(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QString mode = qEnvironmentVariable("ANOA_TEST_HARNESS");
    if (mode.isEmpty())
        return false;

    if (mode == "missing_config") {
        loadConfigFile("/this/path/does/not/exist/config.json");
    } else if (mode == "bad_json") {
        const QString path = qEnvironmentVariable("ANOA_TEST_HARNESS_PATH");
        loadConfigFile(path);
    } else if (mode == "parse_args") {
        // Everything after argv[0] is the CLI under test; parseArgs reads it
        // back off the QCoreApplication constructed above. On a validation
        // failure parseArgs calls ::exit(1) and nothing is printed.
        const Config cfg = parseArgs(argc, argv);
        const QJsonObject obj{
            {"port", cfg.port},
            {"authToken", cfg.authToken},
            {"termHost", cfg.termHost},
            {"termPort", cfg.termPort},
            {"termToken", cfg.termToken},
            {"fps", cfg.fps},
            {"gfxMode", cfg.gfxMode},
            {"cdpUrl", cfg.cdpUrl},
        };
        QTextStream out(stdout);
        out << QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)) << Qt::endl;
    }
    // Reached only when nothing exited: the success path for parse_args, and an
    // unexpected success for the modes that are supposed to exit(1).
    return true; // will fall through to normal exit(0)
}

int main(int argc, char *argv[])
{
    if (runHarnessIfRequested(argc, argv))
        return 0;

    QCoreApplication app(argc, argv);
    TestConfig tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "test_config.moc"
