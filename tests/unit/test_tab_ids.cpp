#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtTest/QtTest>

// Compiled into anoa-tab-ids-lib, a Qt6::Core-only target — the unit CI job
// builds no WebEngine, which is the reason this logic lives apart from the
// registry that will use it.
#include "browser/tab_ids.h"

class TestTabIds : public QObject
{
    Q_OBJECT

private slots:
    void mintingIsMonotonic();
    void mintingNeverRecyclesAClosedId();
    void validIdsAccepted();
    void invalidIdsRejected();
    void namesAcceptedAndRejected();
    void targetListCarriesANameOnlyWhenThereIsOne();
    void targetListHasOneEntryPerTab();
    void targetListPointsAtTheProxyPort();
    void targetListMarksExactlyOneActiveTab();
    void targetListOmitsUnresolvedTabs();
};

void TestTabIds::mintingIsMonotonic()
{
    TabIdMinter minter;
    QCOMPARE(minter.next(), QStringLiteral("t1"));
    QCOMPARE(minter.next(), QStringLiteral("t2"));
    QCOMPARE(minter.next(), QStringLiteral("t3"));
}

void TestTabIds::mintingNeverRecyclesAClosedId()
{
    // The minter has no notion of closing — that is the point. An agent may
    // hold "t2" from an earlier command, so the next tab must not become "t2"
    // again just because the first one went away.
    TabIdMinter minter;
    const QString first = minter.next();
    const QString second = minter.next();
    minter.next();
    const QString fourth = minter.next();

    QCOMPARE(first, QStringLiteral("t1"));
    QCOMPARE(second, QStringLiteral("t2"));
    QCOMPARE(fourth, QStringLiteral("t4"));
    QVERIFY(fourth != first);
    QVERIFY(fourth != second);
}

void TestTabIds::validIdsAccepted()
{
    QVERIFY(isValidTabId(QStringLiteral("t1")));
    QVERIFY(isValidTabId(QStringLiteral("t9")));
    QVERIFY(isValidTabId(QStringLiteral("t12")));
    QVERIFY(isValidTabId(QStringLiteral("t100")));
}

void TestTabIds::invalidIdsRejected()
{
    QVERIFY(!isValidTabId(QString()));
    QVERIFY(!isValidTabId(QStringLiteral("t")));
    // Zero is not a tab: the counter starts at one.
    QVERIFY(!isValidTabId(QStringLiteral("t0")));
    QVERIFY(!isValidTabId(QStringLiteral("T1")));
    // Leading zeros would make "t01" and "t1" two spellings of one tab.
    QVERIFY(!isValidTabId(QStringLiteral("t01")));
    QVERIFY(!isValidTabId(QStringLiteral("tab1")));
    QVERIFY(!isValidTabId(QStringLiteral("t1 ")));
    QVERIFY(!isValidTabId(QStringLiteral(" t1")));
    QVERIFY(!isValidTabId(QStringLiteral("t1junk")));
    QVERIFY(!isValidTabId(QStringLiteral("1")));
}

void TestTabIds::namesAcceptedAndRejected()
{
    QVERIFY(isValidTabName(QStringLiteral("search")));
    QVERIFY(isValidTabName(QStringLiteral("cart-2")));
    QVERIFY(isValidTabName(QStringLiteral("A_b-9")));
    QVERIFY(isValidTabName(QString(32, QLatin1Char('a'))));

    QVERIFY(!isValidTabName(QString()));
    QVERIFY(!isValidTabName(QString(33, QLatin1Char('a'))));   // one over
    QVERIFY(!isValidTabName(QStringLiteral("has space")));
    QVERIFY(!isValidTabName(QStringLiteral("-leading")));      // must start alnum
    QVERIFY(!isValidTabName(QStringLiteral("has/slash")));
    // The rule that keeps --tab unambiguous: a name may never read as an id.
    QVERIFY(!isValidTabName(QStringLiteral("t1")));
    QVERIFY(!isValidTabName(QStringLiteral("t42")));
    // ...but an id-ish string that is not a valid id is a fine name.
    QVERIFY(isValidTabName(QStringLiteral("t0")));
    QVERIFY(isValidTabName(QStringLiteral("tab1")));
}

static TabTargetInfo makeTab(const QString &tabId,
                             const QString &targetId,
                             bool active = false)
{
    TabTargetInfo tab;
    tab.tabId = tabId;
    tab.chromiumTargetId = targetId;
    tab.title = QStringLiteral("Example Domain");
    tab.url = QStringLiteral("https://example.com/");
    tab.active = active;
    return tab;
}

