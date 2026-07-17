#pragma once

#include "ktm/xc2/Xc2StompCodec.h"

#include <QByteArray>
#include <QHostAddress>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QStringList>
#include <QUrl>
#include <QWebSocketProtocol>

class QTcpServer;
class QTcpSocket;
class QWebSocket;
class QWebSocketServer;

struct FakeXc2HttpRequest {
    QByteArray method;
    QByteArray target;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray rawHeaderBlock;
    QByteArray body;

    QByteArray headerValue(const QByteArray &name) const;
    QList<QByteArray> headerValues(const QByteArray &name) const;
    QList<QByteArray> rawHeaderLinesMatchingName(
        const QByteArray &name) const;
};

enum class FakeXc2WebSocketMessageKind { Text, Binary };

struct FakeXc2WebSocketMessage {
    FakeXc2WebSocketMessageKind kind = FakeXc2WebSocketMessageKind::Text;
    QByteArray payload;
};

class FakeXc2TransportServer final : public QObject {
    Q_OBJECT

public:
    enum class UpgradeMode {
        WebSocket,
        NoResponse,
        Redirect,
        Reject,
        DifferentSubprotocol
    };

    enum class ProbeScript {
        Disabled,
        Happy,
        HealthNoResponse,
        HealthTruncated,
        Health204,
        HealthMalformedSuccess,
        HealthBackendUnavailable,
        HealthAuth401,
        HealthMalformedError,
        CurrentUserNoResponse,
        CurrentUserTruncated,
        CurrentUser204,
        CurrentUserMalformedSuccess,
        CurrentUserBackendUnavailable,
        CurrentUserAuth401,
        CurrentUserAuth403,
        CurrentUserMalformedAuthError,
        CurrentUserMissingPermission,
        CurrentUserCaseMismatchedPermission,
        CurrentUserBlankLogin,
        CurrentUserBlankName,
        UpgradeNoResponse,
        UpgradeRejected,
        UpgradeWrongSubprotocol,
        MissingConnected,
        StompError,
        MalformedConnected,
        WrongDisconnectReceipt,
        MissingDisconnectReceipt,
        CloseBeforeDisconnectReceipt,
        ReceiptThenAbnormalClose,
        LateDisconnectReceipt
    };

    explicit FakeXc2TransportServer(QObject *parent = nullptr);
    ~FakeXc2TransportServer() override;

    bool isListening() const;
    QString errorString() const;
    QHostAddress address() const;
    quint16 port() const;
    QByteArray encodedRestBase() const;

    int connectionCount() const;
    int restRequestCount() const;
    int upgradeRequestCount() const;
    int webSocketConnectionCount() const;
    int openConnectionCount() const;
    int stateChangingRestRequestCount() const;
    int unexpectedOperationCount() const;
    const QList<FakeXc2HttpRequest> &restRequests() const;
    const QList<FakeXc2HttpRequest> &capturedRestRequests() const;
    const QList<FakeXc2HttpRequest> &upgradeRequests() const;
    const QList<FakeXc2WebSocketMessage> &messages() const;
    const QList<ktm::xc2::Xc2StompFrame> &stompFrames() const;
    const QList<ktm::xc2::Xc2StompFrame> &sentStompFrames() const;
    const QList<ktm::xc2::Xc2Error> &decodeErrors() const;
    QUrl requestUrl() const;
    QString negotiatedSubprotocol() const;
    QWebSocketProtocol::CloseCode lastPeerCloseCode() const;

    void setRestCookie(QByteArray cookie);
    void setUpgradeMode(UpgradeMode mode);
    void setRedirectTarget(QUrl target);
    void setSupportedSubprotocols(QStringList protocols);
    void setOutgoingFrameSize(quint64 bytes);
    void setProbeScript(ProbeScript script);
    void setProbeDelayMs(int delayMs);

    bool sendText(const QByteArray &payload);
    bool sendBinary(const QByteArray &payload);
    bool sendFrame(const ktm::xc2::Xc2StompFrame &frame,
                   bool binary = false);
    bool sendFrameAndClose(
        const ktm::xc2::Xc2StompFrame &frame,
        QWebSocketProtocol::CloseCode closeCode =
            QWebSocketProtocol::CloseCodeNormal);
    void closeWebSocket(int delayMs = 0);

signals:
    void restRequestCaptured();
    void upgradeRequestCaptured();
    void webSocketConnected();
    void webSocketMessageReceived();
    void stompFrameReceived();
    void webSocketDisconnected();
    void connectionClosed();

private:
    struct RawConnection;

    void acceptConnections();
    void inspect(QTcpSocket *socket);
    void acceptWebSockets();
    void respondToRest(QTcpSocket *socket,
                       const FakeXc2HttpRequest &request);
    void runProbeStompScript(QWebSocket *socket,
                             const ktm::xc2::Xc2StompFrame &frame);
    void classifyRestOperation(const FakeXc2HttpRequest &request);
    void classifyStompOperation(const ktm::xc2::Xc2StompFrame &frame);
    void captureMessage(QWebSocket *socket,
                        FakeXc2WebSocketMessageKind kind,
                        const QByteArray &payload);
    static FakeXc2HttpRequest parseRequest(const QByteArray &headerBlock);

    QTcpServer *m_tcpServer = nullptr;
    QWebSocketServer *m_webSocketServer = nullptr;
    QList<RawConnection *> m_rawConnections;
    QList<QWebSocket *> m_webSockets;
    QList<FakeXc2HttpRequest> m_restRequests;
    QList<FakeXc2HttpRequest> m_capturedRestRequests;
    QList<FakeXc2HttpRequest> m_upgradeRequests;
    QList<FakeXc2WebSocketMessage> m_messages;
    QList<ktm::xc2::Xc2StompFrame> m_stompFrames;
    QList<ktm::xc2::Xc2StompFrame> m_sentStompFrames;
    QList<ktm::xc2::Xc2Error> m_decodeErrors;
    QHash<QWebSocket *, ktm::xc2::Xc2StompCodec> m_codecs;
    QByteArray m_restCookie = QByteArrayLiteral("session=synthetic");
    UpgradeMode m_upgradeMode = UpgradeMode::WebSocket;
    QUrl m_redirectTarget;
    quint64 m_outgoingFrameSize = 0;
    ProbeScript m_probeScript = ProbeScript::Disabled;
    int m_probeDelayMs = 250;
    int m_connectionCount = 0;
    int m_webSocketConnectionCount = 0;
    int m_stateChangingRestRequestCount = 0;
    int m_unexpectedOperationCount = 0;
    QWebSocketProtocol::CloseCode m_lastPeerCloseCode =
        QWebSocketProtocol::CloseCodeNormal;
};

Q_DECLARE_METATYPE(FakeXc2WebSocketMessageKind)
Q_DECLARE_METATYPE(FakeXc2TransportServer::ProbeScript)
