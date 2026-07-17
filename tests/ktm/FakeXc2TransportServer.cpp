#include "FakeXc2TransportServer.h"

#include "ktm/xc2/Xc2ContractProfile.h"

#include <QAbstractSocket>
#include <QCryptographicHash>
#include <QNetworkProxy>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketProtocol>
#include <QWebSocketServer>

#include <utility>

namespace {

bool headerEquals(const QByteArray &left, const QByteArray &right)
{
    return left.compare(right, Qt::CaseInsensitive) == 0;
}

QByteArray httpResponse(int status,
                        const QByteArray &reason,
                        const QByteArray &body,
                        const QByteArray &extraHeaders = {})
{
    return QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status) + ' '
        + reason + QByteArrayLiteral("\r\n") + extraHeaders
        + QByteArrayLiteral("Content-Length: ")
        + QByteArray::number(body.size())
        + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body;
}

QByteArray validErrorPayload(int status)
{
    return QByteArrayLiteral("{\"status\":") + QByteArray::number(status)
        + QByteArrayLiteral(
            ",\"message\":\"BACKEND_MESSAGE_SENTINEL_6f25\","
            "\"code\":7319,"
            "\"devMessage\":\"DEVELOPER_SENTINEL_844a\","
            "\"info\":\"INFO_SENTINEL_502d\"}");
}

QByteArray currentUserPayload(const QStringList &permissions,
                              const QByteArray &loginName =
                                  QByteArrayLiteral("LOGIN_SENTINEL_15c8"),
                              const QByteArray &name =
                                  QByteArrayLiteral("NAME_SENTINEL_b97e"))
{
    QByteArray permissionJson;
    for (const QString &permission : permissions) {
        if (!permissionJson.isEmpty())
            permissionJson += ',';
        permissionJson += '"' + permission.toUtf8() + '"';
    }
    return QByteArrayLiteral("{\"loginName\":\"") + loginName
        + QByteArrayLiteral("\",\"name\":\"") + name
        + QByteArrayLiteral(
            "\",\"dealerId\":\"DEALER_SENTINEL_51aa\","
            "\"country\":\"AT\",\"address1\":\"ADDRESS_SENTINEL_70d4\","
            "\"address2\":\"\",\"dealerType\":\"DEALER\","
            "\"audience\":\"KTM\",\"permissions\":[")
        + permissionJson
        + QByteArrayLiteral(
            "],\"sessionIndex\":\"SESSION_SENTINEL_5ed1\"}");
}

QByteArray methodName(ktm::xc2::HttpMethod method)
{
    switch (method) {
    case ktm::xc2::HttpMethod::Get:
        return QByteArrayLiteral("GET");
    case ktm::xc2::HttpMethod::PostJson:
    case ktm::xc2::HttpMethod::PostForm:
        return QByteArrayLiteral("POST");
    case ktm::xc2::HttpMethod::Delete:
        return QByteArrayLiteral("DELETE");
    }
    return {};
}

bool targetMatchesProfilePath(const QByteArray &target,
                              const QByteArray &profileTarget)
{
    const QByteArray marker = QByteArrayLiteral("{ecuId}");
    const qsizetype markerIndex = profileTarget.indexOf(marker);
    if (markerIndex < 0)
        return target == profileTarget;
    const QByteArray prefix = profileTarget.left(markerIndex);
    const QByteArray suffix = profileTarget.mid(markerIndex + marker.size());
    if (!target.startsWith(prefix) || !target.endsWith(suffix)
        || target.size() <= prefix.size() + suffix.size()) {
        return false;
    }
    const QByteArray value = target.mid(
        prefix.size(), target.size() - prefix.size() - suffix.size());
    return !value.contains('/');
}

} // namespace

struct FakeXc2TransportServer::RawConnection {
    QTcpSocket *socket = nullptr;
    bool decided = false;
};

QByteArray FakeXc2HttpRequest::headerValue(const QByteArray &name) const
{
    const QList<QByteArray> values = headerValues(name);
    return values.isEmpty() ? QByteArray() : values.constFirst();
}

QList<QByteArray> FakeXc2HttpRequest::headerValues(
    const QByteArray &name) const
{
    QList<QByteArray> values;
    for (const auto &header : headers) {
        if (headerEquals(header.first, name))
            values.append(header.second);
    }
    return values;
}

QList<QByteArray> FakeXc2HttpRequest::rawHeaderLinesMatchingName(
    const QByteArray &name) const
{
    QList<QByteArray> matches;
    qsizetype start = rawHeaderBlock.indexOf("\r\n");
    if (start < 0)
        return matches;
    start += 2;
    while (start < rawHeaderBlock.size()) {
        const qsizetype end = rawHeaderBlock.indexOf("\r\n", start);
        if (end < 0)
            break;
        const QByteArray line = rawHeaderBlock.mid(start, end - start);
        if (line.isEmpty())
            break;
        const qsizetype colon = line.indexOf(':');
        if (colon > 0
            && headerEquals(line.left(colon), name)) {
            matches.append(line);
        }
        start = end + 2;
    }
    return matches;
}

