#include "FakeXc2TransportServer.h"

#include "ktm/KtmSessionController.h"
#include "ktm/xc2/Xc2ContractProfile.h"
#include "ktm/xc2/Xc2JsonCodec.h"

#include <QPointer>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>
#include <QtTest>

#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>

using namespace ktm;
using namespace ktm::xc2;

namespace ktm {

class KtmSessionControllerTestAccess final {
public:
    using Ops = KtmSessionController::PrivateTestOps;

    static void installOps(KtmSessionController &controller, Ops ops)
    {
        controller.m_testOps = std::move(ops);
    }

    static void setSubscribeOp(
        KtmSessionController &controller,
        std::function<bool(xc2::Topic, xc2::Xc2Error *)> subscribe)
    {
        controller.m_testOps.subscribeStomp = std::move(subscribe);
    }

    static void setConnectOp(
        KtmSessionController &controller,
        std::function<bool(xc2::Xc2Error *)> connect)
    {
        controller.m_testOps.connectStomp = std::move(connect);
    }

    static void setDisconnectOp(KtmSessionController &controller,
                                 std::function<void()> disconnect)
    {
        controller.m_testOps.disconnectStomp = std::move(disconnect);
    }

    static void setAbortRestOp(
        KtmSessionController &controller,
        std::function<void(xc2::Xc2RequestId)> abortRest)
    {
        controller.m_testOps.abortRest = std::move(abortRest);
    }

    static void setAbortStompOp(KtmSessionController &controller,
                                std::function<void()> abortStomp)
    {
        controller.m_testOps.abortStomp = std::move(abortStomp);
    }

    static void setStopBackendOp(KtmSessionController &controller,
                                 std::function<void()> stopBackend)
    {
        controller.m_testOps.stopBackend = std::move(stopBackend);
    }

    static KtmSessionState state(const KtmSessionController &controller)
    {
        return controller.m_state;
    }

    static KtmSessionOperation operation(
        const KtmSessionController &controller)
    {
        return controller.m_operation;
    }

    static void setOperation(KtmSessionController &controller,
                             KtmSessionOperation operation)
    {
        controller.m_operation = operation;
    }

    static quint64 sessionEpoch(const KtmSessionController &controller)
    {
        return controller.m_sessionEpoch;
    }

    static quint64 selectionEpoch(const KtmSessionController &controller)
    {
        return controller.m_selectionEpoch;
    }

    static Xc2StompGeneration stompGeneration(
        const KtmSessionController &controller)
    {
        return controller.m_stompGeneration;
    }

    static QString lookupJobId(const KtmSessionController &controller)
    {
        return controller.m_lookupJobId;
    }

    static bool stopping(const KtmSessionController &controller)
    {
        return controller.m_stopping;
    }

    static quintptr restIdentity(const KtmSessionController &controller)
    {
        return reinterpret_cast<quintptr>(controller.m_restClient.data());
    }

    static quintptr stompIdentity(const KtmSessionController &controller)
    {
        return reinterpret_cast<quintptr>(controller.m_stompClient.data());
    }

    static quintptr registryIdentity(const KtmSessionController &controller)
    {
        return reinterpret_cast<quintptr>(controller.m_jobRegistry.data());
    }

    static QPointer<xc2::Xc2RestClient> restGuard(
        const KtmSessionController &controller)
    {
        return controller.m_restClient;
    }

    static QPointer<xc2::Xc2BackendManager> backendGuard(
        const KtmSessionController &controller)
    {
        return controller.m_backend;
    }

    static QPointer<xc2::Xc2StompClient> stompGuard(
        const KtmSessionController &controller)
    {
        return controller.m_stompClient;
    }

    static QPointer<xc2::Xc2JobRegistry> registryGuard(
        const KtmSessionController &controller)
    {
        return controller.m_jobRegistry;
    }

    static void destroyBackendChild(KtmSessionController &controller)
    {
        QPointer<xc2::Xc2BackendManager> backend = controller.m_backend;
        controller.m_backend = nullptr;
        delete backend.data();
    }

    static void destroyRestChild(KtmSessionController &controller)
    {
        QPointer<xc2::Xc2RestClient> rest = controller.m_restClient;
        controller.m_restClient = nullptr;
        delete rest.data();
    }

    static void destroyTransportChildren(KtmSessionController &controller)
    {
        QPointer<xc2::Xc2RestClient> rest = controller.m_restClient;
        QPointer<xc2::Xc2StompClient> stomp = controller.m_stompClient;
        controller.m_restClient = nullptr;
        controller.m_stompClient = nullptr;
        delete rest.data();
        delete stomp.data();
    }

    static void backendStateChanged(KtmSessionController &controller,
                                    Xc2BackendState state)
    {
        controller.handleBackendStateChanged(state);
    }

    static void backendReady(KtmSessionController &controller,
                             const Xc2BackendEndpoints &endpoints)
    {
        controller.handleBackendReady(endpoints);
    }

    static void emitBackendStateChanged(KtmSessionController &controller,
                                        Xc2BackendState state)
    {
        emit controller.m_backend->stateChanged(state);
    }

    static void emitBackendReady(
        KtmSessionController &controller,
        const Xc2BackendEndpoints &published)
    {
        emit controller.m_backend->ready(published);
    }

    static void emitServiceStatusFinished(
        KtmSessionController &controller, Xc2RequestId id,
        const Xc2Result<Xc2ServiceStatus> &result)
    {
        emit controller.m_restClient->serviceStatusFinished(id, result);
    }

    static void emitCurrentUserFinished(
        KtmSessionController &controller, Xc2RequestId id,
        const Xc2Result<Xc2CurrentUser> &result)
    {
        emit controller.m_restClient->currentUserFinished(id, result);
    }

    static void emitStompConnected(KtmSessionController &controller,
                                   Xc2StompGeneration generation)
    {
        Xc2StompSession session;
        session.generation = generation;
        session.version = QStringLiteral("1.2");
        emit controller.m_stompClient->connected(session);
    }

    static bool connectRealStompChild(KtmSessionController &controller,
                                      Xc2Error *error)
    {
        return controller.m_stompClient->connectToBackend(
            *controller.m_restClient, error);
    }

    static Xc2StompGeneration realStompGeneration(
        const KtmSessionController &controller)
    {
        return controller.m_stompClient->generation();
    }

    static void serviceStatusFinished(
        KtmSessionController &controller, quint64 epoch, quintptr identity,
        Xc2RequestId id, const Xc2Result<Xc2ServiceStatus> &result)
    {
        controller.handleServiceStatusFinished(
            epoch, identity, id, result);
    }

    static void currentUserFinished(
        KtmSessionController &controller, quint64 epoch, quintptr identity,
        Xc2RequestId id, const Xc2Result<Xc2CurrentUser> &result)
    {
        controller.handleCurrentUserFinished(epoch, identity, id, result);
    }

    static void stompConnected(KtmSessionController &controller,
                               quint64 epoch, quintptr identity,
                               Xc2StompGeneration generation)
    {
        Xc2StompSession session;
        session.generation = generation;
        session.version = QStringLiteral("1.2");
        controller.handleStompConnected(epoch, identity, session);
    }

    static void subscriptionSent(KtmSessionController &controller,
                                 xc2::Topic topic)
    {
        emit controller.m_stompClient->subscriptionSent(
            topic, QStringLiteral("synthetic-subscription"));
    }

    static void stompMessage(KtmSessionController &controller,
                             const xc2::Xc2StompMessage &message)
    {
        emit controller.m_stompClient->messageReceived(message);
    }

    static void deviceLookupFinished(
        KtmSessionController &controller, xc2::Xc2RequestId id,
        const xc2::Xc2Result<xc2::Xc2JobAccepted> &result)
    {
        emit controller.m_restClient->deviceLookupFinished(id, result);
    }

    static void devicesFinished(
        KtmSessionController &controller, xc2::Xc2RequestId id,
        const xc2::Xc2Result<QList<xc2::Xc2VciDevice>> &result)
    {
        emit controller.m_restClient->devicesFinished(id, result);
    }

    static void applyDeviceFinished(KtmSessionController &controller,
                                    xc2::Xc2RequestId id,
                                    const xc2::Xc2Error &error = {})
    {
        emit controller.m_restClient->applyDeviceFinished(id, error);
    }

    static void selectedDeviceFinished(
        KtmSessionController &controller, xc2::Xc2RequestId id,
        const xc2::Xc2Result<xc2::Xc2SelectedVci> &result)
    {
        emit controller.m_restClient->selectedDeviceFinished(id, result);
    }

    static void closeDeviceFinished(KtmSessionController &controller,
                                    xc2::Xc2RequestId id,
                                    const xc2::Xc2Error &error = {})
    {
        emit controller.m_restClient->closeDeviceFinished(id, error);
    }

    static void visibilityLost(KtmSessionController &controller,
                               xc2::Xc2StompGeneration generation,
                               const xc2::Xc2Error &error)
    {
        emit controller.m_stompClient->visibilityLost(generation, error);
    }

    static void stompErrorThenVisibility(
        KtmSessionController &controller,
        xc2::Xc2StompGeneration generation,
        const xc2::Xc2Error &error)
    {
        QPointer<xc2::Xc2StompClient> child = controller.m_stompClient;
        emit child->errorOccurred(generation, error);
        if (child)
            emit child->visibilityLost(generation, error);
    }

    static void staleApplyDeviceFinished(
        KtmSessionController &controller, quint64 sessionEpoch,
        quint64 selectionEpoch, quintptr identity, xc2::Xc2RequestId id,
        const xc2::Xc2Error &error = {})
    {
        controller.handleApplyDeviceFinished(
            sessionEpoch, selectionEpoch, identity, id, error);
    }

    static void advanceSelectionEpoch(KtmSessionController &controller)
    {
        ++controller.m_selectionEpoch;
    }

    static void failSession(KtmSessionController &controller,
                            const xc2::Xc2Error &error)
    {
        controller.failSession(error);
    }

    static void publishSyntheticReady(KtmSessionController &controller)
    {
        controller.m_state = KtmSessionState::VciApplying;
        controller.m_operation = KtmSessionOperation::Apply;
        controller.m_applySucceeded = true;
        controller.m_selectedConfirmed = true;
        controller.m_connectedForSelectionEpoch = true;
        controller.m_confirmedDevice = xc2::Xc2VciDevice{
            QStringLiteral("vci-1"), QStringLiteral("Synthetic"),
            QStringLiteral("AVL Ditest VCI2K_DPDU_API"), std::nullopt};
        controller.m_latestVciStatus = Xc2VciStatus{12.4, true, {}};
        controller.tryPublishVciReady();
    }

    static void publishState(KtmSessionController &controller,
                             KtmSessionState state)
    {
        controller.publishState(state);
    }
};

} // namespace ktm

namespace {

constexpr auto kRestBase = "http://127.0.0.1:49152/xc2/1.0/";
constexpr auto kWebSocket = "ws://127.0.0.1:49152/xc2/1.0/stomp";

Xc2BackendEndpoints endpoints(const char *webSocket = kWebSocket)
{
    return {QUrl(QString::fromLatin1(kRestBase)),
            QUrl(QString::fromLatin1(webSocket))};
}

Xc2CurrentUser authorizedUser(QString permission =
                                  QStringLiteral("EcuDiagnosticRead"))
{
    Xc2CurrentUser user;
    user.loginName = QStringLiteral("synthetic");
    user.name = QStringLiteral("Synthetic Operator");
    user.permissions = {std::move(permission)};
    return user;
}

struct BootstrapHarness {
    KtmSessionController controller;
    QStringList trace;
    Xc2BackendState backendState = Xc2BackendState::Stopped;
    Xc2RequestId serviceStatusId = 11;
    Xc2RequestId currentUserId = 12;
    Xc2RequestId lookupId = 21;
    Xc2RequestId devicesId = 22;
    Xc2RequestId applyId = 31;
    Xc2RequestId selectedId = 32;
    Xc2RequestId closeId = 41;
    Xc2StompGeneration generation = 7;
    QUrl derivedWebSocket = QUrl(QString::fromLatin1(kWebSocket));
    std::optional<Xc2VciDevice> closePayload;

    BootstrapHarness()
    {
        KtmSessionControllerTestAccess::Ops ops;
        ops.backendState = [this] { return backendState; };
        ops.startBackend = [this](const QString &installRoot, Xc2Error *) {
            trace.append(QStringLiteral("start:%1").arg(installRoot));
            return true;
        };
        ops.setRestBaseUrl = [this](const QByteArray &base, Xc2Error *) {
            trace.append(QStringLiteral("base:%1").arg(
                QString::fromLatin1(base)));
            return true;
        };
        ops.restWebSocketUrl = [this] { return derivedWebSocket; };
        ops.requestServiceStatus = [this] {
            trace.append(QStringLiteral("serviceStatus"));
            return serviceStatusId;
        };
        ops.requestCurrentUser = [this] {
            trace.append(QStringLiteral("currentUser"));
            return currentUserId;
        };
        ops.connectStomp = [this](Xc2Error *) {
            trace.append(QStringLiteral("stomp-connect"));
            return true;
        };
        ops.subscribeStomp = [this](Topic topic, Xc2Error *) {
            trace.append(QStringLiteral("subscribe:%1").arg(int(topic)));
            return true;
        };
        ops.stompGeneration = [this] { return generation; };
        ops.requestDeviceLookup = [this] {
            trace.append(QStringLiteral("device-lookup"));
            return lookupId;
        };
        ops.requestDevices = [this] {
            trace.append(QStringLiteral("devices"));
            return devicesId;
        };
        ops.requestApplyDevice = [this](const Xc2VciDevice &device) {
            trace.append(QStringLiteral("apply:%1:%2:%3:%4")
                             .arg(device.id, device.name,
                                  device.internalName,
                                  device.additionalModuleInformation
                                      .value_or(QStringLiteral("<null>"))));
            return applyId;
        };
        ops.requestSelectedDevice = [this] {
            trace.append(QStringLiteral("selected"));
            return selectedId;
        };
        ops.requestCloseDevice = [this](const Xc2VciDevice &device) {
            closePayload = device;
            trace.append(QStringLiteral("close:%1").arg(device.id));
            return closeId;
        };
        ops.abortRest = [this](Xc2RequestId id) {
            trace.append(QStringLiteral("abort-rest:%1").arg(id));
        };
        ops.disconnectStomp = [this] {
            trace.append(QStringLiteral("disconnect-stomp"));
        };
        ops.abortStomp = [this] {
            trace.append(QStringLiteral("abort-stomp"));
        };
        ops.sessionObjectsReset = [this] {
            trace.append(QStringLiteral("reset-session"));
        };
        ops.stopBackend = [this] {
            trace.append(QStringLiteral("stop-backend"));
        };
        KtmSessionControllerTestAccess::installOps(controller,
                                                   std::move(ops));
    }

    bool start()
    {
        return controller.startProduction(QStringLiteral("C:/XC2"));
    }

    void publishReady(const Xc2BackendEndpoints &published = endpoints())
    {
        backendState = Xc2BackendState::Ready;
        KtmSessionControllerTestAccess::backendStateChanged(
            controller, Xc2BackendState::Ready);
        KtmSessionControllerTestAccess::backendReady(controller, published);
    }

    void completeHealth(bool alive = true)
    {
        KtmSessionControllerTestAccess::serviceStatusFinished(
            controller,
            KtmSessionControllerTestAccess::sessionEpoch(controller),
            KtmSessionControllerTestAccess::restIdentity(controller),
            serviceStatusId,
            Xc2Result<Xc2ServiceStatus>::success({alive}));
    }

    void completeUser(const Xc2CurrentUser &user = authorizedUser())
    {
        KtmSessionControllerTestAccess::currentUserFinished(
            controller,
            KtmSessionControllerTestAccess::sessionEpoch(controller),
            KtmSessionControllerTestAccess::restIdentity(controller),
            currentUserId,
            Xc2Result<Xc2CurrentUser>::success(user));
    }

