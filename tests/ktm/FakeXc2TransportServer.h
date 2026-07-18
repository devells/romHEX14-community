#pragma once

#include "ktm/xc2/Xc2StompCodec.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QSet>
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
    QByteArray version;
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
    struct Action {
        enum class Type {
            HttpResponse,
            SendStompFrame,
            CloseHttp,
            CloseWebSocket
        };

        enum class Target {
            EventSocket,
            WebSocket,
            ExpectationSocket
        };

        QString label;
        Type type = Type::HttpResponse;
        Target target = Target::EventSocket;
        QString targetExpectation;
        int delayMs = 0;
        int status = 200;
        QByteArray reason = QByteArrayLiteral("OK");
        QByteArray body;
        QList<QPair<QByteArray, QByteArray>> headers;
        ktm::xc2::Xc2StompFrame stompFrame;
        bool binary = false;
        QWebSocketProtocol::CloseCode closeCode =
            QWebSocketProtocol::CloseCodeNormal;
        QString closeReason = QStringLiteral("scripted close");
    };

    struct Expectation {
        enum class Protocol { Http, Stomp };

        QString label;
        Protocol protocol = Protocol::Http;
        bool webSocketUpgrade = false;
        QByteArray method;
        QByteArray target;
        QByteArray body;
        QList<QPair<QByteArray, QByteArray>> headers;
        QList<QByteArray> absentHeaders;
        ktm::xc2::Xc2StompFrame stompFrame;
        QList<Action> actions;
    };

    struct ScriptPhase {
        QString label;
        bool unordered = false;
        QList<Expectation> expectations;
        QList<Action> completionActions;
    };

    struct TraceEvent {
        enum class Direction { Received, Sent, Canceled };

        quint64 sequence = 0;
        Direction direction = Direction::Received;
        Expectation::Protocol protocol = Expectation::Protocol::Http;
        quint64 connectionId = 0;
        QString label;
        FakeXc2HttpRequest http;
        ktm::xc2::Xc2StompFrame stompFrame;
    };

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
        ApprovedTopicMessagesBeforeReceipt,
        DeadlineBoundaryTerminalBurst,
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
    int webSocketDisconnectionCount() const;
    int openConnectionCount() const;
    int stateChangingRestRequestCount() const;
    int unexpectedOperationCount() const;
    int pendingActionCount() const;
    int pendingUpgradeConnectionCount() const;
    int canceledActionCount() const;
    int liveSocketObjectCount() const;
    bool scriptExhausted() const;
    int terminalBurstExecutionCount() const;
    int terminalCloseAttemptCount() const;
    const QList<FakeXc2HttpRequest> &restRequests() const;
    const QList<FakeXc2HttpRequest> &capturedRestRequests() const;
    const QList<QByteArray> &restResponses() const;
    const QList<FakeXc2HttpRequest> &upgradeRequests() const;
    const QList<QByteArray> &upgradeResponses() const;
    const QList<FakeXc2WebSocketMessage> &messages() const;
    const QList<ktm::xc2::Xc2StompFrame> &stompFrames() const;
    const QList<ktm::xc2::Xc2StompFrame> &sentStompFrames() const;
    const QList<ktm::xc2::Xc2StompFrame> &
    attemptedTerminalFrames() const;
    const QList<ktm::xc2::Xc2Error> &decodeErrors() const;
    const QList<TraceEvent> &trace() const;
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
    bool setScript(QList<ScriptPhase> phases, QString *error = nullptr);
    bool performAction(const Action &action);

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
    void terminalBurstExecuted();