FakeXc2TransportServer::FakeXc2TransportServer(QObject *parent)
    : QObject(parent),
      m_tcpServer(new QTcpServer(this)),
      m_webSocketServer(new QWebSocketServer(
          QStringLiteral("fake-xc2"), QWebSocketServer::NonSecureMode, this))
{
    m_webSocketServer->setProxy(QNetworkProxy::NoProxy);
    m_webSocketServer->setSupportedSubprotocols(
        {QStringLiteral("v12.stomp")});
    connect(m_tcpServer, &QTcpServer::newConnection,
            this, &FakeXc2TransportServer::acceptConnections);
    connect(m_webSocketServer, &QWebSocketServer::newConnection,
            this, &FakeXc2TransportServer::acceptWebSockets);
    m_tcpServer->listen(QHostAddress::LocalHost, 0);
}

FakeXc2TransportServer::~FakeXc2TransportServer()
{
    for (RawConnection *connection : std::as_const(m_rawConnections)) {
        if (connection->socket)
            connection->socket->disconnect(this);
        delete connection;
    }
    m_rawConnections.clear();
}

bool FakeXc2TransportServer::isListening() const
{
    return m_tcpServer->isListening();
}

QString FakeXc2TransportServer::errorString() const
{
    return m_tcpServer->errorString();
}

QHostAddress FakeXc2TransportServer::address() const
{
    return m_tcpServer->serverAddress();
}

quint16 FakeXc2TransportServer::port() const
{
    return m_tcpServer->serverPort();
}

QByteArray FakeXc2TransportServer::encodedRestBase() const
{
    return QByteArrayLiteral("http://127.0.0.1:")
        + QByteArray::number(port()) + QByteArrayLiteral("/xc2/1.0");
}

int FakeXc2TransportServer::connectionCount() const
{
    return m_connectionCount;
}

int FakeXc2TransportServer::restRequestCount() const
{
    return m_restRequests.size();
}

int FakeXc2TransportServer::upgradeRequestCount() const
{
    return m_upgradeRequests.size();
}

int FakeXc2TransportServer::webSocketConnectionCount() const
{
    return m_webSocketConnectionCount;
}

int FakeXc2TransportServer::webSocketDisconnectionCount() const
{
    return m_webSocketDisconnectionCount;
}

int FakeXc2TransportServer::openConnectionCount() const
{
    int count = 0;
    for (const RawConnection *connection : m_rawConnections) {
        if (connection->socket
            && connection->socket->state()
                != QAbstractSocket::UnconnectedState) {
            ++count;
        }
    }
    for (const QWebSocket *socket : m_webSockets) {
        if (socket->state() != QAbstractSocket::UnconnectedState)
            ++count;
    }
    return count;
}

int FakeXc2TransportServer::stateChangingRestRequestCount() const
{
    return m_stateChangingRestRequestCount;
}

int FakeXc2TransportServer::unexpectedOperationCount() const
{
    return m_unexpectedOperationCount;
}

int FakeXc2TransportServer::terminalBurstExecutionCount() const
{
    return m_terminalBurstExecutionCount;
}

int FakeXc2TransportServer::terminalCloseAttemptCount() const
{
    return m_terminalCloseAttemptCount;
}

const QList<FakeXc2HttpRequest> &FakeXc2TransportServer::restRequests() const
{
    return m_restRequests;
}

const QList<FakeXc2HttpRequest> &
FakeXc2TransportServer::capturedRestRequests() const
{
    return m_capturedRestRequests;
}

const QList<QByteArray> &FakeXc2TransportServer::restResponses() const
{
    return m_restResponses;
}

const QList<FakeXc2HttpRequest> &FakeXc2TransportServer::upgradeRequests() const
{
    return m_upgradeRequests;
}

const QList<QByteArray> &FakeXc2TransportServer::upgradeResponses() const
{
    return m_upgradeResponses;
}

const QList<FakeXc2WebSocketMessage> &FakeXc2TransportServer::messages() const
{
    return m_messages;
}

const QList<ktm::xc2::Xc2StompFrame> &
FakeXc2TransportServer::stompFrames() const
{
    return m_stompFrames;
}

const QList<ktm::xc2::Xc2StompFrame> &
FakeXc2TransportServer::sentStompFrames() const
{
    return m_sentStompFrames;
}

const QList<ktm::xc2::Xc2StompFrame> &
FakeXc2TransportServer::attemptedTerminalFrames() const
{
    return m_attemptedTerminalFrames;
}

