#include "ktm/KtmSessionController.h"
#include "ktm/xc2/Xc2ContractProfile.h"

#include <QPointer>
#include <QSignalSpy>
#include <QtTest>

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