void TestTabIds::targetListCarriesANameOnlyWhenThereIsOne()
{
    TabTargetInfo named = makeTab(QStringLiteral("t1"), QStringLiteral("AAAA"), true);
    named.tabName = QStringLiteral("search");
    TabTargetInfo plain = makeTab(QStringLiteral("t2"), QStringLiteral("BBBB"));

    const QJsonArray list = buildTargetList({named, plain},
                                            QStringLiteral("127.0.0.1"), 9224);
    QCOMPARE(list.at(0).toObject().value(QStringLiteral("anoaTabName")).toString(),
             QStringLiteral("search"));
    // Absent, not empty: an empty key reads as "named nothing" rather than
    // "unnamed".
    QVERIFY(!list.at(1).toObject().contains(QStringLiteral("anoaTabName")));
}

void TestTabIds::targetListHasOneEntryPerTab()
{
    const QList<TabTargetInfo> tabs{
        makeTab(QStringLiteral("t1"), QStringLiteral("AAAA"), true),
        makeTab(QStringLiteral("t2"), QStringLiteral("BBBB")),
        makeTab(QStringLiteral("t3"), QStringLiteral("CCCC")),
    };

    const QJsonArray list = buildTargetList(tabs, QStringLiteral("127.0.0.1"), 9224);
    QCOMPARE(list.size(), 3);

    // Order is preserved: the caller decides it, not this function.
    QCOMPARE(list.at(0).toObject().value(QStringLiteral("anoaTabId")).toString(),
             QStringLiteral("t1"));
    QCOMPARE(list.at(2).toObject().value(QStringLiteral("anoaTabId")).toString(),
             QStringLiteral("t3"));

    const QJsonObject first = list.at(0).toObject();
    QCOMPARE(first.value(QStringLiteral("id")).toString(), QStringLiteral("AAAA"));
    QCOMPARE(first.value(QStringLiteral("type")).toString(), QStringLiteral("page"));
    QCOMPARE(first.value(QStringLiteral("title")).toString(),
             QStringLiteral("Example Domain"));
    QCOMPARE(first.value(QStringLiteral("url")).toString(),
             QStringLiteral("https://example.com/"));
}

void TestTabIds::targetListPointsAtTheProxyPort()
{
    const QList<TabTargetInfo> tabs{ makeTab(QStringLiteral("t1"),
                                             QStringLiteral("DEADBEEF"), true) };

    // 9222 is the HTTP port, 9223 Chromium's own debugging port, 9224 the proxy.
    // A client handed 9223 would bypass every command anoa answers itself.
    const QJsonArray list = buildTargetList(tabs, QStringLiteral("127.0.0.1"), 9224);
    const QJsonObject entry = list.at(0).toObject();

    QCOMPARE(entry.value(QStringLiteral("webSocketDebuggerUrl")).toString(),
             QStringLiteral("ws://127.0.0.1:9224/devtools/page/DEADBEEF"));
    QCOMPARE(entry.value(QStringLiteral("devtoolsFrontendUrl")).toString(),
             QStringLiteral("/devtools/inspector.html?ws=127.0.0.1:9224"
                            "/devtools/page/DEADBEEF"));

    // The host travels through too, for a browser reached over a network.
    const QJsonArray remote = buildTargetList(tabs, QStringLiteral("box.local"), 7000);
    QCOMPARE(remote.at(0).toObject().value(QStringLiteral("webSocketDebuggerUrl")).toString(),
             QStringLiteral("ws://box.local:7000/devtools/page/DEADBEEF"));
}

void TestTabIds::targetListMarksExactlyOneActiveTab()
{
    const QList<TabTargetInfo> tabs{
        makeTab(QStringLiteral("t1"), QStringLiteral("AAAA")),
        makeTab(QStringLiteral("t2"), QStringLiteral("BBBB"), true),
        makeTab(QStringLiteral("t3"), QStringLiteral("CCCC")),
    };

    const QJsonArray list = buildTargetList(tabs, QStringLiteral("127.0.0.1"), 9224);

    int actives = 0;
    QString activeTab;
    for (const QJsonValue &value : list) {
        if (value.toObject().value(QStringLiteral("anoaActive")).toBool()) {
            ++actives;
            activeTab = value.toObject().value(QStringLiteral("anoaTabId")).toString();
        }
    }
    QCOMPARE(actives, 1);
    QCOMPARE(activeTab, QStringLiteral("t2"));
}

void TestTabIds::targetListOmitsUnresolvedTabs()
{
    // A page exists before its DevTools target does. Advertising a tab that
    // cannot be attached to is worse than advertising it a moment later.
    const QList<TabTargetInfo> tabs{
        makeTab(QStringLiteral("t1"), QStringLiteral("AAAA"), true),
        makeTab(QStringLiteral("t2"), QString()),
    };

    const QJsonArray list = buildTargetList(tabs, QStringLiteral("127.0.0.1"), 9224);
    QCOMPARE(list.size(), 1);
    QCOMPARE(list.at(0).toObject().value(QStringLiteral("anoaTabId")).toString(),
             QStringLiteral("t1"));

    QVERIFY(buildTargetList({}, QStringLiteral("127.0.0.1"), 9224).isEmpty());
}

QTEST_MAIN(TestTabIds)
#include "test_tab_ids.moc"
