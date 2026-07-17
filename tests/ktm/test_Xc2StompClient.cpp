#include "FakeXc2TransportServer.h"

#include "ktm/xc2/Xc2ContractProfile.h"
#include "ktm/xc2/Xc2RestClient.h"
#include "ktm/xc2/Xc2StompClient.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QNetworkProxy>
#include <QPointer>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <limits>

using namespace ktm::xc2;

namespace {

constexpr int kWaitMs = 1500;

class ApplicationProxyGuard final {
public:
    explicit ApplicationProxyGuard(const QNetworkProxy &proxy)
        : m_previous(QNetworkProxy::applicationProxy())
    {
        QNetworkProxy::setApplicationProxy(proxy);
    }

    ~ApplicationProxyGuard()
    {
        QNetworkProxy::setApplicationProxy(m_previous);
    }

private:
    QNetworkProxy m_previous;
};

bool waitForCount(QSignalSpy &spy, int count, int timeoutMs = kWaitMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (spy.count() < count) {
        const int remaining = timeoutMs - int(elapsed.elapsed());
        if (remaining <= 0 || !spy.wait(remaining))
            break;
    }
    return spy.count() >= count;
}

bool waitForFrames(FakeXc2TransportServer &server,
                   int count,
                   int timeoutMs = kWaitMs)
{
    QSignalSpy frames(&server, &FakeXc2TransportServer::stompFrameReceived);
    QElapsedTimer elapsed;
    elapsed.start();
    while (server.stompFrames().size() < count) {
        const int remaining = timeoutMs - int(elapsed.elapsed());
        if (remaining <= 0 || !frames.wait(remaining))
            break;
    }
    return server.stompFrames().size() >= count;
}

bool waitDuration(int durationMs)
{
    QTimer timer;
    timer.setSingleShot(true);
    QSignalSpy timeout(&timer, &QTimer::timeout);
    timer.start(durationMs);
    return timeout.wait(durationMs + kWaitMs);
}

bool establishRestSession(FakeXc2TransportServer &server,
                          Xc2RestClient &rest)
{
    if (!rest.setBaseUrl(server.encodedRestBase()))
        return false;
    QSignalSpy finished(&rest, &Xc2RestClient::serviceStatusFinished);
    if (rest.requestServiceStatus() == 0 || !waitForCount(finished, 1))
        return false;
    const auto result = qvariant_cast<Xc2Result<Xc2ServiceStatus>>(
        finished.constFirst().at(1));
    return result.ok() && result.value->alive;
}

Xc2StompFrame connectedFrame(const QByteArray &heartbeat = QByteArrayLiteral("0,0"))
{
    Xc2StompFrame frame;
    frame.command = QByteArrayLiteral("CONNECTED");
    frame.headers.insert(QByteArrayLiteral("version"), QByteArrayLiteral("1.2"));
    if (!heartbeat.isNull())
        frame.headers.insert(QByteArrayLiteral("heart-beat"), heartbeat);
    return frame;
}

Xc2StompFrame messageFrame(const QByteArray &destination,
                           const QByteArray &subscription,
                           const QByteArray &messageId = QByteArrayLiteral("message-1"),
                           const QByteArray &body = QByteArrayLiteral("payload"))
{
    Xc2StompFrame frame;
    frame.command = QByteArrayLiteral("MESSAGE");
    if (!destination.isNull())
        frame.headers.insert(QByteArrayLiteral("destination"), destination);
    if (!subscription.isNull())
        frame.headers.insert(QByteArrayLiteral("subscription"), subscription);
    if (!messageId.isNull())
        frame.headers.insert(QByteArrayLiteral("message-id"), messageId);
    frame.body = body;
    return frame;
}

Xc2StompFrame receiptFrame(const QByteArray &receiptId)
{
    Xc2StompFrame frame;
    frame.command = QByteArrayLiteral("RECEIPT");
    if (!receiptId.isNull())
        frame.headers.insert(QByteArrayLiteral("receipt-id"), receiptId);
    return frame;
}

QString stableId(Topic topic)
{
    switch (topic) {
    case Topic::VciStatus:
        return QStringLiteral("vci-status-subscription");
    case Topic::VehicleInfo:
        return QStringLiteral("vehicle-info-subscription");
    case Topic::Ecu:
        return QStringLiteral("ecu-subscription");
    case Topic::Progress:
        return QStringLiteral("progress-subscription");
    case Topic::MeasurementValues:
        return QStringLiteral("measurement-values-subscription");
    case Topic::FlowGui:
        return QStringLiteral("flow-gui-subscription");
    case Topic::FlowProgress:
        return QStringLiteral("flow-progress-subscription");
    case Topic::Login:
        return QStringLiteral("login-subscription");
    }
    return {};
}

bool establishStompSession(FakeXc2TransportServer &server,
                           Xc2RestClient &rest,
                           Xc2StompClient &client,
                           const QByteArray &heartbeat = QByteArrayLiteral("0,0"))
{
    const int baseline = server.stompFrames().size();
    QSignalSpy connected(&client, &Xc2StompClient::connected);
    if (!client.connectToBackend(rest)
        || !waitForFrames(server, baseline + 1)) {
        return false;
    }
    if (server.stompFrames().at(baseline).command
        != QByteArrayLiteral("CONNECT")) {
        return false;
    }
    if (!server.sendFrame(connectedFrame(heartbeat)))
        return false;
    return waitForCount(connected, 1);
}

QByteArray progressDestination()
{
    return Xc2ContractProfile::approved().topic(Topic::Progress).toUtf8();
}

} // namespace