const QList<ktm::xc2::Xc2Error> &FakeXc2TransportServer::decodeErrors() const
{
    return m_decodeErrors;
}

QUrl FakeXc2TransportServer::requestUrl() const
{
    return m_webSockets.isEmpty() ? QUrl() : m_webSockets.constLast()->requestUrl();
}

QString FakeXc2TransportServer::negotiatedSubprotocol() const
{
    return m_webSockets.isEmpty()
        ? QString() : m_webSockets.constLast()->subprotocol();
}

QWebSocketProtocol::CloseCode
FakeXc2TransportServer::lastPeerCloseCode() const
{
    return m_lastPeerCloseCode;
}

void FakeXc2TransportServer::setRestCookie(QByteArray cookie)
{
    m_restCookie = std::move(cookie);
}

void FakeXc2TransportServer::setUpgradeMode(UpgradeMode mode)
{
    m_upgradeMode = mode;
}

void FakeXc2TransportServer::setRedirectTarget(QUrl target)
{
    m_redirectTarget = std::move(target);
}

void FakeXc2TransportServer::setSupportedSubprotocols(QStringList protocols)
{
    m_webSocketServer->setSupportedSubprotocols(protocols);
}

void FakeXc2TransportServer::setOutgoingFrameSize(quint64 bytes)
{
    m_outgoingFrameSize = bytes;
    if (!m_webSockets.isEmpty() && bytes > 0)
        m_webSockets.constLast()->setOutgoingFrameSize(bytes);
}

void FakeXc2TransportServer::setProbeScript(ProbeScript script)
{
    m_probeScript = script;
    switch (script) {
    case ProbeScript::UpgradeNoResponse:
        setUpgradeMode(UpgradeMode::NoResponse);
        break;
    case ProbeScript::UpgradeRejected:
        setUpgradeMode(UpgradeMode::Reject);
        break;
    case ProbeScript::UpgradeWrongSubprotocol:
        setUpgradeMode(UpgradeMode::DifferentSubprotocol);
        break;
    default:
        setUpgradeMode(UpgradeMode::WebSocket);
        break;
    }
}

void FakeXc2TransportServer::setProbeDelayMs(int delayMs)
{
    m_probeDelayMs = qMax(0, delayMs);
}

bool FakeXc2TransportServer::sendText(const QByteArray &payload)
{
    if (m_webSockets.isEmpty())
        return false;
    const QString message = QString::fromUtf8(payload);
    return m_webSockets.constLast()->sendTextMessage(message)
        == message.toUtf8().size();
}

bool FakeXc2TransportServer::sendBinary(const QByteArray &payload)
{
    if (m_webSockets.isEmpty())
        return false;
    return m_webSockets.constLast()->sendBinaryMessage(payload) == payload.size();
}

bool FakeXc2TransportServer::sendFrame(
    const ktm::xc2::Xc2StompFrame &frame, bool binary)
{
    const QByteArray payload = ktm::xc2::Xc2StompCodec::encode(frame);
    const bool sent = binary ? sendBinary(payload) : sendText(payload);
    if (sent)
        m_sentStompFrames.append(frame);
    return sent;
}

bool FakeXc2TransportServer::sendFrameAndClose(
    const ktm::xc2::Xc2StompFrame &frame,
    QWebSocketProtocol::CloseCode closeCode)
{
    if (m_webSockets.isEmpty())
        return false;
    QWebSocket *const socket = m_webSockets.constLast();
    const QByteArray payload = ktm::xc2::Xc2StompCodec::encode(frame);
    const QString message = QString::fromUtf8(payload);
    const bool queued = socket->sendTextMessage(message)
        == message.toUtf8().size();
    if (queued)
        m_sentStompFrames.append(frame);
    socket->close(closeCode,
                  QStringLiteral("scripted close"));
    return queued;
}

void FakeXc2TransportServer::closeWebSocket(int delayMs)
{
    if (m_webSockets.isEmpty())
        return;
    QWebSocket *socket = m_webSockets.constLast();
    QTimer::singleShot(qMax(0, delayMs), socket, [socket] {
        socket->close(QWebSocketProtocol::CloseCodeNormal,
                      QStringLiteral("scripted close"));
    });
}

