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

constexpr int kDefaultProbeTimeoutMs = 2000;
constexpr int kWrapperProbeTimeoutMs = 7000;
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

bool waitForTerminalBurst(FakeXc2TransportServer &server)
{
    if (server.terminalBurstExecutionCount() != 0)
        return true;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.setTimerType(Qt::PreciseTimer);
    QObject::connect(
        &server, &FakeXc2TransportServer::terminalBurstExecuted,
        &loop, &QEventLoop::quit);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(750);
    loop.exec();
    return server.terminalBurstExecutionCount() == 1;
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
        QByteArrayLiteral("BOUNDARY_ERROR_SENTINEL_4c2a"),
        QByteArrayLiteral("BOUNDARY_BODY_SENTINEL_d5e1"),
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

struct ProbeMilestones {
    int restRequests = 0;
    int restResponses = 0;
    int upgrades = 0;
    int webSocketConnections = 0;
    int clientStompFrames = 0;
    int serverStompFrames = 0;
    int webSocketDisconnections = 0;
};

ProbeMilestones expectedMilestones(
    FakeXc2TransportServer::ProbeScript script)
{
    using Script = FakeXc2TransportServer::ProbeScript;
    switch (script) {
    case Script::HealthNoResponse:
        return {1, 0, 0, 0, 0, 0, 0};
    case Script::HealthTruncated:
    case Script::Health204:
    case Script::HealthMalformedSuccess:
    case Script::HealthBackendUnavailable:
    case Script::HealthAuth401:
    case Script::HealthMalformedError:
        return {1, 1, 0, 0, 0, 0, 0};
    case Script::CurrentUserNoResponse:
        return {2, 1, 0, 0, 0, 0, 0};
    case Script::CurrentUserTruncated:
    case Script::CurrentUser204:
    case Script::CurrentUserMalformedSuccess:
    case Script::CurrentUserBackendUnavailable:
    case Script::CurrentUserAuth401:
    case Script::CurrentUserAuth403:
    case Script::CurrentUserMalformedAuthError:
    case Script::CurrentUserMissingPermission:
    case Script::CurrentUserCaseMismatchedPermission:
    case Script::CurrentUserBlankLogin:
    case Script::CurrentUserBlankName:
        return {2, 2, 0, 0, 0, 0, 0};
    case Script::UpgradeNoResponse:
    case Script::UpgradeRejected:
    case Script::UpgradeWrongSubprotocol:
        return {2, 2, 1, 0, 0, 0, 0};
    case Script::MissingConnected:
        return {2, 2, 1, 1, 1, 0, 1};
    case Script::StompError:
    case Script::MalformedConnected:
        return {2, 2, 1, 1, 1, 1, 1};
    case Script::MissingDisconnectReceipt:
    case Script::CloseBeforeDisconnectReceipt:
    case Script::LateDisconnectReceipt:
        return {2, 2, 1, 1, 4, 1, 1};
    case Script::Happy:
    case Script::WrongDisconnectReceipt:
    case Script::ReceiptThenAbnormalClose:
        return {2, 2, 1, 1, 4, 2, 1};
    case Script::ApprovedTopicMessagesBeforeReceipt:
        return {2, 2, 1, 1, 4, 4, 1};
    case Script::DeadlineBoundaryTerminalBurst:
        return {2, 2, 1, 1, 4, -1, 1};
    case Script::Disabled:
        break;
    }
    Q_UNREACHABLE();
    return {};
}

QList<QByteArray> expectedServerCommands(
    FakeXc2TransportServer::ProbeScript script)
{
    using Script = FakeXc2TransportServer::ProbeScript;
    switch (script) {
    case Script::StompError:
        return {QByteArrayLiteral("ERROR")};
    case Script::MalformedConnected:
    case Script::MissingDisconnectReceipt:
    case Script::CloseBeforeDisconnectReceipt:
    case Script::LateDisconnectReceipt:
        return {QByteArrayLiteral("CONNECTED")};
    case Script::Happy:
    case Script::WrongDisconnectReceipt:
    case Script::ReceiptThenAbnormalClose:
        return {QByteArrayLiteral("CONNECTED"),
                QByteArrayLiteral("RECEIPT")};
    case Script::ApprovedTopicMessagesBeforeReceipt:
        return {QByteArrayLiteral("CONNECTED"),
                QByteArrayLiteral("MESSAGE"),
                QByteArrayLiteral("MESSAGE"),
                QByteArrayLiteral("RECEIPT")};
    default:
        return {};
    }
}

bool isOverallDeadlineScript(
    FakeXc2TransportServer::ProbeScript script)
{
    using Script = FakeXc2TransportServer::ProbeScript;
    switch (script) {
    case Script::HealthNoResponse:
    case Script::CurrentUserNoResponse:
    case Script::UpgradeNoResponse:
    case Script::MissingConnected:
    case Script::MissingDisconnectReceipt:
    case Script::LateDisconnectReceipt:
        return true;
    default:
        return false;
    }
}

bool waitForExpectedClientMilestones(
    FakeXc2TransportServer &server,
    FakeXc2TransportServer::ProbeScript script)
{
    const ProbeMilestones expected = expectedMilestones(script);
    const auto observed = [&server, &expected] {
        return server.capturedRestRequests().size()
                >= expected.restRequests
            && server.upgradeRequests().size() >= expected.upgrades
            && server.webSocketConnectionCount()
                >= expected.webSocketConnections
            && server.stompFrames().size() >= expected.clientStompFrames;
    };
    if (observed())
        return true;

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    deadline.setTimerType(Qt::PreciseTimer);
    const auto checkObserved = [&observed, &loop] {
        if (observed())
            loop.quit();
    };
    QObject::connect(&server,
                     &FakeXc2TransportServer::restRequestCaptured,
                     &loop, checkObserved);
    QObject::connect(&server,
                     &FakeXc2TransportServer::upgradeRequestCaptured,
                     &loop, checkObserved);
    QObject::connect(&server,
                     &FakeXc2TransportServer::webSocketConnected,
                     &loop, checkObserved);
    QObject::connect(&server,
                     &FakeXc2TransportServer::stompFrameReceived,
                     &loop, checkObserved);
    QObject::connect(&deadline, &QTimer::timeout,
                     &loop, &QEventLoop::quit);
    if (observed())
        return true;
    deadline.start(kCleanupBoundMs);
    loop.exec();
    return observed();
}

QByteArray expectedDiagnostic(
    FakeXc2TransportServer::ProbeScript script)
{
    using Script = FakeXc2TransportServer::ProbeScript;
    switch (script) {
    case Script::HealthNoResponse:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=health category=transport http=0 "
            "xc2=0 summary=overall-deadline");
    case Script::HealthTruncated:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=health category=transport http=200 "
            "xc2=0 summary=backend-unavailable");
    case Script::Health204:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=health category=contract http=204 "
            "xc2=0 summary=contract-failure");
    case Script::HealthMalformedSuccess:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=health category=contract http=200 "
            "xc2=0 summary=contract-failure");
    case Script::HealthBackendUnavailable:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=health category=backend http=503 "
            "xc2=7319 summary=backend-unavailable");
    case Script::HealthAuth401:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=health category=backend http=401 "
            "xc2=7319 summary=backend-unavailable");
    case Script::HealthMalformedError:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=health category=contract http=503 "
            "xc2=0 summary=contract-failure");
    case Script::CurrentUserNoResponse:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=current-user category=transport http=0 "
            "xc2=0 summary=overall-deadline");
    case Script::CurrentUserTruncated:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=current-user category=transport "
            "http=200 xc2=0 summary=backend-unavailable");
    case Script::CurrentUser204:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=current-user category=contract http=204 "
            "xc2=0 summary=not-authenticated");
    case Script::CurrentUserMalformedSuccess:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=current-user category=contract http=200 "
            "xc2=0 summary=contract-failure");
    case Script::CurrentUserBackendUnavailable:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=current-user category=backend http=503 "
            "xc2=7319 summary=backend-unavailable");
    case Script::CurrentUserAuth401:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=current-user category=backend http=401 "
            "xc2=7319 summary=not-authenticated");
    case Script::CurrentUserAuth403:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=current-user category=backend http=403 "
            "xc2=7319 summary=not-authenticated");
    case Script::CurrentUserMalformedAuthError:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=current-user category=contract http=401 "
            "xc2=0 summary=contract-failure");
    case Script::CurrentUserMissingPermission:
    case Script::CurrentUserCaseMismatchedPermission:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=permission category=session http=200 "
            "xc2=0 summary=permission-missing");
    case Script::CurrentUserBlankLogin:
    case Script::CurrentUserBlankName:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=current-user category=contract http=200 "
            "xc2=0 summary=blank-identity");
    case Script::UpgradeNoResponse:
    case Script::MissingConnected:
    case Script::MissingDisconnectReceipt:
    case Script::LateDisconnectReceipt:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=stomp category=transport http=0 "
            "xc2=0 summary=overall-deadline");
    case Script::UpgradeRejected:
    case Script::UpgradeWrongSubprotocol:
    case Script::CloseBeforeDisconnectReceipt:
    case Script::ReceiptThenAbnormalClose:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=stomp category=transport http=0 "
            "xc2=0 summary=transport-or-contract-failure");
    case Script::StompError:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=stomp category=backend http=0 "
            "xc2=0 summary=transport-or-contract-failure");
    case Script::MalformedConnected:
    case Script::WrongDisconnectReceipt:
        return QByteArrayLiteral(
            "xc2-contract-probe stage=stomp category=contract http=0 "
            "xc2=0 summary=transport-or-contract-failure");
    case Script::Happy:
    case Script::ApprovedTopicMessagesBeforeReceipt:
    case Script::DeadlineBoundaryTerminalBurst:
    case Script::Disabled:
        return {};
    }
    Q_UNREACHABLE();
    return {};
}

