#include <QtTest>

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpSocket>

#include "FakeHttpServer.h"
#include "ktm/xc2/Xc2RestClient.h"

using namespace ktm::xc2;

namespace {

constexpr int kSignalWaitMs = 1500;

QString withPort(QString pattern, quint16 port)
{
    return pattern.replace(QStringLiteral("%PORT%"), QString::number(port));
}

QUrl numericBase(const FakeHttpServer &server,
                 const QString &host = QStringLiteral("127.0.0.1"))
{
    QUrl result;
    result.setScheme(QStringLiteral("http"));
    result.setHost(host);
    result.setPort(server.port());
    result.setPath(QStringLiteral("/xc2/1.0"));
    return result;
}

QByteArray encodedBase(const FakeHttpServer &server,
                       const QString &host = QStringLiteral("127.0.0.1"))
{
    return numericBase(server, host).toEncoded(QUrl::FullyEncoded);
}

QByteArray encodedShutdownTarget(
    const FakeHttpServer &server,
    const QString &host = QStringLiteral("127.0.0.1"))
{
    QUrl result = numericBase(server, host);
    result.setPath(QStringLiteral(
        "/xc2/1.0/serviceStatus/shutdown"));
    return result.toEncoded(QUrl::FullyEncoded);
}

QList<QByteArray> cookiePairs(const QByteArray &header)
{
    QList<QByteArray> result;
    for (const QByteArray &pair : header.split(';')) {
        if (!pair.trimmed().isEmpty())
            result.append(pair.trimmed());
    }
    return result;
}

QUrl trapUrl(const FakeHttpServer &server)
{
    QUrl result = numericBase(server);
    result.setPath(QStringLiteral("/redirect-trap"));
    return result;
}

QByteArray backendError(int status, int code)
{
    return QByteArrayLiteral(R"({"status":)") + QByteArray::number(status)
        + QByteArrayLiteral(R"(,"message":"synthetic failure","code":)")
        + QByteArray::number(code)
        + QByteArrayLiteral(
            R"(,"devMessage":"synthetic detail","info":"synthetic info"})");
}

class ApplicationProxyGuard final {
public:
    explicit ApplicationProxyGuard(const QNetworkProxy &replacement)
        : m_previous(QNetworkProxy::applicationProxy())
    {
        QNetworkProxy::setApplicationProxy(replacement);
    }

    ~ApplicationProxyGuard()
    {
        QNetworkProxy::setApplicationProxy(m_previous);
    }

private:
    QNetworkProxy m_previous;
};

struct ObservedResult {
    bool ok = false;
    Xc2Error error;
};

} // namespace

