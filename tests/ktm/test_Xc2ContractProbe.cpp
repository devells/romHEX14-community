#include "FakeXc2TransportServer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#ifndef XC2_CONTRACT_PROBE_PATH
#error XC2_CONTRACT_PROBE_PATH must name the real probe target
#endif

#ifndef XC2_CONTRACT_WRAPPER_PATH
#error XC2_CONTRACT_WRAPPER_PATH must name the strict wrapper script
#endif

namespace {

constexpr int kDefaultProbeTimeoutMs = 500;
constexpr int kProcessWallBoundMs = 8000;
constexpr int kCleanupBoundMs = 1500;
const QByteArray kCookie =
    QByteArrayLiteral("XC2_COOKIE_SENTINEL_43e9=opaque");

struct ProcessResult {
    bool started = false;
    bool watchdogExpired = false;
    int finishedSignals = 0;
    int exitCode = -999;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    qint64 elapsedMs = -1;
    qint64 runningElapsedMs = -1;
    QByteArray standardOutput;
    QByteArray standardError;
    QString processError;
};

ProcessResult runProcess(const QString &program,
                         const QStringList &arguments,
                         int wallBoundMs = kProcessWallBoundMs)
{
    ProcessResult result;
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);

    QEventLoop loop;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    watchdog.setTimerType(Qt::PreciseTimer);
    QElapsedTimer elapsed;
    QElapsedTimer runningElapsed;

    QObject::connect(&process, &QProcess::started, &loop,
                     [&result, &runningElapsed] {
        result.started = true;
        runningElapsed.start();
    });
    QObject::connect(
        &process,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        &loop,
        [&result, &runningElapsed, &loop](
            int exitCode, QProcess::ExitStatus exitStatus) {
            ++result.finishedSignals;
            result.exitCode = exitCode;
            result.exitStatus = exitStatus;
            if (runningElapsed.isValid())
                result.runningElapsedMs = runningElapsed.elapsed();
            loop.quit();
        });
    QObject::connect(&process, &QProcess::errorOccurred, &loop,
                     [&result, &process, &loop](QProcess::ProcessError error) {
        result.processError = process.errorString();
        if (error == QProcess::FailedToStart)
            loop.quit();
    });
    QObject::connect(&watchdog, &QTimer::timeout, &loop,
                     [&result, &process] {
        result.watchdogExpired = true;
        if (process.state() != QProcess::NotRunning)
            process.kill();
    });

    elapsed.start();
    watchdog.start(wallBoundMs);
    process.start();
    loop.exec();
    watchdog.stop();
    result.elapsedMs = elapsed.elapsed();
    result.standardOutput = process.readAllStandardOutput();
    result.standardError = process.readAllStandardError();
    return result;
}