    void completeBootstrap()
    {
        QVERIFY(start());
        publishReady();
        completeHealth();
        completeUser();
        KtmSessionControllerTestAccess::stompConnected(
            controller,
            KtmSessionControllerTestAccess::sessionEpoch(controller),
            KtmSessionControllerTestAccess::stompIdentity(controller),
            generation);
        QCOMPARE(KtmSessionControllerTestAccess::state(controller),
                 KtmSessionState::SessionReady);
    }
};

QByteArray progressBody(const QString &jobId, Xc2JobState state)
{
    QString wireState;
    switch (state) {
    case Xc2JobState::Created:
        wireState = QStringLiteral("CREATED");
        break;
    case Xc2JobState::InProgress:
        wireState = QStringLiteral("IN_PROGRESS");
        break;
    case Xc2JobState::Finished:
        wireState = QStringLiteral("FINISHED");
        break;
    case Xc2JobState::Canceled:
        wireState = QStringLiteral("CANCELED");
        break;
    case Xc2JobState::Error:
        wireState = QStringLiteral("ERROR");
        break;
    case Xc2JobState::NotAuthorized:
        wireState = QStringLiteral("NOT_AUTHORIZED");
        break;
    }
    return QJsonDocument(QJsonObject{
        {QStringLiteral("jobId"), jobId},
        {QStringLiteral("status"), wireState},
        {QStringLiteral("ticks"), 1},
        {QStringLiteral("totalTicks"), 1},
        {QStringLiteral("message"), QJsonValue(QJsonValue::Null)}})
        .toJson(QJsonDocument::Compact);
}

Xc2StompMessage progressMessage(Xc2StompGeneration generation,
                                const QString &jobId,
                                Xc2JobState state,
                                QString messageId = QStringLiteral("m-1"))
{
    Xc2StompMessage message;
    message.generation = generation;
    message.topic = Topic::Progress;
    message.destination =
        Xc2ContractProfile::approved().topic(Topic::Progress);
    message.subscriptionId = QStringLiteral("progress-subscription");
    message.messageId = std::move(messageId);
    message.body = progressBody(jobId, state);
    return message;
}

Xc2StompMessage vciStatusMessage(Xc2StompGeneration generation,
                                 bool connected,
                                 double voltage)
{
    Xc2StompMessage message;
    message.generation = generation;
    message.topic = Topic::VciStatus;
    message.destination =
        Xc2ContractProfile::approved().topic(Topic::VciStatus);
    message.subscriptionId = QStringLiteral("vci-status-subscription");
    message.messageId = QStringLiteral("vci-status-message");
    message.body = QJsonDocument(QJsonObject{
        {QStringLiteral("voltage"), voltage},
        {QStringLiteral("connected"), connected}})
        .toJson(QJsonDocument::Compact);
    return message;
}

Xc2VciDevice approvedDevice(QString name = QStringLiteral("Candidate"),
                            std::optional<QString> module = std::nullopt)
{
    return {QStringLiteral("vci-1"), std::move(name),
            QStringLiteral("AVL Ditest VCI2K_DPDU_API"),
            std::move(module)};
}

using FakeAction = FakeXc2TransportServer::Action;
using FakeExpectation = FakeXc2TransportServer::Expectation;
using FakePhase = FakeXc2TransportServer::ScriptPhase;

QByteArray compactDevice(const Xc2VciDevice &device)
{
    return QJsonDocument(Xc2JsonCodec::vciDeviceJson(device))
        .toJson(QJsonDocument::Compact);
}

QByteArray compactDevices(const QList<Xc2VciDevice> &devices)
{
    QJsonArray array;
    for (const Xc2VciDevice &device : devices)
        array.append(Xc2JsonCodec::vciDeviceJson(device));
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

Xc2StompFrame connectedFrame()
{
    Xc2StompFrame frame;
    frame.command = QByteArrayLiteral("CONNECTED");
    frame.headers.insert(QByteArrayLiteral("version"),
                         QByteArrayLiteral("1.2"));
    frame.headers.insert(QByteArrayLiteral("heart-beat"),
                         QByteArrayLiteral("0,0"));
    return frame;
}

Xc2StompFrame expectedConnectFrame()
{
    Xc2StompFrame frame;
    frame.command = QByteArrayLiteral("CONNECT");
    frame.headers.insert(QByteArrayLiteral("accept-version"),
                         QByteArrayLiteral("1.2"));
    frame.headers.insert(QByteArrayLiteral("heart-beat"),
                         QByteArrayLiteral("10000,10000"));
    frame.headers.insert(QByteArrayLiteral("host"),
                         QByteArrayLiteral("127.0.0.1"));
    return frame;
}

Xc2StompFrame expectedSubscription(Topic topic, const QByteArray &id)
{
    Xc2StompFrame frame;
    frame.command = QByteArrayLiteral("SUBSCRIBE");
    frame.headers.insert(QByteArrayLiteral("ack"),
                         QByteArrayLiteral("auto"));
    frame.headers.insert(QByteArrayLiteral("destination"),
                         Xc2ContractProfile::approved().topic(topic).toUtf8());
    frame.headers.insert(QByteArrayLiteral("id"), id);
    return frame;
}

Xc2StompFrame topicMessage(Topic topic, const QByteArray &subscription,
                           const QByteArray &messageId,
                           const QByteArray &body)
{
    Xc2StompFrame frame;
    frame.command = QByteArrayLiteral("MESSAGE");
    frame.headers.insert(QByteArrayLiteral("destination"),
                         Xc2ContractProfile::approved().topic(topic).toUtf8());
    frame.headers.insert(QByteArrayLiteral("subscription"), subscription);
    frame.headers.insert(QByteArrayLiteral("message-id"), messageId);
    frame.body = body;
    return frame;
}

FakeAction httpReply(QString label, int status, QByteArray reason,
                     QByteArray body = {},
                     QList<QPair<QByteArray, QByteArray>> headers = {},
                     QString targetExpectation = {})
{
    FakeAction action;
    action.label = std::move(label);
    action.type = FakeAction::Type::HttpResponse;
    action.target = targetExpectation.isEmpty()
        ? FakeAction::Target::EventSocket
        : FakeAction::Target::ExpectationSocket;
    action.targetExpectation = std::move(targetExpectation);
    action.status = status;
    action.reason = std::move(reason);
    action.body = std::move(body);
    action.headers = std::move(headers);
    return action;
}

FakeAction sendTopic(QString label, Xc2StompFrame frame, int delayMs = 0)
{
    FakeAction action;
    action.label = std::move(label);
    action.type = FakeAction::Type::SendStompFrame;
    action.target = FakeAction::Target::WebSocket;
    action.stompFrame = std::move(frame);
    action.delayMs = delayMs;
    return action;
}

FakeExpectation restExpectation(
    const FakeXc2TransportServer &server, QString label, QByteArray method,
    QByteArray target, QByteArray body, std::optional<QByteArray> cookie,
    QList<FakeAction> actions = {})
{
    FakeExpectation expectation;
    expectation.label = std::move(label);
    expectation.protocol = FakeExpectation::Protocol::Http;
    expectation.method = std::move(method);
    expectation.target = std::move(target);
    expectation.body = std::move(body);
    expectation.headers = {
        {QByteArrayLiteral("Host"),
         QByteArrayLiteral("127.0.0.1:")
             + QByteArray::number(server.port())},
        {QByteArrayLiteral("Accept"), QByteArrayLiteral("*/*")},
        {QByteArrayLiteral("Accept-Encoding"),
         QByteArrayLiteral("identity")},
        {QByteArrayLiteral("Connection"), QByteArrayLiteral("close")},
    };
    if (cookie) {
        expectation.headers.append(
            {QByteArrayLiteral("Cookie"), *cookie});
    } else {
        expectation.absentHeaders.append(QByteArrayLiteral("Cookie"));
    }
    if (expectation.method == QByteArrayLiteral("POST")) {
        expectation.headers.append(
            {QByteArrayLiteral("Content-Type"),
             QByteArrayLiteral("application/json")});
        expectation.headers.append(
            {QByteArrayLiteral("Content-Length"),
             QByteArray::number(expectation.body.size())});
    } else {
        expectation.absentHeaders.append(
            QByteArrayLiteral("Content-Type"));
        expectation.absentHeaders.append(
            QByteArrayLiteral("Content-Length"));
    }
    expectation.actions = std::move(actions);
    return expectation;
}

FakeExpectation upgradeExpectation(
    const FakeXc2TransportServer &server, const QByteArray &cookie)
{
    FakeExpectation expectation;
    expectation.label = QStringLiteral("websocket-upgrade");
    expectation.protocol = FakeExpectation::Protocol::Http;
    expectation.webSocketUpgrade = true;
    expectation.method = QByteArrayLiteral("GET");
    expectation.target = QByteArrayLiteral("/xc2-websocket");
    expectation.headers = {
        {QByteArrayLiteral("Host"),
         QByteArrayLiteral("127.0.0.1:")
             + QByteArray::number(server.port())},
        {QByteArrayLiteral("Cookie"), cookie},
        {QByteArrayLiteral("Upgrade"), QByteArrayLiteral("websocket")},
        {QByteArrayLiteral("Connection"), QByteArrayLiteral("Upgrade")},
        {QByteArrayLiteral("Sec-WebSocket-Version"),
         QByteArrayLiteral("13")},
        {QByteArrayLiteral("Sec-WebSocket-Protocol"),
         QByteArrayLiteral("v12.stomp")},
    };
    expectation.absentHeaders = {QByteArrayLiteral("Content-Type"),
                                 QByteArrayLiteral("Content-Length")};
    return expectation;
}

FakeExpectation stompExpectation(QString label, Xc2StompFrame frame,
                                 QList<FakeAction> actions = {})
{
    FakeExpectation expectation;
    expectation.label = std::move(label);
    expectation.protocol = FakeExpectation::Protocol::Stomp;
    expectation.stompFrame = std::move(frame);
    expectation.actions = std::move(actions);
    return expectation;
}

FakePhase orderedPhase(QString label, FakeExpectation expectation)
{
    FakePhase phase;
    phase.label = std::move(label);
    phase.expectations.append(std::move(expectation));
    return phase;
}

QList<FakePhase> completeHappyScript(const FakeXc2TransportServer &server,
                                     const Xc2VciDevice &device)
{
    const QByteArray cookie = QByteArrayLiteral("session=task4");
    const QByteArray currentUser = QByteArrayLiteral(
        "{\"loginName\":\"task4\",\"name\":\"Task 4 Operator\","
        "\"permissions\":[\"EcuDiagnosticRead\"]}");
    const QByteArray lookupId = QByteArrayLiteral("lookup-task4");
    const Xc2StompFrame progress = topicMessage(
        Topic::Progress, QByteArrayLiteral("progress-subscription"),
        QByteArrayLiteral("progress-finished"),
        progressBody(QString::fromLatin1(lookupId), Xc2JobState::Finished));
    const Xc2StompFrame status = topicMessage(
        Topic::VciStatus, QByteArrayLiteral("vci-status-subscription"),
        QByteArrayLiteral("status-connected"),
        QByteArrayLiteral("{\"voltage\":12.4,\"connected\":true}"));

    const QList<QPair<QByteArray, QByteArray>> jsonHeader = {
        {QByteArrayLiteral("Content-Type"),
         QByteArrayLiteral("application/json")}};
    QList<FakePhase> phases;

    FakeExpectation health = restExpectation(
        server, QStringLiteral("health"), QByteArrayLiteral("GET"),
        QByteArrayLiteral("/xc2/1.0/serviceStatus/status"), {},
        std::nullopt);
    health.actions.append(httpReply(
        QStringLiteral("health-response"), 200, QByteArrayLiteral("OK"),
        QByteArrayLiteral("alive"),
        {{QByteArrayLiteral("Content-Type"), QByteArrayLiteral("text/plain")},
         {QByteArrayLiteral("Set-Cookie"),
          cookie + QByteArrayLiteral("; Path=/")}}));
    phases.append(orderedPhase(QStringLiteral("health"), std::move(health)));

    FakeExpectation user = restExpectation(
        server, QStringLiteral("current-user"), QByteArrayLiteral("GET"),
        QByteArrayLiteral("/xc2/1.0/auth/currentUser"), {}, cookie);
    user.actions.append(httpReply(
        QStringLiteral("current-user-response"), 200,
        QByteArrayLiteral("OK"), currentUser, jsonHeader));
    phases.append(
        orderedPhase(QStringLiteral("current-user"), std::move(user)));
    phases.append(orderedPhase(QStringLiteral("upgrade"),
                               upgradeExpectation(server, cookie)));

    FakeExpectation connect = stompExpectation(
        QStringLiteral("stomp-connect"), expectedConnectFrame());
    connect.actions.append(sendTopic(QStringLiteral("stomp-connected"),
                                     connectedFrame()));
    phases.append(
        orderedPhase(QStringLiteral("connect"), std::move(connect)));
    phases.append(orderedPhase(
        QStringLiteral("subscribe-vci"),
        stompExpectation(
            QStringLiteral("subscribe-vci"),
            expectedSubscription(
                Topic::VciStatus,
                QByteArrayLiteral("vci-status-subscription")))));
    phases.append(orderedPhase(
        QStringLiteral("subscribe-progress"),
        stompExpectation(
            QStringLiteral("subscribe-progress"),
            expectedSubscription(
                Topic::Progress,
                QByteArrayLiteral("progress-subscription")))));
    phases.append(orderedPhase(
        QStringLiteral("subscribe-login"),
        stompExpectation(
            QStringLiteral("subscribe-login"),
            expectedSubscription(Topic::Login,
                                 QByteArrayLiteral("login-subscription")))));

    FakeExpectation lookup = restExpectation(
        server, QStringLiteral("device-lookup"), QByteArrayLiteral("GET"),
        QByteArrayLiteral("/xc2/1.0/device/lookup"), {}, cookie);
    lookup.actions.append(sendTopic(
        QStringLiteral("progress-before-lookup-response"), progress));
    lookup.actions.append(httpReply(
        QStringLiteral("lookup-response"), 200, QByteArrayLiteral("OK"),
        QByteArrayLiteral("{\"jobID\":\"") + lookupId
            + QByteArrayLiteral("\"}"),
        jsonHeader));
    phases.append(
        orderedPhase(QStringLiteral("lookup"), std::move(lookup)));

    FakeExpectation devices = restExpectation(
        server, QStringLiteral("device-get"), QByteArrayLiteral("GET"),
        QByteArrayLiteral("/xc2/1.0/device/get"), {}, cookie);
    devices.actions.append(httpReply(
        QStringLiteral("devices-response"), 200, QByteArrayLiteral("OK"),
        compactDevices({device}), jsonHeader));
    phases.append(
        orderedPhase(QStringLiteral("devices"), std::move(devices)));

    FakePhase apply;
    apply.label = QStringLiteral("apply-and-selected");
    apply.unordered = true;
    apply.expectations = {
        restExpectation(
            server, QStringLiteral("device-apply"), QByteArrayLiteral("POST"),
            QByteArrayLiteral("/xc2/1.0/device/apply"),
            compactDevice(device), cookie),
        restExpectation(
            server, QStringLiteral("device-selected"),
            QByteArrayLiteral("GET"),
            QByteArrayLiteral("/xc2/1.0/device/getSelected"), {}, cookie),
    };
    apply.completionActions = {
        httpReply(QStringLiteral("apply-response"), 204,
                  QByteArrayLiteral("No Content"), {}, {},
                  QStringLiteral("device-apply")),
        sendTopic(QStringLiteral("status-before-selected"), status),
        httpReply(QStringLiteral("selected-response"), 200,
                  QByteArrayLiteral("OK"), compactDevice(device),
                  jsonHeader, QStringLiteral("device-selected")),
    };
    phases.append(std::move(apply));

    FakeExpectation close = restExpectation(
        server, QStringLiteral("device-close"), QByteArrayLiteral("POST"),
        QByteArrayLiteral("/xc2/1.0/device/close"), compactDevice(device),
        cookie);
    close.actions.append(httpReply(QStringLiteral("close-response"), 204,
                                   QByteArrayLiteral("No Content")));
    phases.append(orderedPhase(QStringLiteral("close"), std::move(close)));
    return phases;
}

QList<FakePhase> canceledLookupScript(
    const FakeXc2TransportServer &server, const Xc2VciDevice &device)
{
    QList<FakePhase> phases = completeHappyScript(server, device);
    phases.removeLast();
    phases.removeLast();
    phases[7].expectations[0].actions[0].stompFrame.body =
        progressBody(QStringLiteral("lookup-task4"), Xc2JobState::Canceled);
    return phases;
}

QList<FakePhase> selectionVariantScript(
    const FakeXc2TransportServer &server, const Xc2VciDevice &device,
    bool selectedBeforeStatus, double voltage)
{
    QList<FakePhase> phases = completeHappyScript(server, device);
    phases.removeLast();
    phases[9].completionActions[1].stompFrame.body =
        QJsonDocument(QJsonObject{
            {QStringLiteral("voltage"), voltage},
            {QStringLiteral("connected"), true},
        }).toJson(QJsonDocument::Compact);
    if (selectedBeforeStatus)
        phases[9].completionActions.swapItemsAt(1, 2);
    return phases;
}

QList<FakePhase> bootstrapOnlyScript(
    const FakeXc2TransportServer &server, const Xc2VciDevice &device)
{
    QList<FakePhase> phases = completeHappyScript(server, device);
    phases.resize(7);
    return phases;
}

QList<FakePhase> strictHealthOnlyScript(
    const FakeXc2TransportServer &server, bool respond)
{
    QList<FakePhase> phases = completeHappyScript(
        server, approvedDevice());
    phases.resize(1);
    if (!respond)
        phases[0].expectations[0].actions.clear();
    return phases;
}

QList<FakePhase> upgradeOnlyScript(
    const FakeXc2TransportServer &server,
    const QByteArray &cookie = QByteArrayLiteral("session=task4"))
{
    return {orderedPhase(QStringLiteral("upgrade"),
                         upgradeExpectation(server, cookie))};
}

QByteArray rawUpgradeRequest(const FakeXc2TransportServer &server,
                             const QByteArray &cookie,
                             const QString &fault = {})
{
    QByteArray request = QByteArrayLiteral(
        "GET /xc2-websocket HTTP/1.1\r\nHost: 127.0.0.1:")
        + QByteArray::number(server.port())
        + QByteArrayLiteral("\r\nUpgrade: websocket\r\n");
    if (fault != QStringLiteral("missing-connection")) {
        request += QByteArrayLiteral("Connection: Upgrade\r\n");
        if (fault == QStringLiteral("duplicate-connection"))
            request += QByteArrayLiteral("cOnNeCtIoN: Upgrade\r\n");
    }
    if (fault != QStringLiteral("missing-key")) {
        const QByteArray key = fault == QStringLiteral("malformed-key")
            ? QByteArrayLiteral("not-a-valid-websocket-key")
            : QByteArrayLiteral("dGhlIHNhbXBsZSBub25jZQ==");
        request += QByteArrayLiteral("Sec-WebSocket-Key: ") + key
            + QByteArrayLiteral("\r\n");
        if (fault == QStringLiteral("duplicate-key")) {
            request += QByteArrayLiteral(
                "sec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
        }
    }
    request += QByteArrayLiteral("Sec-WebSocket-Version: ")
        + (fault == QStringLiteral("wrong-version")
               ? QByteArrayLiteral("12") : QByteArrayLiteral("13"))
        + QByteArrayLiteral(
            "\r\nSec-WebSocket-Protocol: v12.stomp\r\nCookie: ")
        + cookie + QByteArrayLiteral("\r\n");
    if (fault == QStringLiteral("extra-header"))
        request += QByteArrayLiteral("Authorization: Bearer drift\r\n");
    request += QByteArrayLiteral("\r\n");
    return request;
}

FakeAction transportFailureAction(bool visibilityLoss)
{
    if (visibilityLoss) {
        FakeAction action;
        action.label = QStringLiteral("visibility-loss");
        action.type = FakeAction::Type::CloseWebSocket;
        action.target = FakeAction::Target::WebSocket;
        action.closeReason = QStringLiteral("visibility lost");
        return action;
    }
    return sendTopic(
        QStringLiteral("malformed-status"),
        topicMessage(
            Topic::VciStatus,
            QByteArrayLiteral("vci-status-subscription"),
            QByteArrayLiteral("malformed-status"),
            QByteArrayLiteral("{\"voltage\":12.5}")));
}

void installOwnedTransportBackendSeam(
    KtmSessionController &controller, Xc2BackendState &backendState,
    int &backendStartCount, int &backendStopCount)
{
    KtmSessionControllerTestAccess::Ops ops;
    ops.backendState = [&backendState] { return backendState; };
    ops.startBackend = [&backendStartCount](const QString &, Xc2Error *) {
        ++backendStartCount;
        return true;
    };
    ops.stopBackend = [&controller, &backendState, &backendStopCount] {
        ++backendStopCount;
        backendState = Xc2BackendState::Stopped;
        KtmSessionControllerTestAccess::emitBackendStateChanged(
            controller, Xc2BackendState::Stopped);
    };
    KtmSessionControllerTestAccess::installOps(controller, std::move(ops));
}

void publishOwnedTransportReady(KtmSessionController &controller,
                                Xc2BackendState &backendState,
                                const FakeXc2TransportServer &server)
{
    backendState = Xc2BackendState::Ready;
    KtmSessionControllerTestAccess::emitBackendStateChanged(
        controller, Xc2BackendState::Ready);
    KtmSessionControllerTestAccess::emitBackendReady(
        controller,
        {QUrl::fromEncoded(server.encodedRestBase()),
         QUrl(QStringLiteral("ws://127.0.0.1:%1/xc2-websocket")
                  .arg(server.port()))});
}

qsizetype traceIndex(const FakeXc2TransportServer &server,
                     const QString &label)
{
    for (qsizetype index = 0; index < server.trace().size(); ++index) {
        if (server.trace().at(index).label == label)
            return index;
    }
    return -1;
}

int traceCount(const FakeXc2TransportServer &server,
               const QString &label,
               FakeXc2TransportServer::TraceEvent::Direction direction)
{
    return int(std::count_if(
        server.trace().cbegin(), server.trace().cend(),
        [&label, direction](
            const FakeXc2TransportServer::TraceEvent &event) {
            return event.label == label && event.direction == direction;
        }));
}

void completeReadyApply(
    BootstrapHarness &harness,
    Xc2VciDevice confirmed = approvedDevice())
{
    QVERIFY(harness.controller.applyVci(approvedDevice()));
    KtmSessionControllerTestAccess::applyDeviceFinished(
        harness.controller, harness.applyId);
    KtmSessionControllerTestAccess::selectedDeviceFinished(
        harness.controller, harness.selectedId,
        Xc2Result<Xc2SelectedVci>::success({confirmed}));
    KtmSessionControllerTestAccess::stompMessage(
        harness.controller, vciStatusMessage(harness.generation, true, 12.4));
    QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
             KtmSessionState::VciReady);
}

} // namespace

class KtmSessionControllerTest final : public QObject {
    Q_OBJECT

private slots:
    void sameAuthorityCanceledLookupStillFetchesDevicesOnce()
    {
        FakeXc2TransportServer server;
        const Xc2VciDevice device = approvedDevice();
        QString scriptError;
        QVERIFY2(server.setScript(canceledLookupScript(server, device),
                                  &scriptError),
                 qPrintable(scriptError));
        KtmSessionController controller;
        Xc2BackendState backendState = Xc2BackendState::Stopped;
        int starts = 0;
        int stops = 0;
        installOwnedTransportBackendSeam(controller, backendState,
                                         starts, stops);
        QSignalSpy lookupFinished(
            &controller, &KtmSessionController::vciLookupFinished);
        QVERIFY(controller.startProduction(QStringLiteral("C:/XC2")));
        publishOwnedTransportReady(controller, backendState, server);
        QTRY_COMPARE_WITH_TIMEOUT(
            KtmSessionControllerTestAccess::state(controller),
            KtmSessionState::SessionReady, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(
            traceIndex(server, QStringLiteral("subscribe-login")) >= 0,
            3000);
        QVERIFY(controller.lookupVci());
        QTRY_COMPARE_WITH_TIMEOUT(lookupFinished.count(), 1, 3000);
        int deviceGetCount = 0;
        for (const auto &event : server.trace()) {
            if (event.label == QStringLiteral("device-get"))
                ++deviceGetCount;
        }
        QCOMPARE(deviceGetCount, 1);
        const qsizetype progress = traceIndex(
            server, QStringLiteral("progress-before-lookup-response"));
        QVERIFY(progress >= 0);
        QVERIFY(server.trace().at(progress).stompFrame.body.contains(
            QByteArrayLiteral("\"status\":\"CANCELED\"")));
        QVERIFY(server.scriptExhausted());
        QCOMPARE(server.unexpectedOperationCount(), 0);
        controller.stop();
        QCOMPARE(stops, 1);
    }

    void sameAuthoritySelectionOrderingAndVoltage_data()
    {
        QTest::addColumn<bool>("selectedBeforeStatus");
        QTest::addColumn<double>("voltage");
        QTest::newRow("selected-before-status") << true << 12.4;
        QTest::newRow("zero-voltage-status-before-selected")
            << false << 0.0;
    }

    void sameAuthoritySelectionOrderingAndVoltage()
    {
        QFETCH(bool, selectedBeforeStatus);
        QFETCH(double, voltage);
        FakeXc2TransportServer server;
        const Xc2VciDevice device = approvedDevice();
        QString scriptError;
        QVERIFY2(server.setScript(selectionVariantScript(
                     server, device, selectedBeforeStatus, voltage),
                                  &scriptError),
                 qPrintable(scriptError));
        KtmSessionController controller;
        Xc2BackendState backendState = Xc2BackendState::Stopped;
        int starts = 0;
        int stops = 0;
        installOwnedTransportBackendSeam(controller, backendState,
                                         starts, stops);
        QSignalSpy lookupFinished(
            &controller, &KtmSessionController::vciLookupFinished);
        QSignalSpy ready(&controller, &KtmSessionController::vciReady);
        QVERIFY(controller.startProduction(QStringLiteral("C:/XC2")));
        publishOwnedTransportReady(controller, backendState, server);
        QTRY_VERIFY_WITH_TIMEOUT(
            traceIndex(server, QStringLiteral("subscribe-login")) >= 0,
            3000);
        QVERIFY(controller.lookupVci());
        QTRY_COMPARE_WITH_TIMEOUT(lookupFinished.count(), 1, 3000);
        QVERIFY(controller.applyVci(device));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
        QCOMPARE(KtmSessionControllerTestAccess::state(controller),
                 KtmSessionState::VciReady);
        const Xc2VciStatus status = qvariant_cast<Xc2VciStatus>(
            ready.constFirst().at(1));
        QVERIFY(status.connected);
        QCOMPARE(status.voltage, voltage);
        const qsizetype statusEvent =
            traceIndex(server, QStringLiteral("status-before-selected"));
        const qsizetype selectedEvent =
            traceIndex(server, QStringLiteral("selected-response"));
        QVERIFY(statusEvent >= 0);
        QVERIFY(selectedEvent >= 0);
        QCOMPARE(selectedEvent < statusEvent, selectedBeforeStatus);
        QVERIFY(server.scriptExhausted());
        QCOMPARE(server.unexpectedOperationCount(), 0);
        controller.stop();
        QCOMPARE(stops, 1);
    }

    void sameAuthorityInvalidIdentityWritesNoPost_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("provider");
        QTest::newRow("blank-id")
            << QStringLiteral("   ")
            << QStringLiteral("AVL Ditest VCI2K_DPDU_API");
        QTest::newRow("wrong-provider")
            << QStringLiteral("vci-1")
            << QStringLiteral("AVL Ditest VCI2K_dpdu_api");
    }