bool isExactSingleLine(const QByteArray &actual,
                       const QByteArray &expected)
{
    const QByteArray lfTerminated = expected + '\n';
    const QByteArray crlfTerminated = expected + "\r\n";
    return actual == lfTerminated || actual == crlfTerminated;
}

void verifyExactSingleLine(const QByteArray &actual,
                           const QByteArray &expected)
{
    QVERIFY2(isExactSingleLine(actual, expected),
             "output was not the exact expected single line");
}

void verifyProbeOutcome(
    const ProcessResult &result,
    FakeXc2TransportServer::ProbeScript script,
    int timeoutMs,
    bool wrapperProcess = false)
{
    using Script = FakeXc2TransportServer::ProbeScript;
    const bool successScript = script == Script::Happy
        || script == Script::ApprovedTopicMessagesBeforeReceipt;
    if (successScript) {
        verifyExactSingleLine(
            result.standardOutput,
            QByteArrayLiteral(
                "xc2-contract-probe compatible-read-only-surface"));
        QVERIFY(result.standardError.isEmpty());
    } else {
        const QByteArray diagnostic = expectedDiagnostic(script);
        QVERIFY2(!diagnostic.isEmpty(), "script has no fixed diagnostic");
        QVERIFY(result.standardOutput.isEmpty());
        verifyExactSingleLine(result.standardError, diagnostic);
    }

    if (!successScript && !isOverallDeadlineScript(script)) {
        const qint64 earlyBoundMs = timeoutMs
            - (wrapperProcess ? 1000 : 250);
        QVERIFY2(result.runningElapsedMs <= earlyBoundMs,
                 "immediate scripted result waited for the outer deadline");
    }
}