bool waitForNoOpenConnections(FakeXc2TransportServer &server)
{
    if (server.openConnectionCount() == 0)
        return true;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.setTimerType(Qt::PreciseTimer);
    QObject::connect(&server, &FakeXc2TransportServer::connectionClosed,
                     &loop, [&server, &loop] {
        if (server.openConnectionCount() == 0)
            loop.quit();
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(kCleanupBoundMs);
    loop.exec();
    return server.openConnectionCount() == 0;
}

QStringList probeArguments(const FakeXc2TransportServer &server,
                           int timeoutMs = kDefaultProbeTimeoutMs)
{
    return {
        QStringLiteral("--base-url"),
        QString::fromLatin1(server.encodedRestBase()),
        QStringLiteral("--timeout-ms"),
        QString::number(timeoutMs),
    };
}

QStringList resolveBasePlaceholder(QStringList arguments,
                                   const FakeXc2TransportServer &server)
{
    const QString base = QString::fromLatin1(server.encodedRestBase());
    for (QString &argument : arguments) {
        argument.replace(QStringLiteral("{BASE}"), base);
    }
    return arguments;
}

ProcessResult runProbe(FakeXc2TransportServer &server,
                       int timeoutMs = kDefaultProbeTimeoutMs)
{
    return runProcess(QString::fromUtf8(XC2_CONTRACT_PROBE_PATH),
                      probeArguments(server, timeoutMs));
}

void verifyNoSensitiveOutput(const ProcessResult &result)
{
    const QByteArray output = result.standardOutput + result.standardError;
    const QList<QByteArray> sentinels{
        QByteArrayLiteral("XC2_COOKIE_SENTINEL_43e9"),
        QByteArrayLiteral("LOGIN_SENTINEL_15c8"),
        QByteArrayLiteral("NAME_SENTINEL_b97e"),
        QByteArrayLiteral("DEALER_SENTINEL_51aa"),
        QByteArrayLiteral("ADDRESS_SENTINEL_70d4"),
        QByteArrayLiteral("SESSION_SENTINEL_5ed1"),
        QByteArrayLiteral("BACKEND_MESSAGE_SENTINEL_6f25"),
        QByteArrayLiteral("DEVELOPER_SENTINEL_844a"),
        QByteArrayLiteral("INFO_SENTINEL_502d"),
        QByteArrayLiteral("STOMP_MESSAGE_SENTINEL_95be"),
        QByteArrayLiteral("STOMP_BODY_SENTINEL_d71a"),
        QByteArrayLiteral("EcuDiagnosticRead"),
    };
    for (const QByteArray &sentinel : sentinels) {
        QVERIFY2(!output.contains(sentinel), sentinel.constData());
    }
    QVERIFY(result.standardOutput.size() <= 1024);
    QVERIFY(result.standardError.size() <= 1024);
}

void verifyExactClientFrame(const ktm::xc2::Xc2StompFrame &frame,
                            qsizetype index)
{
    QVERIFY(frame.body.isEmpty());
    if (index == 0) {
        QCOMPARE(frame.command, QByteArrayLiteral("CONNECT"));
        QCOMPARE(frame.headers.size(), 3);
        QCOMPARE(frame.headers.value(QByteArrayLiteral("accept-version")),
                 QByteArrayLiteral("1.2"));
        QCOMPARE(frame.headers.value(QByteArrayLiteral("host")),
                 QByteArrayLiteral("127.0.0.1"));
        QCOMPARE(frame.headers.value(QByteArrayLiteral("heart-beat")),
                 QByteArrayLiteral("10000,10000"));
    } else if (index == 1 || index == 2) {
        const bool vci = index == 1;
        QCOMPARE(frame.command, QByteArrayLiteral("SUBSCRIBE"));
        QCOMPARE(frame.headers.size(), 3);
        QCOMPARE(frame.headers.value(QByteArrayLiteral("ack")),
                 QByteArrayLiteral("auto"));
        QCOMPARE(frame.headers.value(QByteArrayLiteral("destination")),
                 vci ? QByteArrayLiteral("/topic/vci/status")
                     : QByteArrayLiteral("/topic/login"));
        QCOMPARE(frame.headers.value(QByteArrayLiteral("id")),
                 vci ? QByteArrayLiteral("vci-status-subscription")
                     : QByteArrayLiteral("login-subscription"));
    } else {
        QCOMPARE(index, qsizetype(3));
        QCOMPARE(frame.command, QByteArrayLiteral("DISCONNECT"));
        QCOMPARE(frame.headers.size(), 1);
        QCOMPARE(frame.headers.value(QByteArrayLiteral("receipt")),
                 QByteArrayLiteral("disconnect-1"));
    }
}

void verifyCommonResult(FakeXc2TransportServer &server,
                        const ProcessResult &result,
                        int expectedExitCode)
{
    QVERIFY2(result.started, qPrintable(result.processError));
    QVERIFY2(!result.watchdogExpired, "probe process exceeded wall bound");
    QCOMPARE(result.finishedSignals, 1);
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, expectedExitCode);
    QVERIFY(result.elapsedMs >= 0);
    QVERIFY(result.runningElapsedMs >= 0);
    QVERIFY(result.elapsedMs < kProcessWallBoundMs);
    QVERIFY(waitForNoOpenConnections(server));
    QCOMPARE(server.openConnectionCount(), 0);
    QCOMPARE(server.stateChangingRestRequestCount(), 0);
    QCOMPARE(server.unexpectedOperationCount(), 0);
    QCOMPARE(server.decodeErrors().size(), 0);

    const QList<FakeXc2HttpRequest> &requests =
        server.capturedRestRequests();
    QVERIFY(requests.size() <= 2);
    for (qsizetype index = 0; index < requests.size(); ++index) {
        const FakeXc2HttpRequest &request = requests.at(index);
        QCOMPARE(request.method, QByteArrayLiteral("GET"));
        QCOMPARE(request.target,
                 index == 0
                     ? QByteArrayLiteral("/xc2/1.0/serviceStatus/status")
                     : QByteArrayLiteral("/xc2/1.0/auth/currentUser"));
        QVERIFY(request.body.isEmpty());
    }

    QVERIFY(server.upgradeRequests().size() <= 1);
    if (!server.upgradeRequests().isEmpty()) {
        QCOMPARE(requests.size(), 2);
        const FakeXc2HttpRequest &upgrade =
            server.upgradeRequests().constFirst();
        QCOMPARE(upgrade.method, QByteArrayLiteral("GET"));
        QCOMPARE(upgrade.target, QByteArrayLiteral("/xc2-websocket"));
        QVERIFY(upgrade.body.isEmpty());
        QCOMPARE(upgrade.headerValues(QByteArrayLiteral("Cookie")).size(), 1);
        QCOMPARE(upgrade.headerValue(QByteArrayLiteral("Cookie")), kCookie);
        QCOMPARE(upgrade.headerValues(
                     QByteArrayLiteral("Sec-WebSocket-Protocol")).size(),
                 1);
        QCOMPARE(upgrade.headerValue(
                     QByteArrayLiteral("Sec-WebSocket-Protocol")),
                 QByteArrayLiteral("v12.stomp"));
        QCOMPARE(upgrade.headerValue(QByteArrayLiteral("Upgrade")),
                 QByteArrayLiteral("websocket"));
        QCOMPARE(upgrade.headerValue(QByteArrayLiteral("Connection")),
                 QByteArrayLiteral("Upgrade"));
        QCOMPARE(upgrade.headerValue(QByteArrayLiteral("Host")),
                 QByteArrayLiteral("127.0.0.1:")
                     + QByteArray::number(server.port()));
    }

    QVERIFY(server.webSocketConnectionCount() <= 1);
    if (server.webSocketConnectionCount() == 1) {
        QCOMPARE(server.negotiatedSubprotocol(), QStringLiteral("v12.stomp"));
    } else {
        QVERIFY(server.negotiatedSubprotocol().isEmpty());
    }

    QVERIFY(server.stompFrames().size() <= 4);
    for (qsizetype index = 0; index < server.stompFrames().size(); ++index)
        verifyExactClientFrame(server.stompFrames().at(index), index);
    QCOMPARE(server.messages().size(), server.stompFrames().size());
    for (const FakeXc2WebSocketMessage &message : server.messages())
        QCOMPARE(message.kind, FakeXc2WebSocketMessageKind::Text);
    verifyNoSensitiveOutput(result);
}

void verifyExactSuccessCapture(FakeXc2TransportServer &server,
                               const ProcessResult &result)
{
    verifyCommonResult(server, result, 0);
    QCOMPARE(server.capturedRestRequests().size(), 2);
    QCOMPARE(server.upgradeRequests().size(), 1);
    QCOMPARE(server.webSocketConnectionCount(), 1);
    QCOMPARE(server.negotiatedSubprotocol(), QStringLiteral("v12.stomp"));
    QCOMPARE(server.stompFrames().size(), 4);
    QCOMPARE(server.sentStompFrames().size(), 2);

    const ktm::xc2::Xc2StompFrame &connected =
        server.sentStompFrames().at(0);
    QCOMPARE(connected.command, QByteArrayLiteral("CONNECTED"));
    QCOMPARE(connected.headers.size(), 2);
    QCOMPARE(connected.headers.value(QByteArrayLiteral("version")),
             QByteArrayLiteral("1.2"));
    QCOMPARE(connected.headers.value(QByteArrayLiteral("heart-beat")),
             QByteArrayLiteral("0,0"));
    QVERIFY(connected.body.isEmpty());

    const ktm::xc2::Xc2StompFrame &receipt =
        server.sentStompFrames().at(1);
    QCOMPARE(receipt.command, QByteArrayLiteral("RECEIPT"));
    QCOMPARE(receipt.headers.size(), 1);
    QCOMPARE(receipt.headers.value(QByteArrayLiteral("receipt-id")),
             QByteArrayLiteral("disconnect-1"));
    QVERIFY(receipt.body.isEmpty());
    QCOMPARE(server.lastPeerCloseCode(),
             QWebSocketProtocol::CloseCodeNormal);
}

} // namespace