    void sameAuthorityInvalidIdentityWritesNoPost()
    {
        QFETCH(QString, id);
        QFETCH(QString, provider);
        FakeXc2TransportServer server;
        Xc2VciDevice device = approvedDevice();
        QString scriptError;
        QVERIFY2(server.setScript(bootstrapOnlyScript(server, device),
                                  &scriptError),
                 qPrintable(scriptError));
        KtmSessionController controller;
        Xc2BackendState backendState = Xc2BackendState::Stopped;
        int starts = 0;
        int stops = 0;
        installOwnedTransportBackendSeam(controller, backendState,
                                         starts, stops);
        QVERIFY(controller.startProduction(QStringLiteral("C:/XC2")));
        publishOwnedTransportReady(controller, backendState, server);
        QTRY_VERIFY_WITH_TIMEOUT(
            traceIndex(server, QStringLiteral("subscribe-login")) >= 0,
            3000);
        device.id = id;
        device.internalName = provider;
        QVERIFY(!controller.applyVci(device));
        QCOMPARE(KtmSessionControllerTestAccess::state(controller),
                 KtmSessionState::SessionReady);
        QCOMPARE(server.stateChangingRestRequestCount(), 0);
        QCOMPARE(traceIndex(server, QStringLiteral("device-apply")),
                 qsizetype(-1));
        QVERIFY(server.scriptExhausted());
        QCOMPARE(server.unexpectedOperationCount(), 0);
        controller.stop();
        QCOMPARE(stops, 1);
    }

    void sameAuthorityTransportFailureDoesNotRetry_data()
    {
        QTest::addColumn<bool>("visibilityLoss");
        QTest::newRow("malformed-status") << false;
        QTest::newRow("visibility-loss") << true;
    }

    void sameAuthorityTransportFailureDoesNotRetry()
    {
        QFETCH(bool, visibilityLoss);
        FakeXc2TransportServer server;
        const Xc2VciDevice device = approvedDevice();
        QString scriptError;
        QVERIFY2(server.setScript(selectionVariantScript(
                     server, device, false, 12.4),
                                  &scriptError),
                 qPrintable(scriptError));
        KtmSessionController controller;
        Xc2BackendState backendState = Xc2BackendState::Stopped;
        int starts = 0;
        int stops = 0;
        installOwnedTransportBackendSeam(controller, backendState,
                                         starts, stops);
        QSignalSpy lookupFinished(
            &controller, &KtmSessionController::vciLookupFinished);
        QSignalSpy ready(&controller, &KtmSessionController::vciReady);
        QVERIFY(controller.startProduction(QStringLiteral("C:/XC2")));
        publishOwnedTransportReady(controller, backendState, server);
        QTRY_VERIFY_WITH_TIMEOUT(
            traceIndex(server, QStringLiteral("subscribe-login")) >= 0,
            3000);
        QVERIFY(controller.lookupVci());
        QTRY_COMPARE_WITH_TIMEOUT(lookupFinished.count(), 1, 3000);
        QVERIFY(controller.applyVci(device));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
        QCOMPARE(KtmSessionControllerTestAccess::state(controller),
                 KtmSessionState::VciReady);
        QVERIFY(server.scriptExhausted());
        const int restBaseline = server.restRequestCount();
        const int upgradeBaseline = server.upgradeRequestCount();
        QVERIFY(server.performAction(transportFailureAction(visibilityLoss)));
        QTRY_COMPARE_WITH_TIMEOUT(
            KtmSessionControllerTestAccess::state(controller),
            KtmSessionState::Failed, 3000);
        QTest::qWait(250);
        QCOMPARE(server.restRequestCount(), restBaseline);
        QCOMPARE(server.upgradeRequestCount(), upgradeBaseline);
        QCOMPARE(server.webSocketConnectionCount(), 1);
        QCOMPARE(server.unexpectedOperationCount(), 0);
        controller.stop();
        QCOMPARE(stops, 1);
        QTRY_COMPARE_WITH_TIMEOUT(server.openConnectionCount(), 0, 3000);
    }