void verifyProbeMilestones(FakeXc2TransportServer &server,
                           FakeXc2TransportServer::ProbeScript script)
{
    using Script = FakeXc2TransportServer::ProbeScript;
    const ProbeMilestones expected = expectedMilestones(script);
    QCOMPARE(server.capturedRestRequests().size(), expected.restRequests);
    QCOMPARE(server.restResponses().size(), expected.restResponses);
    QCOMPARE(server.upgradeRequests().size(), expected.upgrades);
    const int expectedUpgradeResponses =
        script == Script::UpgradeRejected
            || script == Script::UpgradeWrongSubprotocol
        ? 1
        : 0;
    QCOMPARE(server.upgradeResponses().size(), expectedUpgradeResponses);
    if (script == Script::UpgradeRejected) {
        QVERIFY(server.upgradeResponses().constFirst().startsWith(
            "HTTP/1.1 403 Forbidden"));
    } else if (script == Script::UpgradeWrongSubprotocol) {
        QVERIFY(server.upgradeResponses().constFirst().contains(
            "Sec-WebSocket-Protocol: v11.stomp"));
    }
    QCOMPARE(server.webSocketConnectionCount(),
             expected.webSocketConnections);
    QCOMPARE(server.stompFrames().size(), expected.clientStompFrames);
    if (expected.serverStompFrames >= 0) {
        QCOMPARE(server.sentStompFrames().size(), expected.serverStompFrames);
    } else {
        QVERIFY(server.sentStompFrames().size() >= 1);
        QVERIFY(server.sentStompFrames().size() <= 3);
    }
    QCOMPARE(server.webSocketDisconnectionCount(),
             expected.webSocketDisconnections);

    if (expected.restRequests >= 2) {
        QVERIFY(!server.restResponses().isEmpty());
        QVERIFY(server.restResponses().constFirst().contains("alive"));
    }
    if (expected.upgrades == 1) {
        QCOMPARE(server.restResponses().size(), 2);
        QVERIFY(server.restResponses().at(1).contains("LOGIN_SENTINEL_15c8"));
        QVERIFY(server.restResponses().at(1).contains("EcuDiagnosticRead"));
    }

    if (script
        == FakeXc2TransportServer::ProbeScript::
            DeadlineBoundaryTerminalBurst) {
        const QList<QByteArray> possibleCommands{
            QByteArrayLiteral("CONNECTED"),
            QByteArrayLiteral("ERROR"),
            QByteArrayLiteral("RECEIPT"),
        };
        for (qsizetype index = 0;
             index < server.sentStompFrames().size(); ++index) {
            QCOMPARE(server.sentStompFrames().at(index).command,
                     possibleCommands.at(index));
        }
    } else {
        const QList<QByteArray> commands = expectedServerCommands(script);
        QCOMPARE(commands.size(), server.sentStompFrames().size());
        for (qsizetype index = 0; index < commands.size(); ++index) {
            QCOMPARE(server.sentStompFrames().at(index).command,
                     commands.at(index));
        }
    }
}

