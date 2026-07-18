#pragma once

#include "xc2/Xc2BackendManager.h"
#include "xc2/Xc2JobRegistry.h"
#include "xc2/Xc2RestClient.h"
#include "xc2/Xc2StompClient.h"

#include <QObject>
#include <QUrl>

#include <functional>
#include <optional>

namespace ktm {

enum class KtmSessionState {
    Stopped,
    BackendStarting,
    BackendReady,
    SessionStarting,
    SessionReady,
    VciLookup,
    VciApplying,
    VciReady,
    VciClosing,
    Failed
};

enum class KtmSessionOperation { None, Lookup, Apply, Close };

class KtmSessionControllerTestAccess;

class KtmSessionController final : public QObject {
    Q_OBJECT

public:
    explicit KtmSessionController(QObject *parent = nullptr);

    bool startProduction(const QString &installRoot,
                         xc2::Xc2Error *error = nullptr);
    bool lookupVci(xc2::Xc2Error *error = nullptr);
    bool applyVci(const xc2::Xc2VciDevice &device,
                  xc2::Xc2Error *error = nullptr);
    bool closeVci(xc2::Xc2Error *error = nullptr);
    void stop();

signals:
    void stateChanged(ktm::KtmSessionState state);
    void operationChanged(ktm::KtmSessionOperation operation);
    void failed(const ktm::xc2::Xc2Error &error);
    void vciLookupFinished(const QList<ktm::xc2::Xc2VciDevice> &devices);
    void vciReady(const ktm::xc2::Xc2VciDevice &device,
                  const ktm::xc2::Xc2VciStatus &status);
    void vciStatusChanged(const ktm::xc2::Xc2VciStatus &status);
    void vciReadinessRevoked(const ktm::xc2::Xc2Error &error);
    void vciClosed();

private:
    friend class KtmSessionControllerTestAccess;

    struct PrivateTestOps {
        std::function<xc2::Xc2BackendState()> backendState;
        std::function<bool(const QString &, xc2::Xc2Error *)> startBackend;
        std::function<bool(const QByteArray &, xc2::Xc2Error *)>
            setRestBaseUrl;
        std::function<QUrl()> restWebSocketUrl;
        std::function<xc2::Xc2RequestId()> requestServiceStatus;
        std::function<xc2::Xc2RequestId()> requestCurrentUser;
        std::function<bool(xc2::Xc2Error *)> connectStomp;
        std::function<bool(xc2::Topic, xc2::Xc2Error *)> subscribeStomp;
        std::function<xc2::Xc2StompGeneration()> stompGeneration;
        std::function<xc2::Xc2RequestId()> requestDeviceLookup;
        std::function<xc2::Xc2RequestId()> requestDevices;
        std::function<xc2::Xc2RequestId(const xc2::Xc2VciDevice &)>
            requestApplyDevice;
        std::function<xc2::Xc2RequestId()> requestSelectedDevice;
        std::function<xc2::Xc2RequestId(const xc2::Xc2VciDevice &)>
            requestCloseDevice;
        std::function<void(xc2::Xc2RequestId)> abortRest;
        std::function<void()> disconnectStomp;
        std::function<void()> abortStomp;
        std::function<void()> sessionObjectsReset;
        std::function<void()> stopBackend;
    };

    bool publishState(KtmSessionState state);
    bool publishOperation(KtmSessionOperation operation);
    bool failSession(const xc2::Xc2Error &error);
    bool failLocal(xc2::Xc2ErrorCategory category, const QString &message);
    void resetSessionObjects(bool notifyTest = false);
    bool callbackMatches(quint64 epoch, quintptr identity,
                         xc2::Xc2RequestId id) const;

    xc2::Xc2BackendState backendState() const;
    bool startBackend(const QString &installRoot, xc2::Xc2Error *error);
    bool setRestBaseUrl(const QByteArray &encodedBase,
                        xc2::Xc2Error *error);
    QUrl restWebSocketUrl() const;
    xc2::Xc2RequestId requestServiceStatus();
    xc2::Xc2RequestId requestCurrentUser();
    xc2::Xc2RequestId requestDeviceLookup();
    xc2::Xc2RequestId requestDevices();
    xc2::Xc2RequestId requestApplyDevice(
        const xc2::Xc2VciDevice &device);
    xc2::Xc2RequestId requestSelectedDevice();
    xc2::Xc2RequestId requestCloseDevice(
        const xc2::Xc2VciDevice &device);
    void abortRest(xc2::Xc2RequestId id);
    void disconnectStomp();
    void abortStomp();
    void stopBackend();
    bool connectStomp(xc2::Xc2Error *error);
    bool subscribeStomp(xc2::Topic topic, xc2::Xc2Error *error);
    xc2::Xc2StompGeneration stompGeneration() const;