    void sameAuthorityConnectedFalseRevokesWithoutRetry()
    {
        FakeXc2TransportServer server;
        const Xc2VciDevice device = approvedDevice();
        QString scriptError;
        QVERIFY2(server.setScript(selectionVariantScript(
                     server, device, false, 12.4), &scriptError),
                 qPrintable(scriptError));
        KtmSessionController controller;
        Xc2BackendState backendState = Xc2BackendState::Stopped;
        int starts = 0;
        int stops = 0;
        installOwnedTransportBackendSeam(controller, backendState,
                                         starts, stops);
        QSignalSpy lookupFinished(
            &controller, &KtmSessionController::vciLookupFinished);
        QSignalSpy ready(&controller, &KtmSessionController::vciReady);
        QSignalSpy revoked(
            &controller, &KtmSessionController::vciReadinessRevoked);
        QSignalSpy statusChanged(
            &controller, &KtmSessionController::vciStatusChanged);
        QVERIFY(controller.startProduction(QStringLiteral("C:/XC2")));
        publishOwnedTransportReady(controller, backendState, server);
        QTRY_VERIFY_WITH_TIMEOUT(
            traceIndex(server, QStringLiteral("subscribe-login")) >= 0,
            3000);
        QVERIFY(controller.lookupVci());
        QTRY_COMPARE_WITH_TIMEOUT(lookupFinished.count(), 1, 3000);
        QVERIFY(controller.applyVci(device));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
        QCOMPARE(KtmSessionControllerTestAccess::state(controller),
                 KtmSessionState::VciReady);

        const int restBaseline = server.restRequestCount();
        const int upgradeBaseline = server.upgradeRequestCount();
        const int stateChangingBaseline =
            server.stateChangingRestRequestCount();
        QVERIFY(server.performAction(sendTopic(
            QStringLiteral("current-connected-false"),
            topicMessage(
                Topic::VciStatus,
                QByteArrayLiteral("vci-status-subscription"),
                QByteArrayLiteral("status-current-disconnected"),
                QByteArrayLiteral(
                    "{\"voltage\":0.0,\"connected\":false}")))));
        QTRY_COMPARE_WITH_TIMEOUT(revoked.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(statusChanged.count(), 2, 3000);
        QCOMPARE(KtmSessionControllerTestAccess::state(controller),
                 KtmSessionState::SessionReady);
        QVERIFY(!controller.closeVci());
        QTest::qWait(250);
        QCOMPARE(server.restRequestCount(), restBaseline);
        QCOMPARE(server.upgradeRequestCount(), upgradeBaseline);
        QCOMPARE(server.webSocketConnectionCount(), 1);
        QCOMPARE(server.stateChangingRestRequestCount(),
                 stateChangingBaseline);
        QVERIFY(server.scriptExhausted());
        QCOMPARE(server.unexpectedOperationCount(), 0);
        controller.stop();
        QCOMPARE(stops, 1);
        QTRY_COMPARE_WITH_TIMEOUT(server.liveSocketObjectCount(), 0, 3000);
        QCOMPARE(server.unexpectedOperationCount(), 0);
    }

    void sameAuthorityOldSocketDelayedActionIsCanceledAcrossRestart()
    {
        FakeXc2TransportServer server;
        const Xc2VciDevice device = approvedDevice();
        QString scriptError;
        QVERIFY2(server.setScript(bootstrapOnlyScript(server, device),
                                  &scriptError),
                 qPrintable(scriptError));
        KtmSessionController controller;
        Xc2BackendState backendState = Xc2BackendState::Stopped;
        int starts = 0;
        int stops = 0;
        installOwnedTransportBackendSeam(controller, backendState,
                                         starts, stops);
        QVERIFY(controller.startProduction(QStringLiteral("C:/XC2")));
        publishOwnedTransportReady(controller, backendState, server);
        QTRY_COMPARE_WITH_TIMEOUT(
            KtmSessionControllerTestAccess::state(controller),
            KtmSessionState::SessionReady, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(
            traceIndex(server, QStringLiteral("subscribe-login")) >= 0,
            3000);
        const quint64 oldEpoch =
            KtmSessionControllerTestAccess::sessionEpoch(controller);
        QPointer<Xc2StompClient> oldStomp =
            KtmSessionControllerTestAccess::stompGuard(controller);
        QVERIFY(oldStomp);
        quint64 oldConnectionId = 0;
        for (const auto &event : server.trace()) {
            if (event.label == QStringLiteral("stomp-connect")) {
                oldConnectionId = event.connectionId;
                break;
            }
        }
        QVERIFY(oldConnectionId != 0);

        QVERIFY(server.performAction(sendTopic(
            QStringLiteral("old-generation-delayed-status"),
            topicMessage(
                Topic::VciStatus,
                QByteArrayLiteral("vci-status-subscription"),
                QByteArrayLiteral("old-generation-message"),
                QByteArrayLiteral(
                    "{\"voltage\":99.9,\"connected\":true}")),
            500)));
        QCOMPARE(server.pendingActionCount(), 1);
        controller.stop();
        QTRY_COMPARE_WITH_TIMEOUT(
            KtmSessionControllerTestAccess::state(controller),
            KtmSessionState::Stopped, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(server.pendingActionCount(), 0, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(server.canceledActionCount(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(server.liveSocketObjectCount(), 0, 3000);
        QVERIFY(oldStomp.isNull());
        QCOMPARE(traceCount(
                     server,
                     QStringLiteral("old-generation-delayed-status"),
                     FakeXc2TransportServer::TraceEvent::Direction::Canceled),
                 1);
        QCOMPARE(traceCount(
                     server,
                     QStringLiteral("old-generation-delayed-status"),
                     FakeXc2TransportServer::TraceEvent::Direction::Sent),
                 0);
        const auto canceled = std::find_if(
            server.trace().cbegin(), server.trace().cend(),
            [](const FakeXc2TransportServer::TraceEvent &event) {
                return event.label
                        == QStringLiteral(
                            "old-generation-delayed-status")
                    && event.direction
                        == FakeXc2TransportServer::TraceEvent::Direction::Canceled;
            });
        QVERIFY(canceled != server.trace().cend());
        QCOMPARE(canceled->connectionId, oldConnectionId);

        QVERIFY2(server.setScript(bootstrapOnlyScript(server, device),
                                  &scriptError),
                 qPrintable(scriptError));
        QVERIFY(controller.startProduction(QStringLiteral("C:/XC2")));
        publishOwnedTransportReady(controller, backendState, server);
        QTRY_COMPARE_WITH_TIMEOUT(
            KtmSessionControllerTestAccess::state(controller),
            KtmSessionState::SessionReady, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(
            traceCount(
                server, QStringLiteral("subscribe-login"),
                FakeXc2TransportServer::TraceEvent::Direction::Received),
            2, 3000);
        QVERIFY(KtmSessionControllerTestAccess::sessionEpoch(controller)
                > oldEpoch);
        QVERIFY(KtmSessionControllerTestAccess::stompGuard(controller));
        quint64 newConnectionId = 0;
        for (auto it = server.trace().crbegin();
             it != server.trace().crend(); ++it) {
            if (it->label == QStringLiteral("stomp-connect")) {
                newConnectionId = it->connectionId;
                break;
            }
        }
        QVERIFY(newConnectionId != 0);
        QVERIFY(newConnectionId != oldConnectionId);
        QTest::qWait(600);
        QCOMPARE(traceCount(
                     server,
                     QStringLiteral("old-generation-delayed-status"),
                     FakeXc2TransportServer::TraceEvent::Direction::Sent),
                 0);
        QCOMPARE(server.pendingActionCount(), 0);
        QCOMPARE(server.canceledActionCount(), 1);
        QCOMPARE(server.unexpectedOperationCount(), 0);
        QVERIFY(server.scriptExhausted());
        controller.stop();
        QCOMPARE(starts, 2);
        QCOMPARE(stops, 2);
        QTRY_COMPARE_WITH_TIMEOUT(server.liveSocketObjectCount(), 0, 3000);
        QCOMPARE(server.unexpectedOperationCount(), 0);
    }

    void sameAuthorityStrictHttpFraming_data()
    {
        QTest::addColumn<QString>("fault");
        QTest::newRow("duplicate-content-length")
            << QStringLiteral("duplicate-content-length");
        QTest::newRow("conflicting-content-length")
            << QStringLiteral("conflicting-content-length");
        QTest::newRow("transfer-encoding")
            << QStringLiteral("transfer-encoding");
        QTest::newRow("http-1.0") << QStringLiteral("http-1.0");
        QTest::newRow("get-body") << QStringLiteral("get-body");
        QTest::newRow("duplicate-host")
            << QStringLiteral("duplicate-host");
        QTest::newRow("duplicate-cookie")
            << QStringLiteral("duplicate-cookie");
        QTest::newRow("duplicate-content-type")
            << QStringLiteral("duplicate-content-type");
        QTest::newRow("wrong-content-type")
            << QStringLiteral("wrong-content-type");
        QTest::newRow("initial-tail")
            << QStringLiteral("initial-tail");
        QTest::newRow("extra-header")
            << QStringLiteral("extra-header");
        QTest::newRow("duplicate-mixed-case-header")
            << QStringLiteral("duplicate-mixed-case-header");
        QTest::newRow("case-changed-value")
            << QStringLiteral("case-changed-value");
    }

    void sameAuthorityStrictHttpFraming()
    {
        QFETCH(QString, fault);
        FakeXc2TransportServer server;
        QString scriptError;
        QVERIFY2(server.setScript(strictHealthOnlyScript(server, false),
                                  &scriptError),
                 qPrintable(scriptError));

        const QByteArray version = fault == QStringLiteral("http-1.0")
            ? QByteArrayLiteral("HTTP/1.0")
            : QByteArrayLiteral("HTTP/1.1");
        QByteArray request = QByteArrayLiteral(
            "GET /xc2/1.0/serviceStatus/status ") + version
            + QByteArrayLiteral("\r\nHost: 127.0.0.1:")
            + QByteArray::number(server.port())
            + QByteArrayLiteral("\r\n");
        if (fault == QStringLiteral("duplicate-host")) {
            request += QByteArrayLiteral("Host: 127.0.0.1:")
                + QByteArray::number(server.port())
                + QByteArrayLiteral("\r\n");
        } else if (fault == QStringLiteral("duplicate-cookie")) {
            request += QByteArrayLiteral(
                "Cookie: session=one\r\nCookie: session=two\r\n");
        } else if (fault == QStringLiteral("duplicate-content-type")) {
            request += QByteArrayLiteral(
                "Content-Type: application/json\r\n"
                "Content-Type: application/json\r\n");
        } else if (fault == QStringLiteral("wrong-content-type")) {
            request += QByteArrayLiteral("Content-Type: text/plain\r\n");
        } else if (fault == QStringLiteral("duplicate-content-length")) {
            request += QByteArrayLiteral(
                "Content-Length: 0\r\nContent-Length: 0\r\n");
        } else if (fault
                   == QStringLiteral("conflicting-content-length")) {
            request += QByteArrayLiteral(
                "Content-Length: 0\r\nContent-Length: 1\r\n");
        } else if (fault == QStringLiteral("transfer-encoding")) {
            request += QByteArrayLiteral("Transfer-Encoding: chunked\r\n");
        } else if (fault == QStringLiteral("get-body")) {
            request += QByteArrayLiteral("Content-Length: 1\r\n");
        }
        request += QByteArrayLiteral("Accept: */*\r\n");
        if (fault == QStringLiteral("duplicate-mixed-case-header"))
            request += QByteArrayLiteral("aCcEpT: */*\r\n");
        if (fault == QStringLiteral("extra-header"))
            request += QByteArrayLiteral("X-Retry: 1\r\n");
        request += QByteArrayLiteral(
            "Accept-Encoding: identity\r\nConnection: ");
        request += fault == QStringLiteral("case-changed-value")
            ? QByteArrayLiteral("Close\r\n\r\n")
            : QByteArrayLiteral("close\r\n\r\n");
        if (fault == QStringLiteral("get-body"))
            request += 'x';
        if (fault == QStringLiteral("initial-tail"))
            request += QByteArrayLiteral("GET /pipelined HTTP/1.1\r\n\r\n");

        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, server.port());
        QVERIFY(socket.waitForConnected(1000));
        QCOMPARE(socket.write(request), qint64(request.size()));
        socket.flush();
        QTRY_COMPARE_WITH_TIMEOUT(server.unexpectedOperationCount(), 1,
                                  3000);
        QTest::qWait(25);
        QCOMPARE(server.unexpectedOperationCount(), 1);
        socket.abort();
        QTRY_COMPARE_WITH_TIMEOUT(server.liveSocketObjectCount(), 0, 3000);
        QVERIFY(!server.scriptExhausted());
    }

    void sameAuthorityStrictHttpRejectsPrematureEof()
    {
        FakeXc2TransportServer server;
        QString scriptError;
        QVERIFY2(server.setScript(strictHealthOnlyScript(server, false),
                                  &scriptError),
                 qPrintable(scriptError));
        const QByteArray request = QByteArrayLiteral(
            "GET /xc2/1.0/serviceStatus/status HTTP/1.1\r\n"
            "Host: 127.0.0.1:") + QByteArray::number(server.port())
            + QByteArrayLiteral(
                "\r\nContent-Length: 5\r\nAccept: */*\r\n"
                "Accept-Encoding: identity\r\nConnection: close\r\n\r\nx");
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, server.port());
        QVERIFY(socket.waitForConnected(1000));
        QCOMPARE(socket.write(request), qint64(request.size()));
        QVERIFY(socket.waitForBytesWritten(1000));
        socket.disconnectFromHost();
        if (socket.state() != QAbstractSocket::UnconnectedState)
            QVERIFY(socket.waitForDisconnected(1000));
        QTRY_COMPARE_WITH_TIMEOUT(server.unexpectedOperationCount(), 1,
                                  3000);
        QCOMPARE(traceCount(
                     server, QStringLiteral("invalid-http-eof"),
                     FakeXc2TransportServer::TraceEvent::Direction::Received),
                 1);
        QTRY_COMPARE_WITH_TIMEOUT(server.liveSocketObjectCount(), 0, 3000);
        QVERIFY(!server.scriptExhausted());
    }

    void sameAuthorityStrictUpgradeHeaders_data()
    {
        QTest::addColumn<QString>("fault");
        QTest::newRow("extra-header") << QStringLiteral("extra-header");
        QTest::newRow("missing-key") << QStringLiteral("missing-key");
        QTest::newRow("malformed-key")
            << QStringLiteral("malformed-key");
        QTest::newRow("duplicate-key")
            << QStringLiteral("duplicate-key");
        QTest::newRow("missing-connection")
            << QStringLiteral("missing-connection");
        QTest::newRow("duplicate-connection")
            << QStringLiteral("duplicate-connection");
        QTest::newRow("wrong-version")
            << QStringLiteral("wrong-version");
    }

    void sameAuthorityStrictUpgradeHeaders()
    {
        QFETCH(QString, fault);
        FakeXc2TransportServer server;
        const QByteArray cookie = QByteArrayLiteral("session=task4");
        QString scriptError;
        QVERIFY2(server.setScript(upgradeOnlyScript(server, cookie),
                                  &scriptError),
                 qPrintable(scriptError));
        const QByteArray request = rawUpgradeRequest(server, cookie, fault);
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, server.port());
        QVERIFY(socket.waitForConnected(1000));
        QCOMPARE(socket.write(request), qint64(request.size()));
        socket.flush();
        QTRY_COMPARE_WITH_TIMEOUT(server.unexpectedOperationCount(), 1,
                                  3000);
        QCOMPARE(server.pendingUpgradeConnectionCount(), 0);
        QCOMPARE(server.webSocketConnectionCount(), 0);
        QVERIFY(!server.scriptExhausted());
        socket.abort();
        QTRY_COMPARE_WITH_TIMEOUT(server.liveSocketObjectCount(), 0, 3000);
    }

    void sameAuthorityUpgradeCaptureMayAbortExactSocket()
    {
        FakeXc2TransportServer server;
        const QByteArray cookie = QByteArrayLiteral("session=task4");
        QString scriptError;
        QVERIFY2(server.setScript(upgradeOnlyScript(server, cookie),
                                  &scriptError),
                 qPrintable(scriptError));
        bool closeScheduled = false;
        connect(&server, &FakeXc2TransportServer::upgradeRequestCaptured,
                &server, [&server, &closeScheduled] {
            FakeAction close;
            close.label = QStringLiteral("capture-close-exact-upgrade");
            close.type = FakeAction::Type::CloseHttp;
            close.target = FakeAction::Target::ExpectationSocket;
            close.targetExpectation = QStringLiteral("websocket-upgrade");
            closeScheduled = server.performAction(close);
        });
        const QByteArray request = rawUpgradeRequest(server, cookie);
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, server.port());
        QVERIFY(socket.waitForConnected(1000));
        QCOMPARE(socket.write(request), qint64(request.size()));
        socket.flush();
        QTRY_VERIFY_WITH_TIMEOUT(closeScheduled, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(server.liveSocketObjectCount(), 0, 3000);
        QCOMPARE(server.webSocketConnectionCount(), 0);
        QCOMPARE(server.pendingUpgradeConnectionCount(), 0);
        QCOMPARE(server.pendingActionCount(), 0);
        QCOMPARE(server.canceledActionCount(), 0);
        QCOMPARE(server.unexpectedOperationCount(), 0);
        QVERIFY(server.scriptExhausted());
        const qsizetype captured = traceIndex(
            server, QStringLiteral("websocket-upgrade"));
        const qsizetype closed = traceIndex(
            server, QStringLiteral("capture-close-exact-upgrade"));
        QVERIFY(captured >= 0);
        QVERIFY(closed > captured);
    }

    void sameAuthorityStompActionNeverFallsForwardToNewSocket()
    {
        FakeXc2TransportServer server;
        const QByteArray cookie = QByteArrayLiteral("session=task4");
        QString scriptError;
        QVERIFY2(server.setScript(upgradeOnlyScript(server, cookie),
                                  &scriptError),
                 qPrintable(scriptError));
        const QUrl url(QStringLiteral("ws://127.0.0.1:%1/xc2-websocket")
                           .arg(server.port()));
        QWebSocketHandshakeOptions handshake;
        handshake.setSubprotocols({QStringLiteral("v12.stomp")});

        QWebSocket oldClient;
        oldClient.setProxy(QNetworkProxy::NoProxy);
        QNetworkRequest oldRequest(url);
        oldRequest.setRawHeader(QByteArrayLiteral("Cookie"), cookie);
        QSignalSpy oldConnected(&oldClient, &QWebSocket::connected);
        oldClient.open(oldRequest, handshake);
        QTRY_COMPARE_WITH_TIMEOUT(oldConnected.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(server.webSocketConnectionCount(), 1,
                                  3000);
        QVERIFY(server.scriptExhausted());
        quint64 oldConnectionId = 0;
        for (const auto &event : server.trace()) {
            if (event.label == QStringLiteral("websocket-upgrade")) {
                oldConnectionId = event.connectionId;
                break;
            }
        }
        QVERIFY(oldConnectionId != 0);

        QVERIFY2(server.setScript(upgradeOnlyScript(server, cookie),
                                  &scriptError),
                 qPrintable(scriptError));
        QWebSocket newClient;
        newClient.setProxy(QNetworkProxy::NoProxy);
        QNetworkRequest newRequest(url);
        newRequest.setRawHeader(QByteArrayLiteral("Cookie"), cookie);
        QSignalSpy newConnected(&newClient, &QWebSocket::connected);
        QSignalSpy newMessages(&newClient, &QWebSocket::textMessageReceived);
        newClient.open(newRequest, handshake);
        QTRY_COMPARE_WITH_TIMEOUT(newConnected.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(server.webSocketConnectionCount(), 2,
                                  3000);
        QVERIFY(server.scriptExhausted());

        Xc2StompFrame trigger;
        trigger.command = QByteArrayLiteral("SEND");
        trigger.headers.insert(QByteArrayLiteral("destination"),
                               QByteArrayLiteral("/task4/reentrant"));
        trigger.body = QByteArrayLiteral("old-event");
        Xc2StompFrame marker;
        marker.command = QByteArrayLiteral("MESSAGE");
        marker.headers.insert(QByteArrayLiteral("destination"),
                              QByteArrayLiteral("/task4/marker"));
        marker.body = QByteArrayLiteral("must-not-reach-new-socket");
        FakeAction markerAction;
        markerAction.label = QStringLiteral("old-event-marker");
        markerAction.type = FakeAction::Type::SendStompFrame;
        markerAction.target = FakeAction::Target::WebSocket;
        markerAction.stompFrame = marker;
        FakeExpectation triggerExpectation = stompExpectation(
            QStringLiteral("old-event-trigger"), trigger, {markerAction});
        QVERIFY2(server.setScript(
                     {orderedPhase(QStringLiteral("old-event"),
                                   std::move(triggerExpectation))},
                     &scriptError),
                 qPrintable(scriptError));

        bool oldServerSocketDestroyedInCapture = false;
        connect(&server, &FakeXc2TransportServer::stompFrameReceived,
                &server, [&] {
            const int disconnections = server.webSocketDisconnectionCount();
            oldClient.abort();
            QElapsedTimer wait;
            wait.start();
            while (server.webSocketDisconnectionCount() == disconnections
                   && wait.elapsed() < 1000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            }
            for (int attempt = 0; attempt < 4; ++attempt) {
                QCoreApplication::sendPostedEvents(
                    nullptr, QEvent::DeferredDelete);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            }
            oldServerSocketDestroyedInCapture =
                server.liveSocketObjectCount() == 1;
        }, Qt::DirectConnection);

        const QByteArray encoded = Xc2StompCodec::encode(trigger);
        QCOMPARE(oldClient.sendTextMessage(QString::fromUtf8(encoded)),
                 qint64(encoded.size()));
        QTRY_VERIFY_WITH_TIMEOUT(server.scriptExhausted(), 3000);
        QVERIFY(oldServerSocketDestroyedInCapture);
        QTest::qWait(100);
        QCOMPARE(newMessages.count(), 0);
        QCOMPARE(traceCount(
                     server, QStringLiteral("old-event-marker"),
                     FakeXc2TransportServer::TraceEvent::Direction::Sent),
                 0);
        QCOMPARE(traceCount(
                     server, QStringLiteral("old-event-marker"),
                     FakeXc2TransportServer::TraceEvent::Direction::Canceled),
                 1);
        const auto canceled = std::find_if(
            server.trace().cbegin(), server.trace().cend(),
            [](const FakeXc2TransportServer::TraceEvent &event) {
                return event.label == QStringLiteral("old-event-marker")
                    && event.direction
                        == FakeXc2TransportServer::TraceEvent::Direction::Canceled;
            });
        QVERIFY(canceled != server.trace().cend());
        QCOMPARE(canceled->connectionId, oldConnectionId);
        QCOMPARE(server.pendingActionCount(), 0);
        QCOMPARE(server.unexpectedOperationCount(), 0);
        newClient.abort();
        QTRY_COMPARE_WITH_TIMEOUT(server.liveSocketObjectCount(), 0, 3000);
    }

    void sameAuthorityStrictHttpRejectsLateTail()
    {
        FakeXc2TransportServer server;
        QString scriptError;
        QVERIFY2(server.setScript(strictHealthOnlyScript(server, false),
                                  &scriptError),
                 qPrintable(scriptError));
        const QByteArray request = QByteArrayLiteral(
            "GET /xc2/1.0/serviceStatus/status HTTP/1.1\r\n"
            "Host: 127.0.0.1:") + QByteArray::number(server.port())
            + QByteArrayLiteral(
                "\r\nAccept: */*\r\nAccept-Encoding: identity\r\n"
                "Connection: close\r\n\r\n");
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, server.port());
        QVERIFY(socket.waitForConnected(1000));
        QCOMPARE(socket.write(request), qint64(request.size()));
        socket.flush();
        QTRY_VERIFY_WITH_TIMEOUT(server.scriptExhausted(), 3000);
        QCOMPARE(server.unexpectedOperationCount(), 0);
        QCOMPARE(socket.write(QByteArrayLiteral("late-tail")), qint64(9));
        socket.flush();
        QTRY_COMPARE_WITH_TIMEOUT(server.unexpectedOperationCount(), 1,
                                  3000);
        QCOMPARE(traceCount(
                     server, QStringLiteral("invalid-http-tail"),
                     FakeXc2TransportServer::TraceEvent::Direction::Received),
                 1);
        socket.abort();
        QTRY_COMPARE_WITH_TIMEOUT(server.liveSocketObjectCount(), 0, 3000);
    }

    void sameAuthorityCaptureSignalMayCloseExactSocket()
    {
        FakeXc2TransportServer server;
        QString scriptError;
        QVERIFY2(server.setScript(strictHealthOnlyScript(server, false),
                                  &scriptError),
                 qPrintable(scriptError));
        bool closeScheduled = false;
        connect(&server, &FakeXc2TransportServer::restRequestCaptured,
                &server, [&server, &closeScheduled] {
            FakeAction close;
            close.label = QStringLiteral("capture-close-exact-http");
            close.type = FakeAction::Type::CloseHttp;
            close.target = FakeAction::Target::ExpectationSocket;
            close.targetExpectation = QStringLiteral("health");
            closeScheduled = server.performAction(close);
        });
        const QByteArray request = QByteArrayLiteral(
            "GET /xc2/1.0/serviceStatus/status HTTP/1.1\r\n"
            "Host: 127.0.0.1:") + QByteArray::number(server.port())
            + QByteArrayLiteral(
                "\r\nAccept: */*\r\nAccept-Encoding: identity\r\n"
                "Connection: close\r\n\r\n");
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, server.port());
        QVERIFY(socket.waitForConnected(1000));
        QCOMPARE(socket.write(request), qint64(request.size()));
        socket.flush();
        QTRY_VERIFY_WITH_TIMEOUT(closeScheduled, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(server.liveSocketObjectCount(), 0, 3000);
        const qsizetype captured = traceIndex(
            server, QStringLiteral("health"));
        const qsizetype closed = traceIndex(
            server, QStringLiteral("capture-close-exact-http"));
        QVERIFY(captured >= 0);
        QVERIFY(closed > captured);
        QVERIFY(server.scriptExhausted());
        QCOMPARE(server.pendingActionCount(), 0);
        QCOMPARE(server.canceledActionCount(), 0);
        QCOMPARE(server.unexpectedOperationCount(), 0);
    }

    void sameAuthorityFakeDrivesOwnedHappyFlow()
    {
        FakeXc2TransportServer server;
        QVERIFY(server.isListening());
        const Xc2VciDevice device = approvedDevice(
            QStringLiteral("Task 4 VCI"), QStringLiteral("module-task4"));
        QString scriptError;
        QVERIFY2(server.setScript(completeHappyScript(server, device),
                                  &scriptError),
                 qPrintable(scriptError));

        auto controllerOwner = std::make_unique<KtmSessionController>();
        KtmSessionController &controller = *controllerOwner;
        QPointer<KtmSessionController> controllerGuard(
            controllerOwner.get());
        Xc2BackendState backendState = Xc2BackendState::Stopped;
        int backendStartCount = 0;
        int backendStopCount = 0;
        KtmSessionControllerTestAccess::Ops ops;
        ops.backendState = [&backendState] { return backendState; };
        ops.startBackend = [&backendStartCount](const QString &, Xc2Error *) {
            ++backendStartCount;
            return true;
        };
        ops.stopBackend = [&controller, &backendState, &backendStopCount] {
            ++backendStopCount;
            backendState = Xc2BackendState::Stopped;
            KtmSessionControllerTestAccess::emitBackendStateChanged(
                controller, Xc2BackendState::Stopped);
        };
        KtmSessionControllerTestAccess::installOps(controller,
                                                   std::move(ops));

        QSignalSpy lookupFinished(&controller,
                                  &KtmSessionController::vciLookupFinished);
        QSignalSpy ready(&controller, &KtmSessionController::vciReady);
        QSignalSpy closed(&controller, &KtmSessionController::vciClosed);
        QSignalSpy statusChanged(
            &controller, &KtmSessionController::vciStatusChanged);
        QVERIFY(controller.startProduction(QStringLiteral("C:/XC2")));
        QCOMPARE(backendStartCount, 1);
        backendState = Xc2BackendState::Ready;
        KtmSessionControllerTestAccess::emitBackendStateChanged(
            controller, Xc2BackendState::Ready);
        KtmSessionControllerTestAccess::emitBackendReady(
            controller,
            {QUrl::fromEncoded(server.encodedRestBase()),
             QUrl(QStringLiteral("ws://127.0.0.1:%1/xc2-websocket")
                      .arg(server.port()))});

        QTRY_COMPARE_WITH_TIMEOUT(
            KtmSessionControllerTestAccess::state(controller),
            KtmSessionState::SessionReady, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(
            std::any_of(server.trace().cbegin(), server.trace().cend(),
                        [](const FakeXc2TransportServer::TraceEvent &event) {
                return event.label == QStringLiteral("subscribe-login");
            }),
            3000);
        QVERIFY(controller.lookupVci());
        QTRY_COMPARE_WITH_TIMEOUT(lookupFinished.count(), 1, 3000);
        const QList<Xc2VciDevice> devices =
            qvariant_cast<QList<Xc2VciDevice>>(
                lookupFinished.constFirst().constFirst());
        QCOMPARE(devices.size(), 1);
        QCOMPARE(devices.constFirst().id, device.id);

        QVERIFY(controller.applyVci(device));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
        QCOMPARE(KtmSessionControllerTestAccess::state(controller),
                 KtmSessionState::VciReady);
        const Xc2VciStatus readyStatus = qvariant_cast<Xc2VciStatus>(
            ready.constFirst().at(1));
        QCOMPARE(readyStatus.voltage, 12.4);
        QVERIFY(readyStatus.connected);

        QVERIFY(controller.closeVci());
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 3000);
        QCOMPARE(KtmSessionControllerTestAccess::state(controller),
                 KtmSessionState::SessionReady);
        QVERIFY(!controller.closeVci());
        const auto closeRequest = std::find_if(
            server.capturedRestRequests().cbegin(),
            server.capturedRestRequests().cend(),
            [](const FakeXc2HttpRequest &request) {
                return request.target
                    == QByteArrayLiteral("/xc2/1.0/device/close");
            });
        QVERIFY(closeRequest != server.capturedRestRequests().cend());
        QCOMPARE(closeRequest->body, compactDevice(device));
        QCOMPARE(closeRequest->headerValues(QByteArrayLiteral("Cookie")),
                 QList<QByteArray>{QByteArrayLiteral("session=task4")});
        QCOMPARE(
            closeRequest->headerValues(QByteArrayLiteral("Content-Type")),
            QList<QByteArray>{QByteArrayLiteral("application/json")});
        QJsonParseError closeParseError;
        const QJsonObject closeObject = QJsonDocument::fromJson(
            closeRequest->body, &closeParseError).object();
        QCOMPARE(closeParseError.error, QJsonParseError::NoError);
        QCOMPARE(closeObject.size(), 4);
        QCOMPARE(closeObject.value(QStringLiteral("id")).toString(),
                 device.id);
        QCOMPARE(closeObject.value(QStringLiteral("name")).toString(),
                 device.name);
        QCOMPARE(closeObject.value(QStringLiteral("internalName")).toString(),
                 device.internalName);
        QCOMPARE(closeObject.value(
                     QStringLiteral("additionalModuleInformation"))
                     .toString(),
                 *device.additionalModuleInformation);

        const int statusBaseline = statusChanged.count();
        QVERIFY(server.performAction(sendTopic(
            QStringLiteral("current-disconnected"),
            topicMessage(
                Topic::VciStatus,
                QByteArrayLiteral("vci-status-subscription"),
                QByteArrayLiteral("status-disconnected"),
                QByteArrayLiteral("{\"voltage\":0.0,\"connected\":false}")))));
        QTRY_COMPARE_WITH_TIMEOUT(statusChanged.count(), statusBaseline + 1,
                                  3000);
        const Xc2VciStatus disconnected = qvariant_cast<Xc2VciStatus>(
            statusChanged.constLast().constFirst());
        QCOMPARE(disconnected.voltage, 0.0);
        QVERIFY(!disconnected.connected);
        QCOMPARE(KtmSessionControllerTestAccess::state(controller),
                 KtmSessionState::SessionReady);

        const auto traceIndex = [&server](const QString &label) {
            for (qsizetype index = 0; index < server.trace().size(); ++index) {
                if (server.trace().at(index).label == label)
                    return index;
            }
            return qsizetype(-1);
        };
        const qsizetype progressSubscription =
            traceIndex(QStringLiteral("subscribe-progress"));
        const qsizetype lookup = traceIndex(QStringLiteral("device-lookup"));
        const qsizetype progress =
            traceIndex(QStringLiteral("progress-before-lookup-response"));
        const qsizetype lookupResponse =
            traceIndex(QStringLiteral("lookup-response"));
        QVERIFY(progressSubscription >= 0);
        QVERIFY(lookup > progressSubscription);
        QVERIFY(progress > lookup);
        QVERIFY(lookupResponse > progress);
        QVERIFY(server.scriptExhausted());
        QCOMPARE(server.unexpectedOperationCount(), 0);
        QCOMPARE(server.pendingActionCount(), 0);
        QCOMPARE(server.canceledActionCount(), 0);
        QCOMPARE(server.upgradeRequestCount(), 1);
        QCOMPARE(server.webSocketConnectionCount(), 1);
        QCOMPARE(server.stateChangingRestRequestCount(), 3);

        QPointer<Xc2RestClient> oldRest =
            KtmSessionControllerTestAccess::restGuard(controller);
        QPointer<Xc2StompClient> oldStomp =
            KtmSessionControllerTestAccess::stompGuard(controller);
        QPointer<Xc2JobRegistry> oldRegistry =
            KtmSessionControllerTestAccess::registryGuard(controller);
        QVERIFY(oldRest && oldStomp && oldRegistry);
        controller.stop();
        QTRY_COMPARE_WITH_TIMEOUT(
            KtmSessionControllerTestAccess::state(controller),
            KtmSessionState::Stopped, 3000);
        QCOMPARE(backendStopCount, 1);
        QVERIFY(oldRest.isNull());
        QVERIFY(oldStomp.isNull());
        QVERIFY(oldRegistry.isNull());
        QTRY_COMPARE_WITH_TIMEOUT(server.openConnectionCount(), 0, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(server.liveSocketObjectCount(), 0, 3000);
        controllerOwner.reset();
        QVERIFY(controllerGuard.isNull());
        QCOMPARE(server.unexpectedOperationCount(), 0);
        QCOMPARE(server.pendingActionCount(), 0);
        QCOMPARE(server.canceledActionCount(), 0);
        QVERIFY(server.scriptExhausted());
    }

    void publicSurfaceUsesOnlyOperatorDomainArguments()
    {
        using Start = bool (KtmSessionController::*)(const QString &,
                                                     Xc2Error *);
        using Lookup = bool (KtmSessionController::*)(Xc2Error *);
        using Apply = bool (KtmSessionController::*)(const Xc2VciDevice &,
                                                     Xc2Error *);
        using Close = bool (KtmSessionController::*)(Xc2Error *);
        using Stop = void (KtmSessionController::*)();
        static_assert(std::is_same_v<decltype(
                          &KtmSessionController::startProduction), Start>);
        static_assert(std::is_same_v<decltype(
                          &KtmSessionController::lookupVci), Lookup>);
        static_assert(std::is_same_v<decltype(
                          &KtmSessionController::applyVci), Apply>);
        static_assert(std::is_same_v<decltype(
                          &KtmSessionController::closeVci), Close>);
        static_assert(std::is_same_v<decltype(
                          &KtmSessionController::stop), Stop>);

        QCOMPARE(int(KtmSessionState::Stopped), 0);
        QCOMPARE(int(KtmSessionState::BackendStarting), 1);
        QCOMPARE(int(KtmSessionState::BackendReady), 2);
        QCOMPARE(int(KtmSessionState::SessionStarting), 3);
        QCOMPARE(int(KtmSessionState::SessionReady), 4);
        QCOMPARE(int(KtmSessionState::VciLookup), 5);
        QCOMPARE(int(KtmSessionState::VciApplying), 6);
        QCOMPARE(int(KtmSessionState::VciReady), 7);
        QCOMPARE(int(KtmSessionState::VciClosing), 8);
        QCOMPARE(int(KtmSessionState::Failed), 9);
    }

    void bootstrapUsesOneRestSessionInStrictOrder()
    {
        BootstrapHarness harness;
        QSignalSpy states(&harness.controller,
                          &KtmSessionController::stateChanged);
        QVERIFY(harness.start());
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::BackendStarting);
        QCOMPARE(harness.trace,
                 QStringList{QStringLiteral("start:C:/XC2")});

        harness.publishReady();
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::SessionStarting);
        QCOMPARE(harness.trace,
                 QStringList({QStringLiteral("start:C:/XC2"),
                              QStringLiteral("base:%1").arg(
                                  QString::fromLatin1(kRestBase)),
                              QStringLiteral("serviceStatus")}));

        harness.completeHealth();
        QCOMPARE(harness.trace.constLast(), QStringLiteral("currentUser"));
        harness.completeUser();
        QCOMPARE(harness.trace.constLast(), QStringLiteral("stomp-connect"));
        KtmSessionControllerTestAccess::stompConnected(
            harness.controller,
            KtmSessionControllerTestAccess::sessionEpoch(harness.controller),
            KtmSessionControllerTestAccess::stompIdentity(harness.controller),
            7);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::SessionReady);
        QCOMPARE(harness.trace.mid(harness.trace.size() - 3),
                 QStringList({QStringLiteral("subscribe:7"),
                              QStringLiteral("subscribe:0"),
                              QStringLiteral("subscribe:3")}));
        QVERIFY(states.count() >= 4);
    }

    void backendStartingProjectionRevalidatesBackendBeforeStartCall()
    {
        BootstrapHarness harness;
        bool destroyed = false;
        QObject::connect(
            &harness.controller, &KtmSessionController::stateChanged,
            &harness.controller, [&harness, &destroyed](KtmSessionState state) {
                if (state != KtmSessionState::BackendStarting)
                    return;
                destroyed = true;
                KtmSessionControllerTestAccess::destroyBackendChild(
                    harness.controller);
            }, Qt::DirectConnection);

        QVERIFY(!harness.start());

        QVERIFY(destroyed);
        QVERIFY(harness.trace.isEmpty());
    }

    void sessionStartingProjectionRevalidatesRestBeforeConfigurationCall()
    {
        BootstrapHarness harness;
        QVERIFY(harness.start());
        harness.backendState = Xc2BackendState::Ready;
        KtmSessionControllerTestAccess::backendStateChanged(
            harness.controller, Xc2BackendState::Ready);
        harness.trace.clear();
        bool destroyed = false;
        QObject::connect(
            &harness.controller, &KtmSessionController::stateChanged,
            &harness.controller, [&harness, &destroyed](KtmSessionState state) {
                if (state != KtmSessionState::SessionStarting)
                    return;
                destroyed = true;
                KtmSessionControllerTestAccess::destroyRestChild(
                    harness.controller);
            }, Qt::DirectConnection);

        KtmSessionControllerTestAccess::backendReady(
            harness.controller, endpoints());

        QVERIFY(destroyed);
        QVERIFY(harness.trace.isEmpty());
    }

    void lookupProjectionRevalidatesRestBeforeDeferredRequest()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        harness.trace.clear();
        bool destroyed = false;
        QObject::connect(
            &harness.controller, &KtmSessionController::stateChanged,
            &harness.controller, [&harness, &destroyed](KtmSessionState state) {
                if (state != KtmSessionState::VciLookup)
                    return;
                destroyed = true;
                KtmSessionControllerTestAccess::destroyRestChild(
                    harness.controller);
            }, Qt::DirectConnection);

        QVERIFY(!harness.controller.lookupVci());

        QVERIFY(destroyed);
        QVERIFY(!harness.trace.contains(QStringLiteral("device-lookup")));
    }

    void applyProjectionRevalidatesRestBeforeRequest()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        harness.trace.clear();
        bool destroyed = false;
        QObject::connect(
            &harness.controller, &KtmSessionController::stateChanged,
            &harness.controller, [&harness, &destroyed](KtmSessionState state) {
                if (state != KtmSessionState::VciApplying)
                    return;
                destroyed = true;
                KtmSessionControllerTestAccess::destroyRestChild(
                    harness.controller);
            }, Qt::DirectConnection);

        QVERIFY(!harness.controller.applyVci(approvedDevice()));

        QVERIFY(destroyed);
        QVERIFY(!harness.trace.contains(QStringLiteral("apply:vci-1:Candidate:AVL Ditest VCI2K_DPDU_API:<null>")));
    }

    void closeProjectionRevalidatesRestBeforeRequest()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        completeReadyApply(harness);
        harness.trace.clear();
        bool destroyed = false;
        QObject::connect(
            &harness.controller, &KtmSessionController::stateChanged,
            &harness.controller, [&harness, &destroyed](KtmSessionState state) {
                if (state != KtmSessionState::VciClosing)
                    return;
                destroyed = true;
                KtmSessionControllerTestAccess::destroyRestChild(
                    harness.controller);
            }, Qt::DirectConnection);

        QVERIFY(!harness.controller.closeVci());

        QVERIFY(destroyed);
        QVERIFY(!harness.trace.contains(QStringLiteral("close:vci-1")));
    }