void verifyNoNetworkMilestones(const FakeXc2TransportServer &server)
{
    QCOMPARE(server.connectionCount(), 0);
    QCOMPARE(server.capturedRestRequests().size(), 0);
    QCOMPARE(server.restResponses().size(), 0);
    QCOMPARE(server.upgradeRequests().size(), 0);
    QCOMPARE(server.upgradeResponses().size(), 0);
    QCOMPARE(server.webSocketConnectionCount(), 0);
    QCOMPARE(server.webSocketDisconnectionCount(), 0);
    QCOMPARE(server.stompFrames().size(), 0);
    QCOMPARE(server.sentStompFrames().size(), 0);
}

void verifyExactSuccessCapture(FakeXc2TransportServer &server,
                               const ProcessResult &result)
{
    verifyCommonResult(server, result, 0);
    verifyProbeOutcome(
        result, FakeXc2TransportServer::ProbeScript::Happy,
        kDefaultProbeTimeoutMs);
    verifyProbeMilestones(server,
                          FakeXc2TransportServer::ProbeScript::Happy);
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

    void approvedCurrentGenerationMessagesKeepProbeReadOnly()
    {
        FakeXc2TransportServer server;
        QVERIFY(server.isListening());
        server.setProbeScript(
            FakeXc2TransportServer::ProbeScript::
                ApprovedTopicMessagesBeforeReceipt);
        server.setProbeDelayMs(40);
        server.setRestCookie(kCookie);
        const ProcessResult result = runProbe(server, 2000);
        verifyCommonResult(server, result, 0);
        verifyProbeOutcome(
            result,
            FakeXc2TransportServer::ProbeScript::
                ApprovedTopicMessagesBeforeReceipt,
            2000);
        verifyProbeMilestones(
            server,
            FakeXc2TransportServer::ProbeScript::
                ApprovedTopicMessagesBeforeReceipt);
        QCOMPARE(server.stompFrames().size(), 4);
        QCOMPARE(server.sentStompFrames().size(), 4);
        QCOMPARE(server.sentStompFrames().at(1).command,
                 QByteArrayLiteral("MESSAGE"));
        QCOMPARE(server.sentStompFrames().at(1).headers.value(
                     QByteArrayLiteral("destination")),
                 QByteArrayLiteral("/topic/vci/status"));
        QCOMPARE(server.sentStompFrames().at(1).headers.value(
                     QByteArrayLiteral("subscription")),
                 QByteArrayLiteral("vci-status-subscription"));
        QCOMPARE(server.sentStompFrames().at(2).command,
                 QByteArrayLiteral("MESSAGE"));
        QCOMPARE(server.sentStompFrames().at(2).headers.value(
                     QByteArrayLiteral("destination")),
                 QByteArrayLiteral("/topic/login"));
        QCOMPARE(server.sentStompFrames().at(2).headers.value(
                     QByteArrayLiteral("subscription")),
                 QByteArrayLiteral("login-subscription"));
        QCOMPARE(server.sentStompFrames().at(3).command,
                 QByteArrayLiteral("RECEIPT"));
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
        verifyNoNetworkMilestones(server);
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
        verifyNoNetworkMilestones(server);
    }

    void restTransportAndOverallDeadlineExitTwo_data()
    {
        QTest::addColumn<FakeXc2TransportServer::ProbeScript>("script");
        QTest::addColumn<int>("timeoutMs");
        using Script = FakeXc2TransportServer::ProbeScript;
        QTest::newRow("health-no-response-outer-deadline-120ms")
            << Script::HealthNoResponse << 120;
        QTest::newRow("health-no-response-outer-deadline-150ms")
            << Script::HealthNoResponse << 150;
        QTest::newRow("health-no-response-outer-deadline-500ms")
            << Script::HealthNoResponse << 500;
        QTest::newRow("health-truncated")
            << Script::HealthTruncated << kDefaultProbeTimeoutMs;
        QTest::newRow("health-backend-unavailable")
            << Script::HealthBackendUnavailable << kDefaultProbeTimeoutMs;
        QTest::newRow("health-401-is-not-current-user-auth")
            << Script::HealthAuth401 << kDefaultProbeTimeoutMs;
        QTest::newRow("current-user-no-response-outer-deadline")
            << Script::CurrentUserNoResponse << kDefaultProbeTimeoutMs;
        QTest::newRow("current-user-truncated")
            << Script::CurrentUserTruncated << kDefaultProbeTimeoutMs;
        QTest::newRow("current-user-backend-unavailable")
            << Script::CurrentUserBackendUnavailable
            << kDefaultProbeTimeoutMs;
    }

    void restTransportAndOverallDeadlineExitTwo()
    {
        QFETCH(FakeXc2TransportServer::ProbeScript, script);
        QFETCH(int, timeoutMs);
        FakeXc2TransportServer server;
        server.setProbeScript(script);
        server.setRestCookie(kCookie);
        const ProcessResult result = runProbe(server, timeoutMs);
        if (isOverallDeadlineScript(script)) {
            QVERIFY2(waitForExpectedClientMilestones(server, script),
                     "deadline client milestones were not observed");
        }
        verifyCommonResult(server, result, 2);
        verifyProbeOutcome(result, script, timeoutMs);
        verifyProbeMilestones(server, script);
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
        verifyProbeOutcome(result, script, kDefaultProbeTimeoutMs);
        verifyProbeMilestones(server, script);
    }

    void stompFailuresExitFour_data()
    {
        QTest::addColumn<FakeXc2TransportServer::ProbeScript>("script");
        QTest::addColumn<int>("timeoutMs");
        using Script = FakeXc2TransportServer::ProbeScript;
        QTest::newRow("upgrade-no-response")
            << Script::UpgradeNoResponse << kDefaultProbeTimeoutMs;
        QTest::newRow("upgrade-rejected")
            << Script::UpgradeRejected << kDefaultProbeTimeoutMs;
        QTest::newRow("wrong-subprotocol")
            << Script::UpgradeWrongSubprotocol << kDefaultProbeTimeoutMs;
        QTest::newRow("missing-connected")
            << Script::MissingConnected << kDefaultProbeTimeoutMs;
        QTest::newRow("stomp-error")
            << Script::StompError << kDefaultProbeTimeoutMs;
        QTest::newRow("malformed-connected")
            << Script::MalformedConnected << kDefaultProbeTimeoutMs;
        QTest::newRow("wrong-disconnect-receipt")
            << Script::WrongDisconnectReceipt << kDefaultProbeTimeoutMs;
        QTest::newRow("missing-disconnect-receipt")
            << Script::MissingDisconnectReceipt << kDefaultProbeTimeoutMs;
        QTest::newRow("close-before-disconnect-receipt")
            << Script::CloseBeforeDisconnectReceipt
            << kDefaultProbeTimeoutMs;
        QTest::newRow("receipt-then-abnormal-close")
            << Script::ReceiptThenAbnormalClose
            << kDefaultProbeTimeoutMs;
    }

    void stompFailuresExitFour()
    {
        QFETCH(FakeXc2TransportServer::ProbeScript, script);
        QFETCH(int, timeoutMs);
        FakeXc2TransportServer server;
        server.setProbeScript(script);
        server.setRestCookie(kCookie);
        const ProcessResult result = runProbe(server, timeoutMs);
        if (isOverallDeadlineScript(script)) {
            QVERIFY2(waitForExpectedClientMilestones(server, script),
                     "deadline client milestones were not observed");
        }
        verifyCommonResult(server, result, 4);
        verifyProbeOutcome(result, script, timeoutMs);
        verifyProbeMilestones(server, script);
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
        verifyProbeOutcome(result, script, kDefaultProbeTimeoutMs);
        verifyProbeMilestones(server, script);
    }

    void finishAndDeadlineRaceExitsOnceAndCleansConnections()
    {
        FakeXc2TransportServer server;
        server.setProbeScript(
            FakeXc2TransportServer::ProbeScript::
                DeadlineBoundaryTerminalBurst);
        server.setProbeDelayMs(970);
        server.setRestCookie(kCookie);

        const ProcessResult result = runProbe(server, 1000);
        QVERIFY2(waitForTerminalBurst(server),
                 "deadline-boundary terminal burst never executed");
        verifyCommonResult(server, result, 4);
        verifyProbeMilestones(
            server,
            FakeXc2TransportServer::ProbeScript::
                DeadlineBoundaryTerminalBurst);
        QCOMPARE(result.finishedSignals, 1);
        QCOMPARE(server.terminalBurstExecutionCount(), 1);
        QCOMPARE(result.standardOutput, QByteArray());
        const QList<QByteArray> allowedDiagnostics{
            QByteArrayLiteral(
                "xc2-contract-probe stage=stomp category=backend http=0 "
                "xc2=0 summary=transport-or-contract-failure"),
            QByteArrayLiteral(
                "xc2-contract-probe stage=stomp category=transport http=0 "
                "xc2=0 summary=overall-deadline"),
        };
        bool exactDiagnostic = false;
        for (const QByteArray &diagnostic : allowedDiagnostics)
            exactDiagnostic |= isExactSingleLine(
                result.standardError, diagnostic);
        QVERIFY2(exactDiagnostic,
                 "race output was not an allowed exact single line");
        QCOMPARE(server.attemptedTerminalFrames().size(), 2);
        QCOMPARE(server.attemptedTerminalFrames().at(0).command,
                 QByteArrayLiteral("ERROR"));
        QVERIFY(server.attemptedTerminalFrames().at(0).headers.value(
                     QByteArrayLiteral("message"))
                    .contains("BOUNDARY_ERROR_SENTINEL_4c2a"));
        QVERIFY(server.attemptedTerminalFrames().at(0).body.contains(
            "BOUNDARY_BODY_SENTINEL_d5e1"));
        QCOMPARE(server.attemptedTerminalFrames().at(1).command,
                 QByteArrayLiteral("RECEIPT"));
        QCOMPARE(server.attemptedTerminalFrames().at(1).headers.value(
                     QByteArrayLiteral("receipt-id")),
                 QByteArrayLiteral("disconnect-1"));
        QCOMPARE(server.terminalCloseAttemptCount(), 1);
        QCOMPARE(server.openConnectionCount(), 0);
    }

    void outerDeadlinePreemptsDisconnectBackstop()
    {
        FakeXc2TransportServer server;
        server.setProbeScript(
            FakeXc2TransportServer::ProbeScript::LateDisconnectReceipt);
        server.setProbeDelayMs(3000);
        server.setRestCookie(kCookie);
        const ProcessResult result = runProbe(server, 2000);
        QVERIFY2(waitForExpectedClientMilestones(
                     server,
                     FakeXc2TransportServer::ProbeScript::
                         LateDisconnectReceipt),
                 "deadline client milestones were not observed");
        verifyCommonResult(server, result, 4);
        verifyProbeOutcome(
            result,
            FakeXc2TransportServer::ProbeScript::LateDisconnectReceipt,
            2000);
        verifyProbeMilestones(
            server,
            FakeXc2TransportServer::ProbeScript::LateDisconnectReceipt);
        QVERIFY2(result.runningElapsedMs < 2500,
                 "outer disconnect deadline lost to its backstop");
        QCOMPARE(result.finishedSignals, 1);
        QCOMPARE(server.stompFrames().size(), 4);
        QCOMPARE(server.sentStompFrames().size(), 1);
        QCOMPARE(server.openConnectionCount(), 0);
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
            QVERIFY(server.restResponses().at(1).contains(
                "LOGIN_SENTINEL_15c8"));
            QVERIFY(server.restResponses().at(1).contains(
                "NAME_SENTINEL_b97e"));
            QVERIFY(server.restResponses().at(1).contains(
                "DEALER_SENTINEL_51aa"));
            QVERIFY(server.restResponses().at(1).contains(
                "ADDRESS_SENTINEL_70d4"));
            QVERIFY(server.restResponses().at(1).contains(
                "SESSION_SENTINEL_5ed1"));
        }
        {
            FakeXc2TransportServer server;
            server.setProbeScript(
                FakeXc2TransportServer::ProbeScript::CurrentUserAuth401);
            server.setRestCookie(kCookie);
            const ProcessResult result = runProbe(server);
            verifyCommonResult(server, result, 5);
            verifyProbeOutcome(
                result,
                FakeXc2TransportServer::ProbeScript::CurrentUserAuth401,
                kDefaultProbeTimeoutMs);
            verifyProbeMilestones(
                server,
                FakeXc2TransportServer::ProbeScript::CurrentUserAuth401);
            verifyNoSensitiveOutput(result);
            QVERIFY(server.restResponses().at(1).contains(
                "BACKEND_MESSAGE_SENTINEL_6f25"));
            QVERIFY(server.restResponses().at(1).contains(
                "DEVELOPER_SENTINEL_844a"));
            QVERIFY(server.restResponses().at(1).contains(
                "INFO_SENTINEL_502d"));
        }
        {
            FakeXc2TransportServer server;
            server.setProbeScript(
                FakeXc2TransportServer::ProbeScript::StompError);
            server.setRestCookie(kCookie);
            const ProcessResult result = runProbe(server);
            verifyCommonResult(server, result, 4);
            verifyProbeOutcome(
                result,
                FakeXc2TransportServer::ProbeScript::StompError,
                kDefaultProbeTimeoutMs);
            verifyProbeMilestones(
                server,
                FakeXc2TransportServer::ProbeScript::StompError);
            verifyNoSensitiveOutput(result);
            QCOMPARE(server.sentStompFrames().size(), 1);
            QVERIFY(server.sentStompFrames().constFirst().headers.value(
                         QByteArrayLiteral("message"))
                        .contains("STOMP_MESSAGE_SENTINEL_95be"));
            QVERIFY(server.sentStompFrames().constFirst().body.contains(
                "STOMP_BODY_SENTINEL_d71a"));
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
            QString::number(kWrapperProbeTimeoutMs),
        };
        const ProcessResult result = runProcess(
            QStringLiteral("powershell.exe"), arguments);
        verifyCommonResult(server, result, expectedExitCode);
        if (probePathMode != 0 || emptyBase)
            verifyNoNetworkMilestones(server);
        else {
            verifyProbeOutcome(result, script, kWrapperProbeTimeoutMs,
                               true);
            verifyProbeMilestones(server, script);
        }
    }
};

QTEST_GUILESS_MAIN(Xc2ContractProbeTest)
#include "test_Xc2ContractProbe.moc"
