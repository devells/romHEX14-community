#pragma once

#include "Xc2Models.h"

#include <QHash>
#include <QObject>
#include <QUrl>

class QNetworkCookieJar;

namespace ktm::xc2 {

enum class Endpoint;

using Xc2RequestId = quint64;

struct Xc2RestClientOptions {
    int totalDeadlineMs = 10000;
    int transferTimeoutMs = 3000;
    int deviceOperationDeadlineMs = 30000;
};

class Xc2RestClient final : public QObject {
    Q_OBJECT

public:
    explicit Xc2RestClient(Xc2RestClientOptions options = {},
                           QObject *parent = nullptr);
    ~Xc2RestClient() override;

    bool setBaseUrl(const QByteArray &encodedLoopbackRestBase,
                    Xc2Error *error = nullptr);
    QUrl baseUrl() const;
    QUrl webSocketUrl() const;

    Xc2RequestId requestServiceStatus();
    Xc2RequestId requestCurrentUser();
    Xc2RequestId requestShutdown();
    Xc2RequestId requestDeviceLookup();
    Xc2RequestId requestDevices();
    Xc2RequestId requestSelectedDevice();
    Xc2RequestId requestApplyDevice(const Xc2VciDevice &device);
    Xc2RequestId requestCloseDevice(const Xc2VciDevice &device);
    Xc2Result<QByteArray> cookieHeaderFor(
        const QByteArray &encodedUrl) const;
    Xc2Result<QByteArray> shutdownCookieHeaderFor(
        const QByteArray &encodedShutdownUrl) const;
    void abort(Xc2RequestId id);

signals:
    void serviceStatusFinished(
        Xc2RequestId id,
        const Xc2Result<Xc2ServiceStatus> &result);
    void currentUserFinished(
        Xc2RequestId id,
        const Xc2Result<Xc2CurrentUser> &result);
    void shutdownFinished(Xc2RequestId id, const Xc2Error &error);
    void deviceLookupFinished(
        Xc2RequestId id,
        const Xc2Result<Xc2JobAccepted> &result);
    void devicesFinished(
        Xc2RequestId id,
        const Xc2Result<QList<Xc2VciDevice>> &result);
    void selectedDeviceFinished(
        Xc2RequestId id,
        const Xc2Result<Xc2SelectedVci> &result);
    void applyDeviceFinished(Xc2RequestId id, const Xc2Error &error);
    void closeDeviceFinished(Xc2RequestId id, const Xc2Error &error);

private:
    struct PendingRequest;

    Xc2RequestId startRequest(Endpoint endpoint,
                              QByteArray jsonBody = {});
    QUrl endpointUrl(Endpoint endpoint) const;
    void readAvailable(Xc2RequestId id);
    void drainAvailable(PendingRequest *pending);
    void parseAvailable(PendingRequest *pending);
    void failProtocol(PendingRequest *pending, const QString &message);
    void forceStop(Xc2RequestId id, Xc2TransportReason reason);
    void scheduleCompletion(Xc2RequestId id);
    void completeOnce(Xc2RequestId id);

    Xc2RestClientOptions m_options;
    QNetworkCookieJar *m_cookieJar = nullptr;
    QUrl m_baseUrl;
    QUrl m_webSocketUrl;
    QString m_canonicalHost;
    int m_explicitPort = -1;
    Xc2RequestId m_nextRequestId = 1;
    QHash<Xc2RequestId, PendingRequest *> m_pending;
};

} // namespace ktm::xc2
