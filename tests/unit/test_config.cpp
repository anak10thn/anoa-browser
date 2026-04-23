#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QtTest/QtTest>

// Pull in the function under test directly.
// config.cpp is compiled into a static lib (anoa-config-lib) by the CMake target.
#include "config/config.h"

class TestConfig : public QObject
{
    Q_OBJECT

private:
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
    }
    // If we get here, the expected exit(1) didn't fire — signal unexpected success.
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