class Xc2ContractProbeTest final : public QObject {
    Q_OBJECT

private slots:
    void happyPathUsesExactReadOnlyAllowlist()
    {
        FakeXc2TransportServer server;
        QVERIFY(server.isListening());
        server.setProbeScript(FakeXc2TransportServer::ProbeScript::Happy);
        server.setRestCookie(kCookie);
        const ProcessResult result = runProbe(server);
        verifyExactSuccessCapture(server, result);
    }

    void happyPathUsesDerivedCookieAndV12Stomp()
    {
        FakeXc2TransportServer server;
        QVERIFY(server.isListening());
        server.setProbeScript(FakeXc2TransportServer::ProbeScript::Happy);
        server.setRestCookie(kCookie);
        const ProcessResult result = runProbe(server);
        verifyExactSuccessCapture(server, result);
        QCOMPARE(server.upgradeRequests().constFirst().headerValue(
                     QByteArrayLiteral("Host")),
                 QByteArrayLiteral("127.0.0.1:")
                     + QByteArray::number(server.port()));
        QCOMPARE(server.requestUrl().path(), QStringLiteral("/xc2-websocket"));
    }

    void helpExitsZeroBeforeNetwork()
    {
        FakeXc2TransportServer server;
        server.setProbeScript(FakeXc2TransportServer::ProbeScript::Happy);
        server.setRestCookie(kCookie);
        const ProcessResult result = runProcess(
            QString::fromUtf8(XC2_CONTRACT_PROBE_PATH),
            {QStringLiteral("--help")});
        verifyCommonResult(server, result, 0);
        QCOMPARE(server.connectionCount(), 0);
        QVERIFY(result.standardOutput.contains("--base-url"));
    }

