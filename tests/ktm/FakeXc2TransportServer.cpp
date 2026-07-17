#include "FakeXc2TransportServer.h"

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

const QList<FakeXc2HttpRequest> &FakeXc2TransportServer::restRequests() const
{
    return m_restRequests;
}

const QList<FakeXc2HttpRequest> &FakeXc2TransportServer::upgradeRequests() const
{
    return m_upgradeRequests;
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
    return binary ? sendBinary(payload) : sendText(payload);
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
    const FakeXc2HttpRequest request = parseRequest(headerBlock);
    const bool requestsUpgrade =
        request.target == QByteArrayLiteral("/xc2-websocket")
        && request.headerValue(QByteArrayLiteral("Upgrade")).compare(
               QByteArrayLiteral("websocket"), Qt::CaseInsensitive) == 0;
    const bool isUpgrade = requestsUpgrade
        && request.method == QByteArrayLiteral("GET");
    connection->decided = true;

    if (!isUpgrade) {
        socket->read(marker + 4);
        const bool exactTarget = request.target
            == QByteArrayLiteral("/xc2/1.0/serviceStatus/status");
        const bool exactRest = exactTarget
            && request.method == QByteArrayLiteral("GET");
        QByteArray response;
        if (exactRest) {
            m_restRequests.append(request);
            const QPointer<FakeXc2TransportServer> guard(this);
            emit restRequestCaptured();
            if (guard.isNull())
                return;
            response = QByteArrayLiteral(
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n");
            if (!m_restCookie.isEmpty()) {
                response += QByteArrayLiteral("Set-Cookie: ") + m_restCookie
                    + QByteArrayLiteral("; Path=/\r\n");
            }
            response += QByteArrayLiteral(
                "Content-Length: 5\r\nConnection: close\r\n\r\nalive");
        } else if (request.method != QByteArrayLiteral("GET")
                   && (exactTarget || requestsUpgrade)) {
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

    m_upgradeRequests.append(request);
    const QPointer<FakeXc2TransportServer> guard(this);
    emit upgradeRequestCaptured();
    if (guard.isNull())
        return;
    if (m_upgradeMode == UpgradeMode::NoResponse)
        return;
    if (m_upgradeMode == UpgradeMode::Redirect) {
        socket->read(marker + 4);
        const QByteArray location = m_redirectTarget.toEncoded(QUrl::FullyEncoded);
        const QByteArray response = QByteArrayLiteral(
            "HTTP/1.1 302 Found\r\nLocation: ") + location
            + QByteArrayLiteral(
                "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        socket->write(response);
        socket->disconnectFromHost();
        return;
    }
    if (m_upgradeMode == UpgradeMode::DifferentSubprotocol) {
        socket->read(marker + 4);
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
        socket->write(response);
        return;
    }

    socket->disconnect(this);
    m_rawConnections.removeOne(connection);
    delete connection;
    m_webSocketServer->handleConnection(socket);
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
            emit webSocketDisconnected();
        });
        const QPointer<FakeXc2TransportServer> guard(this);
        emit webSocketConnected();
        if (guard.isNull())
            return;
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
        const QPointer<FakeXc2TransportServer> guard(this);
        emit stompFrameReceived();
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