    void handleBackendStateChanged(xc2::Xc2BackendState state);
    void handleBackendReady(const xc2::Xc2BackendEndpoints &endpoints);
    void handleBackendFailed(const xc2::Xc2Error &error);
    void handleServiceStatusFinished(
        quint64 epoch, quintptr identity, xc2::Xc2RequestId id,
        const xc2::Xc2Result<xc2::Xc2ServiceStatus> &result);
    void handleCurrentUserFinished(
        quint64 epoch, quintptr identity, xc2::Xc2RequestId id,
        const xc2::Xc2Result<xc2::Xc2CurrentUser> &result);
    void handleStompConnected(quint64 epoch, quintptr identity,
                              const xc2::Xc2StompSession &session);
    void handleSubscriptionSent(quint64 epoch, quintptr identity,
                                xc2::Topic topic);
    void handleStompMessage(quint64 epoch, quintptr identity,
                            const xc2::Xc2StompMessage &message);
    void handleVciStatus(quint64 epoch, quint64 selectionEpoch,
                         quintptr identity,
                         const xc2::Xc2StompMessage &message);
    void handleStompError(quint64 epoch, quintptr identity,
                          xc2::Xc2StompGeneration generation,
                          const xc2::Xc2Error &error);
    void handleVisibilityLost(quint64 epoch, quintptr identity,
                              xc2::Xc2StompGeneration generation,
                              const xc2::Xc2Error &error);
    void handleDeviceLookupFinished(
        quint64 epoch, quintptr identity, xc2::Xc2RequestId id,
        const xc2::Xc2Result<xc2::Xc2JobAccepted> &result);
    void handleDevicesFinished(
        quint64 epoch, quintptr identity, xc2::Xc2RequestId id,
        const xc2::Xc2Result<QList<xc2::Xc2VciDevice>> &result);
    void handleApplyDeviceFinished(
        quint64 epoch, quint64 selectionEpoch, quintptr identity,
        xc2::Xc2RequestId id, const xc2::Xc2Error &error);
    void handleSelectedDeviceFinished(
        quint64 epoch, quint64 selectionEpoch, quintptr identity,
        xc2::Xc2RequestId id,
        const xc2::Xc2Result<xc2::Xc2SelectedVci> &result);
    void handleCloseDeviceFinished(
        quint64 epoch, quint64 selectionEpoch, quintptr identity,
        xc2::Xc2RequestId id, const xc2::Xc2Error &error);
    void handleJobTerminal(quint64 epoch, quintptr identity,
                           const xc2::Xc2JobRecord &record);
    bool startDeferredLookup();
    bool requestLookupDevices();
    void processLookupTerminal(const xc2::Xc2JobRecord &record);
    void tryPublishVciReady();
    bool failWithReadinessRevoked(const xc2::Xc2Error &error);
    void revokeReadinessToSession(const xc2::Xc2Error &error);

    PrivateTestOps m_testOps;
    xc2::Xc2BackendManager *m_backend = nullptr;
    xc2::Xc2RestClient *m_restClient = nullptr;
    xc2::Xc2StompClient *m_stompClient = nullptr;
    xc2::Xc2JobRegistry *m_jobRegistry = nullptr;
    KtmSessionState m_state = KtmSessionState::Stopped;
    KtmSessionOperation m_operation = KtmSessionOperation::None;
    quint64 m_sessionEpoch = 0;
    quint64 m_selectionEpoch = 0;
    xc2::Xc2StompGeneration m_stompGeneration = 0;
    xc2::Xc2RequestId m_requestId = 0;
    QString m_lookupJobId;
    bool m_progressSubscriptionSent = false;
    bool m_lookupRequestStarted = false;
    bool m_lookupDevicesRequested = false;
    xc2::Xc2RequestId m_applyRequestId = 0;
    xc2::Xc2RequestId m_selectedRequestId = 0;
    xc2::Xc2RequestId m_closeRequestId = 0;
    quint64 m_applyRequestSelectionEpoch = 0;
    quint64 m_selectedRequestSelectionEpoch = 0;
    quint64 m_closeRequestSelectionEpoch = 0;
    std::optional<xc2::Xc2VciDevice> m_applyTarget;
    std::optional<xc2::Xc2VciDevice> m_confirmedDevice;
    std::optional<xc2::Xc2VciStatus> m_latestVciStatus;
    bool m_applySucceeded = false;
    bool m_selectedConfirmed = false;
    bool m_connectedForSelectionEpoch = false;
    bool m_stopping = false;
};

} // namespace ktm

Q_DECLARE_METATYPE(ktm::KtmSessionState)
Q_DECLARE_METATYPE(ktm::KtmSessionOperation)