    void invalidCliOrBaseExitsTwoBeforeNetwork_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::newRow("missing-base") << QStringList{};
        QTest::newRow("duplicate-base")
            << QStringList{QStringLiteral("--base-url"),
                           QStringLiteral("{BASE}"),
                           QStringLiteral("--base-url"),
                           QStringLiteral("{BASE}")};
        QTest::newRow("unknown-option")
            << QStringList{QStringLiteral("--base-url"),
                           QStringLiteral("{BASE}"),
                           QStringLiteral("--unknown")};
        QTest::newRow("help-all-is-not-a-probe-result")
            << QStringList{QStringLiteral("--help-all")};
        QTest::newRow("help-with-base-is-invalid")
            << QStringList{QStringLiteral("--help"),
                           QStringLiteral("--base-url"),
                           QStringLiteral("{BASE}")};
        QTest::newRow("positional")
            << QStringList{QStringLiteral("--base-url"),
                           QStringLiteral("{BASE}"),
                           QStringLiteral("positional")};
        QTest::newRow("empty-base")
            << QStringList{QStringLiteral("--base-url"), QString()};
        QTest::newRow("duplicate-timeout")
            << QStringList{QStringLiteral("--base-url"),
                           QStringLiteral("{BASE}"),
                           QStringLiteral("--timeout-ms"),
                           QStringLiteral("500"),
                           QStringLiteral("--timeout-ms"),
                           QStringLiteral("500")};
        QTest::newRow("timeout-too-small")
            << QStringList{QStringLiteral("--base-url"),
                           QStringLiteral("{BASE}"),
                           QStringLiteral("--timeout-ms"),
                           QStringLiteral("99")};
        QTest::newRow("timeout-too-large")
            << QStringList{QStringLiteral("--base-url"),
                           QStringLiteral("{BASE}"),
                           QStringLiteral("--timeout-ms"),
                           QStringLiteral("120001")};
        QTest::newRow("timeout-overflow")
            << QStringList{QStringLiteral("--base-url"),
                           QStringLiteral("{BASE}"),
                           QStringLiteral("--timeout-ms"),
                           QStringLiteral("999999999999999999999999")};
        QTest::newRow("timeout-malformed")
            << QStringList{QStringLiteral("--base-url"),
                           QStringLiteral("{BASE}"),
                           QStringLiteral("--timeout-ms"),
                           QStringLiteral("500ms")};
        QTest::newRow("equals-base-syntax")
            << QStringList{QStringLiteral("--base-url={BASE}")};
        QTest::newRow("equals-timeout-syntax")
            << QStringList{QStringLiteral("--base-url"),
                           QStringLiteral("{BASE}"),
                           QStringLiteral("--timeout-ms=500")};
        QTest::newRow("non-ascii-base")
            << QStringList{QStringLiteral("--base-url"),
                           QStringLiteral("{BASE}\u00e9")};
        QTest::newRow("space-in-base")
            << QStringList{QStringLiteral("--base-url"),
                           QStringLiteral("{BASE} ")};
        QTest::newRow("non-loopback-base")
            << QStringList{QStringLiteral("--base-url"),
                           QStringLiteral("http://192.0.2.1:4321/xc2/1.0")};
        QTest::newRow("normalized-path-trick")
            << QStringList{QStringLiteral("--base-url"),
                           QStringLiteral("{BASE}/../1.0")};
    }

    void invalidCliOrBaseExitsTwoBeforeNetwork()
    {
        QFETCH(QStringList, arguments);
        FakeXc2TransportServer server;
        server.setProbeScript(FakeXc2TransportServer::ProbeScript::Happy);
        server.setRestCookie(kCookie);
        arguments = resolveBasePlaceholder(arguments, server);
        const ProcessResult result = runProcess(
            QString::fromUtf8(XC2_CONTRACT_PROBE_PATH), arguments);
        verifyCommonResult(server, result, 2);
        QCOMPARE(server.connectionCount(), 0);
    }

    void restTransportAndOverallDeadlineExitTwo_data()
    {
        QTest::addColumn<FakeXc2TransportServer::ProbeScript>("script");
        QTest::addColumn<int>("timeoutMs");
        using Script = FakeXc2TransportServer::ProbeScript;
        QTest::newRow("health-no-response-outer-deadline")
            << Script::HealthNoResponse << 120;
        QTest::newRow("health-truncated")
            << Script::HealthTruncated << 500;
        QTest::newRow("health-backend-unavailable")
            << Script::HealthBackendUnavailable << 500;
        QTest::newRow("health-401-is-not-current-user-auth")
            << Script::HealthAuth401 << 500;
        QTest::newRow("current-user-no-response-outer-deadline")
            << Script::CurrentUserNoResponse << 120;
        QTest::newRow("current-user-truncated")
            << Script::CurrentUserTruncated << 500;
        QTest::newRow("current-user-backend-unavailable")
            << Script::CurrentUserBackendUnavailable << 500;
    }

    void restTransportAndOverallDeadlineExitTwo()
    {
        QFETCH(FakeXc2TransportServer::ProbeScript, script);
        QFETCH(int, timeoutMs);
        FakeXc2TransportServer server;
        server.setProbeScript(script);
        server.setRestCookie(kCookie);
        const ProcessResult result = runProbe(server, timeoutMs);
        verifyCommonResult(server, result, 2);
        if (script == FakeXc2TransportServer::ProbeScript::HealthNoResponse
            || script
                == FakeXc2TransportServer::ProbeScript::CurrentUserNoResponse) {
            QVERIFY2(result.runningElapsedMs < timeoutMs + 500,
                     "outer REST deadline lost to the +1000ms backstop");
        }
    }

    void malformedRestContractsExitThree_data()
    {
        QTest::addColumn<FakeXc2TransportServer::ProbeScript>("script");
        using Script = FakeXc2TransportServer::ProbeScript;
        QTest::newRow("health-malformed-success")
            << Script::HealthMalformedSuccess;
        QTest::newRow("health-204") << Script::Health204;
        QTest::newRow("health-malformed-error")
            << Script::HealthMalformedError;
        QTest::newRow("current-user-malformed-success")
            << Script::CurrentUserMalformedSuccess;
        QTest::newRow("current-user-malformed-auth-error")
            << Script::CurrentUserMalformedAuthError;
        QTest::newRow("current-user-blank-login")
            << Script::CurrentUserBlankLogin;
        QTest::newRow("current-user-blank-name")
            << Script::CurrentUserBlankName;
    }

    void malformedRestContractsExitThree()
    {
        QFETCH(FakeXc2TransportServer::ProbeScript, script);
        FakeXc2TransportServer server;
        server.setProbeScript(script);
        server.setRestCookie(kCookie);
        const ProcessResult result = runProbe(server);
        verifyCommonResult(server, result, 3);
    }

    void stompFailuresExitFour_data()
    {
        QTest::addColumn<FakeXc2TransportServer::ProbeScript>("script");
        QTest::addColumn<int>("timeoutMs");
        using Script = FakeXc2TransportServer::ProbeScript;
        QTest::newRow("upgrade-no-response")
            << Script::UpgradeNoResponse << 120;
        QTest::newRow("upgrade-rejected")
            << Script::UpgradeRejected << 500;
        QTest::newRow("wrong-subprotocol")
            << Script::UpgradeWrongSubprotocol << 500;
        QTest::newRow("missing-connected")
            << Script::MissingConnected << 120;
        QTest::newRow("stomp-error") << Script::StompError << 500;
        QTest::newRow("malformed-connected")
            << Script::MalformedConnected << 500;
        QTest::newRow("wrong-disconnect-receipt")
            << Script::WrongDisconnectReceipt << 500;
        QTest::newRow("missing-disconnect-receipt")
            << Script::MissingDisconnectReceipt << 150;
        QTest::newRow("close-before-disconnect-receipt")
            << Script::CloseBeforeDisconnectReceipt << 500;
        QTest::newRow("receipt-then-abnormal-close")
            << Script::ReceiptThenAbnormalClose << 500;
    }

    void stompFailuresExitFour()
    {
        QFETCH(FakeXc2TransportServer::ProbeScript, script);
        QFETCH(int, timeoutMs);
        FakeXc2TransportServer server;
        server.setProbeScript(script);
        server.setRestCookie(kCookie);
        const ProcessResult result = runProbe(server, timeoutMs);
        verifyCommonResult(server, result, 4);
        if (script == FakeXc2TransportServer::ProbeScript::UpgradeNoResponse
            || script
                == FakeXc2TransportServer::ProbeScript::MissingConnected
            || script
                == FakeXc2TransportServer::ProbeScript::MissingDisconnectReceipt) {
            QVERIFY2(result.runningElapsedMs < timeoutMs + 500,
                     "outer STOMP deadline lost to the +1000ms backstop");
        }
    }

    void unauthenticated204AndMissingPermissionExitFive_data()
    {
        QTest::addColumn<FakeXc2TransportServer::ProbeScript>("script");
        using Script = FakeXc2TransportServer::ProbeScript;
        QTest::newRow("current-user-204") << Script::CurrentUser204;
        QTest::newRow("current-user-401") << Script::CurrentUserAuth401;
        QTest::newRow("current-user-403") << Script::CurrentUserAuth403;
        QTest::newRow("missing-permission")
            << Script::CurrentUserMissingPermission;
        QTest::newRow("case-mismatched-permission")
            << Script::CurrentUserCaseMismatchedPermission;
    }

    void unauthenticated204AndMissingPermissionExitFive()
    {
        QFETCH(FakeXc2TransportServer::ProbeScript, script);
        FakeXc2TransportServer server;
        server.setProbeScript(script);
        server.setRestCookie(kCookie);
        const ProcessResult result = runProbe(server);
        verifyCommonResult(server, result, 5);
    }

    void finishAndDeadlineRaceExitsOnceAndCleansConnections()
    {
        {
            FakeXc2TransportServer server;
            server.setProbeScript(
                FakeXc2TransportServer::ProbeScript::Happy);
            server.setRestCookie(kCookie);
            const ProcessResult result = runProbe(server, 500);
            verifyExactSuccessCapture(server, result);
            QCOMPARE(result.finishedSignals, 1);
            QCOMPARE(server.openConnectionCount(), 0);
        }
        {
            FakeXc2TransportServer server;
            server.setProbeScript(
                FakeXc2TransportServer::ProbeScript::LateDisconnectReceipt);
            server.setProbeDelayMs(1000);
            server.setRestCookie(kCookie);
            const ProcessResult result = runProbe(server, 500);
            verifyCommonResult(server, result, 4);
            QVERIFY2(result.runningElapsedMs < 1000,
                     "outer disconnect deadline lost to its backstop");
            QCOMPARE(result.finishedSignals, 1);
            QCOMPARE(server.stompFrames().size(), 4);
            QCOMPARE(server.sentStompFrames().size(), 1);
            QCOMPARE(server.openConnectionCount(), 0);
        }
    }

    void outputNeverContainsCookieOrUserSentinels()
    {
        {
            FakeXc2TransportServer server;
            server.setProbeScript(FakeXc2TransportServer::ProbeScript::Happy);
            server.setRestCookie(kCookie);
            const ProcessResult result = runProbe(server);
            verifyExactSuccessCapture(server, result);
            verifyNoSensitiveOutput(result);
        }
        {
            FakeXc2TransportServer server;
            server.setProbeScript(
                FakeXc2TransportServer::ProbeScript::CurrentUserAuth401);
            server.setRestCookie(kCookie);
            const ProcessResult result = runProbe(server);
            verifyCommonResult(server, result, 5);
            verifyNoSensitiveOutput(result);
        }
        {
            FakeXc2TransportServer server;
            server.setProbeScript(
                FakeXc2TransportServer::ProbeScript::StompError);
            server.setRestCookie(kCookie);
            const ProcessResult result = runProbe(server);
            verifyCommonResult(server, result, 4);
            verifyNoSensitiveOutput(result);
        }
    }

    void wrapperPreservesEveryProbeExitCode_data()
    {
        QTest::addColumn<FakeXc2TransportServer::ProbeScript>("script");
        QTest::addColumn<int>("expectedExitCode");
        QTest::addColumn<bool>("emptyBase");
        QTest::addColumn<int>("probePathMode");
        using Script = FakeXc2TransportServer::ProbeScript;
        QTest::newRow("exit-0")
            << Script::Happy << 0 << false << 0;
        QTest::newRow("exit-2-empty-raw-base")
            << Script::Happy << 2 << true << 0;
        QTest::newRow("exit-3")
            << Script::HealthMalformedSuccess << 3 << false << 0;
        QTest::newRow("exit-4")
            << Script::UpgradeRejected << 4 << false << 0;
        QTest::newRow("exit-5")
            << Script::CurrentUser204 << 5 << false << 0;
        QTest::newRow("missing-probe-path")
            << Script::Happy << 2 << false << 1;
        QTest::newRow("existing-non-executable-leaf")
            << Script::Happy << 2 << false << 2;
    }

    void wrapperPreservesEveryProbeExitCode()
    {
        QFETCH(FakeXc2TransportServer::ProbeScript, script);
        QFETCH(int, expectedExitCode);
        QFETCH(bool, emptyBase);
        QFETCH(int, probePathMode);

        FakeXc2TransportServer server;
        server.setProbeScript(script);
        server.setRestCookie(kCookie);

        QTemporaryDir directory(
            QDir::temp().filePath(QStringLiteral("xc2 probe path-XXXXXX")));
        QVERIFY(directory.isValid());
        QVERIFY(directory.path().contains(' '));
        const QString copiedProbe =
            directory.filePath(QStringLiteral("xc2 contract probe.exe"));
        if (probePathMode == 0) {
            QVERIFY2(QFile::copy(QString::fromUtf8(XC2_CONTRACT_PROBE_PATH),
                                copiedProbe),
                     qPrintable(copiedProbe));
        } else if (probePathMode == 2) {
            QFile invalidProbe(copiedProbe);
            QVERIFY(invalidProbe.open(QIODevice::WriteOnly));
            QCOMPARE(invalidProbe.write("not-a-windows-executable"),
                     qint64(24));
            invalidProbe.close();
        }

        const QString base = emptyBase
            ? QString()
            : QString::fromLatin1(server.encodedRestBase());
        const QStringList arguments{
            QStringLiteral("-NoProfile"),
            QStringLiteral("-NonInteractive"),
            QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"),
            QStringLiteral("-File"),
            QString::fromUtf8(XC2_CONTRACT_WRAPPER_PATH),
            QStringLiteral("-BaseUrl"),
            base,
            QStringLiteral("-ProbePath"),
            copiedProbe,
            QStringLiteral("-TimeoutMs"),
            QString::number(kDefaultProbeTimeoutMs),
        };
        const ProcessResult result = runProcess(
            QStringLiteral("powershell.exe"), arguments);
        verifyCommonResult(server, result, expectedExitCode);
        if (probePathMode != 0)
            QCOMPARE(server.connectionCount(), 0);
    }
};

QTEST_GUILESS_MAIN(Xc2ContractProbeTest)
#include "test_Xc2ContractProbe.moc"