class Xc2RestClientTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<Xc2Result<Xc2ServiceStatus>>();
        qRegisterMetaType<Xc2Result<Xc2CurrentUser>>();
        qRegisterMetaType<Xc2Result<Xc2JobAccepted>>();
        qRegisterMetaType<Xc2Result<QList<Xc2VciDevice>>>();
        qRegisterMetaType<Xc2Result<Xc2SelectedVci>>();
        qRegisterMetaType<Xc2Error>();
    }

    void acceptsIpv4Ipv6AndLocalhostBases_data()
    {
        QTest::addColumn<int>("addressKind");
        QTest::addColumn<QString>("basePattern");
        QTest::addColumn<QString>("canonicalHost");

        QTest::newRow("IPv4 exact")
            << 4 << QStringLiteral("http://127.0.0.1:%PORT%/xc2/1.0")
            << QStringLiteral("127.0.0.1");
        QTest::newRow("IPv4 permitted trailing slash")
            << 4 << QStringLiteral("http://127.0.0.1:%PORT%/xc2/1.0/")
            << QStringLiteral("127.0.0.1");
        QTest::newRow("IPv6 exact")
            << 6 << QStringLiteral("http://[::1]:%PORT%/xc2/1.0")
            << QStringLiteral("::1");
        QTest::newRow("localhost case and trailing slash")
            << 4 << QStringLiteral("http://LOCALHOST:%PORT%/xc2/1.0/")
            << QStringLiteral("127.0.0.1");
    }

    void acceptsIpv4Ipv6AndLocalhostBases()
    {
        QFETCH(int, addressKind);
        QFETCH(QString, basePattern);
        QFETCH(QString, canonicalHost);

        FakeHttpServer server(addressKind == 6
                                  ? QHostAddress::LocalHostIPv6
                                  : QHostAddress::LocalHost);
        QVERIFY2(server.isListening(), qPrintable(server.errorString()));
        server.enqueueResponse(FakeHttpServer::complete(
            200, QByteArrayLiteral("alive")));

        Xc2RestClient client({500, 250});
        Xc2Error error;
        QVERIFY2(client.setBaseUrl(
                     withPort(basePattern, server.port()).toLatin1(), &error),
                 qPrintable(error.message));
        QCOMPARE(client.baseUrl().scheme(), QStringLiteral("http"));
        QCOMPARE(client.baseUrl().host(), canonicalHost);
        QCOMPARE(client.baseUrl().port(), int(server.port()));
        QCOMPARE(client.baseUrl().path(QUrl::FullyEncoded),
                 QStringLiteral("/xc2/1.0"));
        QCOMPARE(client.webSocketUrl().scheme(), QStringLiteral("ws"));
        QCOMPARE(client.webSocketUrl().host(), canonicalHost);
        QCOMPARE(client.webSocketUrl().port(), int(server.port()));
        QCOMPARE(client.webSocketUrl().path(QUrl::FullyEncoded),
                 QStringLiteral("/xc2-websocket"));

        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        QVERIFY(client.requestServiceStatus() != 0);
        QVERIFY2(finished.wait(kSignalWaitMs), "service status did not finish");
        QCOMPARE(finished.count(), 1);
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY2(result.ok(), qPrintable(result.error.message));
        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(server.requests().front().target,
                 QByteArrayLiteral("/xc2/1.0/serviceStatus/status"));

        if (basePattern.contains(QStringLiteral("LOCALHOST"))) {
            QCOMPARE(server.requests().front().headerValue("Host"),
                     QByteArrayLiteral("127.0.0.1:")
                         + QByteArray::number(server.port()));
            QCOMPARE(client.webSocketUrl().toEncoded(),
                     QByteArrayLiteral("ws://127.0.0.1:")
                         + QByteArray::number(server.port())
                         + QByteArrayLiteral("/xc2-websocket"));
        } else if (addressKind == 6) {
            QCOMPARE(server.requests().front().headerValue("Host"),
                     QByteArrayLiteral("[::1]:")
                         + QByteArray::number(server.port()));
        }
    }

    void rejectsUnsafeOrAliasedBases_data()
    {
        QTest::addColumn<QString>("basePattern");

        const QStringList rejected = {
            QStringLiteral("/xc2/1.0"),
            QStringLiteral("https://127.0.0.1:%PORT%/xc2/1.0"),
            QStringLiteral("ws://127.0.0.1:%PORT%/xc2/1.0"),
            QStringLiteral("http://127.0.0.1/xc2/1.0"),
            QStringLiteral("http://127.0.0.1:0/xc2/1.0"),
            QStringLiteral("http://127.0.0.1:65536/xc2/1.0"),
            QStringLiteral("http://127.0.0.1:080/xc2/1.0"),
            QStringLiteral("http://user@127.0.0.1:%PORT%/xc2/1.0"),
            QStringLiteral("http://user:pass@127.0.0.1:%PORT%/xc2/1.0"),
            QStringLiteral("http://127.0.0.1:%PORT%/xc2/1.0?q=1"),
            QStringLiteral("http://127.0.0.1:%PORT%/xc2/1.0#fragment"),
            QStringLiteral("http://localhost.:%PORT%/xc2/1.0"),
            QStringLiteral("http://foo.localhost:%PORT%/xc2/1.0"),
            QStringLiteral("http://localhost.localdomain:%PORT%/xc2/1.0"),
            QStringLiteral("http://127.1:%PORT%/xc2/1.0"),
            QStringLiteral("http://2130706433:%PORT%/xc2/1.0"),
            QStringLiteral("http://0177.0.0.1:%PORT%/xc2/1.0"),
            QStringLiteral("http://0300.0250.0001.0001:%PORT%/xc2/1.0"),
            QStringLiteral("http://192.0.2.1:%PORT%/xc2/1.0"),
            QStringLiteral("http://[2001:db8::1]:%PORT%/xc2/1.0"),
            QStringLiteral("http://127.0.0.1:%PORT%/xc2/1.0//"),
            QStringLiteral("http://127.0.0.1:%PORT%/XC2/1.0"),
            QStringLiteral("http://127.0.0.1:%PORT%/xc2/1.0/extra"),
            QStringLiteral("http://127.0.0.1:%PORT%/xc2/./1.0"),
            QStringLiteral("http://127.0.0.1:%PORT%/xc2/1.0/../1.0"),
            QStringLiteral("http://127.0.0.1:%PORT%/%78c2/1.0"),
            QStringLiteral("http://127.0.0.1:%PORT%/xc2%2f1.0"),
            QStringLiteral("http://127.0.0.1:%PORT%/xc2/1%2e0"),
        };

        for (qsizetype i = 0; i < rejected.size(); ++i) {
            QTest::newRow(qPrintable(QStringLiteral("rejected-%1").arg(i)))
                << rejected.at(i);
        }
    }

    void rejectsUnsafeOrAliasedBases()
    {
        QFETCH(QString, basePattern);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY2(server.isListening(), qPrintable(server.errorString()));
        Xc2RestClient client;
        Xc2Error error;
        const QByteArray candidate =
            withPort(basePattern, server.port()).toLatin1();
        const QUrl normalized = QUrl::fromEncoded(candidate, QUrl::StrictMode);
        QVERIFY2(!client.setBaseUrl(candidate, &error),
                 qPrintable(basePattern + QStringLiteral(" normalized to ")
                            + QString::fromLatin1(normalized.toEncoded())));
        QCOMPARE(error.category, Xc2ErrorCategory::Contract);
        QVERIFY(client.baseUrl().isEmpty());
        QVERIFY(client.webSocketUrl().isEmpty());
        QCOMPARE(server.requestCount(), 0);
    }

    void sendsExactRequestTargetsAndHeaders()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY2(server.isListening(), qPrintable(server.errorString()));
        server.enqueueResponse(FakeHttpServer::complete(
            200, QByteArrayLiteral("alive")));
        server.enqueueResponse(FakeHttpServer::complete(
            200,
            QByteArrayLiteral(
                R"({"loginName":"xcd","name":"Synthetic","permissions":[]})")));
        server.enqueueResponse(FakeHttpServer::complete(204));

        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy statusFinished(
            &client, &Xc2RestClient::serviceStatusFinished);
        QSignalSpy userFinished(&client, &Xc2RestClient::currentUserFinished);
        QSignalSpy shutdownFinished(&client, &Xc2RestClient::shutdownFinished);

        client.requestServiceStatus();
        QVERIFY(statusFinished.wait(kSignalWaitMs));
        client.requestCurrentUser();
        QVERIFY(userFinished.wait(kSignalWaitMs));
        client.requestShutdown();
        QVERIFY(shutdownFinished.wait(kSignalWaitMs));

        QCOMPARE(server.requestCount(), 3);
        QCOMPARE(server.requests().at(0).method, QByteArrayLiteral("GET"));
        QCOMPARE(server.requests().at(0).target,
                 QByteArrayLiteral("/xc2/1.0/serviceStatus/status"));
        QCOMPARE(server.requests().at(1).method, QByteArrayLiteral("GET"));
        QCOMPARE(server.requests().at(1).target,
                 QByteArrayLiteral("/xc2/1.0/auth/currentUser"));
        QCOMPARE(server.requests().at(2).method, QByteArrayLiteral("POST"));
        QCOMPARE(server.requests().at(2).target,
                 QByteArrayLiteral("/xc2/1.0/serviceStatus/shutdown"));
        const QByteArray expectedHost = QByteArrayLiteral("127.0.0.1:")
            + QByteArray::number(server.port());
        for (const FakeHttpRequest &request : server.requests()) {
            QCOMPARE(request.headerValue("Host"), expectedHost);
            QCOMPARE(request.headerValue("Connection"),
                     QByteArrayLiteral("close"));
            QCOMPARE(request.headerValue("Accept-Encoding"),
                     QByteArrayLiteral("identity"));
        }
        QCOMPARE(server.requests().at(2).headerValue("Content-Type"),
                 QByteArrayLiteral("application/x-www-form-urlencoded"));
        QCOMPARE(server.requests().at(2).headerValue("Content-Length"),
                 QByteArrayLiteral("0"));
        QVERIFY(server.requests().at(2).body.isEmpty());
    }

    void typedVciRequestsUseExactTargetsBodiesHeadersAndSignals()
    {
        FakeHttpServer origin(QHostAddress::LocalHost);
        FakeHttpServer proxyTrap(QHostAddress::LocalHost);
        QVERIFY(origin.isListening());
        QVERIFY(proxyTrap.isListening());

        origin.enqueueResponse(FakeHttpServer::complete(
            200, QByteArrayLiteral(R"({"jobID":"lookup-1"})")));
        origin.enqueueResponse(FakeHttpServer::complete(
            200,
            QByteArrayLiteral(
                R"([{"id":"vci-1","name":"VCI One","internalName":"AVL Ditest VCI2K_DPDU_API"}])")));
        origin.enqueueResponse(FakeHttpServer::complete(
            200,
            QByteArrayLiteral(
                R"({"id":"vci-1","name":"VCI One","internalName":"AVL Ditest VCI2K_DPDU_API","additionalModuleInformation":null})")));
        origin.enqueueResponse(FakeHttpServer::complete(204));
        origin.enqueueResponse(FakeHttpServer::complete(204));

        QNetworkProxy proxy(QNetworkProxy::HttpProxy,
                            QStringLiteral("127.0.0.1"),
                            proxyTrap.port());
        ApplicationProxyGuard proxyGuard(proxy);

        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(origin)));
        QSignalSpy lookupFinished(
            &client, &Xc2RestClient::deviceLookupFinished);
        QSignalSpy devicesFinished(&client, &Xc2RestClient::devicesFinished);
        QSignalSpy selectedFinished(
            &client, &Xc2RestClient::selectedDeviceFinished);
        QSignalSpy applyFinished(&client, &Xc2RestClient::applyDeviceFinished);
        QSignalSpy closeFinished(&client, &Xc2RestClient::closeDeviceFinished);
        QSignalSpy shutdownFinished(&client, &Xc2RestClient::shutdownFinished);

        const Xc2VciDevice applyDevice{
            QStringLiteral("vci-1"),
            QStringLiteral("VCI One"),
            QStringLiteral("AVL Ditest VCI2K_DPDU_API"),
            std::nullopt,
        };
        const Xc2VciDevice closeDevice{
            QStringLiteral("vci-2"),
            QStringLiteral("VCI Two"),
            QStringLiteral("AVL Ditest VCI2K_DPDU_API"),
            QStringLiteral("module-2"),
        };

        QList<Xc2RequestId> ids;
        ids.append(client.requestDeviceLookup());
        QVERIFY(lookupFinished.wait(kSignalWaitMs));
        ids.append(client.requestDevices());
        QVERIFY(devicesFinished.wait(kSignalWaitMs));
        ids.append(client.requestSelectedDevice());
        QVERIFY(selectedFinished.wait(kSignalWaitMs));
        ids.append(client.requestApplyDevice(applyDevice));
        QVERIFY(applyFinished.wait(kSignalWaitMs));
        ids.append(client.requestCloseDevice(closeDevice));
        QVERIFY(closeFinished.wait(kSignalWaitMs));

        for (qsizetype i = 0; i < ids.size(); ++i) {
            QVERIFY(ids.at(i) != 0);
            for (qsizetype j = i + 1; j < ids.size(); ++j)
                QVERIFY(ids.at(i) != ids.at(j));
        }

        QCOMPARE(lookupFinished.count(), 1);
        const QList<QVariant> lookupArguments = lookupFinished.takeFirst();
        QCOMPARE(lookupArguments.at(0).toULongLong(), ids.at(0));
        const auto lookupResult =
            qvariant_cast<Xc2Result<Xc2JobAccepted>>(lookupArguments.at(1));
        QVERIFY2(lookupResult.ok(), qPrintable(lookupResult.error.message));
        QCOMPARE(lookupResult.value->jobId, QStringLiteral("lookup-1"));

        QCOMPARE(devicesFinished.count(), 1);
        const QList<QVariant> devicesArguments = devicesFinished.takeFirst();
        QCOMPARE(devicesArguments.at(0).toULongLong(), ids.at(1));
        const auto devicesResult =
            qvariant_cast<Xc2Result<QList<Xc2VciDevice>>>(
                devicesArguments.at(1));
        QVERIFY2(devicesResult.ok(), qPrintable(devicesResult.error.message));
        QCOMPARE(devicesResult.value->size(), 1);
        QCOMPARE(devicesResult.value->front().id, QStringLiteral("vci-1"));

        QCOMPARE(selectedFinished.count(), 1);
        const QList<QVariant> selectedArguments = selectedFinished.takeFirst();
        QCOMPARE(selectedArguments.at(0).toULongLong(), ids.at(2));
        const auto selectedResult =
            qvariant_cast<Xc2Result<Xc2SelectedVci>>(
                selectedArguments.at(1));
        QVERIFY2(selectedResult.ok(), qPrintable(selectedResult.error.message));
        QVERIFY(selectedResult.value->device.has_value());
        QCOMPARE(selectedResult.value->device->id, QStringLiteral("vci-1"));

        QCOMPARE(applyFinished.count(), 1);
        const QList<QVariant> applyArguments = applyFinished.takeFirst();
        QCOMPARE(applyArguments.at(0).toULongLong(), ids.at(3));
        QCOMPARE(qvariant_cast<Xc2Error>(applyArguments.at(1)).category,
                 Xc2ErrorCategory::None);
        QCOMPARE(closeFinished.count(), 1);
        const QList<QVariant> closeArguments = closeFinished.takeFirst();
        QCOMPARE(closeArguments.at(0).toULongLong(), ids.at(4));
        QCOMPARE(qvariant_cast<Xc2Error>(closeArguments.at(1)).category,
                 Xc2ErrorCategory::None);
        QCOMPARE(shutdownFinished.count(), 0);

        QCOMPARE(origin.requestCount(), 5);
        QCOMPARE(proxyTrap.requestCount(), 0);
        const QList<QByteArray> methods{
            QByteArrayLiteral("GET"), QByteArrayLiteral("GET"),
            QByteArrayLiteral("GET"), QByteArrayLiteral("POST"),
            QByteArrayLiteral("POST"),
        };
        const QList<QByteArray> targets{
            QByteArrayLiteral("/xc2/1.0/device/lookup"),
            QByteArrayLiteral("/xc2/1.0/device/get"),
            QByteArrayLiteral("/xc2/1.0/device/getSelected"),
            QByteArrayLiteral("/xc2/1.0/device/apply"),
            QByteArrayLiteral("/xc2/1.0/device/close"),
        };
        const QByteArray expectedHost = QByteArrayLiteral("127.0.0.1:")
            + QByteArray::number(origin.port());
        const QList<Xc2VciDevice> postedDevices{applyDevice, closeDevice};
        QList<QByteArray> expectedBodies;
        for (const Xc2VciDevice &device : postedDevices) {
            QJsonObject expectedObject{
                {QStringLiteral("id"), device.id},
                {QStringLiteral("name"), device.name},
                {QStringLiteral("internalName"), device.internalName},
                {QStringLiteral("additionalModuleInformation"),
                 device.additionalModuleInformation.has_value()
                     ? QJsonValue(*device.additionalModuleInformation)
                     : QJsonValue(QJsonValue::Null)},
            };
            expectedBodies.append(
                QJsonDocument(expectedObject).toJson(QJsonDocument::Compact));
        }
        const QByteArray commonHeaders = QByteArrayLiteral("\r\nHost: ")
            + expectedHost
            + QByteArrayLiteral(
                "\r\nAccept: */*\r\nAccept-Encoding: identity\r\n"
                "Connection: close\r\n");
        const QList<QByteArray> expectedWires{
            QByteArrayLiteral("GET /xc2/1.0/device/lookup HTTP/1.1")
                + commonHeaders + QByteArrayLiteral("\r\n"),
            QByteArrayLiteral("GET /xc2/1.0/device/get HTTP/1.1")
                + commonHeaders + QByteArrayLiteral("\r\n"),
            QByteArrayLiteral("GET /xc2/1.0/device/getSelected HTTP/1.1")
                + commonHeaders + QByteArrayLiteral("\r\n"),
            QByteArrayLiteral("POST /xc2/1.0/device/apply HTTP/1.1")
                + commonHeaders
                + QByteArrayLiteral(
                    "Content-Type: application/json\r\nContent-Length: ")
                + QByteArray::number(expectedBodies.at(0).size())
                + QByteArrayLiteral("\r\n\r\n") + expectedBodies.at(0),
            QByteArrayLiteral("POST /xc2/1.0/device/close HTTP/1.1")
                + commonHeaders
                + QByteArrayLiteral(
                    "Content-Type: application/json\r\nContent-Length: ")
                + QByteArray::number(expectedBodies.at(1).size())
                + QByteArrayLiteral("\r\n\r\n") + expectedBodies.at(1),
        };
        for (qsizetype i = 0; i < origin.requests().size(); ++i) {
            const FakeHttpRequest &request = origin.requests().at(i);
            QCOMPARE(request.rawBytes, expectedWires.at(i));
            QCOMPARE(request.method, methods.at(i));
            QCOMPARE(request.target, targets.at(i));
            QCOMPARE(request.headerValues("Host"),
                     QList<QByteArray>{expectedHost});
            QCOMPARE(request.headerValues("Connection"),
                     QList<QByteArray>{QByteArrayLiteral("close")});
            QCOMPARE(request.headerValues("Accept-Encoding"),
                     QList<QByteArray>{QByteArrayLiteral("identity")});
        }

        for (qsizetype i = 0; i < 3; ++i) {
            QVERIFY(origin.requests().at(i).headerValues(
                        "Content-Type").isEmpty());
            QVERIFY(origin.requests().at(i).headerValues(
                        "Content-Length").isEmpty());
            QVERIFY(origin.requests().at(i).body.isEmpty());
        }

        for (qsizetype i = 0; i < postedDevices.size(); ++i) {
            const FakeHttpRequest &request = origin.requests().at(i + 3);
            QCOMPARE(request.headerValues("Content-Type"),
                     QList<QByteArray>{
                         QByteArrayLiteral("application/json")});
            QCOMPARE(request.headerValues("Content-Length"),
                     QList<QByteArray>{
                         QByteArray::number(request.body.size())});
            const QByteArray expectedBody = expectedBodies.at(i);
            QCOMPARE(request.body, expectedBody);
            QCOMPARE(QJsonDocument::fromJson(request.body).object().size(), 4);
            QVERIFY(!request.body.contains('\n'));
            QVERIFY(!request.body.contains('\r'));
        }
    }

    void vciRequestsReturnZeroImmediatelyWithoutConfiguredBase()
    {
        Xc2RestClient client;
        QSignalSpy lookupFinished(
            &client, &Xc2RestClient::deviceLookupFinished);
        QSignalSpy devicesFinished(&client, &Xc2RestClient::devicesFinished);
        QSignalSpy selectedFinished(
            &client, &Xc2RestClient::selectedDeviceFinished);
        QSignalSpy applyFinished(&client, &Xc2RestClient::applyDeviceFinished);
        QSignalSpy closeFinished(&client, &Xc2RestClient::closeDeviceFinished);
        QSignalSpy shutdownFinished(&client, &Xc2RestClient::shutdownFinished);
        const Xc2VciDevice device{
            QStringLiteral("vci-1"), QStringLiteral("VCI One"),
            QStringLiteral("AVL Ditest VCI2K_DPDU_API"), std::nullopt,
        };

        QCOMPARE(client.requestDeviceLookup(), Xc2RequestId(0));
        QCOMPARE(client.requestDevices(), Xc2RequestId(0));
        QCOMPARE(client.requestSelectedDevice(), Xc2RequestId(0));
        QCOMPARE(client.requestApplyDevice(device), Xc2RequestId(0));
        QCOMPARE(client.requestCloseDevice(device), Xc2RequestId(0));
        QCoreApplication::processEvents();
        QCOMPARE(lookupFinished.count(), 0);
        QCOMPARE(devicesFinished.count(), 0);
        QCOMPARE(selectedFinished.count(), 0);
        QCOMPARE(applyFinished.count(), 0);
        QCOMPARE(closeFinished.count(), 0);
        QCOMPARE(shutdownFinished.count(), 0);
    }

    void typedVciRedirectIsNotFollowedOrRetried()
    {
        FakeHttpServer origin(QHostAddress::LocalHost);
        FakeHttpServer trap(QHostAddress::LocalHost);
        QVERIFY(origin.isListening());
        QVERIFY(trap.isListening());
        origin.enqueueResponse(FakeHttpServer::redirect(
            307, trapUrl(trap), backendError(307, 3007)));

        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(origin)));
        QSignalSpy trapCaptured(&trap, &FakeHttpServer::requestCaptured);
        QSignalSpy applyFinished(&client, &Xc2RestClient::applyDeviceFinished);
        QSignalSpy shutdownFinished(&client, &Xc2RestClient::shutdownFinished);
        const Xc2VciDevice device{
            QStringLiteral("vci-1"), QStringLiteral("VCI One"),
            QStringLiteral("AVL Ditest VCI2K_DPDU_API"), std::nullopt,
        };

        const Xc2RequestId id = client.requestApplyDevice(device);
        QVERIFY(id != 0);
        QVERIFY(applyFinished.wait(kSignalWaitMs));
        QCOMPARE(applyFinished.count(), 1);
        const QList<QVariant> arguments = applyFinished.takeFirst();
        QCOMPARE(arguments.at(0).toULongLong(), id);
        const Xc2Error error = qvariant_cast<Xc2Error>(arguments.at(1));
        QCOMPARE(error.category, Xc2ErrorCategory::Backend);
        QCOMPARE(error.httpStatus, 307);
        QCOMPARE(error.rawPayload, backendError(307, 3007));
        QCOMPARE(error.endpoint, QStringLiteral("device/apply"));
        QCOMPARE(origin.requestCount(), 1);
        QVERIFY(!trapCaptured.wait(120));
        QCOMPARE(trap.requestCount(), 0);
        QCOMPARE(shutdownFinished.count(), 0);
    }

    void lookupAndDevicesResponseMatrix_data()
    {
        QTest::addColumn<int>("operation");
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("expectedOk");

        QTest::newRow("lookup valid")
            << 0 << 200 << QByteArrayLiteral(R"({"jobID":"job-1"})")
            << true;
        QTest::newRow("lookup empty job id")
            << 0 << 200 << QByteArrayLiteral(R"({"jobID":""})")
            << false;
        QTest::newRow("lookup malformed")
            << 0 << 200 << QByteArrayLiteral("{") << false;
        QTest::newRow("lookup trailing JSON")
            << 0 << 200
            << QByteArrayLiteral(R"({"jobID":"job-1"} trailing)")
            << false;
        QTest::newRow("lookup other 2xx")
            << 0 << 201 << QByteArrayLiteral(R"({"jobID":"job-1"})")
            << false;
        QTest::newRow("devices valid")
            << 1 << 200
            << QByteArrayLiteral(
                   R"([{"id":"vci-1","name":"VCI One","internalName":"AVL Ditest VCI2K_DPDU_API","additionalModuleInformation":null}])")
            << true;
        QTest::newRow("devices empty array")
            << 1 << 200 << QByteArrayLiteral("[]") << true;
        QTest::newRow("devices object")
            << 1 << 200 << QByteArrayLiteral("{}") << false;
        QTest::newRow("devices 204")
            << 1 << 204 << QByteArray{} << false;
        QTest::newRow("devices other 2xx")
            << 1 << 202 << QByteArrayLiteral("[]") << false;
    }

    void lookupAndDevicesResponseMatrix()
    {
        QFETCH(int, operation);
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(bool, expectedOk);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::complete(status, body));
        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy lookupFinished(
            &client, &Xc2RestClient::deviceLookupFinished);
        QSignalSpy devicesFinished(&client, &Xc2RestClient::devicesFinished);
        QSignalSpy shutdownFinished(&client, &Xc2RestClient::shutdownFinished);

        const Xc2RequestId id = operation == 0
            ? client.requestDeviceLookup() : client.requestDevices();
        QSignalSpy *active = operation == 0
            ? &lookupFinished : &devicesFinished;
        QVERIFY(id != 0);
        QVERIFY(active->wait(kSignalWaitMs));
        QCOMPARE(active->count(), 1);
        const QList<QVariant> arguments = active->takeFirst();
        QCOMPARE(arguments.at(0).toULongLong(), id);

        Xc2Error error;
        bool actualOk = false;
        if (operation == 0) {
            const auto result =
                qvariant_cast<Xc2Result<Xc2JobAccepted>>(arguments.at(1));
            actualOk = result.ok();
            error = result.error;
            if (expectedOk)
                QCOMPARE(result.value->jobId, QStringLiteral("job-1"));
        } else {
            const auto result =
                qvariant_cast<Xc2Result<QList<Xc2VciDevice>>>(
                    arguments.at(1));
            actualOk = result.ok();
            error = result.error;
        }
        QCOMPARE(actualOk, expectedOk);
        if (!expectedOk) {
            QCOMPARE(error.category, Xc2ErrorCategory::Contract);
            QCOMPARE(error.httpStatus, status);
            QCOMPARE(error.rawPayload, body);
            QCOMPARE(error.endpoint, operation == 0
                         ? QStringLiteral("device/lookup")
                         : QStringLiteral("device/get"));
        }
        QCOMPARE(lookupFinished.count(), 0);
        QCOMPARE(devicesFinished.count(), 0);
        QCOMPARE(shutdownFinished.count(), 0);
        QCOMPARE(server.requestCount(), 1);
    }

    void selectedDeviceResponseMatrix_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("expectedOk");
        QTest::addColumn<bool>("expectedDevice");

        const QByteArray device = QByteArrayLiteral(
            R"({"id":"vci-1","name":"VCI One","internalName":"AVL Ditest VCI2K_DPDU_API","additionalModuleInformation":null})");
        QTest::newRow("200 object") << 200 << device << true << true;
        QTest::newRow("200 null")
            << 200 << QByteArrayLiteral("null") << true << false;
        QTest::newRow("200 zero bytes")
            << 200 << QByteArray{} << true << false;
        QTest::newRow("204 zero bytes")
            << 204 << QByteArray{} << true << false;
        QTest::newRow("200 whitespace is not empty")
            << 200 << QByteArrayLiteral(" \t\r\n") << false << false;
        QTest::newRow("200 scalar")
            << 200 << QByteArrayLiteral("true") << false << false;
        QTest::newRow("200 malformed")
            << 200 << QByteArrayLiteral("{") << false << false;
        QTest::newRow("other 2xx")
            << 201 << device << false << false;
    }

    void selectedDeviceResponseMatrix()
    {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(bool, expectedOk);
        QFETCH(bool, expectedDevice);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::complete(status, body));
        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy selectedFinished(
            &client, &Xc2RestClient::selectedDeviceFinished);
        QSignalSpy shutdownFinished(&client, &Xc2RestClient::shutdownFinished);

        const Xc2RequestId id = client.requestSelectedDevice();
        QVERIFY(id != 0);
        QVERIFY(selectedFinished.wait(kSignalWaitMs));
        QCOMPARE(selectedFinished.count(), 1);
        const QList<QVariant> arguments = selectedFinished.takeFirst();
        QCOMPARE(arguments.at(0).toULongLong(), id);
        const auto result = qvariant_cast<Xc2Result<Xc2SelectedVci>>(
            arguments.at(1));
        QCOMPARE(result.ok(), expectedOk);
        if (expectedOk) {
            QCOMPARE(result.value->device.has_value(), expectedDevice);
        } else {
            QCOMPARE(result.error.category, Xc2ErrorCategory::Contract);
            QCOMPARE(result.error.httpStatus, status);
            QCOMPARE(result.error.rawPayload, body);
            QCOMPARE(result.error.endpoint,
                     QStringLiteral("device/getSelected"));
        }
        QCOMPARE(shutdownFinished.count(), 0);
        QCOMPARE(server.requestCount(), 1);
    }

    void applyAndCloseResponseMatrix_data()
    {
        QTest::addColumn<int>("operation");
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("expectedOk");

        QTest::newRow("apply 200 zero bytes")
            << 0 << 200 << QByteArray{} << true;
        QTest::newRow("close 204 zero bytes")
            << 1 << 204 << QByteArray{} << true;
        QTest::newRow("object")
            << 0 << 200 << QByteArrayLiteral(R"({"accepted":true})") << true;
        QTest::newRow("array")
            << 1 << 200 << QByteArrayLiteral("[1,2]") << true;
        QTest::newRow("string with whitespace")
            << 0 << 200 << QByteArrayLiteral(" \t\"accepted\"\r\n") << true;
        QTest::newRow("number")
            << 1 << 200 << QByteArrayLiteral("-12.5e2") << true;
        QTest::newRow("boolean")
            << 0 << 200 << QByteArrayLiteral("true") << true;
        QTest::newRow("null")
            << 1 << 200 << QByteArrayLiteral("null") << true;
        QTest::newRow("BOM scalar")
            << 0 << 200 << QByteArrayLiteral("\xEF\xBB\xBFtrue") << true;
        QTest::newRow("whitespace only")
            << 1 << 200 << QByteArrayLiteral(" \t\r\n") << false;
        QTest::newRow("malformed")
            << 0 << 200 << QByteArrayLiteral("{") << false;
        QTest::newRow("trailing garbage")
            << 1 << 200 << QByteArrayLiteral("{} trailing") << false;
        QTest::newRow("two values")
            << 0 << 200 << QByteArrayLiteral("null true") << false;
        QTest::newRow("apply other 2xx")
            << 0 << 201 << QByteArrayLiteral("{}") << false;
        QTest::newRow("close other 2xx")
            << 1 << 202 << QByteArray{} << false;
    }

    void applyAndCloseResponseMatrix()
    {
        QFETCH(int, operation);
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(bool, expectedOk);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::complete(status, body));
        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy applyFinished(&client, &Xc2RestClient::applyDeviceFinished);
        QSignalSpy closeFinished(&client, &Xc2RestClient::closeDeviceFinished);
        QSignalSpy shutdownFinished(&client, &Xc2RestClient::shutdownFinished);
        const Xc2VciDevice device{
            QStringLiteral("vci-1"), QStringLiteral("VCI One"),
            QStringLiteral("AVL Ditest VCI2K_DPDU_API"), std::nullopt,
        };

        const Xc2RequestId id = operation == 0
            ? client.requestApplyDevice(device)
            : client.requestCloseDevice(device);
        QSignalSpy *active = operation == 0 ? &applyFinished : &closeFinished;
        QVERIFY(id != 0);
        QVERIFY(active->wait(kSignalWaitMs));
        QCOMPARE(active->count(), 1);
        const QList<QVariant> arguments = active->takeFirst();
        QCOMPARE(arguments.at(0).toULongLong(), id);
        const Xc2Error error = qvariant_cast<Xc2Error>(arguments.at(1));
        QCOMPARE(error.category == Xc2ErrorCategory::None, expectedOk);
        if (!expectedOk) {
            QCOMPARE(error.category, Xc2ErrorCategory::Contract);
            QCOMPARE(error.httpStatus, status);
            QCOMPARE(error.rawPayload, body);
            QCOMPARE(error.endpoint, operation == 0
                         ? QStringLiteral("device/apply")
                         : QStringLiteral("device/close"));
        }
        QCOMPARE(applyFinished.count(), 0);
        QCOMPARE(closeFinished.count(), 0);
        QCOMPARE(shutdownFinished.count(), 0);
        QCOMPARE(server.requestCount(), 1);
    }

    void typedVciFailureDispatchMatrix_data()
    {
        QTest::addColumn<int>("operation");
        QTest::addColumn<int>("failureMode");
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("expectedCategory");

        QTest::newRow("lookup transport")
            << 0 << 1 << 0 << QByteArray{}
            << int(Xc2ErrorCategory::Transport);
        QTest::newRow("devices backend error")
            << 1 << 0 << 403 << backendError(403, 1007)
            << int(Xc2ErrorCategory::Backend);
        QTest::newRow("selected malformed backend error")
            << 2 << 0 << 500 << QByteArrayLiteral("not-json")
            << int(Xc2ErrorCategory::Contract);
        QTest::newRow("apply backend error")
            << 3 << 0 << 403 << backendError(403, 1007)
            << int(Xc2ErrorCategory::Backend);
        QTest::newRow("close unapproved success status")
            << 4 << 0 << 201 << QByteArrayLiteral("null")
            << int(Xc2ErrorCategory::Contract);
    }

    void typedVciFailureDispatchMatrix()
    {
        QFETCH(int, operation);
        QFETCH(int, failureMode);
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(int, expectedCategory);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(failureMode == 1
            ? FakeHttpServer::closeBeforeStatus()
            : FakeHttpServer::complete(status, body));
        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy lookupFinished(
            &client, &Xc2RestClient::deviceLookupFinished);
        QSignalSpy devicesFinished(&client, &Xc2RestClient::devicesFinished);
        QSignalSpy selectedFinished(
            &client, &Xc2RestClient::selectedDeviceFinished);
        QSignalSpy applyFinished(&client, &Xc2RestClient::applyDeviceFinished);
        QSignalSpy closeFinished(&client, &Xc2RestClient::closeDeviceFinished);
        QSignalSpy shutdownFinished(&client, &Xc2RestClient::shutdownFinished);
        const Xc2VciDevice device{
            QStringLiteral("vci-1"), QStringLiteral("VCI One"),
            QStringLiteral("AVL Ditest VCI2K_DPDU_API"), std::nullopt,
        };

        Xc2RequestId id = 0;
        switch (operation) {
        case 0: id = client.requestDeviceLookup(); break;
        case 1: id = client.requestDevices(); break;
        case 2: id = client.requestSelectedDevice(); break;
        case 3: id = client.requestApplyDevice(device); break;
        case 4: id = client.requestCloseDevice(device); break;
        }
        QList<QSignalSpy *> spies{
            &lookupFinished, &devicesFinished, &selectedFinished,
            &applyFinished, &closeFinished,
        };
        QSignalSpy *active = spies.at(operation);
        QVERIFY(id != 0);
        QVERIFY(active->wait(kSignalWaitMs));
        QCOMPARE(active->count(), 1);
        const QList<QVariant> arguments = active->takeFirst();
        QCOMPARE(arguments.at(0).toULongLong(), id);

        Xc2Error error;
        if (operation == 0) {
            error = qvariant_cast<Xc2Result<Xc2JobAccepted>>(
                arguments.at(1)).error;
        } else if (operation == 1) {
            error = qvariant_cast<Xc2Result<QList<Xc2VciDevice>>>(
                arguments.at(1)).error;
        } else if (operation == 2) {
            error = qvariant_cast<Xc2Result<Xc2SelectedVci>>(
                arguments.at(1)).error;
        } else {
            error = qvariant_cast<Xc2Error>(arguments.at(1));
        }
        QCOMPARE(int(error.category), expectedCategory);
        QCOMPARE(error.httpStatus, status);
        QCOMPARE(error.rawPayload, body);
        const QStringList endpoints{
            QStringLiteral("device/lookup"), QStringLiteral("device/get"),
            QStringLiteral("device/getSelected"),
            QStringLiteral("device/apply"), QStringLiteral("device/close"),
        };
        QCOMPARE(error.endpoint, endpoints.at(operation));
        for (QSignalSpy *spy : spies)
            QCOMPARE(spy->count(), 0);
        QCOMPARE(shutdownFinished.count(), 0);
        QCOMPARE(server.requestCount(), 1);
    }

    void stateChangingVciRequestsAreSentOnceAfterTimeoutOrFailure_data()
    {
        QTest::addColumn<int>("operation");
        QTest::addColumn<int>("responseMode");
        QTest::addColumn<int>("expectedReason");

        QTest::newRow("apply total deadline")
            << 0 << 0 << int(Xc2TransportReason::Timeout);
        QTest::newRow("close network failure")
            << 1 << 1 << int(Xc2TransportReason::Network);
    }

    void stateChangingVciRequestsAreSentOnceAfterTimeoutOrFailure()
    {
        QFETCH(int, operation);
        QFETCH(int, responseMode);
        QFETCH(int, expectedReason);

        constexpr int deviceDeadlineMs = 80;
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(responseMode == 0
            ? FakeHttpServer::neverRespond()
            : FakeHttpServer::closeBeforeStatus());
        Xc2RestClientOptions options;
        options.totalDeadlineMs = 1000;
        options.transferTimeoutMs = 1000;
        options.deviceOperationDeadlineMs = deviceDeadlineMs;
        Xc2RestClient client(options);
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy captured(&server, &FakeHttpServer::requestCaptured);
        QSignalSpy applyFinished(&client, &Xc2RestClient::applyDeviceFinished);
        QSignalSpy closeFinished(&client, &Xc2RestClient::closeDeviceFinished);
        QSignalSpy shutdownFinished(&client, &Xc2RestClient::shutdownFinished);
        const Xc2VciDevice device{
            QStringLiteral("vci-1"), QStringLiteral("VCI One"),
            QStringLiteral("AVL Ditest VCI2K_DPDU_API"), std::nullopt,
        };

        const Xc2RequestId id = operation == 0
            ? client.requestApplyDevice(device)
            : client.requestCloseDevice(device);
        QSignalSpy *active = operation == 0 ? &applyFinished : &closeFinished;
        QVERIFY(id != 0);
        if (captured.isEmpty())
            QVERIFY(captured.wait(kSignalWaitMs));
        QCOMPARE(captured.count(), 1);
        QVERIFY(active->wait(kSignalWaitMs));
        QCOMPARE(active->count(), 1);
        const QList<QVariant> arguments = active->takeFirst();
        QCOMPARE(arguments.at(0).toULongLong(), id);
        const Xc2Error error = qvariant_cast<Xc2Error>(arguments.at(1));
        QCOMPARE(error.category, Xc2ErrorCategory::Transport);
        QCOMPARE(int(error.transportReason), expectedReason);
        QCOMPARE(error.endpoint, operation == 0
                     ? QStringLiteral("device/apply")
                     : QStringLiteral("device/close"));
        captured.clear();
        QVERIFY(!captured.wait(deviceDeadlineMs + 80));
        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(shutdownFinished.count(), 0);
    }

    void typedVci204FramingFailuresUseTypedSignals_data()
    {
        QTest::addColumn<int>("operation");
        QTest::addColumn<QByteArray>("wire");

        const QByteArray head = QByteArrayLiteral(
            "HTTP/1.1 204 No Content\r\nConnection: close\r\n");
        QTest::newRow("selected content length")
            << 0 << head + QByteArrayLiteral("Content-Length: 0\r\n\r\n");
        QTest::newRow("apply body bytes")
            << 1 << head + QByteArrayLiteral("\r\nx");
        QTest::newRow("close transfer encoding")
            << 2 << head
                + QByteArrayLiteral("Transfer-Encoding: chunked\r\n\r\n");
    }

    void typedVci204FramingFailuresUseTypedSignals()
    {
        QFETCH(int, operation);
        QFETCH(QByteArray, wire);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::rawResponse(wire));
        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy selectedFinished(
            &client, &Xc2RestClient::selectedDeviceFinished);
        QSignalSpy applyFinished(&client, &Xc2RestClient::applyDeviceFinished);
        QSignalSpy closeFinished(&client, &Xc2RestClient::closeDeviceFinished);
        QSignalSpy shutdownFinished(&client, &Xc2RestClient::shutdownFinished);
        const Xc2VciDevice device{
            QStringLiteral("vci-1"), QStringLiteral("VCI One"),
            QStringLiteral("AVL Ditest VCI2K_DPDU_API"), std::nullopt,
        };

        Xc2RequestId id = 0;
        if (operation == 0)
            id = client.requestSelectedDevice();
        else if (operation == 1)
            id = client.requestApplyDevice(device);
        else
            id = client.requestCloseDevice(device);
        QList<QSignalSpy *> spies{
            &selectedFinished, &applyFinished, &closeFinished,
        };
        QSignalSpy *active = spies.at(operation);
        QVERIFY(id != 0);
        QVERIFY(active->wait(kSignalWaitMs));
        QCOMPARE(active->count(), 1);
        const QList<QVariant> arguments = active->takeFirst();
        QCOMPARE(arguments.at(0).toULongLong(), id);
        Xc2Error error;
        if (operation == 0) {
            error = qvariant_cast<Xc2Result<Xc2SelectedVci>>(
                arguments.at(1)).error;
        } else {
            error = qvariant_cast<Xc2Error>(arguments.at(1));
        }
        QCOMPARE(error.category, Xc2ErrorCategory::Transport);
        QCOMPARE(error.transportReason, Xc2TransportReason::Network);
        QCOMPARE(error.httpStatus, 204);
        QCOMPARE(error.rawPayload, QByteArray{});
        const QStringList endpoints{
            QStringLiteral("device/getSelected"),
            QStringLiteral("device/apply"), QStringLiteral("device/close"),
        };
        QCOMPARE(error.endpoint, endpoints.at(operation));
        for (QSignalSpy *spy : spies)
            QCOMPARE(spy->count(), 0);
        QCOMPARE(shutdownFinished.count(), 0);
        QCOMPARE(server.requestCount(), 1);
    }

    void deletingClientInsideTypedFinishedSlotIsSafeAndExactlyOnce()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::complete(204));
        auto *client = new Xc2RestClient({500, 250});
        QPointer<Xc2RestClient> guard(client);
        QVERIFY(client->setBaseUrl(encodedBase(server)));
        QSignalSpy destroyed(client, &QObject::destroyed);
        int completionCount = 0;
        connect(client, &Xc2RestClient::applyDeviceFinished,
                this, [&client, &completionCount](Xc2RequestId,
                                                  const Xc2Error &) {
            ++completionCount;
            delete client;
            client = nullptr;
        });
        const Xc2VciDevice device{
            QStringLiteral("vci-1"), QStringLiteral("VCI One"),
            QStringLiteral("AVL Ditest VCI2K_DPDU_API"), std::nullopt,
        };

        guard->requestApplyDevice(device);
        if (destroyed.isEmpty())
            QVERIFY(destroyed.wait(kSignalWaitMs));
        QCOMPARE(destroyed.count(), 1);
        QCOMPARE(completionCount, 1);
        QVERIFY(guard.isNull());
        QCOMPARE(server.requestCount(), 1);
    }

    void deviceOperationDeadlineDefaultsToThirtySeconds()
    {
        QCOMPARE(Xc2RestClientOptions{}.deviceOperationDeadlineMs, 30000);
    }

    void closeBeforeStatusSendsOneRequestOnly_data()
    {
        QTest::addColumn<int>("endpointKind");

        QTest::newRow("GET") << 0;
        QTest::newRow("POST") << 1;
    }

    void closeBeforeStatusSendsOneRequestOnly()
    {
        QFETCH(int, endpointKind);

        constexpr int deadlineMs = 120;
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::closeBeforeStatus());

        Xc2RestClient client({deadlineMs, 500});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy captured(&server, &FakeHttpServer::requestCaptured);
        QSignalSpy statusFinished(
            &client, &Xc2RestClient::serviceStatusFinished);
        QSignalSpy shutdownFinished(&client, &Xc2RestClient::shutdownFinished);
        if (endpointKind == 0)
            client.requestServiceStatus();
        else
            client.requestShutdown();

        if (captured.isEmpty())
            QVERIFY(captured.wait(kSignalWaitMs));
        QSignalSpy *finished = endpointKind == 0
            ? &statusFinished : &shutdownFinished;
        if (finished->isEmpty())
            QVERIFY(finished->wait(kSignalWaitMs));
        QCOMPARE(finished->count(), 1);
        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(server.connectionCount(), 1);

        QSignalSpy extraCapture(&server, &FakeHttpServer::requestCaptured);
        QVERIFY(!extraCapture.wait(deadlineMs + 50));
        QCOMPARE(server.requestCount(), 1);
        QCOMPARE(server.connectionCount(), 1);
    }

    void freshSocketBypassesApplicationProxy()
    {
        FakeHttpServer origin(QHostAddress::LocalHost);
        FakeHttpServer proxyTrap(QHostAddress::LocalHost);
        QVERIFY(origin.isListening());
        QVERIFY(proxyTrap.isListening());
        origin.enqueueResponse(FakeHttpServer::complete(
            200, QByteArrayLiteral("alive")));
        proxyTrap.enqueueResponse(FakeHttpServer::neverRespond());

        const ApplicationProxyGuard guard(QNetworkProxy(
            QNetworkProxy::HttpProxy,
            QStringLiteral("127.0.0.1"), proxyTrap.port()));
        Xc2RestClient client({300, 200});
        QVERIFY(client.setBaseUrl(encodedBase(origin)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY2(finished.wait(kSignalWaitMs),
                 "request was captured by the application proxy");
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY2(result.ok(), qPrintable(result.error.message));
        QCOMPARE(origin.requestCount(), 1);
        QCOMPARE(proxyTrap.requestCount(), 0);
    }

    void socketReadBufferHasFixedHardLimit()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::neverRespond());

        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy captured(&server, &FakeHttpServer::requestCaptured);
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        const Xc2RequestId id = client.requestServiceStatus();
        if (captured.isEmpty())
            QVERIFY(captured.wait(kSignalWaitMs));
        const QList<QTcpSocket *> sockets = client.findChildren<QTcpSocket *>();
        QCOMPARE(sockets.size(), 1);
        QVERIFY(sockets.front()->readBufferSize() > 0);
        QVERIFY(sockets.front()->readBufferSize() <= 64 * 1024);

        client.abort(id);
        if (finished.isEmpty())
            QVERIFY(finished.wait(kSignalWaitMs));
        QCOMPARE(finished.count(), 1);
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QCOMPARE(result.error.transportReason, Xc2TransportReason::Canceled);
    }

    void get302DoesNotFollowRedirect()
    {
        FakeHttpServer origin(QHostAddress::LocalHost);
        FakeHttpServer trap(QHostAddress::LocalHost);
        QVERIFY(origin.isListening());
        QVERIFY(trap.isListening());
        origin.enqueueResponse(FakeHttpServer::redirect(
            302, trapUrl(trap), backendError(302, 3002)));

        Xc2RestClient client({160, 100});
        QVERIFY(client.setBaseUrl(encodedBase(origin)));
        QSignalSpy trapCaptured(&trap, &FakeHttpServer::requestCaptured);
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        QCOMPARE(finished.count(), 1);
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.category, Xc2ErrorCategory::Backend);
        QCOMPARE(result.error.httpStatus, 302);
        QCOMPARE(origin.requestCount(), 1);
        QVERIFY(!trapCaptured.wait(200));
        QCOMPARE(trap.requestCount(), 0);
    }

    void shutdown307DoesNotFollowRedirect()
    {
        FakeHttpServer origin(QHostAddress::LocalHost);
        FakeHttpServer trap(QHostAddress::LocalHost);
        QVERIFY(origin.isListening());
        QVERIFY(trap.isListening());
        origin.enqueueResponse(FakeHttpServer::redirect(
            307, trapUrl(trap), backendError(307, 3007)));

        Xc2RestClient client({160, 100});
        QVERIFY(client.setBaseUrl(encodedBase(origin)));
        QSignalSpy trapCaptured(&trap, &FakeHttpServer::requestCaptured);
        QSignalSpy finished(&client, &Xc2RestClient::shutdownFinished);
        client.requestShutdown();
        QVERIFY(finished.wait(kSignalWaitMs));
        QCOMPARE(finished.count(), 1);
        const Xc2Error error = qvariant_cast<Xc2Error>(
            finished.takeFirst().at(1));
        QCOMPARE(error.category, Xc2ErrorCategory::Backend);
        QCOMPARE(error.httpStatus, 307);
        QCOMPARE(origin.requestCount(), 1);
        QVERIFY(!trapCaptured.wait(200));
        QCOMPARE(trap.requestCount(), 0);
    }

    void cookieExportRequiresDerivedWebSocketAuthority()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::complete(
            200, QByteArrayLiteral("alive"),
            {{QByteArrayLiteral("Set-Cookie"),
              QByteArrayLiteral("session=synthetic; Path=/")}}));
        server.enqueueResponse(FakeHttpServer::complete(
            200,
            QByteArrayLiteral(
                R"({"loginName":"xcd","name":"Synthetic","permissions":[]})")));

        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const auto serviceResult =
            qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
                finished.takeFirst().at(1));
        QVERIFY(serviceResult.ok());

        const QUrl derived = client.webSocketUrl();
        const auto good = client.cookieHeaderFor(
            derived.toEncoded(QUrl::FullyEncoded));
        QVERIFY2(good.ok(), qPrintable(good.error.message));
        QCOMPARE(*good.value, QByteArrayLiteral("session=synthetic"));

        QSignalSpy userFinished(&client, &Xc2RestClient::currentUserFinished);
        client.requestCurrentUser();
        QVERIFY(userFinished.wait(kSignalWaitMs));
        const auto user = qvariant_cast<Xc2Result<Xc2CurrentUser>>(
            userFinished.takeFirst().at(1));
        QVERIFY2(user.ok(), qPrintable(user.error.message));
        QCOMPARE(server.requestCount(), 2);
        QCOMPARE(server.requests().at(1).headerValue("Cookie"),
                 QByteArrayLiteral("session=synthetic"));

        const quint16 wrongPort = server.port() == 65535
            ? server.port() - 1 : server.port() + 1;
        const QStringList invalid = {
            QStringLiteral("ws://127.0.0.1:%1/xc2-websocket")
                .arg(wrongPort),
            QStringLiteral("ws://127.0.0.1/xc2-websocket"),
            QStringLiteral("ws://localhost:%1/xc2-websocket")
                .arg(server.port()),
            QStringLiteral("wss://127.0.0.1:%1/xc2-websocket")
                .arg(server.port()),
            QStringLiteral("ws://127.0.0.1:%1/xc2-websocket/extra")
                .arg(server.port()),
            QStringLiteral("ws://127.0.0.1:%1/xc2-websocket?q=1")
                .arg(server.port()),
            QStringLiteral("ws://user@127.0.0.1:%1/xc2-websocket")
                .arg(server.port()),
            QStringLiteral("ws://127.0.0.1:%1/xc2-websocket#fragment")
                .arg(server.port()),
        };
        for (const QString &candidate : invalid) {
            const auto bad = client.cookieHeaderFor(candidate.toLatin1());
            QVERIFY2(!bad.ok(), qPrintable(candidate));
            QCOMPARE(bad.error.category, Xc2ErrorCategory::Contract);
        }
    }

    void shutdownCookieExportRequiresExactRestTarget()
    {
        Xc2RestClient unconfigured;
        const auto beforeBase = unconfigured.shutdownCookieHeaderFor(
            QByteArrayLiteral(
                "http://127.0.0.1:49123/xc2/1.0/serviceStatus/shutdown"));
        QVERIFY(!beforeBase.ok());
        QCOMPARE(beforeBase.error.category, Xc2ErrorCategory::Contract);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        Xc2RestClient client;
        QVERIFY(client.setBaseUrl(encodedBase(server)));

        const QByteArray exact = encodedShutdownTarget(server);
        const auto good = client.shutdownCookieHeaderFor(exact);
        QVERIFY2(good.ok(), qPrintable(good.error.message));
        QVERIFY(good.value->isEmpty());

        const quint16 wrongPort = server.port() == 65535
            ? server.port() - 1 : server.port() + 1;
        const QByteArray authority = QByteArrayLiteral("127.0.0.1:")
            + QByteArray::number(server.port());
        const QList<QByteArray> invalid = {
            client.webSocketUrl().toEncoded(QUrl::FullyEncoded),
            client.baseUrl().toEncoded(QUrl::FullyEncoded),
            QByteArrayLiteral("https://") + authority
                + QByteArrayLiteral(
                    "/xc2/1.0/serviceStatus/shutdown"),
            QByteArrayLiteral("http://localhost:")
                + QByteArray::number(server.port())
                + QByteArrayLiteral(
                    "/xc2/1.0/serviceStatus/shutdown"),
            QByteArrayLiteral("http://127.0.0.1:")
                + QByteArray::number(wrongPort)
                + QByteArrayLiteral(
                    "/xc2/1.0/serviceStatus/shutdown"),
            QByteArrayLiteral("http://") + authority
                + QByteArrayLiteral(
                    "/xc2/1.0/serviceStatus/status"),
            exact + QByteArrayLiteral("/"),
            exact + QByteArrayLiteral("?query=1"),
            exact + QByteArrayLiteral("#fragment"),
            QByteArrayLiteral("http://user@") + authority
                + QByteArrayLiteral(
                    "/xc2/1.0/serviceStatus/shutdown"),
            QByteArrayLiteral("http://") + authority
                + QByteArrayLiteral(
                    "/xc2/1.0/%73erviceStatus/shutdown"),
        };
        for (const QByteArray &candidate : invalid) {
            const auto bad = client.shutdownCookieHeaderFor(candidate);
            QVERIFY2(!bad.ok(), candidate.constData());
            QCOMPARE(bad.error.category, Xc2ErrorCategory::Contract);
        }
    }

    void shutdownCookieExportUsesRestPathScope()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::complete(
            200, QByteArrayLiteral("alive"),
            {{QByteArrayLiteral("Set-Cookie"),
              QByteArrayLiteral("defaulted=one")},
             {QByteArrayLiteral("Set-Cookie"),
              QByteArrayLiteral("context=two; Path=/xc2/1.0")},
             {QByteArrayLiteral("Set-Cookie"),
              QByteArrayLiteral("root=three; Path=/")},
             {QByteArrayLiteral("Set-Cookie"),
              QByteArrayLiteral(
                  "websocket=four; Path=/xc2-websocket")}}));

        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const auto status = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY2(status.ok(), qPrintable(status.error.message));

        const auto shutdown = client.shutdownCookieHeaderFor(
            encodedShutdownTarget(server));
        QVERIFY2(shutdown.ok(), qPrintable(shutdown.error.message));
        const QList<QByteArray> shutdownPairs = cookiePairs(*shutdown.value);
        QCOMPARE(shutdownPairs.size(), 3);
        QVERIFY(shutdownPairs.contains(QByteArrayLiteral("defaulted=one")));
        QVERIFY(shutdownPairs.contains(QByteArrayLiteral("context=two")));
        QVERIFY(shutdownPairs.contains(QByteArrayLiteral("root=three")));
        QVERIFY(!shutdownPairs.contains(
            QByteArrayLiteral("websocket=four")));

        const auto websocket = client.cookieHeaderFor(
            client.webSocketUrl().toEncoded(QUrl::FullyEncoded));
        QVERIFY2(websocket.ok(), qPrintable(websocket.error.message));
        const QList<QByteArray> websocketPairs = cookiePairs(*websocket.value);
        QCOMPARE(websocketPairs.size(), 2);
        QVERIFY(websocketPairs.contains(QByteArrayLiteral("root=three")));
        QVERIFY(websocketPairs.contains(
            QByteArrayLiteral("websocket=four")));
        QVERIFY(!websocketPairs.contains(
            QByteArrayLiteral("defaulted=one")));
        QVERIFY(!websocketPairs.contains(QByteArrayLiteral("context=two")));
    }

    void successfulCrossAuthorityRebaseClearsCookieJar()
    {
        FakeHttpServer firstAuthority(QHostAddress::LocalHost);
        FakeHttpServer secondAuthority(QHostAddress::LocalHost);
        QVERIFY(firstAuthority.isListening());
        QVERIFY(secondAuthority.isListening());
        firstAuthority.enqueueResponse(FakeHttpServer::complete(
            200, QByteArrayLiteral("alive"),
            {{QByteArrayLiteral("Set-Cookie"),
              QByteArrayLiteral("session=authority-one; Path=/")}}));

        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(firstAuthority)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const QByteArray firstWebSocket =
            client.webSocketUrl().toEncoded(QUrl::FullyEncoded);
        const auto before = client.cookieHeaderFor(firstWebSocket);
        QVERIFY(before.ok());
        QCOMPARE(*before.value,
                 QByteArrayLiteral("session=authority-one"));
        const QByteArray firstShutdown =
            encodedShutdownTarget(firstAuthority);
        const auto shutdownBefore =
            client.shutdownCookieHeaderFor(firstShutdown);
        QVERIFY(shutdownBefore.ok());
        QCOMPARE(*shutdownBefore.value,
                 QByteArrayLiteral("session=authority-one"));

        QVERIFY(client.setBaseUrl(encodedBase(secondAuthority)));
        const QByteArray secondWebSocket =
            client.webSocketUrl().toEncoded(QUrl::FullyEncoded);
        QVERIFY(secondWebSocket != firstWebSocket);
        const auto after = client.cookieHeaderFor(secondWebSocket);
        QVERIFY(after.ok());
        QVERIFY(after.value->isEmpty());
        const auto oldAuthority = client.cookieHeaderFor(firstWebSocket);
        QVERIFY(!oldAuthority.ok());
        QCOMPARE(oldAuthority.error.category, Xc2ErrorCategory::Contract);
        const QByteArray secondShutdown =
            encodedShutdownTarget(secondAuthority);
        const auto shutdownAfter =
            client.shutdownCookieHeaderFor(secondShutdown);
        QVERIFY(shutdownAfter.ok());
        QVERIFY(shutdownAfter.value->isEmpty());
        const auto oldShutdown =
            client.shutdownCookieHeaderFor(firstShutdown);
        QVERIFY(!oldShutdown.ok());
        QCOMPARE(oldShutdown.error.category, Xc2ErrorCategory::Contract);
    }

    void rebaseWhilePendingFailsAndPreservesAuthorityAndCookies()
    {
        FakeHttpServer origin(QHostAddress::LocalHost);
        FakeHttpServer otherAuthority(QHostAddress::LocalHost);
        QVERIFY(origin.isListening());
        QVERIFY(otherAuthority.isListening());
        origin.enqueueResponse(FakeHttpServer::complete(
            200, QByteArrayLiteral("alive"),
            {{QByteArrayLiteral("Set-Cookie"),
              QByteArrayLiteral("session=stable; Path=/")}}));
        origin.enqueueResponse(FakeHttpServer::neverRespond());

        Xc2RestClient client({1000, 1000});
        QVERIFY(client.setBaseUrl(encodedBase(origin)));
        QSignalSpy initialFinished(
            &client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(initialFinished.wait(kSignalWaitMs));
        const auto initial = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            initialFinished.takeFirst().at(1));
        QVERIFY(initial.ok());

        const QUrl originalBase = client.baseUrl();
        const QUrl originalWebSocket = client.webSocketUrl();
        const QByteArray websocketBytes =
            originalWebSocket.toEncoded(QUrl::FullyEncoded);
        const auto cookieBefore = client.cookieHeaderFor(websocketBytes);
        QVERIFY(cookieBefore.ok());
        QCOMPARE(*cookieBefore.value, QByteArrayLiteral("session=stable"));
        const QByteArray shutdownBytes = encodedShutdownTarget(origin);
        const auto shutdownBefore =
            client.shutdownCookieHeaderFor(shutdownBytes);
        QVERIFY(shutdownBefore.ok());
        QCOMPARE(*shutdownBefore.value, QByteArrayLiteral("session=stable"));

        QSignalSpy captured(&origin, &FakeHttpServer::requestCaptured);
        QSignalSpy pendingFinished(
            &client, &Xc2RestClient::serviceStatusFinished);
        const Xc2RequestId pendingId = client.requestServiceStatus();
        if (captured.isEmpty())
            QVERIFY(captured.wait(kSignalWaitMs));

        Xc2Error rebaseError;
        QVERIFY(!client.setBaseUrl(encodedBase(otherAuthority), &rebaseError));
        QCOMPARE(rebaseError.category, Xc2ErrorCategory::Contract);
        QCOMPARE(client.baseUrl(), originalBase);
        QCOMPARE(client.webSocketUrl(), originalWebSocket);
        const auto cookieAfter = client.cookieHeaderFor(websocketBytes);
        QVERIFY(cookieAfter.ok());
        QCOMPARE(*cookieAfter.value, QByteArrayLiteral("session=stable"));
        const auto shutdownAfter =
            client.shutdownCookieHeaderFor(shutdownBytes);
        QVERIFY(shutdownAfter.ok());
        QCOMPARE(*shutdownAfter.value, QByteArrayLiteral("session=stable"));
        QCOMPARE(otherAuthority.requestCount(), 0);

        client.abort(pendingId);
        if (pendingFinished.isEmpty())
            QVERIFY(pendingFinished.wait(kSignalWaitMs));
        QCOMPARE(pendingFinished.count(), 1);
        const auto canceled = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            pendingFinished.takeFirst().at(1));
        QVERIFY(!canceled.ok());
        QCOMPARE(canceled.error.transportReason,
                 Xc2TransportReason::Canceled);
    }

    void fragmentedContentLengthResponse()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::fragmentedRawResponse({
            QByteArrayLiteral("HTTP/1."),
            QByteArrayLiteral("1 200 O"),
            QByteArrayLiteral("K\r"),
            QByteArrayLiteral("\nContent-Length"),
            QByteArrayLiteral(": 5\r"),
            QByteArrayLiteral("\nX-Synthetic: fragmented\r"),
            QByteArrayLiteral("\n\r"),
            QByteArrayLiteral("\na"),
            QByteArrayLiteral("li"),
            QByteArrayLiteral("ve"),
        }));

        Xc2RestClient client({500, 100});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY2(result.ok(), qPrintable(result.error.message));
        QCOMPARE(server.requestCount(), 1);
    }

    void fragmentedChunkedResponseExportsMultipleCookies()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::fragmentedRawResponse({
            QByteArrayLiteral("HTTP/1.1 200 OK\r\nTransfer-Encoding: ch"),
            QByteArrayLiteral("unked\r\nSet-Cookie: first=one; Path=/\r\n"),
            QByteArrayLiteral(
                "Set-Cookie: second=two; Path=/\r\nConnection: close\r\n\r\n"),
            QByteArrayLiteral("2\r\na"),
            QByteArrayLiteral("l\r\n3\r\niv"),
            QByteArrayLiteral("e\r\n0\r\nX-Synthetic: trailer\r\n\r\n"),
        }));

        Xc2RestClient client({500, 100});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        QCOMPARE(finished.count(), 1);
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY2(result.ok(), qPrintable(result.error.message));

        const auto cookies = client.cookieHeaderFor(
            client.webSocketUrl().toEncoded(QUrl::FullyEncoded));
        QVERIFY(cookies.ok());
        const QList<QByteArray> pairs = cookies.value->split(';');
        QVERIFY(pairs.contains(QByteArrayLiteral("first=one")));
        QVERIFY(pairs.contains(QByteArrayLiteral(" second=two"))
                || pairs.contains(QByteArrayLiteral("second=two")));
        QCOMPARE(server.requestCount(), 1);
    }

    void acceptsStrictFragmentedChunkExtensions()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::fragmentedRawResponse({
            QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                "Set-Cookie: strict=accepted; Path=/\r\n\r\n"
                "5 \t; \tflag;token \t= \tvalue;quoted \t= \t\"a"),
            QByteArrayLiteral("\tb"),
            QByteArray(1, '\\'),
            QByteArrayLiteral("\"c"),
            QByteArray(1, '\\'),
            QByteArrayLiteral("\\d\";empty \t= \t\"\"\r"),
            QByteArrayLiteral("\nalive\r\n0;end=\""),
            QByteArrayLiteral("ok\"\r\n"),
            QByteArrayLiteral(
                "Set-Cookie: trailer=ignored; Path=/\r\n"
                "X-Trailer:\taccepted\r\n\r\n"),
        }));

        Xc2RestClient client({500, 100});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY2(result.ok(), qPrintable(result.error.message));
        const auto cookies = client.cookieHeaderFor(
            client.webSocketUrl().toEncoded(QUrl::FullyEncoded));
        QVERIFY(cookies.ok());
        QCOMPARE(*cookies.value, QByteArrayLiteral("strict=accepted"));
        QCOMPARE(server.requestCount(), 1);
    }

    void rejectsMalformedChunkExtensions_data()
    {
        QTest::addColumn<QByteArray>("chunkLine");

        QTest::newRow("empty extension name") << QByteArrayLiteral("5;");
        QTest::newRow("empty second extension name")
            << QByteArrayLiteral("5;first;;second");
        QTest::newRow("space in extension name")
            << QByteArrayLiteral("5;bad name");
        QTest::newRow("empty token value") << QByteArrayLiteral("5;name=");
        QTest::newRow("unterminated quoted value")
            << QByteArrayLiteral("5;name=\"unterminated");
        QTest::newRow("junk after quoted value")
            << QByteArrayLiteral("5;name=\"ok\"junk");
        QTest::newRow("NUL in quoted value")
            << QByteArrayLiteral("5;name=\"bad") + QByteArray(1, '\0')
                + QByteArrayLiteral("value\"");
        QTest::newRow("DEL in quoted value")
            << QByteArrayLiteral("5;name=\"bad") + QByteArray(1, '\x7f')
                + QByteArrayLiteral("value\"");
        QTest::newRow("bare CR in quoted value")
            << QByteArrayLiteral("5;name=\"bad\rvalue\"");
        QTest::newRow("illegal quoted pair")
            << QByteArrayLiteral("5;name=\"bad") + QByteArray(1, '\\')
                + QByteArray(1, '\x01') + QByteArrayLiteral("\"");
        QTest::newRow("trailing whitespace after size")
            << QByteArrayLiteral("5 ");
        QTest::newRow("trailing whitespace after name")
            << QByteArrayLiteral("5;name\t");
        QTest::newRow("trailing whitespace after value")
            << QByteArrayLiteral("5;name=value ");
    }

    void rejectsMalformedChunkExtensions()
    {
        QFETCH(QByteArray, chunkLine);

        const qsizetype split = qMax<qsizetype>(1, chunkLine.size() / 2);
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::fragmentedRawResponse({
            QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                "Set-Cookie: invalid=must-not-stick; Path=/\r\n\r\n")
                + chunkLine.left(split),
            chunkLine.mid(split),
            QByteArrayLiteral("\r\nalive\r\n0\r\n\r\n"),
        }));

        Xc2RestClient client({500, 100});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.category, Xc2ErrorCategory::Transport);
        QCOMPARE(result.error.transportReason, Xc2TransportReason::Network);
        QCOMPARE(result.error.httpStatus, 200);
        const auto cookies = client.cookieHeaderFor(
            client.webSocketUrl().toEncoded(QUrl::FullyEncoded));
        QVERIFY(cookies.ok());
        QVERIFY(cookies.value->isEmpty());
        const auto shutdownCookies = client.shutdownCookieHeaderFor(
            encodedShutdownTarget(server));
        QVERIFY(shutdownCookies.ok());
        QVERIFY(shutdownCookies.value->isEmpty());
        QCOMPARE(server.requestCount(), 1);
    }

    void acceptsHtabInReasonHeaderAndTrailer()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::rawResponse(QByteArrayLiteral(
            "HTTP/1.1 200 OK\tSynthetic\r\n"
            "X-Synthetic:\tvalue\tpart\r\n"
            "Transfer-Encoding: chunked\r\n\r\n"
            "5\r\nalive\r\n0\r\nX-Trailer:\tvalue\tpart\r\n\r\n")));

        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY2(result.ok(), qPrintable(result.error.message));
        QCOMPARE(server.requestCount(), 1);
    }

    void malformedOrTruncatedChunkedIsTransportFailure_data()
    {
        QTest::addColumn<QByteArray>("wire");

        const QByteArray head = QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
            "Connection: close\r\n\r\n");
        QTest::newRow("invalid chunk size")
            << head + QByteArrayLiteral("Z\r\nalive\r\n0\r\n\r\n");
        QTest::newRow("truncated chunk data")
            << head + QByteArrayLiteral("5\r\nali");
        QTest::newRow("missing chunk terminator")
            << head + QByteArrayLiteral("5\r\naliveX");
        QTest::newRow("transfer encoding with content length")
            << QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
                "Content-Length: 5\r\nConnection: close\r\n\r\n"
                "5\r\nalive\r\n0\r\n\r\n");
    }

    void malformedOrTruncatedChunkedIsTransportFailure()
    {
        QFETCH(QByteArray, wire);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::rawResponse(wire));
        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        QCOMPARE(finished.count(), 1);
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.category, Xc2ErrorCategory::Transport);
        QCOMPARE(result.error.transportReason, Xc2TransportReason::Network);
        QCOMPARE(result.error.httpStatus, 200);
        QCOMPARE(server.requestCount(), 1);
    }

    void rejectsAmbiguousOrOversizedResponses_data()
    {
        QTest::addColumn<QByteArray>("wire");

        const QByteArray chunkedHead = QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
        QTest::newRow("duplicate Content-Length")
            << QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                "Content-Length: 5\r\n\r\nalive");
        QTest::newRow("signed Content-Length")
            << QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nContent-Length: +5\r\n\r\nalive");
        QTest::newRow("unsupported Transfer-Encoding")
            << QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nalive");
        QTest::newRow("overflowing chunk size")
            << chunkedHead
                + QByteArrayLiteral("FFFFFFFFFFFFFFFFF\r\nalive\r\n0\r\n\r\n");
        QTest::newRow("chunk exceeds decoded body cap")
            << chunkedHead + QByteArrayLiteral("800001\r\n");
        QTest::newRow("oversized chunk extension line")
            << chunkedHead + QByteArrayLiteral("1;")
                + QByteArray(64 * 1024, 'x')
                + QByteArrayLiteral("\r\na\r\n0\r\n\r\n");
        QTest::newRow("oversized trailers")
            << chunkedHead + QByteArrayLiteral("1\r\na\r\n0\r\nX-Large: ")
                + QByteArray(64 * 1024, 'x')
                + QByteArrayLiteral("\r\n\r\n");
        QTest::newRow("close-delimited body exceeds cap")
            << QByteArrayLiteral("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n")
                + QByteArray(8 * 1024 * 1024 + 1, 'x');
    }

    void rejectsAmbiguousOrOversizedResponses()
    {
        QFETCH(QByteArray, wire);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::rawResponse(wire));
        Xc2RestClient client({3000, 1000});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(5000));
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.category, Xc2ErrorCategory::Transport);
        QCOMPARE(result.error.transportReason, Xc2TransportReason::Network);
        QCOMPARE(result.error.httpStatus, 200);
        QCOMPARE(server.requestCount(), 1);
    }

    void burstLimitFailuresDoNotInstallCookies_data()
    {
        QTest::addColumn<QByteArray>("wire");
        QTest::addColumn<int>("expectedHttpStatus");

        const QByteArray cookie = QByteArrayLiteral(
            "Set-Cookie: burst=must-not-stick; Path=/\r\n");
        const QByteArray oversizedBody(8 * 1024 * 1024 + 1, 'x');
        QTest::newRow("oversized header burst")
            << QByteArrayLiteral("HTTP/1.1 200 OK\r\n") + cookie
                + QByteArrayLiteral("X-Burst: ")
                + QByteArray(64 * 1024, 'h')
                + QByteArrayLiteral("\r\nContent-Length: 5\r\n\r\nalive")
            << 0;
        QTest::newRow("oversized Content-Length body burst")
            << QByteArrayLiteral(
                   "HTTP/1.1 200 OK\r\nContent-Length: 8388609\r\n")
                + cookie + QByteArrayLiteral("\r\n") + oversizedBody
            << 200;
        QTest::newRow("oversized chunked body burst")
            << QByteArrayLiteral(
                   "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n")
                + cookie + QByteArrayLiteral("\r\n800001\r\n")
                + oversizedBody + QByteArrayLiteral("\r\n0\r\n\r\n")
            << 200;
        QTest::newRow("oversized close-delimited body burst")
            << QByteArrayLiteral("HTTP/1.1 200 OK\r\n") + cookie
                + QByteArrayLiteral("Connection: close\r\n\r\n")
                + oversizedBody
            << 200;
        QTest::newRow("oversized chunk-line burst")
            << QByteArrayLiteral(
                   "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n")
                + cookie + QByteArrayLiteral("\r\n1;name=")
                + QByteArray(64 * 1024, 'c')
                + QByteArrayLiteral("\r\na\r\n0\r\n\r\n")
            << 200;
        QTest::newRow("oversized trailer burst")
            << QByteArrayLiteral(
                   "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n")
                + cookie + QByteArrayLiteral("\r\n1\r\na\r\n0\r\nX-Burst: ")
                + QByteArray(64 * 1024, 't') + QByteArrayLiteral("\r\n\r\n")
            << 200;
    }

    void burstLimitFailuresDoNotInstallCookies()
    {
        QFETCH(QByteArray, wire);
        QFETCH(int, expectedHttpStatus);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::rawResponse(wire));
        Xc2RestClient client({5000, 2000});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(10000));
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.category, Xc2ErrorCategory::Transport);
        QCOMPARE(result.error.transportReason, Xc2TransportReason::Network);
        QCOMPARE(result.error.httpStatus, expectedHttpStatus);
        const auto cookies = client.cookieHeaderFor(
            client.webSocketUrl().toEncoded(QUrl::FullyEncoded));
        QVERIFY(cookies.ok());
        QVERIFY(cookies.value->isEmpty());
        QCOMPARE(server.requestCount(), 1);
    }

    void acceptsCloseDelimitedResponse()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::rawResponse(
            QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nalive")));
        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY2(result.ok(), qPrintable(result.error.message));
        QCOMPARE(server.requestCount(), 1);
    }

    void largeCloseDelimitedResponseDrainsBeforeEofCompletion()
    {
        QByteArray body = backendError(500, 1500);
        body.chop(1);
        body += QByteArrayLiteral(",\"padding\":\"")
            + QByteArray(256 * 1024, 'x') + QByteArrayLiteral("\"}");
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::rawResponse(
            QByteArrayLiteral(
                "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n")
            + body));

        Xc2RestClient client({1500, 500});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.category, Xc2ErrorCategory::Backend);
        QCOMPARE(result.error.httpStatus, 500);
        QCOMPARE(result.error.rawPayload, body);
        QCOMPARE(server.requestCount(), 1);
        QSignalSpy extra(&client, &Xc2RestClient::serviceStatusFinished);
        QVERIFY(!extra.wait(80));
    }

    void rejectsResponseBodyOverEightMiB()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::rawResponse(
            QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nContent-Length: 8388609\r\n"
                "Connection: close\r\n\r\n")));
        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.category, Xc2ErrorCategory::Transport);
        QCOMPARE(result.error.transportReason, Xc2TransportReason::Network);
        QCOMPARE(result.error.httpStatus, 200);
        QCOMPARE(server.requestCount(), 1);
    }

    void rejectsResponseHeadersOver64KiB()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        const QByteArray wire = QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nX-Large: ")
            + QByteArray(64 * 1024, 'a')
            + QByteArrayLiteral("\r\nContent-Length: 5\r\n\r\nalive");
        server.enqueueResponse(FakeHttpServer::rawResponse(wire));

        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.category, Xc2ErrorCategory::Transport);
        QCOMPARE(result.error.transportReason, Xc2TransportReason::Network);
        QCOMPARE(result.error.httpStatus, 0);
        QCOMPARE(server.requestCount(), 1);
    }

    void truncatedResponseDoesNotInstallCookies()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::rawResponse(QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
            "Set-Cookie: session=must-not-stick; Path=/\r\n\r\nali")));

        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.category, Xc2ErrorCategory::Transport);
        QCOMPARE(result.error.transportReason, Xc2TransportReason::Network);

        const auto cookies = client.cookieHeaderFor(
            client.webSocketUrl().toEncoded(QUrl::FullyEncoded));
        QVERIFY(cookies.ok());
        QVERIFY(cookies.value->isEmpty());
        QCOMPARE(server.requestCount(), 1);
    }

    void framedResponseCompletesWhilePeerKeepsConnectionOpen()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::rawResponseKeepOpen(
            QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nalive")));

        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        QElapsedTimer elapsed;
        elapsed.start();
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const qint64 completionMs = elapsed.elapsed();
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY2(result.ok(), qPrintable(result.error.message));
        QVERIFY2(completionMs < 250, qPrintable(QString::number(completionMs)));
        QCOMPARE(server.requestCount(), 1);
    }

    void largeFramedResponseCrossesReadWindowsWhilePeerStaysOpen()
    {
        const QByteArray body(256 * 1024, 'x');
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::rawResponseKeepOpen(
            QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Length: ")
            + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n")
            + body));

        Xc2RestClient client({1500, 500});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::shutdownFinished);
        QElapsedTimer elapsed;
        elapsed.start();
        client.requestShutdown();
        QVERIFY(finished.wait(kSignalWaitMs));
        const qint64 completionMs = elapsed.elapsed();
        const Xc2Error error = qvariant_cast<Xc2Error>(
            finished.takeFirst().at(1));
        QCOMPARE(error.category, Xc2ErrorCategory::None);
        QVERIFY2(completionMs < 1000, qPrintable(QString::number(completionMs)));
        QCOMPARE(server.requestCount(), 1);
        QSignalSpy extra(&client, &Xc2RestClient::shutdownFinished);
        QVERIFY(!extra.wait(80));
    }

    void rejectsForbidden204Framing_data()
    {
        QTest::addColumn<QByteArray>("wire");

        const QByteArray head = QByteArrayLiteral(
            "HTTP/1.1 204 No Content\r\n"
            "Set-Cookie: forbidden=must-not-stick; Path=/\r\n");
        QTest::newRow("Content-Length zero")
            << head + QByteArrayLiteral("Content-Length: 0\r\n\r\n");
        QTest::newRow("Transfer-Encoding")
            << head + QByteArrayLiteral(
                   "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
        QTest::newRow("body bytes without framing")
            << head + QByteArrayLiteral("\r\nalive");
    }

    void rejectsForbidden204Framing()
    {
        QFETCH(QByteArray, wire);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::rawResponse(wire));
        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.category, Xc2ErrorCategory::Transport);
        QCOMPARE(result.error.transportReason, Xc2TransportReason::Network);
        QCOMPARE(result.error.httpStatus, 204);
        const auto cookies = client.cookieHeaderFor(
            client.webSocketUrl().toEncoded(QUrl::FullyEncoded));
        QVERIFY(cookies.ok());
        QVERIFY(cookies.value->isEmpty());
        QCOMPARE(server.requestCount(), 1);
    }

    void rejectsInvalidStatusControlsAndFramingBytes_data()
    {
        QTest::addColumn<QByteArray>("wire");
        QTest::addColumn<int>("expectedHttpStatus");

        QTest::newRow("status above 599")
            << QByteArrayLiteral(
                "HTTP/1.1 600 Synthetic\r\nContent-Length: 5\r\n"
                "Connection: close\r\n\r\nalive")
            << 600;
        QTest::newRow("HTTP 1.0 status line")
            << QByteArrayLiteral(
                "HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nalive")
            << 0;
        QTest::newRow("extra status separator")
            << QByteArrayLiteral(
                "HTTP/1.1  200 OK\r\nContent-Length: 5\r\n\r\nalive")
            << 0;
        QTest::newRow("NUL in reason phrase")
            << (QByteArrayLiteral("HTTP/1.1 200 good")
                + QByteArray(1, '\0')
                + QByteArrayLiteral(
                    "bad\r\nContent-Length: 5\r\n\r\nalive"))
            << 200;
        QTest::newRow("NUL in header value")
            << (QByteArrayLiteral("HTTP/1.1 200 OK\r\nX-Synthetic: good")
                + QByteArray(1, '\0')
                + QByteArrayLiteral(
                    "bad\r\nContent-Length: 5\r\n\r\nalive"))
            << 200;
        QTest::newRow("bare CR in header value")
            << QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nX-Synthetic: good\rbad\r\n"
                "Content-Length: 5\r\n\r\nalive")
            << 200;
        QTest::newRow("DEL in trailer value")
            << (QByteArrayLiteral(
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    "5\r\nalive\r\n0\r\nX-Synthetic: good")
                + QByteArray(1, '\x7f')
                + QByteArrayLiteral("bad\r\n\r\n"))
            << 200;
        QTest::newRow("bytes beyond Content-Length")
            << QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\naliveX")
            << 200;
    }

    void rejectsInvalidStatusControlsAndFramingBytes()
    {
        QFETCH(QByteArray, wire);
        QFETCH(int, expectedHttpStatus);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::rawResponse(wire));
        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.category, Xc2ErrorCategory::Transport);
        QCOMPARE(result.error.transportReason, Xc2TransportReason::Network);
        QCOMPARE(result.error.httpStatus, expectedHttpStatus);
        QCOMPARE(server.requestCount(), 1);
    }

    void totalDeadlineStopsDripAndNeverResponses_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("transferTimeoutMs");

        QTest::newRow("continuous drip") << 0 << 40;
        QTest::newRow("no response bytes") << 1 << 500;
    }

    void totalDeadlineStopsDripAndNeverResponses()
    {
        QFETCH(int, mode);
        QFETCH(int, transferTimeoutMs);

        constexpr int totalDeadlineMs = 120;
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(mode == 0
            ? FakeHttpServer::drip(200, 10)
            : FakeHttpServer::neverRespond());

        Xc2RestClient client({totalDeadlineMs, transferTimeoutMs});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        QElapsedTimer elapsed;
        elapsed.start();
        client.requestServiceStatus();
        QVERIFY2(finished.wait(kSignalWaitMs),
                 "request survived its total wall-clock deadline");
        const qint64 completionMs = elapsed.elapsed();
        QCOMPARE(finished.count(), 1);
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.category, Xc2ErrorCategory::Transport);
        QCOMPARE(result.error.transportReason, Xc2TransportReason::Timeout);
        QVERIFY2(completionMs >= totalDeadlineMs - 20,
                 qPrintable(QString::number(completionMs)));
        QVERIFY2(completionMs < totalDeadlineMs + 500,
                 qPrintable(QString::number(completionMs)));
        QCOMPARE(server.requestCount(), 1);

        QSignalSpy extra(&client, &Xc2RestClient::serviceStatusFinished);
        QVERIFY(!extra.wait(80));
    }

    void abortAfterCaptureCompletesCanceledExactlyOnce()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::neverRespond());

        Xc2RestClient client({1000, 1000});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy captured(&server, &FakeHttpServer::requestCaptured);
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        const Xc2RequestId id = client.requestServiceStatus();
        if (captured.isEmpty())
            QVERIFY(captured.wait(kSignalWaitMs));
        QCOMPARE(server.requestCount(), 1);

        client.abort(id);
        if (finished.isEmpty())
            QVERIFY(finished.wait(kSignalWaitMs));
        QCOMPARE(finished.count(), 1);
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.category, Xc2ErrorCategory::Transport);
        QCOMPARE(result.error.transportReason, Xc2TransportReason::Canceled);

        QSignalSpy extra(&client, &Xc2RestClient::serviceStatusFinished);
        client.abort(id);
        QVERIFY(!extra.wait(80));
    }

    void timeoutAndFinishedRaceCompletesExactlyOnce()
    {
        constexpr int deadlineMs = 50;
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::delayedComplete(
            200, QByteArrayLiteral("alive"), deadlineMs));

        Xc2RestClient client({deadlineMs, 500});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy finished(&client, &Xc2RestClient::serviceStatusFinished);
        const Xc2RequestId id = client.requestServiceStatus();
        QVERIFY(finished.wait(kSignalWaitMs));
        QCOMPARE(finished.count(), 1);
        const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
            finished.takeFirst().at(1));
        QVERIFY(!result.ok());
        QCOMPARE(result.error.category, Xc2ErrorCategory::Transport);
        QCOMPARE(result.error.transportReason, Xc2TransportReason::Timeout);
        QCOMPARE(server.requestCount(), 1);

        QSignalSpy extra(&client, &Xc2RestClient::serviceStatusFinished);
        client.abort(id);
        QVERIFY(!extra.wait(100));
    }

    void deletingClientInsideFinishedSlotIsSafeAndExactlyOnce()
    {
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::complete(
            200, QByteArrayLiteral("alive")));

        auto *client = new Xc2RestClient({500, 250});
        QPointer<Xc2RestClient> guard(client);
        QVERIFY(client->setBaseUrl(encodedBase(server)));
        QSignalSpy destroyed(client, &QObject::destroyed);
        int completionCount = 0;
        connect(client, &Xc2RestClient::serviceStatusFinished,
                this, [&client, &completionCount](
                    Xc2RequestId,
                    const Xc2Result<Xc2ServiceStatus> &) {
            ++completionCount;
            delete client;
            client = nullptr;
        });

        guard->requestServiceStatus();
        if (destroyed.isEmpty())
            QVERIFY(destroyed.wait(kSignalWaitMs));
        QCOMPARE(destroyed.count(), 1);
        QCOMPARE(completionCount, 1);
        QVERIFY(guard.isNull());
        QCOMPARE(server.requestCount(), 1);

        QSignalSpy extraRequest(&server, &FakeHttpServer::requestCaptured);
        QVERIFY(!extraRequest.wait(80));
    }

    void rejectsUnapprovedEndpointSuccessStatuses_data()
    {
        QTest::addColumn<int>("endpointKind");
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QString>("endpoint");

        QTest::newRow("health 201")
            << 0 << 201 << QByteArrayLiteral("alive")
            << QStringLiteral("serviceStatus/status");
        QTest::newRow("current-user 202")
            << 1 << 202
            << QByteArrayLiteral(
                   R"({"loginName":"xcd","name":"Synthetic","permissions":[]})")
            << QStringLiteral("auth/currentUser");
        QTest::newRow("shutdown 206")
            << 2 << 206 << QByteArrayLiteral("partial")
            << QStringLiteral("serviceStatus/shutdown");
    }

    void rejectsUnapprovedEndpointSuccessStatuses()
    {
        QFETCH(int, endpointKind);
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(QString, endpoint);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::complete(
            status, body,
            {{QByteArrayLiteral("Set-Cookie"),
              QByteArrayLiteral("complete=installed; Path=/")}}));
        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy statusFinished(
            &client, &Xc2RestClient::serviceStatusFinished);
        QSignalSpy userFinished(&client, &Xc2RestClient::currentUserFinished);
        QSignalSpy shutdownFinished(&client, &Xc2RestClient::shutdownFinished);

        if (endpointKind == 0)
            client.requestServiceStatus();
        else if (endpointKind == 1)
            client.requestCurrentUser();
        else
            client.requestShutdown();
        QSignalSpy *finished = endpointKind == 0 ? &statusFinished
            : endpointKind == 1 ? &userFinished : &shutdownFinished;
        QVERIFY(finished->wait(kSignalWaitMs));
        QCOMPARE(finished->count(), 1);

        Xc2Error error;
        const QList<QVariant> arguments = finished->takeFirst();
        if (endpointKind == 0) {
            const auto result =
                qvariant_cast<Xc2Result<Xc2ServiceStatus>>(arguments.at(1));
            QVERIFY(!result.ok());
            error = result.error;
        } else if (endpointKind == 1) {
            const auto result =
                qvariant_cast<Xc2Result<Xc2CurrentUser>>(arguments.at(1));
            QVERIFY(!result.ok());
            error = result.error;
        } else {
            error = qvariant_cast<Xc2Error>(arguments.at(1));
        }
        QCOMPARE(error.category, Xc2ErrorCategory::Contract);
        QCOMPARE(error.transportReason, Xc2TransportReason::None);
        QCOMPARE(error.httpStatus, status);
        QCOMPARE(error.rawPayload, body);
        QCOMPARE(error.endpoint, endpoint);
        const auto cookies = client.cookieHeaderFor(
            client.webSocketUrl().toEncoded(QUrl::FullyEncoded));
        QVERIFY(cookies.ok());
        QCOMPARE(*cookies.value, QByteArrayLiteral("complete=installed"));
        QCOMPARE(server.requestCount(), 1);
    }

    void syntacticallyCompleteResponsesInstallCookies_data()
    {
        QTest::addColumn<int>("endpointKind");
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("expectedCategory");
        QTest::addColumn<QByteArray>("expectedCookie");

        QTest::newRow("approved shutdown 204")
            << 2 << 204 << QByteArray() << int(Xc2ErrorCategory::None)
            << QByteArrayLiteral("shutdown=installed");
        QTest::newRow("complete backend 403")
            << 0 << 403 << backendError(403, 1007)
            << int(Xc2ErrorCategory::Backend)
            << QByteArrayLiteral("backend=installed");
    }

    void syntacticallyCompleteResponsesInstallCookies()
    {
        QFETCH(int, endpointKind);
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        QFETCH(int, expectedCategory);
        QFETCH(QByteArray, expectedCookie);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::complete(
            status, body,
            {{QByteArrayLiteral("Set-Cookie"),
              expectedCookie + QByteArrayLiteral("; Path=/")}}));
        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy statusFinished(
            &client, &Xc2RestClient::serviceStatusFinished);
        QSignalSpy shutdownFinished(&client, &Xc2RestClient::shutdownFinished);
        if (endpointKind == 0)
            client.requestServiceStatus();
        else
            client.requestShutdown();
        QSignalSpy *finished = endpointKind == 0
            ? &statusFinished : &shutdownFinished;
        QVERIFY(finished->wait(kSignalWaitMs));
        const QList<QVariant> arguments = finished->takeFirst();
        const Xc2Error error = endpointKind == 0
            ? qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
                  arguments.at(1)).error
            : qvariant_cast<Xc2Error>(arguments.at(1));
        QCOMPARE(int(error.category), expectedCategory);
        const auto cookies = client.cookieHeaderFor(
            client.webSocketUrl().toEncoded(QUrl::FullyEncoded));
        QVERIFY(cookies.ok());
        QCOMPARE(*cookies.value, expectedCookie);
        QCOMPARE(server.requestCount(), 1);
    }

    void classifiesCompleteResponseMatrix_data()
    {
        QTest::addColumn<int>("endpointKind");
        QTest::addColumn<int>("wireMode");
        QTest::addColumn<int>("wireStatus");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<qint64>("declaredLength");
        QTest::addColumn<QByteArray>("mimeType");
        QTest::addColumn<bool>("expectedOk");
        QTest::addColumn<int>("expectedCategory");
        QTest::addColumn<int>("expectedReason");
        QTest::addColumn<int>("expectedHttpStatus");
        QTest::addColumn<int>("expectedXc2Status");
        QTest::addColumn<int>("expectedXc2Code");

        const auto add = [](const char *name,
                            int endpointKind,
                            int wireMode,
                            int wireStatus,
                            const QByteArray &body,
                            qint64 declaredLength,
                            const QByteArray &mimeType,
                            bool expectedOk,
                            Xc2ErrorCategory category,
                            Xc2TransportReason reason,
                            int expectedHttpStatus,
                            int expectedXc2Status = 0,
                            int expectedXc2Code = 0) {
            QTest::newRow(name)
                << endpointKind << wireMode << wireStatus << body
                << declaredLength << mimeType << expectedOk
                << int(category) << int(reason) << expectedHttpStatus
                << expectedXc2Status << expectedXc2Code;
        };

        add("close before status", 0, 1, 0, {}, -1, {}, false,
            Xc2ErrorCategory::Transport, Xc2TransportReason::Network, 0);
        add("truncated 200", 0, 2, 200, QByteArrayLiteral("ali"), 5, {},
            false, Xc2ErrorCategory::Transport,
            Xc2TransportReason::Network, 200);
        add("health missing MIME", 0, 0, 200, QByteArrayLiteral("alive"),
            -1, {}, true, Xc2ErrorCategory::None,
            Xc2TransportReason::None, 0);
        add("health arbitrary MIME", 0, 0, 200, QByteArrayLiteral("alive"),
            -1, QByteArrayLiteral("application/octet-stream"), true,
            Xc2ErrorCategory::None, Xc2TransportReason::None, 0);
        add("health 204 empty", 0, 0, 204, {}, -1, {}, false,
            Xc2ErrorCategory::Contract, Xc2TransportReason::None, 204);
        add("current user 204 empty", 1, 0, 204, {}, -1, {}, false,
            Xc2ErrorCategory::Contract, Xc2TransportReason::None, 204);
        add("shutdown 200 ignores body and MIME", 2, 0, 200,
            QByteArrayLiteral("not-json"), -1,
            QByteArrayLiteral("image/synthetic"), true,
            Xc2ErrorCategory::None, Xc2TransportReason::None, 0);
        add("shutdown 204 succeeds", 2, 0, 204, {}, -1, {}, true,
            Xc2ErrorCategory::None, Xc2TransportReason::None, 0);
        add("valid backend error", 0, 0, 403, backendError(403, 1007),
            -1, QByteArrayLiteral("application/json"), false,
            Xc2ErrorCategory::Backend, Xc2TransportReason::None,
            403, 403, 1007);
        add("malformed health success", 0, 0, 200,
            QByteArrayLiteral("starting"), -1, {}, false,
            Xc2ErrorCategory::Contract, Xc2TransportReason::None, 200);
        add("malformed user success", 1, 0, 200, QByteArrayLiteral("{}"),
            -1, QByteArrayLiteral("application/json"), false,
            Xc2ErrorCategory::Contract, Xc2TransportReason::None, 200);
        add("malformed non-2xx error", 0, 0, 500,
            QByteArrayLiteral("<html>broken</html>"), -1,
            QByteArrayLiteral("text/html"), false,
            Xc2ErrorCategory::Contract, Xc2TransportReason::None, 500);
        add("302 valid backend error", 0, 0, 302,
            backendError(302, 3002), -1,
            QByteArrayLiteral("application/json"), false,
            Xc2ErrorCategory::Backend, Xc2TransportReason::None,
            302, 302, 3002);
    }

    void classifiesCompleteResponseMatrix()
    {
        QFETCH(int, endpointKind);
        QFETCH(int, wireMode);
        QFETCH(int, wireStatus);
        QFETCH(QByteArray, body);
        QFETCH(qint64, declaredLength);
        QFETCH(QByteArray, mimeType);
        QFETCH(bool, expectedOk);
        QFETCH(int, expectedCategory);
        QFETCH(int, expectedReason);
        QFETCH(int, expectedHttpStatus);
        QFETCH(int, expectedXc2Status);
        QFETCH(int, expectedXc2Code);

        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        FakeHttpResponse response;
        if (wireMode == 1) {
            response = FakeHttpServer::closeBeforeStatus();
        } else if (wireMode == 2) {
            response = FakeHttpServer::truncated(
                wireStatus, body, declaredLength);
        } else {
            QList<QPair<QByteArray, QByteArray>> headers;
            if (!mimeType.isNull()) {
                headers.append({QByteArrayLiteral("Content-Type"), mimeType});
            }
            response = FakeHttpServer::complete(
                wireStatus, body, std::move(headers));
        }
        server.enqueueResponse(std::move(response));

        Xc2RestClient client({500, 250});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy statusFinished(
            &client, &Xc2RestClient::serviceStatusFinished);
        QSignalSpy userFinished(&client, &Xc2RestClient::currentUserFinished);
        QSignalSpy shutdownFinished(&client, &Xc2RestClient::shutdownFinished);

        if (endpointKind == 0)
            client.requestServiceStatus();
        else if (endpointKind == 1)
            client.requestCurrentUser();
        else
            client.requestShutdown();

        QSignalSpy *activeSpy = endpointKind == 0 ? &statusFinished
            : endpointKind == 1 ? &userFinished : &shutdownFinished;
        QVERIFY2(activeSpy->wait(kSignalWaitMs), "request did not finish");
        QCOMPARE(activeSpy->count(), 1);

        ObservedResult observed;
        const QList<QVariant> arguments = activeSpy->takeFirst();
        if (endpointKind == 0) {
            const auto result =
                qvariant_cast<Xc2Result<Xc2ServiceStatus>>(arguments.at(1));
            observed.ok = result.ok();
            observed.error = result.error;
        } else if (endpointKind == 1) {
            const auto result =
                qvariant_cast<Xc2Result<Xc2CurrentUser>>(arguments.at(1));
            observed.ok = result.ok();
            observed.error = result.error;
        } else {
            observed.error = qvariant_cast<Xc2Error>(arguments.at(1));
            observed.ok = observed.error.category == Xc2ErrorCategory::None;
        }

        QVERIFY2(observed.ok == expectedOk,
                 qPrintable(QStringLiteral(
                     "ok=%1 category=%2 reason=%3 http=%4 raw=%5 message=%6")
                     .arg(observed.ok)
                     .arg(int(observed.error.category))
                     .arg(int(observed.error.transportReason))
                     .arg(observed.error.httpStatus)
                     .arg(QString::fromLatin1(observed.error.rawPayload.toHex()))
                     .arg(observed.error.message)));
        QVERIFY2(int(observed.error.category) == expectedCategory,
                 qPrintable(QStringLiteral(
                     "category=%1 reason=%2 http=%3 raw=%4 message=%5")
                     .arg(int(observed.error.category))
                     .arg(int(observed.error.transportReason))
                     .arg(observed.error.httpStatus)
                     .arg(QString::fromLatin1(observed.error.rawPayload.toHex()))
                     .arg(observed.error.message)));
        QCOMPARE(int(observed.error.transportReason), expectedReason);
        if (!expectedOk) {
            QCOMPARE(observed.error.httpStatus, expectedHttpStatus);
            QCOMPARE(observed.error.rawPayload, body);
            QCOMPARE(observed.error.xc2Status, expectedXc2Status);
            QCOMPARE(observed.error.xc2Code, expectedXc2Code);
            const QString expectedEndpoint = endpointKind == 0
                ? QStringLiteral("serviceStatus/status")
                : endpointKind == 1
                    ? QStringLiteral("auth/currentUser")
                    : QStringLiteral("serviceStatus/shutdown");
            QCOMPARE(observed.error.endpoint, expectedEndpoint);
        }
        QCOMPARE(server.requestCount(), 1);
    }

    void shutdownIsOneExactEmptyFormPost()
    {
        constexpr int deadlineMs = 120;
        FakeHttpServer server(QHostAddress::LocalHost);
        QVERIFY(server.isListening());
        server.enqueueResponse(FakeHttpServer::closeBeforeStatus());

        Xc2RestClient client({deadlineMs, 500});
        QVERIFY(client.setBaseUrl(encodedBase(server)));
        QSignalSpy captured(&server, &FakeHttpServer::requestCaptured);
        QSignalSpy finished(&client, &Xc2RestClient::shutdownFinished);
        client.requestShutdown();
        if (captured.isEmpty())
            QVERIFY(captured.wait(kSignalWaitMs));
        if (finished.isEmpty())
            QVERIFY(finished.wait(kSignalWaitMs));
        QCOMPARE(finished.count(), 1);

        const Xc2Error error = qvariant_cast<Xc2Error>(
            finished.takeFirst().at(1));
        QVERIFY2(error.category == Xc2ErrorCategory::Transport,
                 qPrintable(QStringLiteral(
                     "category=%1 reason=%2 http=%3 raw=%4 message=%5")
                     .arg(int(error.category))
                     .arg(int(error.transportReason))
                     .arg(error.httpStatus)
                     .arg(QString::fromLatin1(error.rawPayload.toHex()))
                     .arg(error.message)));
        QCOMPARE(error.transportReason, Xc2TransportReason::Network);
        QCOMPARE(server.requestCount(), 1);
        const FakeHttpRequest &request = server.requests().front();
        QCOMPARE(request.method, QByteArrayLiteral("POST"));
        QCOMPARE(request.target,
                 QByteArrayLiteral("/xc2/1.0/serviceStatus/shutdown"));
        QCOMPARE(request.headerValue("Content-Type"),
                 QByteArrayLiteral("application/x-www-form-urlencoded"));
        QCOMPARE(request.headerValue("Content-Length"),
                 QByteArrayLiteral("0"));
        QVERIFY(request.body.isEmpty());

        QSignalSpy extraCapture(&server, &FakeHttpServer::requestCaptured);
        QVERIFY(!extraCapture.wait(deadlineMs + 50));
        QCOMPARE(server.requestCount(), 1);
    }
};

QTEST_GUILESS_MAIN(Xc2RestClientTest)
#include "test_Xc2RestClient.moc"