void FakeXc2TransportServer::acceptConnections()
{
    while (m_tcpServer->hasPendingConnections()) {
        QTcpSocket *socket = m_tcpServer->nextPendingConnection();
        if (!m_connectionElapsed.isValid())
            m_connectionElapsed.start();
        ++m_connectionCount;
        auto *connection = new RawConnection{socket, false};
        m_rawConnections.append(connection);
        connect(socket, &QTcpSocket::readyRead, this,
                [this, socket] { inspect(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, connection] {
            QTcpSocket *const socket = connection->socket;
            if (m_rawConnections.removeOne(connection)) {
                delete connection;
                socket->deleteLater();
            }
            emit connectionClosed();
        });
        const QPointer<FakeXc2TransportServer> guard(this);
        inspect(socket);
        if (guard.isNull())
            return;
    }
}

void FakeXc2TransportServer::inspect(QTcpSocket *socket)
{
    RawConnection *connection = nullptr;
    for (RawConnection *candidate : std::as_const(m_rawConnections)) {
        if (candidate->socket == socket) {
            connection = candidate;
            break;
        }
    }
    if (!connection || connection->decided)
        return;

    const QByteArray bytes = socket->peek(64 * 1024 + 1);
    const qsizetype marker = bytes.indexOf("\r\n\r\n");
    if (marker < 0) {
        if (bytes.size() > 64 * 1024) {
            connection->decided = true;
            socket->abort();
        }
        return;
    }
    if (marker > 64 * 1024) {
        connection->decided = true;
        socket->abort();
        return;
    }

    const QByteArray headerBlock = bytes.left(marker + 4);
    FakeXc2HttpRequest request = parseRequest(headerBlock);
    qint64 contentLength = 0;
    const QList<QByteArray> contentLengths =
        request.headerValues(QByteArrayLiteral("Content-Length"));
    if (!contentLengths.isEmpty()) {
        bool lengthOk = false;
        contentLength = contentLengths.constFirst().toLongLong(&lengthOk);
        if (!lengthOk || contentLength < 0
            || contentLength > 8 * 1024 * 1024) {
            connection->decided = true;
            socket->abort();
            return;
        }
    }
    const qint64 totalRequestBytes = marker + 4 + contentLength;
    if (bytes.size() < totalRequestBytes)
        return;
    request.body = bytes.mid(marker + 4, contentLength);
    const bool requestsUpgrade =
        request.target == QByteArrayLiteral("/xc2-websocket")
        && request.headerValue(QByteArrayLiteral("Upgrade")).compare(
               QByteArrayLiteral("websocket"), Qt::CaseInsensitive) == 0;
    const bool isUpgrade = requestsUpgrade
        && request.method == QByteArrayLiteral("GET");
    connection->decided = true;

    if (!isUpgrade) {
        socket->read(totalRequestBytes);
        m_capturedRestRequests.append(request);
        classifyRestOperation(request);
        const bool healthTarget = request.target
            == QByteArrayLiteral("/xc2/1.0/serviceStatus/status");
        const bool currentUserTarget = request.target
            == QByteArrayLiteral("/xc2/1.0/auth/currentUser");
        const bool exactRest = request.method == QByteArrayLiteral("GET")
            && (healthTarget
                || (m_probeScript != ProbeScript::Disabled
                    && currentUserTarget));
        if (exactRest) {
            m_restRequests.append(request);
            const QPointer<FakeXc2TransportServer> guard(this);
            emit restRequestCaptured();
            if (guard.isNull())
                return;
            respondToRest(socket, request);
            return;
        }

        QByteArray response;
        if (request.method != QByteArrayLiteral("GET")
            && (healthTarget || currentUserTarget || requestsUpgrade)) {
            response = QByteArrayLiteral(
                "HTTP/1.1 405 Method Not Allowed\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n");
        } else {
            response = QByteArrayLiteral(
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n");
        }
        socket->write(response);
        socket->disconnectFromHost();
        return;
    }

    if (m_probeScript != ProbeScript::Disabled) {
        const bool exactUpgrade = request.method == QByteArrayLiteral("GET")
            && request.target == QByteArrayLiteral("/xc2-websocket")
            && request.body.isEmpty();
        if (!exactUpgrade || !m_upgradeRequests.isEmpty())
            ++m_unexpectedOperationCount;
    }
    m_upgradeRequests.append(request);
    const QPointer<FakeXc2TransportServer> guard(this);
    emit upgradeRequestCaptured();
    if (guard.isNull())
        return;
    if (m_upgradeMode == UpgradeMode::NoResponse)
        return;
    if (m_upgradeMode == UpgradeMode::Redirect) {
        socket->read(totalRequestBytes);
        const QByteArray location = m_redirectTarget.toEncoded(QUrl::FullyEncoded);
        const QByteArray response = QByteArrayLiteral(
            "HTTP/1.1 302 Found\r\nLocation: ") + location
            + QByteArrayLiteral(
                "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        m_upgradeResponses.append(response);
        socket->write(response);
        socket->disconnectFromHost();
        return;
    }
    if (m_upgradeMode == UpgradeMode::Reject) {
        socket->read(totalRequestBytes);
        const QByteArray response = QByteArrayLiteral(
            "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n");
        m_upgradeResponses.append(response);
        socket->write(response);
        socket->disconnectFromHost();
        return;
    }
    if (m_upgradeMode == UpgradeMode::DifferentSubprotocol) {
        socket->read(totalRequestBytes);
        const QByteArray accept = QCryptographicHash::hash(
            request.headerValue(QByteArrayLiteral("Sec-WebSocket-Key"))
                + QByteArrayLiteral("258EAFA5-E914-47DA-95CA-C5AB0DC85B11"),
            QCryptographicHash::Sha1).toBase64();
        const QByteArray response = QByteArrayLiteral(
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: ") + accept
            + QByteArrayLiteral(
                "\r\nSec-WebSocket-Protocol: v11.stomp\r\n\r\n");
        m_upgradeResponses.append(response);
        socket->write(response);
        return;
    }

    socket->disconnect(this);
    m_rawConnections.removeOne(connection);
    delete connection;
    m_webSocketServer->handleConnection(socket);
}

void FakeXc2TransportServer::respondToRest(
    QTcpSocket *socket, const FakeXc2HttpRequest &request)
{
    const auto writeResponse = [this, socket](QByteArray response) {
        m_restResponses.append(response);
        socket->write(response);
    };
    const bool health = request.target
        == QByteArrayLiteral("/xc2/1.0/serviceStatus/status");
    if (health) {
        switch (m_probeScript) {
        case ProbeScript::HealthNoResponse:
            return;
        case ProbeScript::HealthTruncated:
            writeResponse(QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                "Connection: close\r\n\r\nal"));
            socket->disconnectFromHost();
            return;
        case ProbeScript::Health204:
            writeResponse(QByteArrayLiteral(
                "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"));
            socket->disconnectFromHost();
            return;
        case ProbeScript::HealthMalformedSuccess:
            writeResponse(httpResponse(
                200, QByteArrayLiteral("OK"), QByteArrayLiteral("dead"),
                QByteArrayLiteral("Content-Type: text/plain\r\n")));
            socket->disconnectFromHost();
            return;
        case ProbeScript::HealthBackendUnavailable:
            writeResponse(httpResponse(
                503, QByteArrayLiteral("Service Unavailable"),
                validErrorPayload(503),
                QByteArrayLiteral("Content-Type: application/json\r\n")));
            socket->disconnectFromHost();
            return;
        case ProbeScript::HealthAuth401:
            writeResponse(httpResponse(
                401, QByteArrayLiteral("Unauthorized"),
                validErrorPayload(401),
                QByteArrayLiteral("Content-Type: application/json\r\n")));
            socket->disconnectFromHost();
            return;
        case ProbeScript::HealthMalformedError:
            writeResponse(httpResponse(
                503, QByteArrayLiteral("Service Unavailable"),
                QByteArrayLiteral("{"),
                QByteArrayLiteral("Content-Type: application/json\r\n")));
            socket->disconnectFromHost();
            return;
        default:
            break;
        }

        QByteArray headers = QByteArrayLiteral("Content-Type: text/plain\r\n");
        if (!m_restCookie.isEmpty()) {
            headers += QByteArrayLiteral("Set-Cookie: ") + m_restCookie
                + QByteArrayLiteral("; Path=/\r\n");
        }
        writeResponse(httpResponse(200, QByteArrayLiteral("OK"),
                                   QByteArrayLiteral("alive"), headers));
        socket->disconnectFromHost();
        return;
    }

    switch (m_probeScript) {
    case ProbeScript::CurrentUserNoResponse:
        return;
    case ProbeScript::CurrentUserTruncated:
        writeResponse(QByteArrayLiteral(
            "HTTP/1.1 200 OK\r\nContent-Length: 64\r\n"
            "Connection: close\r\n\r\n{\"loginName\":"));
        socket->disconnectFromHost();
        return;
    case ProbeScript::CurrentUser204:
        writeResponse(QByteArrayLiteral(
            "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"));
        socket->disconnectFromHost();
        return;
    case ProbeScript::CurrentUserMalformedSuccess:
        writeResponse(httpResponse(
            200, QByteArrayLiteral("OK"),
            QByteArrayLiteral("{\"loginName\":"),
            QByteArrayLiteral("Content-Type: application/json\r\n")));
        socket->disconnectFromHost();
        return;
    case ProbeScript::CurrentUserBackendUnavailable:
        writeResponse(httpResponse(
            503, QByteArrayLiteral("Service Unavailable"),
            validErrorPayload(503),
            QByteArrayLiteral("Content-Type: application/json\r\n")));
        socket->disconnectFromHost();
        return;
    case ProbeScript::CurrentUserAuth401:
        writeResponse(httpResponse(
            401, QByteArrayLiteral("Unauthorized"), validErrorPayload(401),
            QByteArrayLiteral("Content-Type: application/json\r\n")));
        socket->disconnectFromHost();
        return;
    case ProbeScript::CurrentUserAuth403:
        writeResponse(httpResponse(
            403, QByteArrayLiteral("Forbidden"), validErrorPayload(403),
            QByteArrayLiteral("Content-Type: application/json\r\n")));
        socket->disconnectFromHost();
        return;
    case ProbeScript::CurrentUserMalformedAuthError:
        writeResponse(httpResponse(
            401, QByteArrayLiteral("Unauthorized"), QByteArrayLiteral("{"),
            QByteArrayLiteral("Content-Type: application/json\r\n")));
        socket->disconnectFromHost();
        return;
    default:
        break;
    }

    QStringList permissions{QStringLiteral("EcuDiagnosticRead")};
    QByteArray login = QByteArrayLiteral("LOGIN_SENTINEL_15c8");
    QByteArray name = QByteArrayLiteral("NAME_SENTINEL_b97e");
    if (m_probeScript == ProbeScript::CurrentUserMissingPermission)
        permissions = {QStringLiteral("VehicleDetectExecute")};
    else if (m_probeScript
             == ProbeScript::CurrentUserCaseMismatchedPermission) {
        permissions = {QStringLiteral("ecudiagnosticread")};
    } else if (m_probeScript == ProbeScript::CurrentUserBlankLogin) {
        login = QByteArrayLiteral("   ");
    } else if (m_probeScript == ProbeScript::CurrentUserBlankName) {
        name = QByteArrayLiteral("   ");
    }
    writeResponse(httpResponse(
        200, QByteArrayLiteral("OK"),
        currentUserPayload(permissions, login, name),
        QByteArrayLiteral("Content-Type: application/json\r\n")));
    socket->disconnectFromHost();
}

void FakeXc2TransportServer::classifyRestOperation(
    const FakeXc2HttpRequest &request)
{
    const ktm::xc2::Xc2ContractProfile &profile =
        ktm::xc2::Xc2ContractProfile::approved();
    for (const ktm::xc2::Endpoint endpoint
         : ktm::xc2::Xc2ContractProfile::allEndpoints()) {
        const ktm::xc2::EndpointSpec spec = profile.endpoint(endpoint);
        const QByteArray target = profile.restContext().toLatin1() + '/'
            + spec.path.toLatin1();
        if (targetMatchesProfilePath(request.target, target)
            && request.method == methodName(spec.method)
            && spec.semantics
                == ktm::xc2::OperationSemantics::StateChanging) {
            ++m_stateChangingRestRequestCount;
        }
    }

    if (m_probeScript == ProbeScript::Disabled)
        return;
    const qsizetype index = m_capturedRestRequests.size() - 1;
    const QByteArray expectedTarget = index == 0
        ? QByteArrayLiteral("/xc2/1.0/serviceStatus/status")
        : index == 1
            ? QByteArrayLiteral("/xc2/1.0/auth/currentUser")
            : QByteArray();
    if (request.method != QByteArrayLiteral("GET")
        || request.target != expectedTarget || !request.body.isEmpty()) {
        ++m_unexpectedOperationCount;
    }
}

void FakeXc2TransportServer::classifyStompOperation(
    const ktm::xc2::Xc2StompFrame &frame)
{
    if (m_probeScript == ProbeScript::Disabled)
        return;

    const qsizetype index = m_stompFrames.size() - 1;
    bool expected = frame.body.isEmpty();
    if (index == 0) {
        expected = expected
            && frame.command == QByteArrayLiteral("CONNECT")
            && frame.headers.size() == 3
            && frame.headers.value(QByteArrayLiteral("accept-version"))
                == QByteArrayLiteral("1.2")
            && frame.headers.value(QByteArrayLiteral("host"))
                == QByteArrayLiteral("127.0.0.1")
            && frame.headers.contains(QByteArrayLiteral("heart-beat"));
    } else if (index == 1 || index == 2) {
        const bool vci = index == 1;
        expected = expected
            && frame.command == QByteArrayLiteral("SUBSCRIBE")
            && frame.headers.size() == 3
            && frame.headers.value(QByteArrayLiteral("ack"))
                == QByteArrayLiteral("auto")
            && frame.headers.value(QByteArrayLiteral("destination"))
                == (vci ? QByteArrayLiteral("/topic/vci/status")
                        : QByteArrayLiteral("/topic/login"))
            && frame.headers.value(QByteArrayLiteral("id"))
                == (vci ? QByteArrayLiteral("vci-status-subscription")
                        : QByteArrayLiteral("login-subscription"));
    } else if (index == 3) {
        const QByteArray receipt =
            frame.headers.value(QByteArrayLiteral("receipt"));
        const QByteArray suffix = receipt.mid(
            QByteArrayLiteral("disconnect-").size());
        bool generationOk = !suffix.isEmpty();
        for (const char byte : suffix)
            generationOk = generationOk && byte >= '0' && byte <= '9';
        expected = expected
            && frame.command == QByteArrayLiteral("DISCONNECT")
            && frame.headers.size() == 1
            && receipt.startsWith(QByteArrayLiteral("disconnect-"))
            && generationOk;
    } else {
        expected = false;
    }
    if (!expected)
        ++m_unexpectedOperationCount;
}

void FakeXc2TransportServer::acceptWebSockets()
{
    while (m_webSocketServer->hasPendingConnections()) {
        QWebSocket *socket = m_webSocketServer->nextPendingConnection();
        socket->setParent(this);
        ++m_webSocketConnectionCount;
        m_webSockets.append(socket);
        m_codecs.insert(socket, {});
        if (m_outgoingFrameSize > 0)
            socket->setOutgoingFrameSize(m_outgoingFrameSize);
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket](const QString &message) {
            captureMessage(socket, FakeXc2WebSocketMessageKind::Text,
                           message.toUtf8());
        });
        connect(socket, &QWebSocket::binaryMessageReceived, this,
                [this, socket](const QByteArray &message) {
            captureMessage(socket, FakeXc2WebSocketMessageKind::Binary,
                           message);
        });
        connect(socket, &QWebSocket::disconnected, this, [this, socket] {
            m_lastPeerCloseCode = socket->closeCode();
            ++m_webSocketDisconnectionCount;
            emit webSocketDisconnected();
            emit connectionClosed();
        });
        const QPointer<FakeXc2TransportServer> guard(this);
        emit webSocketConnected();
        if (guard.isNull())
            return;
    }
}

void FakeXc2TransportServer::runProbeStompScript(
    QWebSocket *socket, const ktm::xc2::Xc2StompFrame &frame)
{
    if (m_probeScript == ProbeScript::Disabled)
        return;

    if (frame.command == QByteArrayLiteral("CONNECT")) {
        if (m_probeScript == ProbeScript::MissingConnected)
            return;
        ktm::xc2::Xc2StompFrame response;
        if (m_probeScript == ProbeScript::StompError) {
            response.command = QByteArrayLiteral("ERROR");
            response.headers.insert(
                QByteArrayLiteral("message"),
                QByteArrayLiteral("STOMP_MESSAGE_SENTINEL_95be"));
            response.body = QByteArrayLiteral("STOMP_BODY_SENTINEL_d71a");
        } else {
            response.command = QByteArrayLiteral("CONNECTED");
            if (m_probeScript != ProbeScript::MalformedConnected) {
                response.headers.insert(QByteArrayLiteral("version"),
                                        QByteArrayLiteral("1.2"));
            }
            response.headers.insert(QByteArrayLiteral("heart-beat"),
                                    QByteArrayLiteral("0,0"));
        }
        sendFrame(response);
        return;
    }

    if (m_probeScript == ProbeScript::ApprovedTopicMessagesBeforeReceipt
        && frame.command == QByteArrayLiteral("SUBSCRIBE")
        && frame.headers.value(QByteArrayLiteral("destination"))
            == QByteArrayLiteral("/topic/login")) {
        ktm::xc2::Xc2StompFrame vciMessage;
        vciMessage.command = QByteArrayLiteral("MESSAGE");
        vciMessage.headers.insert(
            QByteArrayLiteral("destination"),
            QByteArrayLiteral("/topic/vci/status"));
        vciMessage.headers.insert(
            QByteArrayLiteral("subscription"),
            QByteArrayLiteral("vci-status-subscription"));
        vciMessage.headers.insert(
            QByteArrayLiteral("message-id"),
            QByteArrayLiteral("probe-vci-message"));
        vciMessage.body = QByteArrayLiteral("{\"status\":\"available\"}");
        sendFrame(vciMessage);

        ktm::xc2::Xc2StompFrame loginMessage;
        loginMessage.command = QByteArrayLiteral("MESSAGE");
        loginMessage.headers.insert(
            QByteArrayLiteral("destination"),
            QByteArrayLiteral("/topic/login"));
        loginMessage.headers.insert(
            QByteArrayLiteral("subscription"),
            QByteArrayLiteral("login-subscription"));
        loginMessage.headers.insert(
            QByteArrayLiteral("message-id"),
            QByteArrayLiteral("probe-login-message"));
        loginMessage.body = QByteArrayLiteral("{\"loggedIn\":true}");
        sendFrame(loginMessage);
        return;
    }

    if (frame.command != QByteArrayLiteral("DISCONNECT"))
        return;
    const QByteArray receipt =
        frame.headers.value(QByteArrayLiteral("receipt"));
    if (m_probeScript == ProbeScript::MissingDisconnectReceipt)
        return;
    if (m_probeScript == ProbeScript::CloseBeforeDisconnectReceipt) {
        socket->close(QWebSocketProtocol::CloseCodeNormal,
                      QStringLiteral("close-before-receipt"));
        return;
    }

    ktm::xc2::Xc2StompFrame response;
    response.command = QByteArrayLiteral("RECEIPT");
    response.headers.insert(
        QByteArrayLiteral("receipt-id"),
        m_probeScript == ProbeScript::WrongDisconnectReceipt
            ? QByteArrayLiteral("disconnect-wrong-generation")
            : receipt);
    if (m_probeScript == ProbeScript::DeadlineBoundaryTerminalBurst) {
        const QPointer<FakeXc2TransportServer> owner(this);
        const QPointer<QWebSocket> guarded(socket);
        const int delayMs = qMax(
            0, m_probeDelayMs
                - int(m_connectionElapsed.isValid()
                          ? m_connectionElapsed.elapsed()
                          : 0));
        QTimer::singleShot(delayMs, Qt::PreciseTimer, this,
                           [owner, guarded, response] {
            if (!owner)
                return;
            ++owner->m_terminalBurstExecutionCount;

            ktm::xc2::Xc2StompFrame error;
            error.command = QByteArrayLiteral("ERROR");
            error.headers.insert(
                QByteArrayLiteral("message"),
                QByteArrayLiteral("BOUNDARY_ERROR_SENTINEL_4c2a"));
            error.body = QByteArrayLiteral("BOUNDARY_BODY_SENTINEL_d5e1");
            owner->m_attemptedTerminalFrames.append(error);
            owner->m_attemptedTerminalFrames.append(response);
            owner->sendFrame(error);
            owner->sendFrame(response);
            ++owner->m_terminalCloseAttemptCount;
            if (guarded) {
                guarded->close(
                    QWebSocketProtocol::CloseCodeGoingAway,
                    QStringLiteral("boundary-terminal-burst"));
            }
            emit owner->terminalBurstExecuted();
        });
        return;
    }
    if (m_probeScript == ProbeScript::LateDisconnectReceipt
        || m_probeScript
            == ProbeScript::ApprovedTopicMessagesBeforeReceipt) {
        const QPointer<FakeXc2TransportServer> owner(this);
        const QPointer<QWebSocket> guarded(socket);
        QTimer::singleShot(m_probeDelayMs, this,
                           [owner, guarded, response] {
            if (owner && guarded
                && guarded->state() == QAbstractSocket::ConnectedState) {
                owner->sendFrame(response);
            }
        });
        return;
    }

    sendFrame(response);
    if (m_probeScript == ProbeScript::ReceiptThenAbnormalClose) {
        socket->close(QWebSocketProtocol::CloseCodeGoingAway,
                      QStringLiteral("abnormal-after-receipt"));
    }
}

void FakeXc2TransportServer::captureMessage(
    QWebSocket *socket,
    FakeXc2WebSocketMessageKind kind,
    const QByteArray &payload)
{
    m_messages.append({kind, payload});
    const ktm::xc2::Xc2StompDecodeResult decoded =
        m_codecs[socket].feed(payload);
    for (const auto &frame : decoded.frames) {
        m_stompFrames.append(frame);
        classifyStompOperation(frame);
        const QPointer<FakeXc2TransportServer> guard(this);
        emit stompFrameReceived();
        if (guard.isNull())
            return;
        runProbeStompScript(socket, frame);
        if (guard.isNull())
            return;
    }
    m_decodeErrors.append(decoded.errors);
    emit webSocketMessageReceived();
}

FakeXc2HttpRequest FakeXc2TransportServer::parseRequest(
    const QByteArray &headerBlock)
{
    FakeXc2HttpRequest request;
    request.rawHeaderBlock = headerBlock;
    const QList<QByteArray> lines = headerBlock.split('\n');
    if (lines.isEmpty())
        return request;
    QByteArray requestLine = lines.front();
    if (requestLine.endsWith('\r'))
        requestLine.chop(1);
    const qsizetype firstSpace = requestLine.indexOf(' ');
    const qsizetype secondSpace = requestLine.indexOf(' ', firstSpace + 1);
    if (firstSpace > 0 && secondSpace > firstSpace) {
        request.method = requestLine.left(firstSpace);
        request.target = requestLine.mid(firstSpace + 1,
                                         secondSpace - firstSpace - 1);
    }
    for (qsizetype index = 1; index < lines.size(); ++index) {
        QByteArray line = lines.at(index);
        if (line.endsWith('\r'))
            line.chop(1);
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        request.headers.append({line.left(colon).trimmed(),
                                line.mid(colon + 1).trimmed()});
    }
    return request;
}