    void stopProjectionRevalidatesTransportBeforeTeardownCall()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.lookupVci());
        harness.trace.clear();
        bool destroyed = false;
        QObject::connect(
            &harness.controller, &KtmSessionController::operationChanged,
            &harness.controller,
            [&harness, &destroyed](KtmSessionOperation operation) {
                if (operation != KtmSessionOperation::None)
                    return;
                destroyed = true;
                KtmSessionControllerTestAccess::destroyTransportChildren(
                    harness.controller);
            }, Qt::DirectConnection);
        harness.backendState = Xc2BackendState::Stopped;

        harness.controller.stop();

        QVERIFY(destroyed);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
        QVERIFY(!KtmSessionControllerTestAccess::stopping(
            harness.controller));
        QVERIFY(harness.start());
    }

    void failureProjectionRevalidatesTransportBeforeTeardownCall()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.lookupVci());
        harness.trace.clear();
        bool destroyed = false;
        QObject::connect(
            &harness.controller, &KtmSessionController::operationChanged,
            &harness.controller,
            [&harness, &destroyed](KtmSessionOperation operation) {
                if (operation != KtmSessionOperation::None)
                    return;
                destroyed = true;
                KtmSessionControllerTestAccess::destroyTransportChildren(
                    harness.controller);
            }, Qt::DirectConnection);

        Xc2Error failure;
        failure.category = Xc2ErrorCategory::Transport;
        failure.message = QStringLiteral("failed");
        KtmSessionControllerTestAccess::failSession(harness.controller,
                                                    failure);

        QVERIFY(destroyed);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
        harness.backendState = Xc2BackendState::Stopped;
        harness.controller.stop();
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
        QVERIFY(harness.start());
    }

    void teardownSiblingMutationRecoversTransaction_data()
    {
        QTest::addColumn<bool>("failureTransaction");
        QTest::addColumn<bool>("mutateStompAfterRestAbort");
        QTest::newRow("stop-rest-to-stomp") << false << true;
        QTest::newRow("stop-stomp-to-rest") << false << false;
        QTest::newRow("fail-rest-to-stomp") << true << true;
        QTest::newRow("fail-stomp-to-rest") << true << false;
    }

    void teardownSiblingMutationRecoversTransaction()
    {
        QFETCH(bool, failureTransaction);
        QFETCH(bool, mutateStompAfterRestAbort);
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.lookupVci());
        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        QPointer<Xc2RestClient> rest =
            KtmSessionControllerTestAccess::restGuard(harness.controller);
        QPointer<Xc2StompClient> stomp =
            KtmSessionControllerTestAccess::stompGuard(harness.controller);
        QVERIFY(rest);
        QVERIFY(stomp);
        bool mutated = false;
        quint64 mutationEpoch = 0;
        int sameEpochSiblingCalls = 0;
        KtmSessionControllerTestAccess::setAbortRestOp(
            harness.controller,
            [&harness, &stomp, &mutated, &mutationEpoch,
             mutateStompAfterRestAbort](Xc2RequestId id) {
                harness.trace.append(
                    QStringLiteral("abort-rest:%1").arg(id));
                if (!mutateStompAfterRestAbort || mutated)
                    return;
                mutated = true;
                mutationEpoch = KtmSessionControllerTestAccess::sessionEpoch(
                    harness.controller);
                delete stomp.data();
            });
        KtmSessionControllerTestAccess::setDisconnectOp(
            harness.controller,
            [&harness, &mutated, &mutationEpoch, &sameEpochSiblingCalls,
             mutateStompAfterRestAbort] {
                harness.trace.append(QStringLiteral("disconnect-stomp"));
                const quint64 epoch =
                    KtmSessionControllerTestAccess::sessionEpoch(
                        harness.controller);
                if (mutateStompAfterRestAbort) {
                    if (mutated && epoch == mutationEpoch)
                        ++sameEpochSiblingCalls;
                    return;
                }
                if (mutated)
                    return;
                mutated = true;
                mutationEpoch = epoch;
                KtmSessionControllerTestAccess::destroyRestChild(
                    harness.controller);
            });
        KtmSessionControllerTestAccess::setAbortStompOp(
            harness.controller,
            [&harness, &mutated, &mutationEpoch, &sameEpochSiblingCalls,
             mutateStompAfterRestAbort] {
                harness.trace.append(QStringLiteral("abort-stomp"));
                if (!mutateStompAfterRestAbort && mutated
                    && KtmSessionControllerTestAccess::sessionEpoch(
                           harness.controller) == mutationEpoch) {
                    ++sameEpochSiblingCalls;
                }
            });
        KtmSessionControllerTestAccess::setStopBackendOp(
            harness.controller, [&harness] {
                harness.trace.append(QStringLiteral("stop-backend"));
                harness.backendState = Xc2BackendState::Stopped;
                KtmSessionControllerTestAccess::emitBackendStateChanged(
                    harness.controller, Xc2BackendState::Stopped);
            });
        harness.trace.clear();

        if (failureTransaction) {
            Xc2Error failure;
            failure.category = Xc2ErrorCategory::Transport;
            failure.message = QStringLiteral("sibling mutation");
            KtmSessionControllerTestAccess::failSession(harness.controller,
                                                        failure);
        } else {
            harness.controller.stop();
        }

        QVERIFY(mutated);
        QCOMPARE(sameEpochSiblingCalls, 0);
        if (failureTransaction) {
            QCOMPARE(KtmSessionControllerTestAccess::state(
                         harness.controller), KtmSessionState::Failed);
            harness.controller.stop();
        }
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
        QVERIFY(!KtmSessionControllerTestAccess::stopping(
            harness.controller));
        harness.trace.clear();
        QVERIFY(harness.start());
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::BackendStarting);
    }

    void lookupOperationProjectionCannotOverwriteBackendStop()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QObject::connect(
            &harness.controller, &KtmSessionController::operationChanged,
            &harness.controller,
            [&harness](KtmSessionOperation operation) {
                if (operation == KtmSessionOperation::Lookup) {
                    KtmSessionControllerTestAccess::emitBackendStateChanged(
                        harness.controller, Xc2BackendState::Stopped);
                }
            }, Qt::DirectConnection);

        QVERIFY(!harness.controller.lookupVci());

        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
        QCOMPARE(KtmSessionControllerTestAccess::operation(
                     harness.controller), KtmSessionOperation::None);
    }

    void lookupCompletionCannotOverwriteBackendStop()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.lookupVci());
        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        KtmSessionControllerTestAccess::deviceLookupFinished(
            harness.controller, harness.lookupId,
            Xc2Result<Xc2JobAccepted>::success(
                {QStringLiteral("job-terminal-stop")}));
        KtmSessionControllerTestAccess::stompMessage(
            harness.controller,
            progressMessage(harness.generation,
                            QStringLiteral("job-terminal-stop"),
                            Xc2JobState::Finished));
        QSignalSpy finished(&harness.controller,
                            &KtmSessionController::vciLookupFinished);
        QObject::connect(
            &harness.controller, &KtmSessionController::operationChanged,
            &harness.controller,
            [&harness](KtmSessionOperation operation) {
                if (operation == KtmSessionOperation::None) {
                    KtmSessionControllerTestAccess::emitBackendStateChanged(
                        harness.controller, Xc2BackendState::Stopped);
                }
            }, Qt::DirectConnection);

        KtmSessionControllerTestAccess::devicesFinished(
            harness.controller, harness.devicesId,
            Xc2Result<QList<Xc2VciDevice>>::success({approvedDevice()}));

        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
        QCOMPARE(finished.count(), 0);
    }

    void readinessRevocationCannotOverwriteBackendStop()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        completeReadyApply(harness);
        QObject::connect(
            &harness.controller,
            &KtmSessionController::vciReadinessRevoked,
            &harness.controller, [&harness](const Xc2Error &) {
                KtmSessionControllerTestAccess::emitBackendStateChanged(
                    harness.controller, Xc2BackendState::Stopped);
            }, Qt::DirectConnection);

        KtmSessionControllerTestAccess::stompMessage(
            harness.controller,
            vciStatusMessage(harness.generation, false, 12.1));

        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
    }

    void readinessFailureCannotOverwriteBackendStop()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        completeReadyApply(harness);
        QSignalSpy failed(&harness.controller,
                          &KtmSessionController::failed);
        QObject::connect(
            &harness.controller,
            &KtmSessionController::vciReadinessRevoked,
            &harness.controller, [&harness](const Xc2Error &) {
                KtmSessionControllerTestAccess::emitBackendStateChanged(
                    harness.controller, Xc2BackendState::Stopped);
            }, Qt::DirectConnection);
        Xc2StompMessage malformed =
            vciStatusMessage(harness.generation, true, 12.1);
        malformed.body = QByteArrayLiteral(R"({"voltage":12.1})");

        KtmSessionControllerTestAccess::stompMessage(
            harness.controller, malformed);

        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
        QCOMPARE(failed.count(), 0);
    }

    void directlyDestroyedOwnedChildRecoversThroughStopAndFreshStart_data()
    {
        QTest::addColumn<int>("childKind");
        QTest::newRow("backend") << 0;
        QTest::newRow("rest") << 1;
        QTest::newRow("stomp") << 2;
        QTest::newRow("registry") << 3;
    }

    void directlyDestroyedOwnedChildRecoversThroughStopAndFreshStart()
    {
        QFETCH(int, childKind);
        BootstrapHarness harness;
        harness.completeBootstrap();
        QObject *child = nullptr;
        switch (childKind) {
        case 0:
            child = KtmSessionControllerTestAccess::backendGuard(
                        harness.controller).data();
            break;
        case 1:
            child = KtmSessionControllerTestAccess::restGuard(
                        harness.controller).data();
            break;
        case 2:
            child = KtmSessionControllerTestAccess::stompGuard(
                        harness.controller).data();
            break;
        default:
            child = KtmSessionControllerTestAccess::registryGuard(
                        harness.controller).data();
            break;
        }
        QPointer<QObject> destroyedGuard = child;

        delete child;

        QVERIFY(destroyedGuard.isNull());
        harness.backendState = Xc2BackendState::Stopped;
        harness.trace.clear();
        harness.controller.stop();
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
        QVERIFY(!KtmSessionControllerTestAccess::stopping(
            harness.controller));

        harness.trace.clear();
        QVERIFY(harness.start());
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::BackendStarting);
        QCOMPARE(harness.trace,
                 QStringList({QStringLiteral("start:C:/XC2")}));
    }

    void directlyDestroyedRegistryThenSiblingSignalFailsSafely()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QPointer<Xc2JobRegistry> registry =
            KtmSessionControllerTestAccess::registryGuard(
                harness.controller);
        QVERIFY(registry);
        QSignalSpy failed(&harness.controller,
                          &KtmSessionController::failed);

        delete registry.data();

        QVERIFY(registry.isNull());
        Xc2Error error;
        error.category = Xc2ErrorCategory::Transport;
        error.message = QStringLiteral("registry sibling callback");
        KtmSessionControllerTestAccess::stompErrorThenVisibility(
            harness.controller, harness.generation, error);

        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
        QCOMPARE(failed.count(), 1);
        QVERIFY(KtmSessionControllerTestAccess::registryGuard(
            harness.controller));
    }

    void pendingStopCompletesWhenBackendChildIsDestroyed()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QPointer<Xc2BackendManager> backend =
            KtmSessionControllerTestAccess::backendGuard(
                harness.controller);
        QVERIFY(backend);

        harness.controller.stop();

        QVERIFY(KtmSessionControllerTestAccess::stopping(
            harness.controller));
        delete backend.data();
        QVERIFY(backend.isNull());
        harness.controller.stop();

        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
        QVERIFY(!KtmSessionControllerTestAccess::stopping(
            harness.controller));
        harness.backendState = Xc2BackendState::Stopped;
        harness.trace.clear();
        QVERIFY(harness.start());
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::BackendStarting);
    }

    void backendTerminalDuringStopProjectionFinishesSessionTeardown_data()
    {
        QTest::addColumn<bool>("destroyBackend");
        QTest::newRow("destroyed") << true;
        QTest::newRow("stopped-signal") << false;
    }

    void backendTerminalDuringStopProjectionFinishesSessionTeardown()
    {
        QFETCH(bool, destroyBackend);
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.lookupVci());
        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        QPointer<Xc2BackendManager> backend =
            KtmSessionControllerTestAccess::backendGuard(
                harness.controller);
        QPointer<Xc2RestClient> rest =
            KtmSessionControllerTestAccess::restGuard(harness.controller);
        QPointer<Xc2StompClient> stomp =
            KtmSessionControllerTestAccess::stompGuard(harness.controller);
        QPointer<Xc2JobRegistry> registry =
            KtmSessionControllerTestAccess::registryGuard(
                harness.controller);
        harness.trace.clear();
        QObject::connect(
            &harness.controller, &KtmSessionController::operationChanged,
            &harness.controller,
            [&harness, &backend, destroyBackend](KtmSessionOperation operation) {
                if (operation != KtmSessionOperation::None || !backend)
                    return;
                if (destroyBackend)
                    delete backend.data();
                else
                    KtmSessionControllerTestAccess::emitBackendStateChanged(
                        harness.controller, Xc2BackendState::Stopped);
                harness.controller.stop();
            }, Qt::DirectConnection);

        harness.controller.stop();

        QCOMPARE(backend.isNull(), destroyBackend);
        QVERIFY(rest.isNull());
        QVERIFY(stomp.isNull());
        QVERIFY(registry.isNull());
        QVERIFY(harness.trace.contains(QStringLiteral("abort-rest:21")));
        QVERIFY(harness.trace.contains(QStringLiteral("disconnect-stomp")));
        QVERIFY(harness.trace.contains(QStringLiteral("abort-stomp")));
        QVERIFY(harness.trace.contains(QStringLiteral("reset-session")));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
        QVERIFY(!KtmSessionControllerTestAccess::stopping(
            harness.controller));
    }

    void startNormalizationCannotLeakBackendTerminalIntoNextStop()
    {
        BootstrapHarness harness;
        KtmSessionControllerTestAccess::setOperation(
            harness.controller, KtmSessionOperation::Lookup);
        bool injected = false;
        QObject::connect(
            &harness.controller, &KtmSessionController::operationChanged,
            &harness.controller,
            [&harness, &injected](KtmSessionOperation operation) {
                if (operation != KtmSessionOperation::None || injected)
                    return;
                injected = true;
                KtmSessionControllerTestAccess::emitBackendStateChanged(
                    harness.controller, Xc2BackendState::Stopped);
            }, Qt::DirectConnection);

        QVERIFY(harness.start());
        QVERIFY(injected);
        harness.backendState = Xc2BackendState::Ready;
        harness.trace.clear();

        harness.controller.stop();

        QVERIFY(harness.trace.contains(QStringLiteral("stop-backend")));
        QVERIFY(KtmSessionControllerTestAccess::stopping(
            harness.controller));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::BackendStarting);
        harness.backendState = Xc2BackendState::Stopped;
        KtmSessionControllerTestAccess::emitBackendStateChanged(
            harness.controller, Xc2BackendState::Stopped);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
    }

    void stopDuringStartNormalizationCancelsOuterStart()
    {
        BootstrapHarness harness;
        QPointer<Xc2BackendManager> oldBackend =
            KtmSessionControllerTestAccess::backendGuard(
                harness.controller);
        QVERIFY(oldBackend);
        delete oldBackend.data();
        QVERIFY(oldBackend.isNull());
        KtmSessionControllerTestAccess::setOperation(
            harness.controller, KtmSessionOperation::Lookup);
        bool stopRequested = false;
        QObject::connect(
            &harness.controller, &KtmSessionController::operationChanged,
            &harness.controller,
            [&harness, &stopRequested](KtmSessionOperation operation) {
                if (operation != KtmSessionOperation::None || stopRequested)
                    return;
                stopRequested = true;
                harness.controller.stop();
            }, Qt::DirectConnection);
        harness.trace.clear();

        const bool started = harness.start();

        QVERIFY(stopRequested);
        QCOMPARE(harness.trace.count(QStringLiteral("start:C:/XC2")), 0);
        QVERIFY(!started);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
        QVERIFY(!KtmSessionControllerTestAccess::stopping(
            harness.controller));
        harness.trace.clear();
        QVERIFY(harness.start());
        QCOMPARE(harness.trace.count(QStringLiteral("start:C:/XC2")), 1);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::BackendStarting);
    }

    void rejectsPublishedWebSocketAuthorityMismatch()
    {
        BootstrapHarness harness;
        QVERIFY(harness.start());
        harness.publishReady(endpoints(
            "ws://127.0.0.1:49153/xc2/1.0/stomp"));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
        QVERIFY(!harness.trace.contains(QStringLiteral("serviceStatus")));
    }

    void requiresExactCaseSensitivePermission()
    {
        BootstrapHarness harness;
        QVERIFY(harness.start());
        harness.publishReady();
        harness.completeHealth();
        harness.completeUser(
            authorizedUser(QStringLiteral("ecudiagnosticread")));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
        QVERIFY(!harness.trace.contains(QStringLiteral("stomp-connect")));
    }

    void bootstrapRequestIdZeroFailsImmediately_data()
    {
        QTest::addColumn<bool>("zeroHealth");
        QTest::newRow("service-status") << true;
        QTest::newRow("current-user") << false;
    }

    void bootstrapRequestIdZeroFailsImmediately()
    {
        QFETCH(bool, zeroHealth);
        BootstrapHarness harness;
        if (zeroHealth)
            harness.serviceStatusId = 0;
        else
            harness.currentUserId = 0;
        QVERIFY(harness.start());
        harness.publishReady();
        if (!zeroHealth)
            harness.completeHealth();
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
    }

    void lookupWaitsForCurrentProgressSubscription()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.lookupVci());
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::VciLookup);
        QVERIFY(!harness.trace.contains(QStringLiteral("device-lookup")));

        harness.generation = 8;
        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        QVERIFY(!harness.trace.contains(QStringLiteral("device-lookup")));

        harness.generation = 7;
        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        QCOMPARE(harness.trace.count(QStringLiteral("device-lookup")), 1);
        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        QCOMPARE(harness.trace.count(QStringLiteral("device-lookup")), 1);
    }

    void progressBeforeLookupAcceptanceIsRetained()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.lookupVci());
        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        KtmSessionControllerTestAccess::stompMessage(
            harness.controller,
            progressMessage(7, QStringLiteral("lookup-job"),
                            Xc2JobState::Finished));
        QVERIFY(!harness.trace.contains(QStringLiteral("devices")));

        KtmSessionControllerTestAccess::deviceLookupFinished(
            harness.controller, harness.lookupId,
            Xc2Result<Xc2JobAccepted>::success(
                {QStringLiteral("lookup-job")}));
        QCOMPARE(harness.trace.count(QStringLiteral("devices")), 1);

        const QList<Xc2VciDevice> devices{
            {QStringLiteral("vci-1"), QStringLiteral("Synthetic"),
             QStringLiteral("AVL Ditest VCI2K_DPDU_API"), std::nullopt}};
        QSignalSpy finished(&harness.controller,
                            &KtmSessionController::vciLookupFinished);
        KtmSessionControllerTestAccess::devicesFinished(
            harness.controller, harness.devicesId,
            Xc2Result<QList<Xc2VciDevice>>::success(devices));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::SessionReady);
        QCOMPARE(finished.count(), 1);
        QCOMPARE(qvariant_cast<QList<Xc2VciDevice>>(
                     finished.constFirst().constFirst()).constFirst().id,
                 QStringLiteral("vci-1"));
    }

    void lookupTerminalMatrix_data()
    {
        QTest::addColumn<int>("terminalState");
        QTest::addColumn<bool>("requestsDevices");
        QTest::newRow("finished") << int(Xc2JobState::Finished) << true;
        QTest::newRow("canceled") << int(Xc2JobState::Canceled) << true;
        QTest::newRow("error") << int(Xc2JobState::Error) << false;
        QTest::newRow("not-authorized")
            << int(Xc2JobState::NotAuthorized) << false;
    }

    void lookupTerminalMatrix()
    {
        QFETCH(int, terminalState);
        QFETCH(bool, requestsDevices);
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.lookupVci());
        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        KtmSessionControllerTestAccess::deviceLookupFinished(
            harness.controller, harness.lookupId,
            Xc2Result<Xc2JobAccepted>::success(
                {QStringLiteral("lookup-job")}));
        KtmSessionControllerTestAccess::stompMessage(
            harness.controller,
            progressMessage(7, QStringLiteral("lookup-job"),
                            Xc2JobState(terminalState)));
        QCOMPARE(harness.trace.contains(QStringLiteral("devices")),
                 requestsDevices);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 requestsDevices ? KtmSessionState::VciLookup
                                 : KtmSessionState::Failed);
    }

    void lookupRequestIdZeroFailsAtTheAttemptedStage_data()
    {
        QTest::addColumn<bool>("zeroLookup");
        QTest::newRow("lookup") << true;
        QTest::newRow("devices") << false;
    }

    void lookupRequestIdZeroFailsAtTheAttemptedStage()
    {
        QFETCH(bool, zeroLookup);
        BootstrapHarness harness;
        harness.completeBootstrap();
        if (zeroLookup)
            harness.lookupId = 0;
        else
            harness.devicesId = 0;
        QVERIFY(harness.controller.lookupVci());
        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        if (!zeroLookup) {
            KtmSessionControllerTestAccess::deviceLookupFinished(
                harness.controller, harness.lookupId,
                Xc2Result<Xc2JobAccepted>::success(
                    {QStringLiteral("lookup-job")}));
            KtmSessionControllerTestAccess::stompMessage(
                harness.controller,
                progressMessage(7, QStringLiteral("lookup-job"),
                                Xc2JobState::Finished));
        }
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
    }

    void applyRejectsInvalidIdentityBeforeNetworkWrite_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QString>("provider");
        QTest::newRow("blank-id")
            << QStringLiteral("   ")
            << QStringLiteral("AVL Ditest VCI2K_DPDU_API");
        QTest::newRow("wrong-provider")
            << QStringLiteral("vci-1")
            << QStringLiteral("AVL Ditest VCI2K_dpdu_api");
    }

    void applyRejectsInvalidIdentityBeforeNetworkWrite()
    {
        QFETCH(QString, id);
        QFETCH(QString, provider);
        BootstrapHarness harness;
        harness.completeBootstrap();
        Xc2VciDevice device = approvedDevice();
        device.id = id;
        device.internalName = provider;
        QVERIFY(!harness.controller.applyVci(device));
        QVERIFY(!harness.trace.join(QLatin1Char('|')).contains(
            QStringLiteral("apply:")));
        QVERIFY(!harness.trace.contains(QStringLiteral("selected")));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::SessionReady);
    }

    void applyCorrelatesThreeFactsInEitherOrder_data()
    {
        QTest::addColumn<bool>("statusFirst");
        QTest::newRow("status-before-selected") << true;
        QTest::newRow("selected-before-status") << false;
    }

    void applyCorrelatesThreeFactsInEitherOrder()
    {
        QFETCH(bool, statusFirst);
        BootstrapHarness harness;
        harness.completeBootstrap();
        QSignalSpy ready(&harness.controller,
                         &KtmSessionController::vciReady);
        const Xc2VciDevice candidate = approvedDevice(
            QStringLiteral("Candidate Name"), QStringLiteral("candidate-info"));
        const Xc2VciDevice confirmed = approvedDevice(
            QStringLiteral("Backend Name"), QStringLiteral("backend-info"));
        const quint64 before =
            KtmSessionControllerTestAccess::selectionEpoch(harness.controller);
        QVERIFY(harness.controller.applyVci(candidate));
        QCOMPARE(KtmSessionControllerTestAccess::selectionEpoch(
                     harness.controller),
                 before + 1);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::VciApplying);
        QCOMPARE(harness.trace.count(QStringLiteral("selected")), 1);
        QCOMPARE(harness.trace.filter(QStringLiteral("apply:")).size(), 1);

        KtmSessionControllerTestAccess::applyDeviceFinished(
            harness.controller, harness.applyId);
        if (statusFirst) {
            KtmSessionControllerTestAccess::stompMessage(
                harness.controller, vciStatusMessage(7, true, 0.0));
            KtmSessionControllerTestAccess::selectedDeviceFinished(
                harness.controller, harness.selectedId,
                Xc2Result<Xc2SelectedVci>::success({confirmed}));
        } else {
            KtmSessionControllerTestAccess::selectedDeviceFinished(
                harness.controller, harness.selectedId,
                Xc2Result<Xc2SelectedVci>::success({confirmed}));
            KtmSessionControllerTestAccess::stompMessage(
                harness.controller, vciStatusMessage(7, true, 0.0));
        }

        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::VciReady);
        QCOMPARE(ready.count(), 1);
        const Xc2VciDevice projected = qvariant_cast<Xc2VciDevice>(
            ready.constFirst().at(0));
        const Xc2VciStatus status = qvariant_cast<Xc2VciStatus>(
            ready.constFirst().at(1));
        QCOMPARE(projected.id, candidate.id);
        QCOMPARE(projected.name, confirmed.name);
        QCOMPARE(projected.additionalModuleInformation,
                 confirmed.additionalModuleInformation);
        QCOMPARE(status.voltage, 0.0);
        QVERIFY(status.connected);
    }

    void staleConnectedTrueCannotSatisfyNewApply()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        KtmSessionControllerTestAccess::stompMessage(
            harness.controller, vciStatusMessage(7, true, 12.5));
        QVERIFY(harness.controller.applyVci(approvedDevice()));
        KtmSessionControllerTestAccess::applyDeviceFinished(
            harness.controller, harness.applyId);
        KtmSessionControllerTestAccess::selectedDeviceFinished(
            harness.controller, harness.selectedId,
            Xc2Result<Xc2SelectedVci>::success({approvedDevice()}));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::VciApplying);

        KtmSessionControllerTestAccess::stompMessage(
            harness.controller, vciStatusMessage(6, true, 12.6));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::VciApplying);
        KtmSessionControllerTestAccess::stompMessage(
            harness.controller, vciStatusMessage(7, true, 12.7));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::VciReady);
    }

    void applyRequestIdZeroFailsImmediately_data()
    {
        QTest::addColumn<bool>("zeroApply");
        QTest::newRow("apply") << true;
        QTest::newRow("selected") << false;
    }

    void applyRequestIdZeroFailsImmediately()
    {
        QFETCH(bool, zeroApply);
        BootstrapHarness harness;
        harness.completeBootstrap();
        if (zeroApply)
            harness.applyId = 0;
        else
            harness.selectedId = 0;
        QVERIFY(!harness.controller.applyVci(approvedDevice()));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
        if (!zeroApply) {
            QCOMPARE(harness.trace.count(QStringLiteral("abort-rest:31")),
                     1);
        }
    }

    void onlyOneVciOperationMayOwnTheController()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.lookupVci());
        QVERIFY(!harness.controller.lookupVci());
        QVERIFY(!harness.controller.applyVci(approvedDevice()));
        QVERIFY(!harness.controller.closeVci());
        QCOMPARE(harness.trace.count(QStringLiteral("device-lookup")), 0);

        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        QCOMPARE(harness.trace.count(QStringLiteral("device-lookup")), 1);
    }

    void connectedFalseRevokesReadinessWithoutNetworkRetry()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        completeReadyApply(harness);
        const int writesBefore = harness.trace.size();
        QSignalSpy revoked(&harness.controller,
                           &KtmSessionController::vciReadinessRevoked);
        KtmSessionControllerTestAccess::stompMessage(
            harness.controller, vciStatusMessage(7, false, 0.0));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::SessionReady);
        QCOMPARE(revoked.count(), 1);
        QCOMPARE(harness.trace.size(), writesBefore);
        QVERIFY(!harness.controller.closeVci());
    }

    void falseStatusProjectionCannotStartClose()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        completeReadyApply(harness);
        bool closeAttempted = false;
        bool closeAccepted = true;
        QObject::connect(
            &harness.controller, &KtmSessionController::vciStatusChanged,
            &harness.controller,
            [&harness, &closeAttempted, &closeAccepted](
                const Xc2VciStatus &status) {
                if (!status.connected) {
                    closeAttempted = true;
                    closeAccepted = harness.controller.closeVci();
                }
            }, Qt::DirectConnection);

        KtmSessionControllerTestAccess::stompMessage(
            harness.controller, vciStatusMessage(7, false, 0.0));

        QVERIFY(closeAttempted);
        QVERIFY(!closeAccepted);
        QCOMPARE(harness.trace.count(QStringLiteral("close:vci-1")), 0);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::SessionReady);
    }

    void visibilityLossAndMalformedStatusRevokeWithoutReconnect_data()
    {
        QTest::addColumn<bool>("visibilityLoss");
        QTest::newRow("visibility-loss") << true;
        QTest::newRow("malformed-status") << false;
    }

    void visibilityLossAndMalformedStatusRevokeWithoutReconnect()
    {
        QFETCH(bool, visibilityLoss);
        BootstrapHarness harness;
        harness.completeBootstrap();
        completeReadyApply(harness);
        const int connectsBefore =
            harness.trace.count(QStringLiteral("stomp-connect"));
        QSignalSpy revoked(&harness.controller,
                           &KtmSessionController::vciReadinessRevoked);
        if (visibilityLoss) {
            Xc2Error error;
            error.category = Xc2ErrorCategory::Transport;
            error.message = QStringLiteral("synthetic visibility loss");
            KtmSessionControllerTestAccess::visibilityLost(
                harness.controller, 7, error);
        } else {
            Xc2StompMessage message = vciStatusMessage(7, true, 12.5);
            message.body = QByteArrayLiteral(R"({"voltage":12.5})");
            KtmSessionControllerTestAccess::stompMessage(
                harness.controller, message);
        }
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
        QCOMPARE(revoked.count(), 1);
        QCOMPARE(harness.trace.count(QStringLiteral("stomp-connect")),
                 connectsBefore);
    }

    void readinessRevocationProjectionCannotStartClose()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        completeReadyApply(harness);
        bool closeAttempted = false;
        bool closeAccepted = true;
        QObject::connect(
            &harness.controller,
            &KtmSessionController::vciReadinessRevoked,
            &harness.controller,
            [&harness, &closeAttempted, &closeAccepted](const Xc2Error &) {
                closeAttempted = true;
                closeAccepted = harness.controller.closeVci();
            }, Qt::DirectConnection);
        Xc2StompMessage message = vciStatusMessage(7, true, 12.5);
        message.body = QByteArrayLiteral(R"({"voltage":12.5})");

        KtmSessionControllerTestAccess::stompMessage(
            harness.controller, message);

        QVERIFY(closeAttempted);
        QVERIFY(!closeAccepted);
        QCOMPARE(harness.trace.count(QStringLiteral("close:vci-1")), 0);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
    }

    void failureQuiescesStompAndDiscardsSessionObjectsBeforeProjection()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QPointer<Xc2RestClient> oldRest =
            KtmSessionControllerTestAccess::restGuard(harness.controller);
        QPointer<Xc2StompClient> oldStomp =
            KtmSessionControllerTestAccess::stompGuard(harness.controller);
        QPointer<Xc2JobRegistry> oldRegistry =
            KtmSessionControllerTestAccess::registryGuard(
                harness.controller);
        QStringList failureTrace;
        QObject::connect(
            &harness.controller, &KtmSessionController::stateChanged,
            &harness.controller, [&failureTrace](KtmSessionState state) {
                if (state == KtmSessionState::Failed)
                    failureTrace.append(QStringLiteral("state-failed"));
            }, Qt::DirectConnection);
        QObject::connect(
            &harness.controller, &KtmSessionController::failed,
            &harness.controller, [&failureTrace](const Xc2Error &) {
                failureTrace.append(QStringLiteral("payload-failed"));
            }, Qt::DirectConnection);
        harness.trace.clear();
        Xc2StompMessage malformed =
            vciStatusMessage(harness.generation, true, 12.5);
        malformed.body = QByteArrayLiteral(R"({"voltage":12.5})");

        KtmSessionControllerTestAccess::stompMessage(
            harness.controller, malformed);

        QVERIFY(oldRest.isNull());
        QVERIFY(oldStomp.isNull());
        QVERIFY(oldRegistry.isNull());
        QCOMPARE(harness.trace,
                 QStringList({QStringLiteral("disconnect-stomp"),
                              QStringLiteral("abort-stomp"),
                              QStringLiteral("reset-session")}));
        QCOMPARE(failureTrace,
                 QStringList({QStringLiteral("state-failed"),
                              QStringLiteral("payload-failed")}));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
    }

    void failureTeardownRejectsNewCommandFromSynchronousDisconnect()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        bool lookupAccepted = true;
        KtmSessionControllerTestAccess::setDisconnectOp(
            harness.controller, [&harness, &lookupAccepted] {
                harness.trace.append(QStringLiteral("disconnect-stomp"));
                lookupAccepted = harness.controller.lookupVci();
            });
        Xc2StompMessage malformed =
            vciStatusMessage(harness.generation, true, 12.5);
        malformed.body = QByteArrayLiteral(R"({"voltage":12.5})");

        KtmSessionControllerTestAccess::stompMessage(
            harness.controller, malformed);

        QVERIFY(!lookupAccepted);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
        QCOMPARE(KtmSessionControllerTestAccess::operation(
                     harness.controller), KtmSessionOperation::None);
    }

    void productionErrorThenVisibilityMarksActiveRegistryBeforeFailure()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.lookupVci());
        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        KtmSessionControllerTestAccess::deviceLookupFinished(
            harness.controller, harness.lookupId,
            Xc2Result<Xc2JobAccepted>::success(
                {QStringLiteral("job-visibility")}));
        QPointer<Xc2JobRegistry> registry =
            KtmSessionControllerTestAccess::registryGuard(
                harness.controller);
        QVERIFY(registry);
        int visibilityChanges = 0;
        QObject::connect(
            registry, &Xc2JobRegistry::jobChanged, &harness.controller,
            [&visibilityChanges](const Xc2JobRecord &record) {
                if (record.jobId == QStringLiteral("job-visibility")
                    && record.visibilityLost) {
                    ++visibilityChanges;
                }
            }, Qt::DirectConnection);
        Xc2Error error;
        error.category = Xc2ErrorCategory::Transport;
        error.message = QStringLiteral("synthetic terminal transport loss");

        KtmSessionControllerTestAccess::stompErrorThenVisibility(
            harness.controller, harness.generation, error);

        QCOMPARE(visibilityChanges, 1);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
    }

    void vciReadyStateCannotBeOvertakenBeforePayload()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QStringList projection;
        bool closeAccepted = true;
        QObject::connect(
            &harness.controller, &KtmSessionController::stateChanged,
            &harness.controller,
            [&harness, &projection, &closeAccepted](KtmSessionState state) {
                if (state == KtmSessionState::VciReady) {
                    projection.append(QStringLiteral("state-ready"));
                    closeAccepted = harness.controller.closeVci();
                }
            }, Qt::DirectConnection);
        QObject::connect(
            &harness.controller, &KtmSessionController::vciReady,
            &harness.controller,
            [&projection](const Xc2VciDevice &, const Xc2VciStatus &) {
                projection.append(QStringLiteral("payload-ready"));
            }, Qt::DirectConnection);

        completeReadyApply(harness);

        QVERIFY(!closeAccepted);
        QCOMPARE(projection,
                 QStringList({QStringLiteral("state-ready"),
                              QStringLiteral("payload-ready")}));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::VciReady);
        QVERIFY(harness.controller.closeVci());
    }

    void lookupReadyStateCannotBeOvertakenBeforePayload()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.lookupVci());
        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        KtmSessionControllerTestAccess::deviceLookupFinished(
            harness.controller, harness.lookupId,
            Xc2Result<Xc2JobAccepted>::success(
                {QStringLiteral("job-projection")}));
        KtmSessionControllerTestAccess::stompMessage(
            harness.controller,
            progressMessage(harness.generation,
                            QStringLiteral("job-projection"),
                            Xc2JobState::Finished));
        QStringList projection;
        bool lookupAccepted = true;
        QObject::connect(
            &harness.controller, &KtmSessionController::stateChanged,
            &harness.controller,
            [&harness, &projection, &lookupAccepted](KtmSessionState state) {
                if (state == KtmSessionState::SessionReady) {
                    projection.append(QStringLiteral("state-ready"));
                    lookupAccepted = harness.controller.lookupVci();
                }
            }, Qt::DirectConnection);
        QObject::connect(
            &harness.controller, &KtmSessionController::vciLookupFinished,
            &harness.controller,
            [&projection](const QList<Xc2VciDevice> &) {
                projection.append(QStringLiteral("payload-lookup"));
            }, Qt::DirectConnection);

        KtmSessionControllerTestAccess::devicesFinished(
            harness.controller, harness.devicesId,
            Xc2Result<QList<Xc2VciDevice>>::success({approvedDevice()}));

        QVERIFY(!lookupAccepted);
        QCOMPARE(projection,
                 QStringList({QStringLiteral("state-ready"),
                              QStringLiteral("payload-lookup")}));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::SessionReady);
        QVERIFY(harness.controller.lookupVci());
    }

    void closeReadyStateCannotBeOvertakenBeforePayload()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        completeReadyApply(harness);
        QVERIFY(harness.controller.closeVci());
        QStringList projection;
        bool lookupAccepted = true;
        QObject::connect(
            &harness.controller, &KtmSessionController::stateChanged,
            &harness.controller,
            [&harness, &projection, &lookupAccepted](KtmSessionState state) {
                if (state == KtmSessionState::SessionReady) {
                    projection.append(QStringLiteral("state-ready"));
                    lookupAccepted = harness.controller.lookupVci();
                }
            }, Qt::DirectConnection);
        QObject::connect(
            &harness.controller, &KtmSessionController::vciClosed,
            &harness.controller, [&projection] {
                projection.append(QStringLiteral("payload-closed"));
            }, Qt::DirectConnection);

        KtmSessionControllerTestAccess::closeDeviceFinished(
            harness.controller, harness.closeId);

        QVERIFY(!lookupAccepted);
        QCOMPARE(projection,
                 QStringList({QStringLiteral("state-ready"),
                              QStringLiteral("payload-closed")}));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::SessionReady);
        QVERIFY(harness.controller.lookupVci());
    }

    void failedStateCannotBeOvertakenBeforePayload()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QStringList projection;
        bool stopStartedDuringState = true;
        QObject::connect(
            &harness.controller, &KtmSessionController::stateChanged,
            &harness.controller,
            [&harness, &projection, &stopStartedDuringState](
                KtmSessionState state) {
                if (state == KtmSessionState::Failed) {
                    projection.append(QStringLiteral("state-failed"));
                    harness.controller.stop();
                    stopStartedDuringState =
                        KtmSessionControllerTestAccess::stopping(
                            harness.controller);
                }
            }, Qt::DirectConnection);
        QObject::connect(
            &harness.controller, &KtmSessionController::failed,
            &harness.controller, [&projection](const Xc2Error &) {
                projection.append(QStringLiteral("payload-failed"));
            }, Qt::DirectConnection);
        Xc2StompMessage malformed =
            vciStatusMessage(harness.generation, true, 12.5);
        malformed.body = QByteArrayLiteral(R"({"voltage":12.5})");

        KtmSessionControllerTestAccess::stompMessage(
            harness.controller, malformed);

        QVERIFY(!stopStartedDuringState);
        QCOMPARE(projection,
                 QStringList({QStringLiteral("state-failed"),
                              QStringLiteral("payload-failed")}));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
        harness.controller.stop();
        QVERIFY(KtmSessionControllerTestAccess::stopping(
            harness.controller));
    }

    void nestedTransportFailureSupersedesTerminalProjection_data()
    {
        QTest::addColumn<int>("projectionKind");
        QTest::newRow("vci-ready") << 0;
        QTest::newRow("lookup-ready") << 1;
        QTest::newRow("close-ready") << 2;
    }

    void nestedTransportFailureSupersedesTerminalProjection()
    {
        QFETCH(int, projectionKind);
        BootstrapHarness harness;
        harness.completeBootstrap();
        if (projectionKind == 0) {
            QVERIFY(harness.controller.applyVci(approvedDevice()));
            KtmSessionControllerTestAccess::applyDeviceFinished(
                harness.controller, harness.applyId);
            KtmSessionControllerTestAccess::selectedDeviceFinished(
                harness.controller, harness.selectedId,
                Xc2Result<Xc2SelectedVci>::success({approvedDevice()}));
        } else if (projectionKind == 1) {
            QVERIFY(harness.controller.lookupVci());
            KtmSessionControllerTestAccess::subscriptionSent(
                harness.controller, Topic::Progress);
            KtmSessionControllerTestAccess::deviceLookupFinished(
                harness.controller, harness.lookupId,
                Xc2Result<Xc2JobAccepted>::success(
                    {QStringLiteral("job-nested-failure")}));
            KtmSessionControllerTestAccess::stompMessage(
                harness.controller,
                progressMessage(harness.generation,
                                QStringLiteral("job-nested-failure"),
                                Xc2JobState::Finished));
        } else {
            completeReadyApply(harness);
            QVERIFY(harness.controller.closeVci());
        }
        QPointer<Xc2RestClient> oldRest =
            KtmSessionControllerTestAccess::restGuard(harness.controller);
        QPointer<Xc2StompClient> oldStomp =
            KtmSessionControllerTestAccess::stompGuard(harness.controller);
        QPointer<Xc2JobRegistry> oldRegistry =
            KtmSessionControllerTestAccess::registryGuard(
                harness.controller);
        QSignalSpy ready(&harness.controller,
                         &KtmSessionController::vciReady);
        QSignalSpy lookupFinished(
            &harness.controller,
            &KtmSessionController::vciLookupFinished);
        QSignalSpy closed(&harness.controller,
                          &KtmSessionController::vciClosed);
        QSignalSpy failed(&harness.controller,
                          &KtmSessionController::failed);
        const KtmSessionState targetState = projectionKind == 0
            ? KtmSessionState::VciReady : KtmSessionState::SessionReady;
        bool nestedFailureDelivered = false;
        QObject::connect(
            &harness.controller, &KtmSessionController::stateChanged,
            &harness.controller,
            [&harness, targetState, &nestedFailureDelivered](
                KtmSessionState state) {
                if (state != targetState || nestedFailureDelivered)
                    return;
                nestedFailureDelivered = true;
                Xc2Error error;
                error.category = Xc2ErrorCategory::Transport;
                error.message = QStringLiteral("nested transport failure");
                KtmSessionControllerTestAccess::stompErrorThenVisibility(
                    harness.controller, harness.generation, error);
            }, Qt::DirectConnection);

        if (projectionKind == 0) {
            KtmSessionControllerTestAccess::stompMessage(
                harness.controller,
                vciStatusMessage(harness.generation, true, 12.4));
        } else if (projectionKind == 1) {
            KtmSessionControllerTestAccess::devicesFinished(
                harness.controller, harness.devicesId,
                Xc2Result<QList<Xc2VciDevice>>::success(
                    {approvedDevice()}));
        } else {
            KtmSessionControllerTestAccess::closeDeviceFinished(
                harness.controller, harness.closeId);
        }

        QVERIFY(nestedFailureDelivered);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
        QCOMPARE(KtmSessionControllerTestAccess::operation(
                     harness.controller), KtmSessionOperation::None);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(lookupFinished.count(), 0);
        QCOMPARE(closed.count(), 0);
        QVERIFY(oldRest.isNull());
        QVERIFY(oldStomp.isNull());
        QVERIFY(oldRegistry.isNull());
    }

    void staleRequestEpochAndGenerationCallbacksAreIgnored()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.applyVci(approvedDevice()));
        const quint64 sessionEpoch =
            KtmSessionControllerTestAccess::sessionEpoch(harness.controller);
        const quint64 selectionEpoch =
            KtmSessionControllerTestAccess::selectionEpoch(harness.controller);
        const quintptr identity =
            KtmSessionControllerTestAccess::restIdentity(harness.controller);

        KtmSessionControllerTestAccess::staleApplyDeviceFinished(
            harness.controller, sessionEpoch - 1, selectionEpoch,
            identity, harness.applyId);
        KtmSessionControllerTestAccess::staleApplyDeviceFinished(
            harness.controller, sessionEpoch, selectionEpoch - 1,
            identity, harness.applyId);
        KtmSessionControllerTestAccess::staleApplyDeviceFinished(
            harness.controller, sessionEpoch, selectionEpoch,
            identity + 1, harness.applyId);
        KtmSessionControllerTestAccess::staleApplyDeviceFinished(
            harness.controller, sessionEpoch, selectionEpoch,
            identity, harness.applyId + 1);
        KtmSessionControllerTestAccess::selectedDeviceFinished(
            harness.controller, harness.selectedId,
            Xc2Result<Xc2SelectedVci>::success({approvedDevice()}));
        KtmSessionControllerTestAccess::stompMessage(
            harness.controller, vciStatusMessage(6, true, 12.4));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::VciApplying);

        KtmSessionControllerTestAccess::applyDeviceFinished(
            harness.controller, harness.applyId);
        KtmSessionControllerTestAccess::stompMessage(
            harness.controller, vciStatusMessage(7, true, 12.4));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::VciReady);
    }

    void closeSendsConfirmedCompleteDto()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        const Xc2VciDevice confirmed = approvedDevice(
            QStringLiteral("Server Name"), QStringLiteral("server-module"));
        completeReadyApply(harness, confirmed);
        QSignalSpy closed(&harness.controller,
                          &KtmSessionController::vciClosed);
        QVERIFY(harness.controller.closeVci());
        QVERIFY(harness.closePayload.has_value());
        QCOMPARE(harness.closePayload->id, confirmed.id);
        QCOMPARE(harness.closePayload->name, confirmed.name);
        QCOMPARE(harness.closePayload->internalName, confirmed.internalName);
        QCOMPARE(harness.closePayload->additionalModuleInformation,
                 confirmed.additionalModuleInformation);
        KtmSessionControllerTestAccess::closeDeviceFinished(
            harness.controller, harness.closeId);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::SessionReady);
        QCOMPARE(closed.count(), 1);
    }

    void closeRequestIdZeroFailsImmediately()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        completeReadyApply(harness);
        harness.closeId = 0;
        QVERIFY(!harness.controller.closeVci());
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
    }

    void stopInvalidatesCallbacksResetsSessionThenStopsBackend()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.applyVci(approvedDevice()));
        const quint64 oldEpoch =
            KtmSessionControllerTestAccess::sessionEpoch(harness.controller);
        const QPointer<Xc2RestClient> oldRest =
            KtmSessionControllerTestAccess::restGuard(harness.controller);
        const QPointer<Xc2StompClient> oldStomp =
            KtmSessionControllerTestAccess::stompGuard(harness.controller);
        const QPointer<Xc2JobRegistry> oldRegistry =
            KtmSessionControllerTestAccess::registryGuard(harness.controller);
        harness.trace.clear();

        harness.controller.stop();
        QVERIFY(KtmSessionControllerTestAccess::state(harness.controller)
                != KtmSessionState::Stopped);
        QVERIFY(KtmSessionControllerTestAccess::sessionEpoch(
                    harness.controller) > oldEpoch);
        QVERIFY(oldRest.isNull());
        QVERIFY(oldStomp.isNull());
        QVERIFY(oldRegistry.isNull());
        QCOMPARE(harness.trace,
                 QStringList({QStringLiteral("abort-rest:31"),
                              QStringLiteral("abort-rest:32"),
                              QStringLiteral("disconnect-stomp"),
                              QStringLiteral("abort-stomp"),
                              QStringLiteral("reset-session"),
                              QStringLiteral("stop-backend")}));

        Xc2Error error;
        QVERIFY(!harness.controller.startProduction(
            QStringLiteral("C:/XC2"), &error));
        bool restartedFromStoppedSignal = false;
        QObject::connect(
            &harness.controller, &KtmSessionController::stateChanged,
            &harness.controller,
            [&harness, &restartedFromStoppedSignal](KtmSessionState state) {
                if (state == KtmSessionState::Stopped) {
                    restartedFromStoppedSignal =
                        harness.controller.startProduction(
                            QStringLiteral("C:/XC2"));
                }
            }, Qt::DirectConnection);
        harness.backendState = Xc2BackendState::Stopped;
        KtmSessionControllerTestAccess::backendStateChanged(
            harness.controller, Xc2BackendState::Stopped);
        QVERIFY(restartedFromStoppedSignal);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::BackendStarting);
    }

    void stopPendingRejectsCommandsAndCannotContaminateRestart_data()
    {
        QTest::addColumn<int>("command");
        QTest::newRow("lookup") << 0;
        QTest::newRow("apply") << 1;
        QTest::newRow("close") << 2;
    }

    void stopPendingRejectsCommandsAndCannotContaminateRestart()
    {
        QFETCH(int, command);
        BootstrapHarness harness;
        harness.completeBootstrap();
        if (command == 2)
            completeReadyApply(harness);

        harness.controller.stop();

        QVERIFY(KtmSessionControllerTestAccess::stopping(
            harness.controller));
        bool accepted = false;
        if (command == 0)
            accepted = harness.controller.lookupVci();
        else if (command == 1)
            accepted = harness.controller.applyVci(approvedDevice());
        else
            accepted = harness.controller.closeVci();
        QVERIFY(!accepted);
        QCOMPARE(KtmSessionControllerTestAccess::operation(
                     harness.controller), KtmSessionOperation::None);

        harness.backendState = Xc2BackendState::Stopped;
        QVERIFY(!harness.controller.startProduction(
            QStringLiteral("C:/XC2")));
        KtmSessionControllerTestAccess::backendStateChanged(
            harness.controller, Xc2BackendState::Stopped);
        QVERIFY(!KtmSessionControllerTestAccess::stopping(
            harness.controller));
        QVERIFY(harness.controller.startProduction(
            QStringLiteral("C:/XC2")));
        QCOMPARE(KtmSessionControllerTestAccess::operation(
                     harness.controller), KtmSessionOperation::None);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::BackendStarting);
    }

    void stopInsideSessionReadyStateSlotPreventsFurtherWork()
    {
        BootstrapHarness harness;
        QObject::connect(
            &harness.controller, &KtmSessionController::stateChanged,
            &harness.controller, [&harness](KtmSessionState state) {
                if (state == KtmSessionState::SessionReady)
                    harness.controller.stop();
            }, Qt::DirectConnection);
        QVERIFY(harness.start());
        harness.publishReady();
        harness.completeHealth();
        harness.completeUser();
        KtmSessionControllerTestAccess::stompConnected(
            harness.controller,
            KtmSessionControllerTestAccess::sessionEpoch(harness.controller),
            KtmSessionControllerTestAccess::stompIdentity(harness.controller),
            7);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::SessionReady);
        harness.backendState = Xc2BackendState::Stopped;
        KtmSessionControllerTestAccess::backendStateChanged(
            harness.controller, Xc2BackendState::Stopped);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
        QVERIFY(!harness.trace.contains(QStringLiteral("device-lookup")));
    }

    void stopInsideSynchronousSubscribePreventsFurtherFailure()
    {
        BootstrapHarness harness;
        int subscribeCalls = 0;
        KtmSessionControllerTestAccess::setSubscribeOp(
            harness.controller,
            [&harness, &subscribeCalls](Topic, Xc2Error *error) {
                ++subscribeCalls;
                if (error) {
                    error->category = Xc2ErrorCategory::Transport;
                    error->message = QStringLiteral("synthetic send failure");
                }
                harness.controller.stop();
                return false;
            });
        QVERIFY(harness.start());
        harness.publishReady();
        harness.completeHealth();
        harness.completeUser();
        KtmSessionControllerTestAccess::stompConnected(
            harness.controller,
            KtmSessionControllerTestAccess::sessionEpoch(harness.controller),
            KtmSessionControllerTestAccess::stompIdentity(harness.controller),
            7);
        QCOMPARE(subscribeCalls, 1);
        QVERIFY(KtmSessionControllerTestAccess::state(harness.controller)
                != KtmSessionState::Failed);
    }

    void oneParallelSelectionFailureAbortsTheOtherRequest_data()
    {
        QTest::addColumn<bool>("applyFails");
        QTest::newRow("apply-fails") << true;
        QTest::newRow("selected-fails") << false;
    }

    void oneParallelSelectionFailureAbortsTheOtherRequest()
    {
        QFETCH(bool, applyFails);
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.applyVci(approvedDevice()));
        Xc2Error failure;
        failure.category = Xc2ErrorCategory::Vci;
        failure.message = QStringLiteral("synthetic selection failure");
        if (applyFails) {
            KtmSessionControllerTestAccess::applyDeviceFinished(
                harness.controller, harness.applyId, failure);
            QCOMPARE(harness.trace.count(QStringLiteral("abort-rest:32")),
                     1);
        } else {
            KtmSessionControllerTestAccess::selectedDeviceFinished(
                harness.controller, harness.selectedId,
                Xc2Result<Xc2SelectedVci>::failure(failure));
            QCOMPARE(harness.trace.count(QStringLiteral("abort-rest:31")),
                     1);
        }
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Failed);
    }

    void stopFromOperationProjectionWinsOverFailure()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.applyVci(approvedDevice()));
        QObject::connect(
            &harness.controller, &KtmSessionController::operationChanged,
            &harness.controller, [&harness](KtmSessionOperation operation) {
                if (operation == KtmSessionOperation::None)
                    harness.controller.stop();
            }, Qt::DirectConnection);
        Xc2Error error;
        error.category = Xc2ErrorCategory::Vci;
        error.message = QStringLiteral("synthetic apply failure");
        KtmSessionControllerTestAccess::applyDeviceFinished(
            harness.controller, harness.applyId, error);
        QVERIFY(KtmSessionControllerTestAccess::state(harness.controller)
                != KtmSessionState::Failed);
        harness.backendState = Xc2BackendState::Stopped;
        KtmSessionControllerTestAccess::backendStateChanged(
            harness.controller, Xc2BackendState::Stopped);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
    }

    void synchronousConnectStopInvalidatesOuterBootstrap()
    {
        BootstrapHarness harness;
        QVERIFY(harness.start());
        harness.publishReady();
        harness.completeHealth();
        KtmSessionControllerTestAccess::setConnectOp(
            harness.controller, [&harness](Xc2Error *) {
                harness.controller.stop();
                return true;
            });

        harness.completeUser();

        QCOMPARE(KtmSessionControllerTestAccess::stompGeneration(
                     harness.controller), Xc2StompGeneration(0));
        QVERIFY(KtmSessionControllerTestAccess::state(harness.controller)
                != KtmSessionState::Failed);
    }

    void synchronousRegistryAcceptStopCannotRestoreLookupState()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.lookupVci());
        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        QPointer<Xc2JobRegistry> registry =
            KtmSessionControllerTestAccess::registryGuard(
                harness.controller);
        QVERIFY(registry);
        QObject::connect(
            registry, &Xc2JobRegistry::jobChanged, &harness.controller,
            [&harness](const Xc2JobRecord &) {
                harness.controller.stop();
            }, Qt::DirectConnection);

        KtmSessionControllerTestAccess::deviceLookupFinished(
            harness.controller, harness.lookupId,
            Xc2Result<Xc2JobAccepted>::success(
                {QStringLiteral("job-sync-stop")}));

        QCOMPARE(KtmSessionControllerTestAccess::lookupJobId(
                     harness.controller), QString());
        QCOMPARE(KtmSessionControllerTestAccess::operation(
                     harness.controller), KtmSessionOperation::None);
        QCOMPARE(KtmSessionControllerTestAccess::stompGeneration(
                     harness.controller), Xc2StompGeneration(0));
    }

    void synchronousDisconnectRestartStopsOuterTeardown()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QObject::connect(
            &harness.controller, &KtmSessionController::stateChanged,
            &harness.controller, [&harness](KtmSessionState state) {
                if (state == KtmSessionState::Stopped) {
                    QVERIFY(harness.controller.startProduction(
                        QStringLiteral("C:/XC2")));
                }
            }, Qt::DirectConnection);
        KtmSessionControllerTestAccess::setDisconnectOp(
            harness.controller, [&harness] {
                harness.trace.append(QStringLiteral("disconnect-stomp"));
                harness.backendState = Xc2BackendState::Stopped;
                KtmSessionControllerTestAccess::backendStateChanged(
                    harness.controller, Xc2BackendState::Stopped);
            });
        harness.trace.clear();

        harness.controller.stop();

        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::BackendStarting);
        QVERIFY(!KtmSessionControllerTestAccess::stopping(
            harness.controller));
        const int restarted =
            harness.trace.indexOf(QStringLiteral("start:C:/XC2"));
        QVERIFY(restarted >= 0);
        QCOMPARE(harness.trace.mid(restarted + 1), QStringList());
    }

    void destroyedSessionChildCannotReenterResetTransaction()
    {
        BootstrapHarness harness;
        QPointer<Xc2RestClient> oldRest =
            KtmSessionControllerTestAccess::restGuard(harness.controller);
        QVERIFY(oldRest);
        int destroyedReentryCount = 0;
        QObject::connect(
            oldRest, &QObject::destroyed, &harness.controller,
            [&harness, &destroyedReentryCount] {
                ++destroyedReentryCount;
                harness.controller.stop();
            }, Qt::DirectConnection);
        harness.trace.clear();

        QVERIFY(!harness.start());

        QCOMPARE(destroyedReentryCount, 1);
        QVERIFY(!harness.trace.contains(QStringLiteral("start:C:/XC2")));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::Stopped);
        QVERIFY(!KtmSessionControllerTestAccess::stopping(
            harness.controller));

        harness.trace.clear();
        QVERIFY(harness.start());
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::BackendStarting);
    }

    void deletingControllerInsideStateAndFailureSignalsIsSafe_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("session-ready-state") << 0;
        QTest::newRow("vci-ready-state") << 1;
        QTest::newRow("failure-result") << 2;
    }

    void deletingControllerInsideStateAndFailureSignalsIsSafe()
    {
        QFETCH(int, mode);
        auto *controller = new KtmSessionController;
        QPointer<KtmSessionController> guard(controller);
        if (mode == 0) {
            QObject::connect(
                controller, &KtmSessionController::stateChanged, this,
                [controller](KtmSessionState state) {
                    if (state == KtmSessionState::SessionReady)
                        delete controller;
                }, Qt::DirectConnection);
            KtmSessionControllerTestAccess::publishState(
                *controller, KtmSessionState::SessionReady);
            QVERIFY(guard.isNull());
        } else if (mode == 1) {
            int readyProjectionCount = 0;
            QObject::connect(
                controller, &KtmSessionController::vciReady, this,
                [&readyProjectionCount](const Xc2VciDevice &,
                                        const Xc2VciStatus &) {
                    ++readyProjectionCount;
                });
            QObject::connect(
                controller, &KtmSessionController::stateChanged, this,
                [controller](KtmSessionState state) {
                    if (state == KtmSessionState::VciReady)
                        delete controller;
                }, Qt::DirectConnection);
            KtmSessionControllerTestAccess::publishSyntheticReady(*controller);
            QVERIFY(guard.isNull());
            QCOMPARE(readyProjectionCount, 0);
        } else {
            QObject::connect(
                controller, &KtmSessionController::failed, this,
                [controller](const Xc2Error &) { delete controller; },
                Qt::DirectConnection);
            Xc2Error error;
            error.category = Xc2ErrorCategory::Session;
            error.message = QStringLiteral("synthetic failure");
            KtmSessionControllerTestAccess::failSession(*controller, error);
            QVERIFY(guard.isNull());
        }
    }

    void restSignalWiringRetainsRequestSelectionEpoch()
    {
        BootstrapHarness harness;
        harness.completeBootstrap();
        QVERIFY(harness.controller.applyVci(approvedDevice()));
        KtmSessionControllerTestAccess::advanceSelectionEpoch(
            harness.controller);

        KtmSessionControllerTestAccess::applyDeviceFinished(
            harness.controller, harness.applyId);
        KtmSessionControllerTestAccess::selectedDeviceFinished(
            harness.controller, harness.selectedId,
            Xc2Result<Xc2SelectedVci>::success({approvedDevice()}));
        KtmSessionControllerTestAccess::stompMessage(
            harness.controller, vciStatusMessage(7, true, 12.4));
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::VciApplying);
    }

    void productionChildSignalsDriveOwnedBootstrapAndLookup()
    {
        BootstrapHarness harness;
        QVERIFY(harness.start());
        QPointer<Xc2BackendManager> backend =
            KtmSessionControllerTestAccess::backendGuard(
                harness.controller);
        QPointer<Xc2RestClient> rest =
            KtmSessionControllerTestAccess::restGuard(harness.controller);
        QPointer<Xc2StompClient> stomp =
            KtmSessionControllerTestAccess::stompGuard(harness.controller);
        QPointer<Xc2JobRegistry> registry =
            KtmSessionControllerTestAccess::registryGuard(
                harness.controller);
        QVERIFY(backend && rest && stomp && registry);
        bool connectSawOwnedChildren = false;
        KtmSessionControllerTestAccess::setConnectOp(
            harness.controller,
            [&harness, rest, stomp, &connectSawOwnedChildren](Xc2Error *) {
                harness.trace.append(QStringLiteral("stomp-connect"));
                connectSawOwnedChildren =
                    rest == KtmSessionControllerTestAccess::restGuard(
                                harness.controller)
                    && stomp == KtmSessionControllerTestAccess::stompGuard(
                                    harness.controller);
                return true;
            });

        harness.backendState = Xc2BackendState::Ready;
        KtmSessionControllerTestAccess::emitBackendStateChanged(
            harness.controller, Xc2BackendState::Ready);
        KtmSessionControllerTestAccess::emitBackendReady(
            harness.controller, endpoints());
        KtmSessionControllerTestAccess::emitServiceStatusFinished(
            harness.controller, harness.serviceStatusId,
            Xc2Result<Xc2ServiceStatus>::success({true}));
        KtmSessionControllerTestAccess::emitCurrentUserFinished(
            harness.controller, harness.currentUserId,
            Xc2Result<Xc2CurrentUser>::success(authorizedUser()));
        QVERIFY(connectSawOwnedChildren);
        KtmSessionControllerTestAccess::emitStompConnected(
            harness.controller, harness.generation);
        QCOMPARE(KtmSessionControllerTestAccess::state(harness.controller),
                 KtmSessionState::SessionReady);

        QSignalSpy lookupFinished(
            &harness.controller,
            &KtmSessionController::vciLookupFinished);
        QVERIFY(harness.controller.lookupVci());
        KtmSessionControllerTestAccess::subscriptionSent(
            harness.controller, Topic::Progress);
        KtmSessionControllerTestAccess::deviceLookupFinished(
            harness.controller, harness.lookupId,
            Xc2Result<Xc2JobAccepted>::success(
                {QStringLiteral("job-wiring")}));
        KtmSessionControllerTestAccess::stompMessage(
            harness.controller,
            progressMessage(harness.generation,
                            QStringLiteral("job-wiring"),
                            Xc2JobState::Finished));
        KtmSessionControllerTestAccess::devicesFinished(
            harness.controller, harness.devicesId,
            Xc2Result<QList<Xc2VciDevice>>::success({approvedDevice()}));

        QCOMPARE(lookupFinished.count(), 1);
        QCOMPARE(rest,
                 KtmSessionControllerTestAccess::restGuard(
                     harness.controller));
        QCOMPARE(stomp,
                 KtmSessionControllerTestAccess::stompGuard(
                     harness.controller));
        QCOMPARE(registry,
                 KtmSessionControllerTestAccess::registryGuard(
                     harness.controller));
    }

    void productionStopDrivesRealStompTerminalAndReplacesChildren()
    {
        KtmSessionController controller;
        Xc2BackendState backendState = Xc2BackendState::Stopped;
        QStringList trace;
        KtmSessionControllerTestAccess::Ops ops;
        ops.backendState = [&backendState] { return backendState; };
        ops.startBackend = [](const QString &, Xc2Error *) { return true; };
        ops.requestServiceStatus = [] { return Xc2RequestId(11); };
        ops.requestCurrentUser = [] { return Xc2RequestId(12); };
        ops.stopBackend = [&trace] {
            trace.append(QStringLiteral("stop-backend"));
        };
        ops.sessionObjectsReset = [&trace] {
            trace.append(QStringLiteral("reset-session"));
        };
        KtmSessionControllerTestAccess::installOps(controller,
                                                   std::move(ops));
        QVERIFY(controller.startProduction(QStringLiteral("C:/XC2")));
        backendState = Xc2BackendState::Ready;
        KtmSessionControllerTestAccess::emitBackendStateChanged(
            controller, Xc2BackendState::Ready);
        KtmSessionControllerTestAccess::emitBackendReady(
            controller,
            {QUrl(QString::fromLatin1(kRestBase)),
             QUrl(QStringLiteral(
                 "ws://127.0.0.1:49152/xc2-websocket"))});
        QPointer<Xc2RestClient> oldRest =
            KtmSessionControllerTestAccess::restGuard(controller);
        QPointer<Xc2StompClient> oldStomp =
            KtmSessionControllerTestAccess::stompGuard(controller);
        QPointer<Xc2JobRegistry> oldRegistry =
            KtmSessionControllerTestAccess::registryGuard(controller);
        QStringList stompTerminal;
        QObject::connect(
            oldStomp, &Xc2StompClient::stateChanged, &controller,
            [&stompTerminal](Xc2StompGeneration, Xc2StompState state) {
                if (state == Xc2StompState::Failed
                    || state == Xc2StompState::Disconnected) {
                    stompTerminal.append(QStringLiteral("state-terminal"));
                }
            }, Qt::DirectConnection);
        QObject::connect(
            oldStomp, &Xc2StompClient::errorOccurred, &controller,
            [&stompTerminal](Xc2StompGeneration, const Xc2Error &) {
                stompTerminal.append(QStringLiteral("error"));
            }, Qt::DirectConnection);
        QObject::connect(
            oldStomp, &Xc2StompClient::disconnected, &controller,
            [&stompTerminal](Xc2StompGeneration) {
                stompTerminal.append(QStringLiteral("disconnected"));
            }, Qt::DirectConnection);
        KtmSessionControllerTestAccess::emitServiceStatusFinished(
            controller, 11,
            Xc2Result<Xc2ServiceStatus>::success({true}));
        Xc2Error connectError;
        QVERIFY2(KtmSessionControllerTestAccess::connectRealStompChild(
                     controller, &connectError),
                 qPrintable(connectError.message));
        QCOMPARE(KtmSessionControllerTestAccess::realStompGeneration(
                     controller),
                 Xc2StompGeneration(1));

        controller.stop();

        QVERIFY(oldRest.isNull());
        QVERIFY(oldStomp.isNull());
        QVERIFY(oldRegistry.isNull());
        QCOMPARE(stompTerminal,
                 QStringList({QStringLiteral("state-terminal"),
                              QStringLiteral("error"),
                              QStringLiteral("disconnected")}));
        QCOMPARE(trace,
                 QStringList({QStringLiteral("reset-session"),
                              QStringLiteral("stop-backend")}));
        QVERIFY(KtmSessionControllerTestAccess::restGuard(controller));
        QVERIFY(KtmSessionControllerTestAccess::stompGuard(controller));
        QVERIFY(KtmSessionControllerTestAccess::registryGuard(controller));
        QVERIFY(KtmSessionControllerTestAccess::stopping(controller));
    }
};

QTEST_GUILESS_MAIN(KtmSessionControllerTest)
#include "test_KtmSessionController.moc"