class TestXc2StompClient final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<Xc2StompState>();
        qRegisterMetaType<Xc2StompSession>();
        qRegisterMetaType<Xc2StompMessage>();
        qRegisterMetaType<Topic>();
        qRegisterMetaType<Xc2Error>();
    }

    void derivesExactUrlAndCookieFromRestClient()
    {
        FakeXc2TransportServer server;
        QVERIFY(server.isListening());
        server.setRestCookie(QByteArrayLiteral("session=authority-bound"));
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));

        Xc2StompClient client;
        QVERIFY(client.connectToBackend(rest));
        QSignalSpy upgraded(&server,
                            &FakeXc2TransportServer::webSocketConnected);
        if (server.webSocketConnectionCount() == 0)
            QVERIFY(waitForCount(upgraded, 1));
        QCOMPARE(server.upgradeRequestCount(), 1);
        QCOMPARE(server.upgradeRequests().constFirst().target,
                 QByteArrayLiteral("/xc2-websocket"));
        QCOMPARE(server.upgradeRequests().constFirst().headerValue("Cookie"),
                 QByteArrayLiteral("session=authority-bound"));
        QCOMPARE(server.requestUrl(), rest.webSocketUrl());
    }

    void cannotRetargetCookieToAnotherAuthority()
    {
        FakeXc2TransportServer origin;
        FakeXc2TransportServer other;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(origin, rest));

        QUrl retargeted = rest.webSocketUrl();
        retargeted.setPort(other.port());
        const auto cookie = rest.cookieHeaderFor(
            retargeted.toEncoded(QUrl::FullyEncoded));
        QVERIFY(!cookie.ok());
        QCOMPARE(cookie.error.category, Xc2ErrorCategory::Contract);

        Xc2StompClient client;
        QVERIFY(client.connectToBackend(rest));
        QSignalSpy upgraded(&origin,
                            &FakeXc2TransportServer::webSocketConnected);
        if (origin.webSocketConnectionCount() == 0)
            QVERIFY(waitForCount(upgraded, 1));
        QCOMPARE(origin.upgradeRequestCount(), 1);
        QCOMPARE(other.connectionCount(), 0);
    }

    void ownedSocketBypassesApplicationProxy()
    {
        FakeXc2TransportServer origin;
        FakeXc2TransportServer proxyTrap;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(origin, rest));

        const ApplicationProxyGuard guard(QNetworkProxy(
            QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"),
            proxyTrap.port()));
        Xc2StompClient client;
        QVERIFY(client.connectToBackend(rest));
        QSignalSpy upgraded(&origin,
                            &FakeXc2TransportServer::webSocketConnected);
        if (origin.webSocketConnectionCount() == 0)
            QVERIFY(waitForCount(upgraded, 1));
        QCOMPARE(proxyTrap.connectionCount(), 0);
        QCOMPARE(origin.upgradeRequestCount(), 1);
    }

    void handshakeRedirectNeverReachesTrapOrLeaksCookie()
    {
        FakeXc2TransportServer origin;
        FakeXc2TransportServer redirectTrap;
        Xc2RestClient rest;
        origin.setRestCookie(QByteArrayLiteral("session=redirect-secret"));
        QVERIFY(establishRestSession(origin, rest));
        origin.setUpgradeMode(FakeXc2TransportServer::UpgradeMode::Redirect);
        QUrl trapUrl;
        trapUrl.setScheme(QStringLiteral("ws"));
        trapUrl.setHost(QStringLiteral("127.0.0.1"));
        trapUrl.setPort(redirectTrap.port());
        trapUrl.setPath(QStringLiteral("/trap"));
        origin.setRedirectTarget(trapUrl);

        Xc2StompClient client({250, 100, 10000, 10000, 2});
        QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
        QVERIFY(client.connectToBackend(rest));
        QVERIFY(waitForCount(errors, 1));
        QCOMPARE(redirectTrap.connectionCount(), 0);
        QCOMPARE(origin.upgradeRequestCount(), 1);
        QCOMPARE(origin.upgradeRequests().constFirst().headerValue("Cookie"),
                 QByteArrayLiteral("session=redirect-secret"));
    }

    void offersAndRequiresOnlyV12StompSubprotocol()
    {
        {
            FakeXc2TransportServer server;
            Xc2RestClient rest;
            QVERIFY(establishRestSession(server, rest));
            Xc2StompClient client;
            QVERIFY(client.connectToBackend(rest));
            QVERIFY(waitForFrames(server, 1));
            QCOMPARE(server.negotiatedSubprotocol(), QStringLiteral("v12.stomp"));
            QCOMPARE(server.upgradeRequests().constFirst().headerValue(
                         "Sec-WebSocket-Protocol"),
                     QByteArrayLiteral("v12.stomp"));
        }

        for (const int mode : {0, 1}) {
            FakeXc2TransportServer server;
            if (mode == 0) {
                server.setSupportedSubprotocols({});
            } else {
                server.setUpgradeMode(
                    FakeXc2TransportServer::UpgradeMode::DifferentSubprotocol);
            }
            Xc2RestClient rest;
            QVERIFY(establishRestSession(server, rest));
            Xc2StompClient client({250, 100, 10000, 10000, 2});
            QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
            QVERIFY(client.connectToBackend(rest));
            QVERIFY(waitForCount(errors, 1));
            QVERIFY(server.stompFrames().isEmpty());
        }
    }

    void sendsTextConnectWithExactRequiredHeaders()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client;
        QVERIFY(client.connectToBackend(rest));
        QVERIFY(waitForFrames(server, 1));
        const Xc2StompFrame &frame = server.stompFrames().constFirst();
        QCOMPARE(frame.command, QByteArrayLiteral("CONNECT"));
        QCOMPARE(frame.headers.size(), 3);
        QCOMPARE(frame.headers.value("accept-version"), QByteArrayLiteral("1.2"));
        QCOMPARE(frame.headers.value("host"), QByteArrayLiteral("127.0.0.1"));
        QCOMPARE(frame.headers.value("heart-beat"),
                 QByteArrayLiteral("10000,10000"));
        QCOMPARE(server.messages().constFirst().kind,
                 FakeXc2WebSocketMessageKind::Text);
    }

    void subscribesOnlyAfterValidConnected()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client;
        QVERIFY(client.subscribe(Topic::Progress));
        QVERIFY(client.connectToBackend(rest));
        QVERIFY(waitForFrames(server, 1));
        QCOMPARE(server.stompFrames().size(), 1);
        QCOMPARE(client.state(), Xc2StompState::StompConnecting);
        QVERIFY(server.sendFrame(connectedFrame()));
        QVERIFY(waitForFrames(server, 2));
        QCOMPARE(server.stompFrames().at(1).command,
                 QByteArrayLiteral("SUBSCRIBE"));
    }

    void connectedInEstablishedStateIsRejected()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client;
        QVERIFY(establishStompSession(server, rest, client));
        QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
        QVERIFY(server.sendFrame(connectedFrame()));
        QVERIFY(waitForCount(errors, 1));
        QCOMPARE(qvariant_cast<Xc2Error>(errors.constFirst().at(1)).category,
                 Xc2ErrorCategory::Contract);
        QCOMPARE(client.state(), Xc2StompState::Failed);
    }

    void rejectsInvalidConnectedFrames_data()
    {
        QTest::addColumn<QByteArray>("payload");
        Xc2StompFrame missing = connectedFrame();
        missing.headers.remove("version");
        QTest::newRow("missing version") << Xc2StompCodec::encode(missing);
        Xc2StompFrame downgraded = connectedFrame();
        downgraded.headers["version"] = QByteArrayLiteral("1.1");
        QTest::newRow("downgraded") << Xc2StompCodec::encode(downgraded);
        QTest::newRow("duplicate version")
            << QByteArrayLiteral(
                   "CONNECTED\nversion:1.2\nversion:1.2\nheart-beat:0,0\n\n\0");
        QTest::newRow("negative heartbeat")
            << QByteArrayLiteral(
                   "CONNECTED\nversion:1.2\nheart-beat:-1,0\n\n\0");
        QTest::newRow("message before connected")
            << Xc2StompCodec::encode(messageFrame(
                   progressDestination(), QByteArrayLiteral("progress-subscription")));
        QTest::newRow("receipt before connected")
            << Xc2StompCodec::encode(receiptFrame(QByteArrayLiteral("early")));
        QTest::newRow("unknown command")
            << QByteArrayLiteral("UNKNOWN\n\n\0");
    }

    void rejectsInvalidConnectedFrames()
    {
        QFETCH(QByteArray, payload);
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client;
        QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
        QVERIFY(client.connectToBackend(rest));
        QVERIFY(waitForFrames(server, 1));
        QVERIFY(server.sendText(payload));
        QVERIFY(waitForCount(errors, 1));
        const Xc2Error error = qvariant_cast<Xc2Error>(
            errors.constFirst().at(1));
        QCOMPARE(error.category, Xc2ErrorCategory::Contract);
        QCOMPARE(client.state(), Xc2StompState::Failed);
    }

    void errorFrameTerminatesGenerationExactlyOnce()
    {
        for (const bool afterConnected : {false, true}) {
            FakeXc2TransportServer server;
            Xc2RestClient rest;
            QVERIFY(establishRestSession(server, rest));
            Xc2StompClient client;
            QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
            QSignalSpy disconnected(&client, &Xc2StompClient::disconnected);
            if (afterConnected) {
                QVERIFY(establishStompSession(server, rest, client));
            } else {
                QVERIFY(client.connectToBackend(rest));
                QVERIFY(waitForFrames(server, 1));
            }
            Xc2StompFrame frame;
            frame.command = QByteArrayLiteral("ERROR");
            frame.headers.insert(QByteArrayLiteral("message"),
                                 QByteArrayLiteral("synthetic failure"));
            frame.body = QByteArrayLiteral("backend evidence");
            QVERIFY(server.sendFrame(frame));
            QVERIFY(waitForCount(errors, 1));
            QCOMPARE(waitForCount(disconnected, 1), true);
            const Xc2Error error = qvariant_cast<Xc2Error>(
                errors.constFirst().at(1));
            QCOMPARE(error.message, QStringLiteral("synthetic failure"));
            QCOMPARE(error.rawPayload, QByteArrayLiteral("backend evidence"));
            server.closeWebSocket();
            QVERIFY(waitDuration(80));
            QCOMPARE(errors.count(), 1);
            QCOMPARE(disconnected.count(), 1);
        }
    }

    void deletingClientInsideTerminalErrorSignalIsSafe()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        server.setUpgradeMode(FakeXc2TransportServer::UpgradeMode::NoResponse);
        auto *client = new Xc2StompClient({50, 50, 10000, 10000, 2});
        QPointer<Xc2StompClient> guard(client);
        QSignalSpy destroyed(client, &QObject::destroyed);
        connect(client, &Xc2StompClient::errorOccurred, this,
                [&client](Xc2StompGeneration, const Xc2Error &) {
            delete client;
            client = nullptr;
        });
        QVERIFY(client->connectToBackend(rest));
        QVERIFY(waitForCount(destroyed, 1));
        QVERIFY(guard.isNull());
    }

    void totalDeadlineCoversUpgradeAndStompNegotiation_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("accepted without upgrade") << 0;
        QTest::newRow("upgrade without connected") << 1;
        QTest::newRow("periodic partial stomp") << 2;
    }

    void totalDeadlineCoversUpgradeAndStompNegotiation()
    {
        QFETCH(int, mode);
        constexpr int deadlineMs = 100;
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        if (mode == 0)
            server.setUpgradeMode(FakeXc2TransportServer::UpgradeMode::NoResponse);
        Xc2StompClient client({deadlineMs, 80, 10000, 10000, 2});
        QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(client.connectToBackend(rest));
        if (mode != 0) {
            QVERIFY(waitForFrames(server, 1));
            if (mode == 2) {
                QTimer::singleShot(20, &server, [&server] {
                    server.sendText(QByteArrayLiteral("CON"));
                });
                QTimer::singleShot(45, &server, [&server] {
                    server.sendText(QByteArrayLiteral("NEC"));
                });
                QTimer::singleShot(70, &server, [&server] {
                    server.sendText(QByteArrayLiteral("TED"));
                });
            }
        }
        QVERIFY(waitForCount(errors, 1));
        QVERIFY(elapsed.elapsed() >= deadlineMs - 30);
        QVERIFY(elapsed.elapsed() < deadlineMs + 50);
        const Xc2Error error = qvariant_cast<Xc2Error>(errors.constFirst().at(1));
        QCOMPARE(error.transportReason, Xc2TransportReason::Timeout);
    }

    void connectIsSingleFlightAndStaleGenerationSignalsAreIgnored()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        server.setUpgradeMode(FakeXc2TransportServer::UpgradeMode::NoResponse);
        Xc2StompClient client({500, 80, 10000, 10000, 2});
        QVERIFY(client.connectToBackend(rest));
        Xc2Error secondError;
        QVERIFY(!client.connectToBackend(rest, &secondError));
        QCOMPARE(secondError.category, Xc2ErrorCategory::Contract);
        QSignalSpy captured(&server,
                            &FakeXc2TransportServer::upgradeRequestCaptured);
        if (server.upgradeRequestCount() == 0)
            QVERIFY(waitForCount(captured, 1));
        QCOMPARE(server.upgradeRequestCount(), 1);
        const Xc2StompGeneration generationA = client.generation();

        QTimer *timerA = nullptr;
        for (QTimer *candidate : client.findChildren<QTimer *>()) {
            if (candidate->isSingleShot() && candidate->isActive()
                && candidate->parent() == &client) {
                timerA = candidate;
                break;
            }
        }
        QVERIFY(timerA);
        QSignalSpy timerTimeouts(timerA, &QTimer::timeout);
        QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
        bool staleTimerDeliveredAfterB = false;
        connect(timerA, &QTimer::timeout, &server,
                [&client, generationA, &staleTimerDeliveredAfterB] {
            staleTimerDeliveredAfterB = client.generation() == generationA + 1;
        }, Qt::QueuedConnection);
        QVERIFY(QMetaObject::invokeMethod(timerA, "timeout",
                                          Qt::DirectConnection));
        QCOMPARE(timerTimeouts.count(), 1);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(client.state(), Xc2StompState::WebSocketConnecting);

        client.abortCurrentGeneration();
        QCOMPARE(errors.count(), 1);

        server.setUpgradeMode(FakeXc2TransportServer::UpgradeMode::WebSocket);
        QVERIFY(establishStompSession(server, rest, client));
        QVERIFY(staleTimerDeliveredAfterB);
        QCOMPARE(client.generation(), generationA + 1);
        QCOMPARE(errors.count(), 1);
        QVERIFY(waitDuration(550));
        QCOMPARE(client.state(), Xc2StompState::Connected);
        QCOMPARE(errors.count(), 1);
        QCOMPARE(server.connectionCount(), 3); // REST plus generations A and B.
    }

    void webSocketConnectingStateReentryCannotOpenStaleSocket()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client({500, 100, 10000, 10000, 2});
        bool reentered = false;
        bool generationBAccepted = false;
        connect(&client, &Xc2StompClient::stateChanged, &client,
                [&](Xc2StompGeneration generation, Xc2StompState state) {
            if (!reentered && generation == 1
                && state == Xc2StompState::WebSocketConnecting) {
                reentered = true;
                client.abortCurrentGeneration();
                generationBAccepted = client.connectToBackend(rest);
            }
        });
        QSignalSpy connected(&client, &Xc2StompClient::connected);
        QVERIFY(client.connectToBackend(rest));
        QVERIFY(reentered);
        QVERIFY(generationBAccepted);
        QVERIFY(waitForFrames(server, 1));
        QVERIFY(waitDuration(40));
        QCOMPARE(server.upgradeRequestCount(), 1);
        QVERIFY(server.sendFrame(connectedFrame()));
        QVERIFY(waitForCount(connected, 1));
        QCOMPARE(qvariant_cast<Xc2StompSession>(
                     connected.constFirst().at(0)).generation,
                 Xc2StompGeneration(2));
    }

    void connectDeadlineStartsBeforeWebSocketConnectingSignal()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client({60, 100, 10000, 10000, 2});
        connect(&client, &Xc2StompClient::stateChanged, &client,
                [](Xc2StompGeneration, Xc2StompState state) {
            if (state != Xc2StompState::WebSocketConnecting)
                return;
            QEventLoop nested;
            QTimer::singleShot(90, &nested, &QEventLoop::quit);
            nested.exec();
        });
        QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
        QVERIFY(client.connectToBackend(rest));
        QCOMPARE(errors.count(), 1);
        QCOMPARE(qvariant_cast<Xc2Error>(errors.constFirst().at(1))
                     .transportReason,
                 Xc2TransportReason::Timeout);
        QCOMPARE(server.upgradeRequestCount(), 0);
    }

    void stompConnectingStateReentryCannotContaminateNextGeneration()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client({500, 100, 10000, 10000, 2});
        bool reentered = false;
        bool generationBAccepted = false;
        connect(&client, &Xc2StompClient::stateChanged, &client,
                [&](Xc2StompGeneration generation, Xc2StompState state) {
            if (!reentered && generation == 1
                && state == Xc2StompState::StompConnecting) {
                reentered = true;
                client.abortCurrentGeneration();
                generationBAccepted = client.connectToBackend(rest);
            }
        });
        QSignalSpy connected(&client, &Xc2StompClient::connected);
        QVERIFY(client.connectToBackend(rest));
        QVERIFY(waitForFrames(server, 1));
        QVERIFY(reentered);
        QVERIFY(generationBAccepted);
        QCOMPARE(server.stompFrames().size(), 1);
        QVERIFY(server.sendFrame(connectedFrame()));
        QVERIFY(waitForCount(connected, 1));
        QCOMPARE(qvariant_cast<Xc2StompSession>(
                     connected.constFirst().at(0)).generation,
                 Xc2StompGeneration(2));
        QCOMPARE(client.state(), Xc2StompState::Connected);
    }

    void negotiatesHeartbeatMatrix_data()
    {
        QTest::addColumn<QByteArray>("serverHeartbeat");
        QTest::addColumn<qint64>("outgoing");
        QTest::addColumn<qint64>("incoming");
        QTest::addColumn<bool>("valid");
        QTest::newRow("zero zero") << QByteArray("0,0") << qint64(0)
                                    << qint64(0) << true;
        QTest::newRow("zero N") << QByteArray("0,25") << qint64(25)
                                 << qint64(0) << true;
        QTest::newRow("N zero") << QByteArray("25,0") << qint64(0)
                                 << qint64(30) << true;
        QTest::newRow("N M") << QByteArray("25,35") << qint64(35)
                              << qint64(30) << true;
        QTest::newRow("missing") << QByteArray() << qint64(0)
                                  << qint64(0) << true;
        QTest::newRow("negative") << QByteArray("-1,0") << qint64(0)
                                   << qint64(0) << false;
        QTest::newRow("non numeric") << QByteArray("x,0") << qint64(0)
                                      << qint64(0) << false;
        QTest::newRow("incomplete") << QByteArray("1") << qint64(0)
                                     << qint64(0) << false;
        QTest::newRow("overflow")
            << QByteArray("9223372036854775808,0") << qint64(0)
            << qint64(0) << false;
        QTest::newRow("grace overflow")
            << QByteArray("9223372036854775807,0") << qint64(0)
            << qint64(0) << false;
    }

    void negotiatesHeartbeatMatrix()
    {
        QFETCH(QByteArray, serverHeartbeat);
        QFETCH(qint64, outgoing);
        QFETCH(qint64, incoming);
        QFETCH(bool, valid);
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client({500, 100, 20, 30, 2});
        QSignalSpy connected(&client, &Xc2StompClient::connected);
        QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
        QVERIFY(client.connectToBackend(rest));
        QVERIFY(waitForFrames(server, 1));
        Xc2StompFrame frame = connectedFrame(serverHeartbeat);
        if (serverHeartbeat.isNull())
            frame.headers.remove("heart-beat");
        QVERIFY(server.sendFrame(frame));
        if (!valid) {
            QVERIFY(waitForCount(errors, 1));
            QCOMPARE(client.state(), Xc2StompState::Failed);
            return;
        }
        QVERIFY(waitForCount(connected, 1));
        const Xc2StompSession session = qvariant_cast<Xc2StompSession>(
            connected.constFirst().at(0));
        QCOMPARE(session.outgoingHeartbeatMs, outgoing);
        QCOMPARE(session.incomingHeartbeatMs, incoming);
    }

    void zeroZeroHeartbeatNeverTimesOut()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client({500, 100, 20, 20, 2});
        QSignalSpy lost(&client, &Xc2StompClient::visibilityLost);
        QSignalSpy writes(&server,
                          &FakeXc2TransportServer::webSocketMessageReceived);
        QVERIFY(establishStompSession(server, rest, client,
                                      QByteArrayLiteral("0,0")));
        const int baseline = server.messages().size();
        QVERIFY(waitDuration(120));
        QCOMPARE(client.state(), Xc2StompState::Connected);
        QCOMPARE(lost.count(), 0);
        QCOMPARE(server.messages().size(), baseline);
        Q_UNUSED(writes);
    }

    void heartbeatDeadlineCoversConnectedStateSignal()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client({500, 100, 0, 20, 2});
        connect(&client, &Xc2StompClient::stateChanged, &client,
                [](Xc2StompGeneration, Xc2StompState state) {
            if (state != Xc2StompState::Connected)
                return;
            QEventLoop nested;
            QTimer::singleShot(70, &nested, &QEventLoop::quit);
            nested.exec();
        });
        QSignalSpy connected(&client, &Xc2StompClient::connected);
        QSignalSpy lost(&client, &Xc2StompClient::visibilityLost);
        QVERIFY(client.connectToBackend(rest));
        QVERIFY(waitForFrames(server, 1));
        QVERIFY(server.sendFrame(connectedFrame(QByteArrayLiteral("20,0"))));
        QVERIFY(waitForCount(lost, 1));
        QCOMPARE(connected.count(), 0);
        QCOMPARE(client.state(), Xc2StompState::Failed);
    }

    void incomingActivityUsesMonotonicDeadline()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client({500, 100, 0, 20, 3});
        QVERIFY(client.subscribe(Topic::Progress));
        QVERIFY(establishStompSession(server, rest, client,
                                      QByteArrayLiteral("20,0")));
        QVERIFY(waitForFrames(server, 2));
        QSignalSpy messages(&client, &Xc2StompClient::messageReceived);
        QSignalSpy lost(&client, &Xc2StompClient::visibilityLost);
        const QByteArray wire = Xc2StompCodec::encode(messageFrame(
            progressDestination(), QByteArrayLiteral("progress-subscription")));
        QTimer::singleShot(20, &server, [&server] {
            server.sendText(QByteArrayLiteral("\n"));
        });
        QTimer::singleShot(50, &server, [&server, wire] {
            server.sendText(wire.left(wire.size() / 2));
        });
        QTimer::singleShot(80, &server, [&server, wire] {
            server.sendText(wire.mid(wire.size() / 2));
        });
        QVERIFY(waitForCount(messages, 1));
        QVERIFY(!lost.wait(30));
        QVERIFY(waitForCount(lost, 1, 150));
    }

    void outgoingHeartbeatIsSentOnlyAfterOutboundSilence()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client({500, 100, 60, 0, 2});
        QVERIFY(establishStompSession(server, rest, client,
                                      QByteArrayLiteral("0,60")));
        QSignalSpy writes(&server,
                          &FakeXc2TransportServer::webSocketMessageReceived);
        QVERIFY(waitDuration(40));
        QVERIFY(client.subscribe(Topic::Progress));
        QVERIFY(waitForFrames(server, 2));
        writes.clear();
        QVERIFY(!writes.wait(35));
        QVERIFY(waitForCount(writes, 1, 70));
        QCOMPARE(server.messages().constLast().payload, QByteArrayLiteral("\n"));
    }

    void websocketAndStompFragmentationDeliverExactlyOnce_data()
    {
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("expectedMessages");
        QTest::newRow("stomp split across messages") << 0 << 1;
        QTest::newRow("stomp frames coalesced") << 1 << 2;
        QTest::newRow("websocket fragmented") << 2 << 1;
        QTest::newRow("binary message") << 3 << 1;
    }

    void websocketAndStompFragmentationDeliverExactlyOnce()
    {
        QFETCH(int, mode);
        QFETCH(int, expectedMessages);
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client;
        QVERIFY(client.subscribe(Topic::Progress));
        QVERIFY(establishStompSession(server, rest, client));
        QVERIFY(waitForFrames(server, 2));
        QSignalSpy messages(&client, &Xc2StompClient::messageReceived);
        const QByteArray first = Xc2StompCodec::encode(messageFrame(
            progressDestination(), QByteArrayLiteral("progress-subscription"),
            QByteArrayLiteral("m-1")));
        if (mode == 0) {
            QVERIFY(server.sendText(first.left(7)));
            QVERIFY(server.sendText(first.mid(7, 11)));
            QVERIFY(server.sendText(first.mid(18)));
        } else if (mode == 1) {
            const QByteArray second = Xc2StompCodec::encode(messageFrame(
                progressDestination(), QByteArrayLiteral("progress-subscription"),
                QByteArrayLiteral("m-2")));
            QVERIFY(server.sendText(first + second));
        } else if (mode == 2) {
            server.setOutgoingFrameSize(3);
            QVERIFY(server.sendText(first));
        } else {
            QVERIFY(server.sendBinary(first));
        }
        QVERIFY(waitForCount(messages, expectedMessages));
        QVERIFY(waitDuration(50));
        QCOMPARE(messages.count(), expectedMessages);
    }

    void codecBufferDoesNotCrossConnectionGeneration()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client;
        QVERIFY(client.subscribe(Topic::Progress));
        QVERIFY(establishStompSession(server, rest, client));
        QVERIFY(waitForFrames(server, 2));
        QVERIFY(server.sendText(QByteArrayLiteral("MESSAGE\ndestination:/topic/pro")));
        server.closeWebSocket(40);
        client.abortCurrentGeneration();

        QVERIFY(establishStompSession(server, rest, client));
        QVERIFY(waitForFrames(server, 4));
        QSignalSpy messages(&client, &Xc2StompClient::messageReceived);
        QVERIFY(server.sendFrame(messageFrame(
            progressDestination(), QByteArrayLiteral("progress-subscription"),
            QByteArrayLiteral("fresh"))));
        QVERIFY(waitForCount(messages, 1));
        const Xc2StompMessage message = qvariant_cast<Xc2StompMessage>(
            messages.constFirst().at(0));
        QCOMPARE(message.messageId, QStringLiteral("fresh"));
        QVERIFY(waitDuration(70));
        QCOMPARE(client.state(), Xc2StompState::Connected);
    }

    void stableSubscriptionsQueueDeduplicateAndKeepOrder()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client;
        QSignalSpy sent(&client, &Xc2StompClient::subscriptionSent);
        QVERIFY(client.subscribe(Topic::Login));
        QVERIFY(client.subscribe(Topic::Ecu));
        QVERIFY(client.subscribe(Topic::VciStatus));
        QVERIFY(client.subscribe(Topic::FlowProgress));
        QVERIFY(client.subscribe(Topic::MeasurementValues));
        QVERIFY(client.subscribe(Topic::VehicleInfo));
        QVERIFY(client.subscribe(Topic::FlowGui));
        QVERIFY(client.subscribe(Topic::Progress));
        QVERIFY(client.subscribe(Topic::Ecu));
        QVERIFY(establishStompSession(server, rest, client));
        const QList<Topic> expected = Xc2ContractProfile::allTopics();
        QVERIFY(waitForFrames(server, expected.size() + 1));
        QCOMPARE(sent.count(), expected.size());
        for (qsizetype index = 0; index < expected.size(); ++index) {
            const Xc2StompFrame &frame = server.stompFrames().at(index + 1);
            QCOMPARE(frame.command, QByteArrayLiteral("SUBSCRIBE"));
            QCOMPARE(QString::fromUtf8(frame.headers.value("id")),
                     stableId(expected.at(index)));
            QCOMPARE(QString::fromUtf8(frame.headers.value("destination")),
                     Xc2ContractProfile::approved().topic(expected.at(index)));
            QCOMPARE(frame.headers.value("ack"), QByteArrayLiteral("auto"));
        }
    }

    void disconnectInConnectedCallbackDoesNotFlushSubscriptions()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client({500, 100, 10000, 10000, 2});
        QVERIFY(client.subscribe(Topic::VciStatus));
        connect(&client, &Xc2StompClient::connected,
                &client, &Xc2StompClient::disconnectFromBackend);
        QVERIFY(client.connectToBackend(rest));
        QVERIFY(waitForFrames(server, 1));
        QVERIFY(server.sendFrame(connectedFrame()));
        QVERIFY(waitForFrames(server, 2));
        QCOMPARE(server.stompFrames().at(1).command,
                 QByteArrayLiteral("DISCONNECT"));
        QVERIFY(waitDuration(30));
        QCOMPARE(server.stompFrames().size(), 2);
    }

    void abortInConnectedStateChangeDoesNotEmitConnected()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client;
        connect(&client, &Xc2StompClient::stateChanged, &client,
                [&client](Xc2StompGeneration, Xc2StompState state) {
            if (state == Xc2StompState::Connected)
                client.abortCurrentGeneration();
        });
        QSignalSpy connected(&client, &Xc2StompClient::connected);
        QSignalSpy disconnected(&client, &Xc2StompClient::disconnected);
        QVERIFY(client.connectToBackend(rest));
        QVERIFY(waitForFrames(server, 1));
        QVERIFY(server.sendFrame(connectedFrame()));
        QVERIFY(waitForCount(disconnected, 1));
        QCOMPARE(connected.count(), 0);
        QCOMPARE(client.state(), Xc2StompState::Failed);
    }

    void deletingClientInsideConnectedSignalStopsSubscriptionFlush()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        auto *client = new Xc2StompClient;
        QVERIFY(client->subscribe(Topic::VciStatus));
        QPointer<Xc2StompClient> guard(client);
        QSignalSpy destroyed(client, &QObject::destroyed);
        connect(client, &Xc2StompClient::connected, this,
                [&client](const Xc2StompSession &) {
            delete client;
            client = nullptr;
        });
        QVERIFY(client->connectToBackend(rest));
        QVERIFY(waitForFrames(server, 1));
        QVERIFY(server.sendFrame(connectedFrame()));
        QVERIFY(waitForCount(destroyed, 1));
        QVERIFY(guard.isNull());
        QCOMPARE(server.stompFrames().size(), 1);
    }

    void deletingClientInsideSubscriptionSignalStopsFlush()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        auto *client = new Xc2StompClient;
        QVERIFY(client->subscribe(Topic::VciStatus));
        QVERIFY(client->subscribe(Topic::Ecu));
        QPointer<Xc2StompClient> guard(client);
        QSignalSpy destroyed(client, &QObject::destroyed);
        int sent = 0;
        connect(client, &Xc2StompClient::subscriptionSent, this,
                [&client, &sent](Topic, const QString &) {
            ++sent;
            delete client;
            client = nullptr;
        });
        QVERIFY(client->connectToBackend(rest));
        QVERIFY(waitForFrames(server, 1));
        QVERIFY(server.sendFrame(connectedFrame()));
        QVERIFY(waitForCount(destroyed, 1));
        QVERIFY(guard.isNull());
        QCOMPARE(sent, 1);
        QVERIFY(waitDuration(30));
        QVERIFY(server.stompFrames().size() <= 2);
        if (server.stompFrames().size() == 2) {
            QCOMPARE(server.stompFrames().constLast().command,
                     QByteArrayLiteral("SUBSCRIBE"));
            QCOMPARE(server.stompFrames().constLast().headers.value("id"),
                     QByteArrayLiteral("vci-status-subscription"));
        }
    }

    void disconnectingStateReentryCannotInstallStaleTimerOnNextGeneration()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client({500, 60, 10000, 10000, 2});
        QVERIFY(establishStompSession(server, rest, client));
        const int baselineFrames = server.stompFrames().size();
        bool reentered = false;
        bool generationBAccepted = false;
        connect(&client, &Xc2StompClient::stateChanged, &client,
                [&](Xc2StompGeneration generation, Xc2StompState state) {
            if (!reentered && generation == 1
                && state == Xc2StompState::Disconnecting) {
                reentered = true;
                client.abortCurrentGeneration();
                generationBAccepted = client.connectToBackend(rest);
            }
        });
        QSignalSpy connected(&client, &Xc2StompClient::connected);
        client.disconnectFromBackend();
        QVERIFY(reentered);
        QVERIFY(generationBAccepted);
        QVERIFY(waitForFrames(server, baselineFrames + 1));
        QCOMPARE(server.stompFrames().constLast().command,
                 QByteArrayLiteral("CONNECT"));
        QVERIFY(server.sendFrame(connectedFrame()));
        QVERIFY(waitForCount(connected, 1));
        QVERIFY(waitDuration(100));
        QCOMPARE(client.findChildren<QTimer *>(
                     QString(), Qt::FindDirectChildrenOnly).size(),
                 0);
        QCOMPARE(client.state(), Xc2StompState::Connected);
    }

    void disconnectDeadlineStartsBeforeDisconnectingSignal()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client({500, 60, 10000, 10000, 2});
        QVERIFY(establishStompSession(server, rest, client));
        connect(&client, &Xc2StompClient::stateChanged, &client,
                [](Xc2StompGeneration, Xc2StompState state) {
            if (state != Xc2StompState::Disconnecting)
                return;
            QEventLoop nested;
            QTimer::singleShot(90, &nested, &QEventLoop::quit);
            nested.exec();
        });
        QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
        client.disconnectFromBackend();
        QCOMPARE(errors.count(), 1);
        QCOMPARE(qvariant_cast<Xc2Error>(errors.constFirst().at(1))
                     .transportReason,
                 Xc2TransportReason::Timeout);
        QCOMPARE(client.state(), Xc2StompState::Failed);
    }

    void unsubscribeUsesTheStableIdExactlyOnce()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client;
        QVERIFY(client.subscribe(Topic::Progress));
        QVERIFY(client.unsubscribe(Topic::Progress));
        QVERIFY(client.subscribe(Topic::Progress));
        QVERIFY(establishStompSession(server, rest, client));
        QVERIFY(waitForFrames(server, 2));
        QSignalSpy unsent(&client, &Xc2StompClient::unsubscriptionSent);
        QVERIFY(client.unsubscribe(Topic::Progress));
        QVERIFY(client.unsubscribe(Topic::Progress));
        QVERIFY(waitForFrames(server, 3));
        QCOMPARE(unsent.count(), 1);
        const Xc2StompFrame &frame = server.stompFrames().at(2);
        QCOMPARE(frame.command, QByteArrayLiteral("UNSUBSCRIBE"));
        QCOMPARE(frame.headers.value("id"),
                 QByteArrayLiteral("progress-subscription"));
        QCOMPARE(frame.headers.size(), 1);
    }

    void rejectsInvalidTopicValueWithoutWriting()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client;
        const Topic invalid = static_cast<Topic>(999);
        Xc2Error error;
        QVERIFY(!client.subscribe(invalid, &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Contract);
        QVERIFY(!client.unsubscribe(invalid, &error));
        QVERIFY(establishStompSession(server, rest, client));
        QCOMPARE(server.stompFrames().size(), 1);
        QVERIFY(!client.subscribe(invalid, &error));
        QVERIFY(waitDuration(50));
        QCOMPARE(server.stompFrames().size(), 1);
    }

    void messageRoutingRequiresConsistentHeaders_data()
    {
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<bool>("valid");
        QTest::addColumn<bool>("unsubscribeFirst");
        const QByteArray destination = progressDestination();
        const QByteArray subscription("progress-subscription");
        QFile golden(QStringLiteral(
            KTM_FIXTURE_DIR "/stomp/progress-message.frame"));
        QVERIFY(golden.open(QIODevice::ReadOnly));
        QTest::newRow("valid golden tuple")
            << golden.readAll()
            << true << false;
        QTest::newRow("missing destination")
            << Xc2StompCodec::encode(messageFrame(
                   QByteArray(), subscription)) << false << false;
        QTest::newRow("missing subscription")
            << Xc2StompCodec::encode(messageFrame(
                   destination, QByteArray())) << false << false;
        QTest::newRow("missing message id")
            << Xc2StompCodec::encode(messageFrame(
                   destination, subscription, QByteArray())) << false << false;
        QTest::newRow("unknown subscription")
            << Xc2StompCodec::encode(messageFrame(
                   destination, QByteArrayLiteral("unknown"))) << false << false;
        QTest::newRow("allowed destination wrong subscription")
            << Xc2StompCodec::encode(messageFrame(
                   destination, QByteArrayLiteral("ecu-subscription")))
            << false << false;
        QTest::newRow("known subscription wrong destination")
            << Xc2StompCodec::encode(messageFrame(
                   QByteArrayLiteral("/topic/ecu"), subscription))
            << false << false;
        QTest::newRow("out of profile destination")
            << Xc2StompCodec::encode(messageFrame(
                   QByteArrayLiteral("/topic/dealernet"), subscription))
            << false << false;
        QTest::newRow("after unsubscribe")
            << Xc2StompCodec::encode(messageFrame(destination, subscription))
            << false << true;
    }

    void messageRoutingRequiresConsistentHeaders()
    {
        QFETCH(QByteArray, payload);
        QFETCH(bool, valid);
        QFETCH(bool, unsubscribeFirst);
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client;
        QVERIFY(client.subscribe(Topic::Progress));
        QVERIFY(establishStompSession(server, rest, client));
        QVERIFY(waitForFrames(server, 2));
        if (unsubscribeFirst) {
            QVERIFY(client.unsubscribe(Topic::Progress));
            QVERIFY(waitForFrames(server, 3));
        }
        QSignalSpy messages(&client, &Xc2StompClient::messageReceived);
        QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
        QVERIFY(server.sendText(payload));
        if (valid) {
            QVERIFY(waitForCount(messages, 1));
            QCOMPARE(errors.count(), 0);
            const Xc2StompMessage message = qvariant_cast<Xc2StompMessage>(
                messages.constFirst().at(0));
            QCOMPARE(message.topic, Topic::Progress);
            QCOMPARE(message.messageId,
                     QStringLiteral("synthetic-progress-1"));
            QCOMPARE(message.body, QByteArrayLiteral(
                R"({"jobId":"00000000-0000-0000-0000-000000000001","status":"IN_PROGRESS","ticks":1,"totalTicks":2,"message":{"id":42,"text":"synthetic progress"}})"));
        } else {
            QVERIFY(waitForCount(errors, 1));
            QCOMPARE(messages.count(), 0);
            const Xc2Error error = qvariant_cast<Xc2Error>(
                errors.constFirst().at(1));
            QCOMPARE(error.category, Xc2ErrorCategory::Contract);
            QCOMPARE(client.state(), Xc2StompState::Connected);
        }
    }

    void deletingClientInsideMessageSignalStopsCoalescedDispatch()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        auto *client = new Xc2StompClient;
        QVERIFY(client->subscribe(Topic::Progress));
        QVERIFY(establishStompSession(server, rest, *client));
        QVERIFY(waitForFrames(server, 2));
        QPointer<Xc2StompClient> guard(client);
        QSignalSpy destroyed(client, &QObject::destroyed);
        int delivered = 0;
        connect(client, &Xc2StompClient::messageReceived, this,
                [&client, &delivered](const Xc2StompMessage &) {
            ++delivered;
            delete client;
            client = nullptr;
        });
        const QByteArray first = Xc2StompCodec::encode(messageFrame(
            progressDestination(), QByteArrayLiteral("progress-subscription"),
            QByteArrayLiteral("delete-1")));
        const QByteArray second = Xc2StompCodec::encode(messageFrame(
            progressDestination(), QByteArrayLiteral("progress-subscription"),
            QByteArrayLiteral("delete-2")));
        QVERIFY(server.sendText(first + second));
        QVERIFY(waitForCount(destroyed, 1));
        QVERIFY(guard.isNull());
        QCOMPARE(delivered, 1);
    }

    void messageRoutingRejectsNonUtf8SubscriptionBeforeConversion()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client;
        QVERIFY(client.subscribe(Topic::Progress));
        QVERIFY(establishStompSession(server, rest, client));
        QVERIFY(waitForFrames(server, 2));
        QByteArray invalidSubscription(1, char(0xff));
        invalidSubscription += QByteArrayLiteral("progress-subscription");
        const QByteArray payload = Xc2StompCodec::encode(messageFrame(
            progressDestination(), invalidSubscription));
        QSignalSpy messages(&client, &Xc2StompClient::messageReceived);
        QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
        QVERIFY(server.sendBinary(payload));
        QVERIFY(waitForCount(errors, 1));
        QCOMPARE(messages.count(), 0);
        QCOMPARE(client.state(), Xc2StompState::Connected);
    }

    void matchingDisconnectReceiptClosesGracefully()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client({500, 150, 10000, 10000, 2});
        QVERIFY(establishStompSession(server, rest, client));
        QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
        QSignalSpy disconnected(&client, &Xc2StompClient::disconnected);
        client.disconnectFromBackend();
        client.disconnectFromBackend();
        QVERIFY(waitForFrames(server, 2));
        const Xc2StompFrame &frame = server.stompFrames().constLast();
        QCOMPARE(frame.command, QByteArrayLiteral("DISCONNECT"));
        const QByteArray receipt = frame.headers.value("receipt");
        QCOMPARE(receipt, QByteArrayLiteral("disconnect-")
                     + QByteArray::number(client.generation()));
        QVERIFY(server.sendFrame(receiptFrame(receipt)));
        QVERIFY(waitForCount(disconnected, 1));
        QCOMPARE(server.stompFrames().size(), 2);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(client.state(), Xc2StompState::Disconnected);
    }

    void wrongOrMissingDisconnectReceiptUsesBoundedAbort()
    {
        for (const bool sendWrong : {true, false}) {
            FakeXc2TransportServer server;
            Xc2RestClient rest;
            QVERIFY(establishRestSession(server, rest));
            Xc2StompClient client({500, 70, 10000, 10000, 2});
            QVERIFY(establishStompSession(server, rest, client));
            QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
            QSignalSpy disconnected(&client, &Xc2StompClient::disconnected);
            client.disconnectFromBackend();
            QVERIFY(waitForFrames(server, 2));
            if (sendWrong) {
                QVERIFY(server.sendFrame(receiptFrame(
                    QByteArrayLiteral("stale-receipt"))));
            }
            QVERIFY(waitForCount(disconnected, 1));
            QVERIFY(errors.count() >= 1);
            const Xc2Error terminal = qvariant_cast<Xc2Error>(
                errors.constLast().at(1));
            QCOMPARE(terminal.transportReason, Xc2TransportReason::Timeout);
        }
    }

    void disconnectReceiptCloseAndTimeoutRaceCompletesOnce()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client({500, 80, 10000, 10000, 2});
        QVERIFY(establishStompSession(server, rest, client));
        QSignalSpy disconnected(&client, &Xc2StompClient::disconnected);
        QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
        QSignalSpy lost(&client, &Xc2StompClient::visibilityLost);
        QElapsedTimer elapsed;
        QTimer collision;
        collision.setSingleShot(true);
        collision.setTimerType(Qt::PreciseTimer);
        collision.setInterval(80);
        QSignalSpy collisionFired(&collision, &QTimer::timeout);
        const QByteArray receipt = QByteArrayLiteral("disconnect-")
            + QByteArray::number(client.generation());
        bool collisionWriteQueued = false;
        qint64 collisionAtMs = -1;
        connect(&collision, &QTimer::timeout, &server,
                [&server, receipt, &elapsed, &collisionWriteQueued,
                 &collisionAtMs] {
            collisionAtMs = elapsed.elapsed();
            collisionWriteQueued = server.sendFrameAndClose(
                receiptFrame(receipt));
        });
        elapsed.start();
        collision.start();
        client.disconnectFromBackend();
        QVERIFY(waitForFrames(server, 2));
        QVERIFY(waitForCount(disconnected, 1));
        QVERIFY(waitDuration(100));
        QCOMPARE(collisionFired.count(), 1);
        QVERIFY(collisionWriteQueued);
        QVERIFY(collisionAtMs >= 50);
        QVERIFY(collisionAtMs < 150);
        QCOMPARE(disconnected.count(), 1);
        QVERIFY(errors.count() <= 1);
        if (errors.count() == 1) {
            QCOMPARE(qvariant_cast<Xc2Error>(errors.constFirst().at(1))
                         .transportReason,
                     Xc2TransportReason::Timeout);
        }
        QCOMPARE(lost.count(), 0);
        QVERIFY(client.state() == Xc2StompState::Disconnected
                || client.state() == Xc2StompState::Failed);
    }

    void abortCurrentGenerationIsImmediateAndIdempotent()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client;
        QVERIFY(establishStompSession(server, rest, client));
        QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
        QSignalSpy disconnected(&client, &Xc2StompClient::disconnected);
        QSignalSpy lost(&client, &Xc2StompClient::visibilityLost);
        client.abortCurrentGeneration();
        client.abortCurrentGeneration();
        QVERIFY(waitForCount(disconnected, 1));
        QCOMPARE(errors.count(), 1);
        QCOMPARE(lost.count(), 0);
        const Xc2Error error = qvariant_cast<Xc2Error>(errors.constFirst().at(1));
        QCOMPARE(error.transportReason, Xc2TransportReason::Canceled);
    }

    void unexpectedEstablishedLossEmitsVisibilityOnceAndNeverReconnects()
    {
        FakeXc2TransportServer server;
        Xc2RestClient rest;
        QVERIFY(establishRestSession(server, rest));
        Xc2StompClient client;
        QVERIFY(establishStompSession(server, rest, client));
        QSignalSpy lost(&client, &Xc2StompClient::visibilityLost);
        QSignalSpy disconnected(&client, &Xc2StompClient::disconnected);
        const int connections = server.connectionCount();
        server.closeWebSocket();
        QVERIFY(waitForCount(lost, 1));
        QVERIFY(waitForCount(disconnected, 1));
        QVERIFY(waitDuration(120));
        QCOMPARE(lost.count(), 1);
        QCOMPARE(disconnected.count(), 1);
        QCOMPARE(server.connectionCount(), connections);
    }

    void connectFailureAndIntentionalDisconnectDoNotLoseVisibility()
    {
        {
            FakeXc2TransportServer server;
            Xc2RestClient rest;
            QVERIFY(establishRestSession(server, rest));
            server.setUpgradeMode(FakeXc2TransportServer::UpgradeMode::NoResponse);
            Xc2StompClient client({70, 50, 10000, 10000, 2});
            QSignalSpy lost(&client, &Xc2StompClient::visibilityLost);
            QSignalSpy errors(&client, &Xc2StompClient::errorOccurred);
            QVERIFY(client.connectToBackend(rest));
            QVERIFY(waitForCount(errors, 1));
            QCOMPARE(lost.count(), 0);
        }
        {
            FakeXc2TransportServer server;
            Xc2RestClient rest;
            QVERIFY(establishRestSession(server, rest));
            Xc2StompClient client({500, 100, 10000, 10000, 2});
            QVERIFY(establishStompSession(server, rest, client));
            QSignalSpy lost(&client, &Xc2StompClient::visibilityLost);
            QSignalSpy disconnected(&client, &Xc2StompClient::disconnected);
            client.disconnectFromBackend();
            QVERIFY(waitForFrames(server, 2));
            const QByteArray receipt = server.stompFrames().constLast()
                                           .headers.value("receipt");
            QVERIFY(server.sendFrame(receiptFrame(receipt)));
            QVERIFY(waitForCount(disconnected, 1));
            QCOMPARE(lost.count(), 0);
        }
    }
};

QTEST_GUILESS_MAIN(TestXc2StompClient)
#include "test_Xc2StompClient.moc"