private:
    struct RawConnection;
    struct PendingAction;
    struct ResolvedAction {
        Action action;
        QPointer<QTcpSocket> httpSocket;
        QPointer<QWebSocket> webSocket;
        quint64 connectionId = 0;
        bool targetWasPresent = false;
    };

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
    bool validateScriptHttp(const FakeXc2HttpRequest &request,
                            bool upgrade) const;
    bool consumeScriptHttp(QTcpSocket *socket,
                           const FakeXc2HttpRequest &request,
                           bool upgrade, quint64 connectionId);
    bool consumeScriptStomp(QWebSocket *socket,
                            const ktm::xc2::Xc2StompFrame &frame,
                            quint64 connectionId);
    bool consumeExpectation(const Expectation &expectation, int index,
                            QTcpSocket *httpSocket,
                            QWebSocket *webSocket,
                            quint64 connectionId,
                            const FakeXc2HttpRequest *request,
                            const ktm::xc2::Xc2StompFrame *frame);
    QList<ResolvedAction> resolveActions(
        const QList<Action> &actions,
        QTcpSocket *eventHttpSocket,
        QWebSocket *eventWebSocket,
        quint64 eventConnectionId) const;
    void runActions(const QList<ResolvedAction> &actions);
    bool scheduleAction(const Action &action,
                        QTcpSocket *httpSocket,
                        QWebSocket *webSocket,
                        quint64 connectionId);
    bool executeAction(const Action &action,
                       QTcpSocket *httpSocket,
                       QWebSocket *webSocket,
                       quint64 connectionId);
    bool sendFrameTo(QWebSocket *socket,
                     const ktm::xc2::Xc2StompFrame &frame,
                     bool binary = false);
    void cancelPendingActions(QTcpSocket *socket);
    void cancelPendingActions(QWebSocket *socket);
    void appendTrace(TraceEvent event);
    RawConnection *rawConnection(QTcpSocket *socket,
                                 quint64 connectionId) const;
    QWebSocket *activeWebSocket() const;
    static FakeXc2HttpRequest parseRequest(const QByteArray &headerBlock);

    QTcpServer *m_tcpServer = nullptr;
    QWebSocketServer *m_webSocketServer = nullptr;
    QList<RawConnection *> m_rawConnections;
    QList<QWebSocket *> m_webSockets;
    QList<FakeXc2HttpRequest> m_restRequests;
    QList<FakeXc2HttpRequest> m_capturedRestRequests;
    QList<QByteArray> m_restResponses;
    QList<FakeXc2HttpRequest> m_upgradeRequests;
    QList<QByteArray> m_upgradeResponses;
    QList<FakeXc2WebSocketMessage> m_messages;
    QList<ktm::xc2::Xc2StompFrame> m_stompFrames;
    QList<ktm::xc2::Xc2StompFrame> m_sentStompFrames;
    QList<ktm::xc2::Xc2StompFrame> m_attemptedTerminalFrames;
    QList<ktm::xc2::Xc2Error> m_decodeErrors;
    QHash<QWebSocket *, ktm::xc2::Xc2StompCodec> m_codecs;
    QHash<QWebSocket *, quint64> m_webSocketIds;
    QList<quint64> m_pendingUpgradeConnectionIds;
    QPointer<QWebSocket> m_activeWebSocket;
    QList<QPointer<QObject>> m_retiredSocketObjects;
    QList<ScriptPhase> m_script;
    qsizetype m_scriptPhaseIndex = 0;
    QSet<int> m_consumedExpectations;
    QHash<QString, QPointer<QTcpSocket>> m_phaseHttpSockets;
    QHash<QString, QPointer<QWebSocket>> m_phaseWebSockets;
    QHash<QString, quint64> m_phaseConnectionIds;
    QList<PendingAction *> m_pendingActions;
    QList<TraceEvent> m_trace;
    quint64 m_nextTraceSequence = 1;
    quint64 m_nextConnectionId = 1;
    bool m_scriptEnabled = false;
    QByteArray m_restCookie = QByteArrayLiteral("session=synthetic");
    UpgradeMode m_upgradeMode = UpgradeMode::WebSocket;
    QUrl m_redirectTarget;
    quint64 m_outgoingFrameSize = 0;
    ProbeScript m_probeScript = ProbeScript::Disabled;
    int m_probeDelayMs = 250;
    int m_connectionCount = 0;
    int m_webSocketConnectionCount = 0;
    int m_webSocketDisconnectionCount = 0;
    int m_stateChangingRestRequestCount = 0;
    int m_unexpectedOperationCount = 0;
    int m_canceledActionCount = 0;
    int m_terminalBurstExecutionCount = 0;
    int m_terminalCloseAttemptCount = 0;
    QElapsedTimer m_connectionElapsed;
    QWebSocketProtocol::CloseCode m_lastPeerCloseCode =
        QWebSocketProtocol::CloseCodeNormal;
    QUrl m_lastRequestUrl;
    QString m_lastNegotiatedSubprotocol;
};

Q_DECLARE_METATYPE(FakeXc2WebSocketMessageKind)
Q_DECLARE_METATYPE(FakeXc2TransportServer::ProbeScript)
