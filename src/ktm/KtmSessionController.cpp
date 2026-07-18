#include "KtmSessionController.h"
#include "xc2/Xc2ContractProfile.h"
#include "xc2/Xc2JsonCodec.h"

#include <QPointer>

namespace ktm {

namespace {

constexpr auto kRequiredPermission = "EcuDiagnosticRead";

xc2::Xc2Error localError(xc2::Xc2ErrorCategory category,
                         const QString &message)
{
    xc2::Xc2Error error;
    error.category = category;
    error.message = message;
    return error;
}

bool sameEncodedUrl(const QUrl &left, const QUrl &right)
{
    return left.toEncoded(QUrl::FullyEncoded)
        == right.toEncoded(QUrl::FullyEncoded);
}

} // namespace

KtmSessionController::KtmSessionController(QObject *parent)
    : QObject(parent), m_backend(new xc2::Xc2BackendManager(this))
{
    qRegisterMetaType<KtmSessionState>();
    qRegisterMetaType<KtmSessionOperation>();

    connect(m_backend, &xc2::Xc2BackendManager::stateChanged,
            this, [this](xc2::Xc2BackendState state) {
        handleBackendStateChanged(state);
    });
    connect(m_backend, &xc2::Xc2BackendManager::ready,
            this, [this](const xc2::Xc2BackendEndpoints &endpoints) {
        handleBackendReady(endpoints);
    });
    connect(m_backend, &xc2::Xc2BackendManager::failed,
            this, [this](const xc2::Xc2Error &error) {
        handleBackendFailed(error);
    });
    resetSessionObjects();
}

bool KtmSessionController::startProduction(const QString &installRoot,
                                           xc2::Xc2Error *error)
{
    if (m_resetting || m_stopping || m_projectingPayload || m_failing)
        return false;
    const quint64 entryEpoch = m_sessionEpoch;
    const KtmSessionState entryState = m_state;
    const KtmSessionOperation entryOperation = m_operation;
    QPointer<KtmSessionController> entryGuard(this);
    QPointer<xc2::Xc2BackendManager> backendGuard = m_backend;
    const xc2::Xc2BackendState observedBackendState = backendState();
    if (!entryGuard || entryEpoch != m_sessionEpoch
        || !backendGuard || backendGuard != m_backend
        || entryState != m_state || entryOperation != m_operation)
        return false;
    if (observedBackendState != xc2::Xc2BackendState::Stopped) {
        const xc2::Xc2Error failure = localError(
            xc2::Xc2ErrorCategory::Backend,
            QStringLiteral("An XC2 backend run is already active"));
        if (error)
            *error = failure;
        return false;
    }

    if (m_operation != KtmSessionOperation::None) {
        m_stopping = true;
        const bool normalized =
            publishOperation(KtmSessionOperation::None);
        if (!entryGuard)
            return false;
        m_stopping = false;
        if (!normalized || entryEpoch != m_sessionEpoch
            || entryState != m_state || !backendGuard
            || backendGuard != m_backend
            || m_operation != KtmSessionOperation::None) {
            return false;
        }
    }

    if (error)
        *error = {};
    ++m_sessionEpoch;
    const quint64 epoch = m_sessionEpoch;
    ++m_selectionEpoch;
    m_requestId = 0;
    m_stompGeneration = 0;
    if (!resetSessionObjects())
        return false;
    if (!entryGuard || !backendGuard || backendGuard != m_backend
        || epoch != m_sessionEpoch || m_state != entryState
        || m_operation != KtmSessionOperation::None) {
        return false;
    }
    QPointer<KtmSessionController> guard(this);
    if (!publishState(KtmSessionState::BackendStarting) || !guard
        || epoch != m_sessionEpoch || !backendGuard
        || backendGuard != m_backend
        || m_state != KtmSessionState::BackendStarting
        || m_operation != KtmSessionOperation::None) {
        return false;
    }

    xc2::Xc2Error failure;
    const bool started = startBackend(installRoot, &failure);
    if (!guard || epoch != m_sessionEpoch
        || !backendGuard || backendGuard != m_backend
        || m_state != KtmSessionState::BackendStarting
        || m_operation != KtmSessionOperation::None)
        return false;
    if (!started) {
        if (error)
            *error = failure;
        if (guard && epoch == m_sessionEpoch)
            failSession(failure);
        return false;
    }
    return true;
}

bool KtmSessionController::lookupVci(xc2::Xc2Error *error)
{
    if (m_resetting || m_stopping || m_projectingPayload || m_failing
        || m_state != KtmSessionState::SessionReady
        || m_operation != KtmSessionOperation::None) {
        const xc2::Xc2Error failure = localError(
            xc2::Xc2ErrorCategory::Vci,
            QStringLiteral("Another VCI operation is active"));
        if (error)
            *error = failure;
        return false;
    }
    if (error)
        *error = {};
    m_lookupJobId.clear();
    m_lookupRequestStarted = false;
    m_lookupDevicesRequested = false;
    const quint64 epoch = m_sessionEpoch;
    QPointer<KtmSessionController> guard(this);
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    if (!publishOperation(KtmSessionOperation::Lookup) || !guard
        || epoch != m_sessionEpoch || !restGuard
        || restGuard != m_restClient
        || m_state != KtmSessionState::SessionReady
        || m_operation != KtmSessionOperation::Lookup) {
        return false;
    }
    if (!publishState(KtmSessionState::VciLookup) || !guard
        || epoch != m_sessionEpoch || !restGuard
        || restGuard != m_restClient
        || m_state != KtmSessionState::VciLookup
        || m_operation != KtmSessionOperation::Lookup) {
        return false;
    }
    if (m_progressSubscriptionSent)
        return startDeferredLookup();
    return true;
}

bool KtmSessionController::applyVci(const xc2::Xc2VciDevice &device,
                                    xc2::Xc2Error *error)
{
    if (m_resetting || m_stopping || m_projectingPayload || m_failing)
        return false;
    const QString approvedProvider =
        xc2::Xc2ContractProfile::approved().supportedPduApiShortName();
    if (device.id.trimmed().isEmpty()
        || device.internalName != approvedProvider) {
        const xc2::Xc2Error failure = localError(
            xc2::Xc2ErrorCategory::Vci,
            QStringLiteral("VCI identity or provider is not approved"));
        if (error)
            *error = failure;
        return false;
    }
    if (m_state != KtmSessionState::SessionReady
        || m_operation != KtmSessionOperation::None) {
        const xc2::Xc2Error failure = localError(
            xc2::Xc2ErrorCategory::Vci,
            QStringLiteral("Another VCI operation is active"));
        if (error)
            *error = failure;
        return false;
    }
    if (error)
        *error = {};

    ++m_selectionEpoch;
    const quint64 sessionEpoch = m_sessionEpoch;
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    const quint64 selectionEpoch = m_selectionEpoch;
    m_applyTarget = device;
    m_confirmedDevice.reset();
    m_latestVciStatus.reset();
    m_applySucceeded = false;
    m_selectedConfirmed = false;
    m_connectedForSelectionEpoch = false;
    m_applyRequestId = 0;
    m_selectedRequestId = 0;
    m_applyRequestSelectionEpoch = selectionEpoch;
    m_selectedRequestSelectionEpoch = selectionEpoch;
    QPointer<KtmSessionController> guard(this);
    if (!publishOperation(KtmSessionOperation::Apply) || !guard
        || sessionEpoch != m_sessionEpoch
        || selectionEpoch != m_selectionEpoch
        || !restGuard || restGuard != m_restClient
        || m_state != KtmSessionState::SessionReady
        || m_operation != KtmSessionOperation::Apply) {
        return false;
    }
    if (!publishState(KtmSessionState::VciApplying) || !guard
        || sessionEpoch != m_sessionEpoch
        || selectionEpoch != m_selectionEpoch
        || !restGuard || restGuard != m_restClient
        || m_state != KtmSessionState::VciApplying
        || m_operation != KtmSessionOperation::Apply) {
        return false;
    }

    const xc2::Xc2RequestId applyRequestId = requestApplyDevice(device);
    if (!guard || sessionEpoch != m_sessionEpoch
        || selectionEpoch != m_selectionEpoch
        || !restGuard || restGuard != m_restClient
        || m_operation != KtmSessionOperation::Apply
        || m_state != KtmSessionState::VciApplying) {
        return false;
    }
    m_applyRequestId = applyRequestId;
    if (applyRequestId == 0) {
        m_applyRequestSelectionEpoch = 0;
        failLocal(xc2::Xc2ErrorCategory::Transport,
                  QStringLiteral("Failed to start VCI apply request"));
        return false;
    }
    const xc2::Xc2RequestId selectedRequestId = requestSelectedDevice();
    if (!guard || sessionEpoch != m_sessionEpoch
        || selectionEpoch != m_selectionEpoch
        || !restGuard || restGuard != m_restClient
        || m_operation != KtmSessionOperation::Apply
        || m_state != KtmSessionState::VciApplying) {
        return false;
    }
    m_selectedRequestId = selectedRequestId;
    if (selectedRequestId == 0) {
        m_selectedRequestSelectionEpoch = 0;
        failLocal(xc2::Xc2ErrorCategory::Transport,
                  QStringLiteral("Failed to start selected-VCI request"));
        return false;
    }
    return true;
}

bool KtmSessionController::closeVci(xc2::Xc2Error *error)
{
    if (m_resetting || m_stopping || m_projectingPayload || m_failing
        || m_state != KtmSessionState::VciReady
        || m_operation != KtmSessionOperation::None
        || !m_confirmedDevice) {
        const xc2::Xc2Error failure = localError(
            xc2::Xc2ErrorCategory::Vci,
            QStringLiteral("No confirmed VCI is ready to close"));
        if (error)
            *error = failure;
        return false;
    }
    if (error)
        *error = {};
    const quint64 sessionEpoch = m_sessionEpoch;
    const quint64 selectionEpoch = m_selectionEpoch;
    const xc2::Xc2VciDevice confirmed = *m_confirmedDevice;
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    QPointer<KtmSessionController> guard(this);
    if (!publishOperation(KtmSessionOperation::Close) || !guard
        || sessionEpoch != m_sessionEpoch
        || selectionEpoch != m_selectionEpoch
        || !restGuard || restGuard != m_restClient
        || m_state != KtmSessionState::VciReady
        || m_operation != KtmSessionOperation::Close) {
        return false;
    }
    if (!publishState(KtmSessionState::VciClosing) || !guard
        || sessionEpoch != m_sessionEpoch
        || selectionEpoch != m_selectionEpoch
        || !restGuard || restGuard != m_restClient
        || m_state != KtmSessionState::VciClosing
        || m_operation != KtmSessionOperation::Close) {
        return false;
    }
    m_closeRequestSelectionEpoch = selectionEpoch;
    const xc2::Xc2RequestId closeRequestId = requestCloseDevice(confirmed);
    if (!guard || sessionEpoch != m_sessionEpoch
        || selectionEpoch != m_selectionEpoch
        || !restGuard || restGuard != m_restClient
        || m_operation != KtmSessionOperation::Close
        || m_state != KtmSessionState::VciClosing) {
        return false;
    }
    m_closeRequestId = closeRequestId;
    if (closeRequestId != 0)
        return true;
    m_closeRequestSelectionEpoch = 0;
    failLocal(xc2::Xc2ErrorCategory::Transport,
              QStringLiteral("Failed to start VCI close request"));
    return false;
}

void KtmSessionController::stop()
{
    if (m_stopping || m_resetting || m_projectingPayload)
        return;
    m_failing = false;
    m_stopping = true;
    ++m_sessionEpoch;
    ++m_selectionEpoch;
    const xc2::Xc2RequestId pendingIds[] = {
        m_requestId, m_applyRequestId, m_selectedRequestId, m_closeRequestId};
    m_requestId = 0;
    m_applyRequestId = 0;
    m_selectedRequestId = 0;
    m_closeRequestId = 0;
    m_applyRequestSelectionEpoch = 0;
    m_selectedRequestSelectionEpoch = 0;
    m_closeRequestSelectionEpoch = 0;
    m_stompGeneration = 0;
    m_lookupJobId.clear();
    m_lookupRequestStarted = false;
    m_lookupDevicesRequested = false;
    m_progressSubscriptionSent = false;
    m_applyTarget.reset();
    m_confirmedDevice.reset();
    m_latestVciStatus.reset();
    m_applySucceeded = false;
    m_selectedConfirmed = false;
    m_connectedForSelectionEpoch = false;
    const quint64 stopEpoch = m_sessionEpoch;
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    QPointer<xc2::Xc2StompClient> stompComponentGuard = m_stompClient;
    QPointer<KtmSessionController> stopGuard(this);
    const KtmSessionState stopState = m_state;
    if (!publishOperation(KtmSessionOperation::None) || !stopGuard
        || stopEpoch != m_sessionEpoch || !restGuard
        || restGuard != m_restClient || !stompComponentGuard
        || stompComponentGuard != m_stompClient
        || m_operation != KtmSessionOperation::None
        || !m_stopping || m_state != stopState) {
        return;
    }

    QList<xc2::Xc2RequestId> aborted;
    for (xc2::Xc2RequestId id : pendingIds) {
        if (id != 0 && !aborted.contains(id)) {
            aborted.append(id);
            abortRest(id);
            if (!stopGuard || stopEpoch != m_sessionEpoch
                || !restGuard || restGuard != m_restClient
                || m_operation != KtmSessionOperation::None
                || !m_stopping || m_state != stopState) {
                return;
            }
        }
    }
    disconnectStomp();
    if (!stopGuard || stopEpoch != m_sessionEpoch
        || !stompComponentGuard || stompComponentGuard != m_stompClient
        || m_operation != KtmSessionOperation::None
        || !m_stopping || m_state != stopState) {
        return;
    }
    abortStomp();
    if (!stopGuard || stopEpoch != m_sessionEpoch
        || !stompComponentGuard || stompComponentGuard != m_stompClient
        || m_operation != KtmSessionOperation::None
        || !m_stopping || m_state != stopState) {
        return;
    }
    if (!resetSessionObjects(true) || !stopGuard
        || stopEpoch != m_sessionEpoch
        || m_operation != KtmSessionOperation::None
        || !m_stopping || m_state != stopState) {
        return;
    }
    QPointer<xc2::Xc2BackendManager> backendGuard = m_backend;
    const xc2::Xc2BackendState observedBackendState = backendState();
    if (!stopGuard || stopEpoch != m_sessionEpoch
        || !backendGuard || backendGuard != m_backend
        || !m_stopping || m_operation != KtmSessionOperation::None
        || m_state != stopState) {
        return;
    }
    if (observedBackendState == xc2::Xc2BackendState::Stopped) {
        m_stopping = false;
        publishState(KtmSessionState::Stopped);
        return;
    }
    stopBackend();
}

bool KtmSessionController::publishState(KtmSessionState state)
{
    if (m_state == state)
        return true;
    m_state = state;
    QPointer<KtmSessionController> guard(this);
    emit stateChanged(state);
    return !guard.isNull();
}

bool KtmSessionController::publishOperation(KtmSessionOperation operation)
{
    if (m_operation == operation)
        return true;
    m_operation = operation;
    QPointer<KtmSessionController> guard(this);
    emit operationChanged(operation);
    return !guard.isNull();
}

bool KtmSessionController::failSession(const xc2::Xc2Error &error)
{
    if (m_resetting || m_stopping || m_projectingPayload || m_failing)
        return false;
    const xc2::Xc2Error stableError = error;
    m_failing = true;
    ++m_selectionEpoch;
    const quint64 sessionEpoch = m_sessionEpoch;
    const quint64 failureSelectionEpoch = m_selectionEpoch;
    const KtmSessionState failureState = m_state;
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    QPointer<xc2::Xc2StompClient> stompGuard = m_stompClient;
    const xc2::Xc2RequestId pendingIds[] = {
        m_requestId, m_applyRequestId, m_selectedRequestId, m_closeRequestId};
    m_stompGeneration = 0;
    m_lookupJobId.clear();
    m_lookupRequestStarted = false;
    m_lookupDevicesRequested = false;
    m_applyTarget.reset();
    m_confirmedDevice.reset();
    m_latestVciStatus.reset();
    m_applySucceeded = false;
    m_selectedConfirmed = false;
    m_connectedForSelectionEpoch = false;
    QPointer<KtmSessionController> guard(this);
    const auto abandonFailure = [&guard]() {
        if (guard)
            guard->m_failing = false;
        return false;
    };
    if (!publishOperation(KtmSessionOperation::None) || !guard
        || sessionEpoch != m_sessionEpoch
        || !restGuard || restGuard != m_restClient
        || !stompGuard || stompGuard != m_stompClient
        || failureSelectionEpoch != m_selectionEpoch
        || m_state != failureState
        || m_operation != KtmSessionOperation::None)
        return abandonFailure();

    m_requestId = 0;
    m_applyRequestId = 0;
    m_selectedRequestId = 0;
    m_closeRequestId = 0;
    m_applyRequestSelectionEpoch = 0;
    m_selectedRequestSelectionEpoch = 0;
    m_closeRequestSelectionEpoch = 0;
    QList<xc2::Xc2RequestId> aborted;
    for (xc2::Xc2RequestId id : pendingIds) {
        if (id == 0 || aborted.contains(id))
            continue;
        aborted.append(id);
        abortRest(id);
        if (!guard || sessionEpoch != m_sessionEpoch
            || !restGuard || restGuard != m_restClient
            || m_operation != KtmSessionOperation::None
            || failureSelectionEpoch != m_selectionEpoch
            || m_state != failureState)
            return abandonFailure();
    }
    disconnectStomp();
    if (!guard || sessionEpoch != m_sessionEpoch
        || !stompGuard || stompGuard != m_stompClient
        || m_operation != KtmSessionOperation::None
        || failureSelectionEpoch != m_selectionEpoch
        || m_state != failureState) {
        return abandonFailure();
    }
    abortStomp();
    if (!guard || sessionEpoch != m_sessionEpoch
        || !stompGuard || stompGuard != m_stompClient
        || m_operation != KtmSessionOperation::None
        || failureSelectionEpoch != m_selectionEpoch
        || m_state != failureState) {
        return abandonFailure();
    }
    if (!resetSessionObjects(true) || !guard
        || sessionEpoch != m_sessionEpoch
        || m_operation != KtmSessionOperation::None
        || failureSelectionEpoch != m_selectionEpoch
        || m_state != failureState) {
        return abandonFailure();
    }
    QPointer<xc2::Xc2RestClient> failedRestGuard = m_restClient;
    QPointer<xc2::Xc2StompClient> failedStompGuard = m_stompClient;
    m_projectingPayload = true;
    if (!publishState(KtmSessionState::Failed) || !guard)
        return abandonFailure();
    if (sessionEpoch != m_sessionEpoch
        || failureSelectionEpoch != m_selectionEpoch
        || !failedRestGuard || failedRestGuard != m_restClient
        || !failedStompGuard || failedStompGuard != m_stompClient
        || m_state != KtmSessionState::Failed
        || m_operation != KtmSessionOperation::None) {
        m_projectingPayload = false;
        return abandonFailure();
    }
    emit failed(stableError);
    if (!guard)
        return false;
    m_projectingPayload = false;
    m_failing = false;
    return true;
}

bool KtmSessionController::failLocal(xc2::Xc2ErrorCategory category,
                                     const QString &message)
{
    return failSession(localError(category, message));
}

bool KtmSessionController::resetSessionObjects(bool notifyTest)
{
    if (m_resetting)
        return false;
    m_resetting = true;
    const quint64 resetEpoch = m_sessionEpoch;
    QPointer<KtmSessionController> guard(this);
    QPointer<xc2::Xc2JobRegistry> oldRegistry = m_jobRegistry;
    QPointer<xc2::Xc2StompClient> oldStomp = m_stompClient;
    QPointer<xc2::Xc2RestClient> oldRest = m_restClient;
    m_jobRegistry = nullptr;
    m_stompClient = nullptr;
    m_restClient = nullptr;
    delete oldRegistry;
    if (!guard)
        return false;
    if (resetEpoch != m_sessionEpoch) {
        m_resetting = false;
        return false;
    }
    delete oldStomp;
    if (!guard)
        return false;
    if (resetEpoch != m_sessionEpoch) {
        m_resetting = false;
        return false;
    }
    delete oldRest;
    if (!guard)
        return false;
    if (resetEpoch != m_sessionEpoch) {
        m_resetting = false;
        return false;
    }

    m_jobRegistry = new xc2::Xc2JobRegistry(this);
    m_stompClient = new xc2::Xc2StompClient({}, this);
    m_restClient = new xc2::Xc2RestClient({}, this);

    const quint64 epoch = m_sessionEpoch;
    const quintptr restIdentity = reinterpret_cast<quintptr>(m_restClient);
    const quintptr stompIdentity = reinterpret_cast<quintptr>(m_stompClient);
    const quintptr registryIdentity =
        reinterpret_cast<quintptr>(m_jobRegistry);
    QPointer<xc2::Xc2RestClient> resetRestGuard = m_restClient;
    QPointer<xc2::Xc2StompClient> resetStompGuard = m_stompClient;
    QPointer<xc2::Xc2JobRegistry> resetRegistryGuard = m_jobRegistry;
    connect(m_restClient, &xc2::Xc2RestClient::serviceStatusFinished,
            this, [this, epoch, restIdentity](
                      xc2::Xc2RequestId id,
                      const xc2::Xc2Result<xc2::Xc2ServiceStatus> &result) {
        handleServiceStatusFinished(epoch, restIdentity, id, result);
    });
    connect(m_restClient, &xc2::Xc2RestClient::currentUserFinished,
            this, [this, epoch, restIdentity](
                      xc2::Xc2RequestId id,
                      const xc2::Xc2Result<xc2::Xc2CurrentUser> &result) {
        handleCurrentUserFinished(epoch, restIdentity, id, result);
    });
    connect(m_restClient, &xc2::Xc2RestClient::deviceLookupFinished,
            this, [this, epoch, restIdentity](
                      xc2::Xc2RequestId id,
                      const xc2::Xc2Result<xc2::Xc2JobAccepted> &result) {
        handleDeviceLookupFinished(epoch, restIdentity, id, result);
    });
    connect(m_restClient, &xc2::Xc2RestClient::devicesFinished,
            this, [this, epoch, restIdentity](
                      xc2::Xc2RequestId id,
                      const xc2::Xc2Result<QList<xc2::Xc2VciDevice>> &result) {
        handleDevicesFinished(epoch, restIdentity, id, result);
    });
    connect(m_restClient, &xc2::Xc2RestClient::applyDeviceFinished,
            this, [this, epoch, restIdentity](xc2::Xc2RequestId id,
                                               const xc2::Xc2Error &error) {
        const quint64 requestSelectionEpoch = id == m_applyRequestId
            ? m_applyRequestSelectionEpoch : 0;
        handleApplyDeviceFinished(epoch, requestSelectionEpoch,
                                  restIdentity, id, error);
    });
    connect(m_restClient, &xc2::Xc2RestClient::selectedDeviceFinished,
            this, [this, epoch, restIdentity](
                      xc2::Xc2RequestId id,
                      const xc2::Xc2Result<xc2::Xc2SelectedVci> &result) {
        const quint64 requestSelectionEpoch = id == m_selectedRequestId
            ? m_selectedRequestSelectionEpoch : 0;
        handleSelectedDeviceFinished(epoch, requestSelectionEpoch,
                                     restIdentity, id, result);
    });
    connect(m_restClient, &xc2::Xc2RestClient::closeDeviceFinished,
            this, [this, epoch, restIdentity](xc2::Xc2RequestId id,
                                               const xc2::Xc2Error &error) {
        const quint64 requestSelectionEpoch = id == m_closeRequestId
            ? m_closeRequestSelectionEpoch : 0;
        handleCloseDeviceFinished(epoch, requestSelectionEpoch,
                                  restIdentity, id, error);
    });
    connect(m_stompClient, &xc2::Xc2StompClient::connected,
            this, [this, epoch, stompIdentity](
                      const xc2::Xc2StompSession &session) {
        handleStompConnected(epoch, stompIdentity, session);
    });
    connect(m_stompClient, &xc2::Xc2StompClient::subscriptionSent,
            this, [this, epoch, stompIdentity](xc2::Topic topic,
                                               const QString &) {
        handleSubscriptionSent(epoch, stompIdentity, topic);
    });
    connect(m_stompClient, &xc2::Xc2StompClient::messageReceived,
            this, [this, epoch, stompIdentity](
                      const xc2::Xc2StompMessage &message) {
        handleStompMessage(epoch, stompIdentity, message);
    });
    connect(m_stompClient, &xc2::Xc2StompClient::errorOccurred,
            this, [this, epoch, stompIdentity](
                      xc2::Xc2StompGeneration generation,
                      const xc2::Xc2Error &error) {
        handleStompError(epoch, stompIdentity, generation, error);
    });
    connect(m_stompClient, &xc2::Xc2StompClient::visibilityLost,
            this, [this, epoch, stompIdentity](
                      xc2::Xc2StompGeneration generation,
                      const xc2::Xc2Error &error) {
        handleVisibilityLost(epoch, stompIdentity, generation, error);
    });
    connect(m_jobRegistry, &xc2::Xc2JobRegistry::jobTerminal,
            this, [this, epoch, registryIdentity](
                      const xc2::Xc2JobRecord &record) {
        handleJobTerminal(epoch, registryIdentity, record);
    });

    m_progressSubscriptionSent = false;
    m_lookupJobId.clear();
    m_lookupRequestStarted = false;
    m_lookupDevicesRequested = false;
    m_applyRequestId = 0;
    m_selectedRequestId = 0;
    m_closeRequestId = 0;
    m_applyRequestSelectionEpoch = 0;
    m_selectedRequestSelectionEpoch = 0;
    m_closeRequestSelectionEpoch = 0;
    m_applyTarget.reset();
    m_confirmedDevice.reset();
    m_latestVciStatus.reset();
    m_applySucceeded = false;
    m_selectedConfirmed = false;
    m_connectedForSelectionEpoch = false;
    m_resetting = false;
    const auto resetObserved = m_testOps.sessionObjectsReset;
    if (notifyTest && resetObserved) {
        resetObserved();
        if (!guard || resetEpoch != m_sessionEpoch
            || !resetRestGuard || resetRestGuard != m_restClient
            || !resetStompGuard || resetStompGuard != m_stompClient
            || !resetRegistryGuard
            || resetRegistryGuard != m_jobRegistry) {
            return false;
        }
    }
    return true;
}

bool KtmSessionController::callbackMatches(
    quint64 epoch, quintptr identity, xc2::Xc2RequestId id) const
{
    return epoch == m_sessionEpoch
        && identity == reinterpret_cast<quintptr>(m_restClient)
        && id != 0 && id == m_requestId;
}

xc2::Xc2BackendState KtmSessionController::backendState() const
{
    const auto operation = m_testOps.backendState;
    return operation ? operation() : m_backend->state();
}

bool KtmSessionController::startBackend(const QString &installRoot,
                                        xc2::Xc2Error *error)
{
    const auto operation = m_testOps.startBackend;
    return operation
        ? operation(installRoot, error)
        : m_backend->startProduction(installRoot, error);
}

bool KtmSessionController::setRestBaseUrl(const QByteArray &encodedBase,
                                          xc2::Xc2Error *error)
{
    const auto operation = m_testOps.setRestBaseUrl;
    return operation
        ? operation(encodedBase, error)
        : m_restClient && m_restClient->setBaseUrl(encodedBase, error);
}

QUrl KtmSessionController::restWebSocketUrl() const
{
    const auto operation = m_testOps.restWebSocketUrl;
    return operation ? operation()
                     : m_restClient ? m_restClient->webSocketUrl() : QUrl{};
}

xc2::Xc2RequestId KtmSessionController::requestServiceStatus()
{
    const auto operation = m_testOps.requestServiceStatus;
    return operation
        ? operation()
        : m_restClient ? m_restClient->requestServiceStatus() : 0;
}

xc2::Xc2RequestId KtmSessionController::requestCurrentUser()
{
    const auto operation = m_testOps.requestCurrentUser;
    return operation ? operation()
                     : m_restClient ? m_restClient->requestCurrentUser() : 0;
}

xc2::Xc2RequestId KtmSessionController::requestDeviceLookup()
{
    const auto operation = m_testOps.requestDeviceLookup;
    return operation
        ? operation()
        : m_restClient ? m_restClient->requestDeviceLookup() : 0;
}

xc2::Xc2RequestId KtmSessionController::requestDevices()
{
    const auto operation = m_testOps.requestDevices;
    return operation ? operation()
                     : m_restClient ? m_restClient->requestDevices() : 0;
}

xc2::Xc2RequestId KtmSessionController::requestApplyDevice(
    const xc2::Xc2VciDevice &device)
{
    const auto operation = m_testOps.requestApplyDevice;
    return operation
        ? operation(device)
        : m_restClient ? m_restClient->requestApplyDevice(device) : 0;
}

xc2::Xc2RequestId KtmSessionController::requestSelectedDevice()
{
    const auto operation = m_testOps.requestSelectedDevice;
    return operation
        ? operation()
        : m_restClient ? m_restClient->requestSelectedDevice() : 0;
}

xc2::Xc2RequestId KtmSessionController::requestCloseDevice(
    const xc2::Xc2VciDevice &device)
{
    const auto operation = m_testOps.requestCloseDevice;
    return operation
        ? operation(device)
        : m_restClient ? m_restClient->requestCloseDevice(device) : 0;
}

void KtmSessionController::abortRest(xc2::Xc2RequestId id)
{
    const auto operation = m_testOps.abortRest;
    if (operation)
        operation(id);
    else if (m_restClient)
        m_restClient->abort(id);
}

void KtmSessionController::disconnectStomp()
{
    const auto operation = m_testOps.disconnectStomp;
    if (operation)
        operation();
    else if (m_stompClient)
        m_stompClient->disconnectFromBackend();
}

void KtmSessionController::abortStomp()
{
    const auto operation = m_testOps.abortStomp;
    if (operation)
        operation();
    else if (m_stompClient)
        m_stompClient->abortCurrentGeneration();
}

void KtmSessionController::stopBackend()
{
    const auto operation = m_testOps.stopBackend;
    if (operation)
        operation();
    else
        m_backend->stop();
}

bool KtmSessionController::connectStomp(xc2::Xc2Error *error)
{
    const auto operation = m_testOps.connectStomp;
    return operation
        ? operation(error)
        : m_stompClient && m_restClient
            && m_stompClient->connectToBackend(*m_restClient, error);
}

bool KtmSessionController::subscribeStomp(xc2::Topic topic,
                                          xc2::Xc2Error *error)
{
    const auto operation = m_testOps.subscribeStomp;
    return operation
        ? operation(topic, error)
        : m_stompClient && m_stompClient->subscribe(topic, error);
}

xc2::Xc2StompGeneration KtmSessionController::stompGeneration() const
{
    const auto operation = m_testOps.stompGeneration;
    return operation ? operation()
                     : m_stompClient ? m_stompClient->generation() : 0;
}

void KtmSessionController::handleBackendStateChanged(
    xc2::Xc2BackendState state)
{
    if (state == xc2::Xc2BackendState::Ready
        && m_state == KtmSessionState::BackendStarting) {
        publishState(KtmSessionState::BackendReady);
    } else if (state == xc2::Xc2BackendState::Stopped) {
        if (m_resetting)
            return;
        const quint64 sessionEpoch = m_sessionEpoch;
        const KtmSessionState expectedState = m_state;
        QPointer<KtmSessionController> guard(this);
        m_stopping = true;
        if (!publishOperation(KtmSessionOperation::None) || !guard
            || sessionEpoch != m_sessionEpoch
            || m_state != expectedState
            || m_operation != KtmSessionOperation::None
            || !m_stopping) {
            return;
        }
        m_stopping = false;
        m_failing = false;
        m_projectingPayload = false;
        publishState(KtmSessionState::Stopped);
    }
}

void KtmSessionController::handleBackendReady(
    const xc2::Xc2BackendEndpoints &endpoints)
{
    if (m_state != KtmSessionState::BackendReady)
        return;
    const quint64 epoch = m_sessionEpoch;
    QPointer<KtmSessionController> guard(this);
    QPointer<xc2::Xc2BackendManager> backendGuard = m_backend;
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    const xc2::Xc2BackendState observedBackendState = backendState();
    if (!guard || epoch != m_sessionEpoch
        || !backendGuard || backendGuard != m_backend
        || !restGuard || restGuard != m_restClient
        || m_state != KtmSessionState::BackendReady
        || m_operation != KtmSessionOperation::None
        || observedBackendState != xc2::Xc2BackendState::Ready) {
        return;
    }
    if (!publishState(KtmSessionState::SessionStarting) || !guard
        || epoch != m_sessionEpoch
        || !backendGuard || backendGuard != m_backend
        || !restGuard || restGuard != m_restClient
        || m_state != KtmSessionState::SessionStarting
        || m_operation != KtmSessionOperation::None) {
        return;
    }

    xc2::Xc2Error error;
    const bool baseConfigured = setRestBaseUrl(
        endpoints.restBaseUrl.toEncoded(QUrl::FullyEncoded), &error);
    if (!guard || epoch != m_sessionEpoch
        || !restGuard || restGuard != m_restClient
        || m_state != KtmSessionState::SessionStarting
        || m_operation != KtmSessionOperation::None) {
        return;
    }
    if (!baseConfigured) {
        failSession(error);
        return;
    }
    const QUrl derivedWebSocketUrl = restWebSocketUrl();
    if (!guard || epoch != m_sessionEpoch
        || !restGuard || restGuard != m_restClient
        || m_state != KtmSessionState::SessionStarting
        || m_operation != KtmSessionOperation::None) {
        return;
    }
    if (!sameEncodedUrl(derivedWebSocketUrl, endpoints.webSocketUrl)) {
        failLocal(xc2::Xc2ErrorCategory::Contract,
                  QStringLiteral("REST and STOMP authorities do not match"));
        return;
    }
    const xc2::Xc2RequestId requestId = requestServiceStatus();
    if (!guard || epoch != m_sessionEpoch
        || !restGuard || restGuard != m_restClient
        || m_state != KtmSessionState::SessionStarting
        || m_operation != KtmSessionOperation::None) {
        return;
    }
    m_requestId = requestId;
    if (requestId == 0) {
        failLocal(xc2::Xc2ErrorCategory::Transport,
                  QStringLiteral("Failed to start service-status request"));
    }
}

void KtmSessionController::handleBackendFailed(
    const xc2::Xc2Error &error)
{
    if (m_state != KtmSessionState::Stopped)
        failSession(error);
}

void KtmSessionController::handleServiceStatusFinished(
    quint64 epoch, quintptr identity, xc2::Xc2RequestId id,
    const xc2::Xc2Result<xc2::Xc2ServiceStatus> &result)
{
    if (!callbackMatches(epoch, identity, id)
        || m_state != KtmSessionState::SessionStarting) {
        return;
    }
    m_requestId = 0;
    if (!result.ok()) {
        failSession(result.error);
        return;
    }
    if (!result.value->alive) {
        failLocal(xc2::Xc2ErrorCategory::Contract,
                  QStringLiteral("XC2 service is not alive"));
        return;
    }
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    QPointer<KtmSessionController> guard(this);
    const xc2::Xc2RequestId requestId = requestCurrentUser();
    if (!guard || epoch != m_sessionEpoch
        || identity != reinterpret_cast<quintptr>(restGuard.data())
        || !restGuard || restGuard != m_restClient
        || m_state != KtmSessionState::SessionStarting
        || m_operation != KtmSessionOperation::None) {
        return;
    }
    m_requestId = requestId;
    if (requestId == 0) {
        failLocal(xc2::Xc2ErrorCategory::Transport,
                  QStringLiteral("Failed to start current-user request"));
    }
}

void KtmSessionController::handleCurrentUserFinished(
    quint64 epoch, quintptr identity, xc2::Xc2RequestId id,
    const xc2::Xc2Result<xc2::Xc2CurrentUser> &result)
{
    if (!callbackMatches(epoch, identity, id)
        || m_state != KtmSessionState::SessionStarting) {
        return;
    }
    m_requestId = 0;
    if (!result.ok()) {
        failSession(result.error);
        return;
    }
    if (!result.value->permissions.contains(
            QString::fromLatin1(kRequiredPermission), Qt::CaseSensitive)) {
        failLocal(xc2::Xc2ErrorCategory::Session,
                  QStringLiteral("Required EcuDiagnosticRead permission is missing"));
        return;
    }

    const quint64 sessionEpoch = m_sessionEpoch;
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    QPointer<xc2::Xc2StompClient> stompGuard = m_stompClient;
    QPointer<KtmSessionController> guard(this);
    xc2::Xc2Error error;
    const bool connecting = connectStomp(&error);
    if (!guard || sessionEpoch != m_sessionEpoch
        || identity != reinterpret_cast<quintptr>(restGuard.data())
        || !restGuard || restGuard != m_restClient
        || !stompGuard || stompGuard != m_stompClient
        || m_state != KtmSessionState::SessionStarting
        || m_operation != KtmSessionOperation::None) {
        return;
    }
    if (!connecting) {
        failSession(error);
        return;
    }
    const xc2::Xc2StompGeneration generation = stompGeneration();
    if (!guard || sessionEpoch != m_sessionEpoch
        || !restGuard || restGuard != m_restClient
        || !stompGuard || stompGuard != m_stompClient
        || m_state != KtmSessionState::SessionStarting
        || m_operation != KtmSessionOperation::None) {
        return;
    }
    m_stompGeneration = generation;
    if (generation == 0) {
        failLocal(xc2::Xc2ErrorCategory::Session,
                  QStringLiteral("STOMP generation is zero"));
    }
}

void KtmSessionController::handleStompConnected(
    quint64 epoch, quintptr identity, const xc2::Xc2StompSession &session)
{
    if (epoch != m_sessionEpoch
        || identity != reinterpret_cast<quintptr>(m_stompClient)
        || m_state != KtmSessionState::SessionStarting
        || session.generation == 0
        || session.generation != m_stompGeneration) {
        return;
    }

    const quint64 sessionEpoch = m_sessionEpoch;
    QPointer<xc2::Xc2StompClient> stompGuard = m_stompClient;
    QPointer<KtmSessionController> guard(this);
    const xc2::Xc2StompGeneration generation = stompGeneration();
    if (!guard || sessionEpoch != m_sessionEpoch
        || identity != reinterpret_cast<quintptr>(stompGuard.data())
        || !stompGuard || stompGuard != m_stompClient
        || m_state != KtmSessionState::SessionStarting
        || m_operation != KtmSessionOperation::None
        || generation != m_stompGeneration) {
        return;
    }
    xc2::Xc2Error error;
    const xc2::Topic topics[] = {
        xc2::Topic::Login, xc2::Topic::VciStatus, xc2::Topic::Progress};
    for (xc2::Topic topic : topics) {
        const bool subscribed = subscribeStomp(topic, &error);
        if (!guard || sessionEpoch != m_sessionEpoch
            || !stompGuard || stompGuard != m_stompClient
            || m_state != KtmSessionState::SessionStarting
            || m_operation != KtmSessionOperation::None) {
            return;
        }
        if (!subscribed) {
            failSession(error);
            return;
        }
    }
    if (guard && sessionEpoch == m_sessionEpoch
        && stompGuard && stompGuard == m_stompClient
        && m_state == KtmSessionState::SessionStarting
        && m_operation == KtmSessionOperation::None)
        publishState(KtmSessionState::SessionReady);
}

void KtmSessionController::handleSubscriptionSent(
    quint64 epoch, quintptr identity, xc2::Topic topic)
{
    if (epoch != m_sessionEpoch
        || identity != reinterpret_cast<quintptr>(m_stompClient)
        || topic != xc2::Topic::Progress || m_stompGeneration == 0) {
        return;
    }
    const KtmSessionState expectedState = m_state;
    const KtmSessionOperation expectedOperation = m_operation;
    QPointer<KtmSessionController> guard(this);
    QPointer<xc2::Xc2StompClient> stompGuard = m_stompClient;
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    const xc2::Xc2StompGeneration generation = stompGeneration();
    if (!guard || epoch != m_sessionEpoch
        || identity != reinterpret_cast<quintptr>(stompGuard.data())
        || !stompGuard || stompGuard != m_stompClient
        || !restGuard || restGuard != m_restClient
        || expectedState != m_state || expectedOperation != m_operation
        || generation != m_stompGeneration) {
        return;
    }
    m_progressSubscriptionSent = true;
    if (m_operation == KtmSessionOperation::Lookup)
        startDeferredLookup();
}

void KtmSessionController::handleStompMessage(
    quint64 epoch, quintptr identity, const xc2::Xc2StompMessage &message)
{
    if (epoch != m_sessionEpoch
        || identity != reinterpret_cast<quintptr>(m_stompClient)
        || message.generation == 0
        || message.generation != m_stompGeneration) {
        return;
    }
    if (m_state == KtmSessionState::Stopped
        || m_state == KtmSessionState::Failed) {
        return;
    }
    const KtmSessionState expectedState = m_state;
    const KtmSessionOperation expectedOperation = m_operation;
    QPointer<KtmSessionController> guard(this);
    QPointer<xc2::Xc2StompClient> stompGuard = m_stompClient;
    QPointer<xc2::Xc2JobRegistry> registryGuard = m_jobRegistry;
    const xc2::Xc2StompGeneration generation = stompGeneration();
    if (!guard || epoch != m_sessionEpoch
        || identity != reinterpret_cast<quintptr>(stompGuard.data())
        || !stompGuard || stompGuard != m_stompClient
        || !registryGuard || registryGuard != m_jobRegistry
        || expectedState != m_state || expectedOperation != m_operation
        || generation != m_stompGeneration) {
        return;
    }
    if (message.topic != xc2::Topic::Progress)
    {
        if (message.topic == xc2::Topic::VciStatus) {
            handleVciStatus(epoch, m_selectionEpoch, identity, message);
        }
        return;
    }

    xc2::Xc2Error error;
    const bool applied = m_jobRegistry->applyMessage(message, &error);
    if (!guard || epoch != m_sessionEpoch
        || !stompGuard || stompGuard != m_stompClient
        || !registryGuard || registryGuard != m_jobRegistry
        || expectedState != m_state || expectedOperation != m_operation) {
        return;
    }
    if (!applied && m_operation == KtmSessionOperation::Lookup) {
        failSession(error);
    }
}

void KtmSessionController::handleVciStatus(
    quint64 epoch, quint64 selectionEpoch, quintptr identity,
    const xc2::Xc2StompMessage &message)
{
    if (epoch != m_sessionEpoch || selectionEpoch != m_selectionEpoch
        || identity != reinterpret_cast<quintptr>(m_stompClient)
        || message.generation != m_stompGeneration) {
        return;
    }
    const xc2::Xc2Result<xc2::Xc2VciStatus> decoded =
        xc2::Xc2JsonCodec::vciStatus(message.body);
    if (!decoded.ok()) {
        failWithReadinessRevoked(decoded.error);
        return;
    }

    const xc2::Xc2VciStatus status = *decoded.value;
    if (!status.connected && m_state == KtmSessionState::VciReady) {
        const quint64 currentSessionEpoch = m_sessionEpoch;
        const quint64 revokedSelectionEpoch = m_selectionEpoch + 1;
        QPointer<KtmSessionController> guard(this);
        revokeReadinessToSession(localError(
            xc2::Xc2ErrorCategory::Vci,
            QStringLiteral("The confirmed VCI is no longer connected")));
        if (!guard || currentSessionEpoch != m_sessionEpoch
            || revokedSelectionEpoch != m_selectionEpoch
            || m_state != KtmSessionState::SessionReady) {
            return;
        }
        emit vciStatusChanged(status);
        return;
    }

    m_latestVciStatus = status;
    if (m_operation == KtmSessionOperation::Apply
        && m_state == KtmSessionState::VciApplying) {
        m_connectedForSelectionEpoch = status.connected;
    }
    const quint64 currentSessionEpoch = m_sessionEpoch;
    const quint64 currentSelectionEpoch = m_selectionEpoch;
    QPointer<KtmSessionController> guard(this);
    emit vciStatusChanged(status);
    if (!guard || currentSessionEpoch != m_sessionEpoch
        || currentSelectionEpoch != m_selectionEpoch) {
        return;
    }
    tryPublishVciReady();
}

void KtmSessionController::handleStompError(
    quint64 epoch, quintptr identity,
    xc2::Xc2StompGeneration generation, const xc2::Xc2Error &error)
{
    if (epoch != m_sessionEpoch
        || identity != reinterpret_cast<quintptr>(m_stompClient)
        || generation != m_stompGeneration) {
        return;
    }
    const KtmSessionState expectedState = m_state;
    const KtmSessionOperation expectedOperation = m_operation;
    QPointer<KtmSessionController> guard(this);
    QPointer<xc2::Xc2StompClient> stompGuard = m_stompClient;
    QPointer<xc2::Xc2JobRegistry> registryGuard = m_jobRegistry;
    m_jobRegistry->markVisibilityLost(generation);
    if (!guard || epoch != m_sessionEpoch || !stompGuard
        || stompGuard != m_stompClient || !registryGuard
        || registryGuard != m_jobRegistry
        || expectedState != m_state || expectedOperation != m_operation
        || generation != m_stompGeneration) {
        return;
    }
    failWithReadinessRevoked(error);
}

void KtmSessionController::handleVisibilityLost(
    quint64 epoch, quintptr identity,
    xc2::Xc2StompGeneration generation, const xc2::Xc2Error &error)
{
    if (epoch != m_sessionEpoch
        || identity != reinterpret_cast<quintptr>(m_stompClient)
        || generation == 0 || generation != m_stompGeneration) {
        return;
    }
    const KtmSessionState expectedState = m_state;
    const KtmSessionOperation expectedOperation = m_operation;
    QPointer<KtmSessionController> guard(this);
    QPointer<xc2::Xc2StompClient> stompGuard = m_stompClient;
    QPointer<xc2::Xc2JobRegistry> registryGuard = m_jobRegistry;
    const xc2::Xc2StompGeneration activeGeneration = stompGeneration();
    if (!guard || epoch != m_sessionEpoch
        || identity != reinterpret_cast<quintptr>(stompGuard.data())
        || !stompGuard || stompGuard != m_stompClient
        || !registryGuard || registryGuard != m_jobRegistry
        || expectedState != m_state || expectedOperation != m_operation
        || activeGeneration != m_stompGeneration) {
        return;
    }
    m_jobRegistry->markVisibilityLost(generation);
    if (!guard || epoch != m_sessionEpoch
        || !stompGuard || stompGuard != m_stompClient
        || !registryGuard || registryGuard != m_jobRegistry
        || expectedState != m_state || expectedOperation != m_operation) {
        return;
    }
    failWithReadinessRevoked(error);
}

void KtmSessionController::handleDeviceLookupFinished(
    quint64 epoch, quintptr identity, xc2::Xc2RequestId id,
    const xc2::Xc2Result<xc2::Xc2JobAccepted> &result)
{
    if (!callbackMatches(epoch, identity, id)
        || m_operation != KtmSessionOperation::Lookup
        || m_state != KtmSessionState::VciLookup
        || m_lookupDevicesRequested) {
        return;
    }
    m_requestId = 0;
    if (!result.ok()) {
        failSession(result.error);
        return;
    }

    QPointer<KtmSessionController> guard(this);
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    QPointer<xc2::Xc2JobRegistry> registryGuard = m_jobRegistry;
    xc2::Xc2Error error;
    const bool accepted = m_jobRegistry->accept(*result.value, &error);
    if (!guard || epoch != m_sessionEpoch
        || identity != reinterpret_cast<quintptr>(restGuard.data())
        || !restGuard || restGuard != m_restClient
        || !registryGuard || registryGuard != m_jobRegistry
        || m_operation != KtmSessionOperation::Lookup
        || m_state != KtmSessionState::VciLookup) {
        return;
    }
    if (!accepted) {
        failSession(error);
        return;
    }
    m_lookupJobId = result.value->jobId;
    const std::optional<xc2::Xc2JobRecord> retained =
        m_jobRegistry->job(m_lookupJobId);
    if (retained && retained->terminal())
        processLookupTerminal(*retained);
}

void KtmSessionController::handleDevicesFinished(
    quint64 epoch, quintptr identity, xc2::Xc2RequestId id,
    const xc2::Xc2Result<QList<xc2::Xc2VciDevice>> &result)
{
    if (!callbackMatches(epoch, identity, id)
        || m_operation != KtmSessionOperation::Lookup
        || m_state != KtmSessionState::VciLookup
        || !m_lookupDevicesRequested) {
        return;
    }
    m_requestId = 0;
    if (!result.ok()) {
        failSession(result.error);
        return;
    }

    QPointer<KtmSessionController> guard(this);
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    QPointer<xc2::Xc2JobRegistry> registryGuard = m_jobRegistry;
    if (!m_lookupJobId.isEmpty()) {
        xc2::Xc2Error ignored;
        m_jobRegistry->clearTerminal(m_lookupJobId, &ignored);
        if (!guard || epoch != m_sessionEpoch
            || identity != reinterpret_cast<quintptr>(restGuard.data())
            || !restGuard || restGuard != m_restClient
            || !registryGuard || registryGuard != m_jobRegistry
            || m_operation != KtmSessionOperation::Lookup
            || m_state != KtmSessionState::VciLookup) {
            return;
        }
    }
    m_lookupJobId.clear();
    m_lookupRequestStarted = false;
    m_lookupDevicesRequested = false;
    const QList<xc2::Xc2VciDevice> devices = *result.value;
    const quint64 currentEpoch = m_sessionEpoch;
    const quint64 currentSelectionEpoch = m_selectionEpoch;
    if (!publishOperation(KtmSessionOperation::None) || !guard
        || currentEpoch != m_sessionEpoch
        || currentSelectionEpoch != m_selectionEpoch
        || !restGuard || restGuard != m_restClient
        || !registryGuard || registryGuard != m_jobRegistry
        || m_state != KtmSessionState::VciLookup
        || m_operation != KtmSessionOperation::None) {
        return;
    }
    m_projectingPayload = true;
    if (!publishState(KtmSessionState::SessionReady) || !guard)
        return;
    if (currentEpoch != m_sessionEpoch
        || currentSelectionEpoch != m_selectionEpoch
        || !restGuard || restGuard != m_restClient
        || !registryGuard || registryGuard != m_jobRegistry
        || m_state != KtmSessionState::SessionReady
        || m_operation != KtmSessionOperation::None) {
        m_projectingPayload = false;
        return;
    }
    emit vciLookupFinished(devices);
    if (guard)
        m_projectingPayload = false;
}

void KtmSessionController::handleApplyDeviceFinished(
    quint64 epoch, quint64 selectionEpoch, quintptr identity,
    xc2::Xc2RequestId id, const xc2::Xc2Error &error)
{
    if (epoch != m_sessionEpoch || selectionEpoch != m_selectionEpoch
        || identity != reinterpret_cast<quintptr>(m_restClient)
        || m_operation != KtmSessionOperation::Apply
        || m_state != KtmSessionState::VciApplying || id == 0
        || id != m_applyRequestId) {
        return;
    }
    m_applyRequestId = 0;
    m_applyRequestSelectionEpoch = 0;
    if (error.category != xc2::Xc2ErrorCategory::None) {
        failSession(error);
        return;
    }
    m_applySucceeded = true;
    tryPublishVciReady();
}

void KtmSessionController::handleSelectedDeviceFinished(
    quint64 epoch, quint64 selectionEpoch, quintptr identity,
    xc2::Xc2RequestId id,
    const xc2::Xc2Result<xc2::Xc2SelectedVci> &result)
{
    if (epoch != m_sessionEpoch || selectionEpoch != m_selectionEpoch
        || identity != reinterpret_cast<quintptr>(m_restClient)
        || m_operation != KtmSessionOperation::Apply
        || m_state != KtmSessionState::VciApplying || id == 0
        || id != m_selectedRequestId) {
        return;
    }
    m_selectedRequestId = 0;
    m_selectedRequestSelectionEpoch = 0;
    if (!result.ok()) {
        failSession(result.error);
        return;
    }
    if (!result.value->device || !m_applyTarget
        || result.value->device->id != m_applyTarget->id
        || result.value->device->internalName
            != xc2::Xc2ContractProfile::approved()
                   .supportedPduApiShortName()) {
        failLocal(xc2::Xc2ErrorCategory::Vci,
                  QStringLiteral("Selected VCI confirmation does not match"));
        return;
    }
    m_confirmedDevice = *result.value->device;
    m_selectedConfirmed = true;
    tryPublishVciReady();
}

void KtmSessionController::handleCloseDeviceFinished(
    quint64 epoch, quint64 selectionEpoch, quintptr identity,
    xc2::Xc2RequestId id, const xc2::Xc2Error &error)
{
    if (epoch != m_sessionEpoch || selectionEpoch != m_selectionEpoch
        || identity != reinterpret_cast<quintptr>(m_restClient)
        || m_operation != KtmSessionOperation::Close
        || m_state != KtmSessionState::VciClosing || id == 0
        || id != m_closeRequestId) {
        return;
    }
    m_closeRequestId = 0;
    m_closeRequestSelectionEpoch = 0;
    if (error.category != xc2::Xc2ErrorCategory::None) {
        failSession(error);
        return;
    }

    ++m_selectionEpoch;
    m_applyTarget.reset();
    m_confirmedDevice.reset();
    m_latestVciStatus.reset();
    m_applySucceeded = false;
    m_selectedConfirmed = false;
    m_connectedForSelectionEpoch = false;
    const quint64 currentSessionEpoch = m_sessionEpoch;
    const quint64 currentSelectionEpoch = m_selectionEpoch;
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    QPointer<KtmSessionController> guard(this);
    if (!publishOperation(KtmSessionOperation::None) || !guard
        || currentSessionEpoch != m_sessionEpoch
        || currentSelectionEpoch != m_selectionEpoch
        || !restGuard || restGuard != m_restClient
        || m_state != KtmSessionState::VciClosing
        || m_operation != KtmSessionOperation::None) {
        return;
    }
    m_projectingPayload = true;
    if (!publishState(KtmSessionState::SessionReady) || !guard)
        return;
    if (currentSessionEpoch != m_sessionEpoch
        || currentSelectionEpoch != m_selectionEpoch
        || !restGuard || restGuard != m_restClient
        || m_state != KtmSessionState::SessionReady
        || m_operation != KtmSessionOperation::None) {
        m_projectingPayload = false;
        return;
    }
    emit vciClosed();
    if (guard)
        m_projectingPayload = false;
}

void KtmSessionController::handleJobTerminal(
    quint64 epoch, quintptr identity, const xc2::Xc2JobRecord &record)
{
    if (epoch != m_sessionEpoch
        || identity != reinterpret_cast<quintptr>(m_jobRegistry)
        || m_operation != KtmSessionOperation::Lookup
        || record.jobId != m_lookupJobId) {
        return;
    }
    processLookupTerminal(record);
}

bool KtmSessionController::startDeferredLookup()
{
    if (m_operation != KtmSessionOperation::Lookup
        || m_state != KtmSessionState::VciLookup
        || !m_progressSubscriptionSent || m_lookupRequestStarted) {
        return true;
    }
    m_lookupRequestStarted = true;
    const quint64 sessionEpoch = m_sessionEpoch;
    QPointer<KtmSessionController> guard(this);
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    const xc2::Xc2RequestId requestId = requestDeviceLookup();
    if (!guard || sessionEpoch != m_sessionEpoch
        || !restGuard || restGuard != m_restClient
        || m_operation != KtmSessionOperation::Lookup
        || m_state != KtmSessionState::VciLookup) {
        return false;
    }
    m_requestId = requestId;
    if (requestId != 0)
        return true;
    failLocal(xc2::Xc2ErrorCategory::Transport,
              QStringLiteral("Failed to start VCI lookup request"));
    return false;
}

bool KtmSessionController::requestLookupDevices()
{
    if (m_lookupDevicesRequested)
        return true;
    m_lookupDevicesRequested = true;
    const quint64 sessionEpoch = m_sessionEpoch;
    QPointer<KtmSessionController> guard(this);
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    const xc2::Xc2RequestId requestId = requestDevices();
    if (!guard || sessionEpoch != m_sessionEpoch
        || !restGuard || restGuard != m_restClient
        || m_operation != KtmSessionOperation::Lookup
        || m_state != KtmSessionState::VciLookup) {
        return false;
    }
    m_requestId = requestId;
    if (requestId != 0)
        return true;
    failLocal(xc2::Xc2ErrorCategory::Transport,
              QStringLiteral("Failed to start VCI devices request"));
    return false;
}

void KtmSessionController::processLookupTerminal(
    const xc2::Xc2JobRecord &record)
{
    if (m_operation != KtmSessionOperation::Lookup
        || record.jobId != m_lookupJobId || !record.terminal()) {
        return;
    }
    if (record.state == xc2::Xc2JobState::Finished
        || record.state == xc2::Xc2JobState::Canceled) {
        requestLookupDevices();
        return;
    }

    xc2::Xc2Error error = localError(
        xc2::Xc2ErrorCategory::Job,
        record.state == xc2::Xc2JobState::NotAuthorized
            ? QStringLiteral("VCI lookup was not authorized")
            : QStringLiteral("VCI lookup failed"));
    error.jobId = record.jobId;
    failSession(error);
}

void KtmSessionController::tryPublishVciReady()
{
    if (m_operation != KtmSessionOperation::Apply
        || m_state != KtmSessionState::VciApplying || !m_applySucceeded
        || !m_selectedConfirmed || !m_connectedForSelectionEpoch
        || !m_confirmedDevice || !m_latestVciStatus
        || !m_latestVciStatus->connected) {
        return;
    }
    const xc2::Xc2VciDevice device = *m_confirmedDevice;
    const xc2::Xc2VciStatus status = *m_latestVciStatus;
    const quint64 sessionEpoch = m_sessionEpoch;
    const quint64 selectionEpoch = m_selectionEpoch;
    QPointer<xc2::Xc2RestClient> restGuard = m_restClient;
    QPointer<xc2::Xc2StompClient> stompGuard = m_stompClient;
    QPointer<KtmSessionController> guard(this);
    if (!publishOperation(KtmSessionOperation::None) || !guard
        || sessionEpoch != m_sessionEpoch
        || selectionEpoch != m_selectionEpoch
        || !restGuard || restGuard != m_restClient
        || !stompGuard || stompGuard != m_stompClient
        || m_state != KtmSessionState::VciApplying
        || m_operation != KtmSessionOperation::None) {
        return;
    }
    m_projectingPayload = true;
    if (!publishState(KtmSessionState::VciReady) || !guard)
        return;
    if (sessionEpoch != m_sessionEpoch
        || selectionEpoch != m_selectionEpoch
        || !restGuard || restGuard != m_restClient
        || !stompGuard || stompGuard != m_stompClient
        || m_state != KtmSessionState::VciReady
        || m_operation != KtmSessionOperation::None) {
        m_projectingPayload = false;
        return;
    }
    emit vciReady(device, status);
    if (guard)
        m_projectingPayload = false;
}

bool KtmSessionController::failWithReadinessRevoked(
    const xc2::Xc2Error &error)
{
    if (m_state == KtmSessionState::VciReady) {
        ++m_selectionEpoch;
        m_applyTarget.reset();
        m_confirmedDevice.reset();
        m_latestVciStatus.reset();
        m_applySucceeded = false;
        m_selectedConfirmed = false;
        m_connectedForSelectionEpoch = false;
        const quint64 sessionEpoch = m_sessionEpoch;
        const quint64 selectionEpoch = m_selectionEpoch;
        const KtmSessionState expectedState = m_state;
        const KtmSessionOperation expectedOperation = m_operation;
        QPointer<KtmSessionController> guard(this);
        emit vciReadinessRevoked(error);
        if (!guard || sessionEpoch != m_sessionEpoch
            || selectionEpoch != m_selectionEpoch
            || m_state != expectedState
            || m_operation != expectedOperation) {
            return false;
        }
    }
    return failSession(error);
}

void KtmSessionController::revokeReadinessToSession(
    const xc2::Xc2Error &error)
{
    if (m_state != KtmSessionState::VciReady)
        return;
    ++m_selectionEpoch;
    m_applyTarget.reset();
    m_confirmedDevice.reset();
    m_latestVciStatus.reset();
    m_applySucceeded = false;
    m_selectedConfirmed = false;
    m_connectedForSelectionEpoch = false;
    const quint64 sessionEpoch = m_sessionEpoch;
    const quint64 selectionEpoch = m_selectionEpoch;
    const KtmSessionState expectedState = m_state;
    const KtmSessionOperation expectedOperation = m_operation;
    QPointer<KtmSessionController> guard(this);
    emit vciReadinessRevoked(error);
    if (!guard || sessionEpoch != m_sessionEpoch
        || selectionEpoch != m_selectionEpoch
        || m_state != expectedState
        || m_operation != expectedOperation) {
        return;
    }
    if (!publishState(KtmSessionState::SessionReady) || !guard
        || sessionEpoch != m_sessionEpoch
        || selectionEpoch != m_selectionEpoch
        || m_state != KtmSessionState::SessionReady
        || m_operation != expectedOperation) {
        return;
    }
}

} // namespace ktm
