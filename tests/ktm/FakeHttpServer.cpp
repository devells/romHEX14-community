#include "FakeHttpServer.h"

#include <QAbstractSocket>
#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <limits>
#include <memory>

namespace {

QByteArray reasonPhrase(int status)
{
    switch (status) {
    case 200:
        return QByteArrayLiteral("OK");
    case 204:
        return QByteArrayLiteral("No Content");
    case 302:
        return QByteArrayLiteral("Found");
    case 307:
        return QByteArrayLiteral("Temporary Redirect");
    case 400:
        return QByteArrayLiteral("Bad Request");
    case 403:
        return QByteArrayLiteral("Forbidden");
    case 500:
        return QByteArrayLiteral("Internal Server Error");
    default:
        return QByteArrayLiteral("Synthetic");
    }
}

bool equalsHeaderName(const QByteArray &left, const QByteArray &right)
{
    return left.compare(right, Qt::CaseInsensitive) == 0;
}

QByteArray responseHead(const FakeHttpResponse &response,
                        qint64 contentLength)
{
    QByteArray wire = QByteArrayLiteral("HTTP/1.1 ")
        + QByteArray::number(response.status) + ' '
        + reasonPhrase(response.status) + QByteArrayLiteral("\r\n");

    for (const auto &header : response.headers) {
        if (equalsHeaderName(header.first, QByteArrayLiteral("Content-Length"))
            || equalsHeaderName(header.first,
                                QByteArrayLiteral("Connection"))) {
            continue;
        }
        wire += header.first + QByteArrayLiteral(": ") + header.second
            + QByteArrayLiteral("\r\n");
    }
    if (response.status != 204) {
        wire += QByteArrayLiteral("Content-Length: ")
            + QByteArray::number(contentLength) + QByteArrayLiteral("\r\n");
    }
    wire += QByteArrayLiteral("Connection: close\r\n\r\n");
    return wire;
}

} // namespace

QByteArray FakeHttpRequest::headerValue(const QByteArray &name) const
{
    for (const auto &header : headers) {
        if (equalsHeaderName(header.first, name))
            return header.second;
    }
    return {};
}

QList<QByteArray> FakeHttpRequest::headerValues(
    const QByteArray &name) const
{
    QList<QByteArray> values;
    for (const auto &header : headers) {
        if (equalsHeaderName(header.first, name))
            values.append(header.second);
    }
    return values;
}

FakeHttpServer::FakeHttpServer(const QHostAddress &address, QObject *parent)
    : QObject(parent), m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection,
            this, &FakeHttpServer::acceptConnections);
    m_server->listen(address, 0);
}

bool FakeHttpServer::isListening() const
{
    return m_server->isListening();
}

QString FakeHttpServer::errorString() const
{
    return m_server->errorString();
}

QHostAddress FakeHttpServer::address() const
{
    return m_server->serverAddress();
}

quint16 FakeHttpServer::port() const
{
    return m_server->serverPort();
}

int FakeHttpServer::connectionCount() const
{
    return m_connectionCount;
}

int FakeHttpServer::requestCount() const
{
    return m_requests.size();
}

const QList<FakeHttpRequest> &FakeHttpServer::requests() const
{
    return m_requests;
}

void FakeHttpServer::enqueueResponse(FakeHttpResponse response)
{
    m_responses.enqueue(std::move(response));
}

FakeHttpResponse FakeHttpServer::complete(
    int status,
    QByteArray body,
    QList<QPair<QByteArray, QByteArray>> headers)
{
    FakeHttpResponse response;
    response.status = status;
    response.body = std::move(body);
    response.headers = std::move(headers);
    return response;
}

FakeHttpResponse FakeHttpServer::closeBeforeStatus()
{
    FakeHttpResponse response;
    response.mode = FakeHttpResponse::Mode::CloseBeforeStatus;
    return response;
}

FakeHttpResponse FakeHttpServer::truncated(int status,
                                           QByteArray partialBody,
                                           qint64 declaredContentLength)
{
    FakeHttpResponse response;
    response.mode = FakeHttpResponse::Mode::TruncateBody;
    response.status = status;
    response.body = std::move(partialBody);
    response.declaredContentLength = declaredContentLength;
    return response;
}

FakeHttpResponse FakeHttpServer::drip(int status,
                                     int intervalMs,
                                     QByteArray chunk)
{
    FakeHttpResponse response;
    response.mode = FakeHttpResponse::Mode::DripWithoutCompletion;
    response.status = status;
    response.intervalMs = intervalMs;
    response.dripChunk = std::move(chunk);
    response.declaredContentLength = 8 * 1024 * 1024;
    return response;
}

FakeHttpResponse FakeHttpServer::neverRespond()
{
    FakeHttpResponse response;
    response.mode = FakeHttpResponse::Mode::NeverRespond;
    return response;
}

FakeHttpResponse FakeHttpServer::delayedComplete(int status,
                                                 QByteArray body,
                                                 int delayMs)
{
    FakeHttpResponse response;
    response.mode = FakeHttpResponse::Mode::DelayedComplete;
    response.status = status;
    response.body = std::move(body);
    response.intervalMs = delayMs;
    return response;
}

FakeHttpResponse FakeHttpServer::rawResponse(QByteArray wire)
{
    FakeHttpResponse response;
    response.mode = FakeHttpResponse::Mode::RawResponse;
    response.body = std::move(wire);
    return response;
}

FakeHttpResponse FakeHttpServer::rawResponseKeepOpen(QByteArray wire)
{
    FakeHttpResponse response;
    response.mode = FakeHttpResponse::Mode::RawResponseKeepOpen;
    response.body = std::move(wire);
    return response;
}

FakeHttpResponse FakeHttpServer::fragmentedRawResponse(
    QList<QByteArray> fragments,
    int intervalMs)
{
    FakeHttpResponse response;
    response.mode = FakeHttpResponse::Mode::FragmentedRawResponse;
    response.fragments = std::move(fragments);
    response.intervalMs = intervalMs;
    return response;
}

FakeHttpResponse FakeHttpServer::redirect(int status,
                                          const QUrl &location,
                                          QByteArray body)
{
    FakeHttpResponse response = complete(status, std::move(body));
    response.headers.append(
        {QByteArrayLiteral("Location"), location.toEncoded()});
    return response;
}

void FakeHttpServer::acceptConnections()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        ++m_connectionCount;
        m_connections.insert(socket, {});
        connect(socket, &QTcpSocket::readyRead, this,
                [this, socket] { consume(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            m_connections.remove(socket);
            socket->deleteLater();
        });
    }
}

void FakeHttpServer::consume(QTcpSocket *socket)
{
    auto iterator = m_connections.find(socket);
    if (iterator == m_connections.end() || iterator->captured)
        return;

    ConnectionState &state = iterator.value();
    state.bytes += socket->readAll();

    if (state.headerEnd < 0) {
        const qsizetype marker = state.bytes.indexOf("\r\n\r\n");
        if (marker < 0)
            return;
        state.headerEnd = marker + 4;

        const QList<QByteArray> lines = state.bytes.left(marker).split('\n');
        for (qsizetype i = 1; i < lines.size(); ++i) {
            QByteArray line = lines.at(i);
            if (line.endsWith('\r'))
                line.chop(1);
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            if (line.left(colon).trimmed().compare(
                    QByteArrayLiteral("Content-Length"),
                    Qt::CaseInsensitive) == 0) {
                bool ok = false;
                const qint64 parsed = line.mid(colon + 1).trimmed().toLongLong(&ok);
                if (ok && parsed >= 0)
                    state.contentLength = parsed;
            }
        }
    }

    if (state.contentLength
        > state.bytes.size() - state.headerEnd) {
        return;
    }

    const QByteArray headerBlock = state.bytes.left(state.headerEnd - 4);
    const QList<QByteArray> lines = headerBlock.split('\n');
    if (lines.isEmpty())
        return;

    QByteArray requestLine = lines.front();
    if (requestLine.endsWith('\r'))
        requestLine.chop(1);
    const qsizetype firstSpace = requestLine.indexOf(' ');
    const qsizetype secondSpace = requestLine.indexOf(' ', firstSpace + 1);
    if (firstSpace <= 0 || secondSpace <= firstSpace + 1)
        return;

    FakeHttpRequest request;
    request.method = requestLine.left(firstSpace);
    request.target = requestLine.mid(firstSpace + 1,
                                     secondSpace - firstSpace - 1);
    for (qsizetype i = 1; i < lines.size(); ++i) {
        QByteArray line = lines.at(i);
        if (line.endsWith('\r'))
            line.chop(1);
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        request.headers.append(
            {line.left(colon).trimmed(), line.mid(colon + 1).trimmed()});
    }
    request.body = state.bytes.mid(state.headerEnd, state.contentLength);

    state.captured = true;
    m_requests.append(std::move(request));
    emit requestCaptured();

    const FakeHttpResponse response = m_responses.isEmpty()
        ? complete(500, QByteArrayLiteral("unscripted request"))
        : m_responses.dequeue();
    sendResponse(socket, response);
}

void FakeHttpServer::sendResponse(QTcpSocket *socket,
                                  const FakeHttpResponse &response)
{
    using Mode = FakeHttpResponse::Mode;
    switch (response.mode) {
    case Mode::CloseBeforeStatus:
        socket->disconnectFromHost();
        return;
    case Mode::NeverRespond:
        return;
    case Mode::TruncateBody: {
        const qint64 declared = response.declaredContentLength >= 0
            ? response.declaredContentLength
            : response.body.size() + 1;
        const QByteArray wire = responseHead(response, declared)
            + response.body;
        if (socket->write(wire) != wire.size()) {
            socket->abort();
            return;
        }
        socket->disconnectFromHost();
        return;
    }
    case Mode::DripWithoutCompletion: {
        const qint64 declared = response.declaredContentLength >= 0
            ? response.declaredContentLength
            : std::numeric_limits<int>::max();
        socket->write(responseHead(response, declared));
        auto *timer = new QTimer(socket);
        timer->setTimerType(Qt::PreciseTimer);
        timer->setInterval(qMax(1, response.intervalMs));
        const QByteArray chunk = response.dripChunk.isEmpty()
            ? QByteArrayLiteral("x") : response.dripChunk;
        connect(timer, &QTimer::timeout, socket, [socket, chunk] {
            if (socket->state() == QAbstractSocket::ConnectedState)
                socket->write(chunk);
        });
        timer->start();
        return;
    }
    case Mode::DelayedComplete: {
        QTimer::singleShot(qMax(0, response.intervalMs), socket,
                           [socket, response] {
            if (socket->state() != QAbstractSocket::ConnectedState)
                return;
            const QByteArray wire =
                responseHead(response, response.body.size()) + response.body;
            if (socket->write(wire) != wire.size()) {
                socket->abort();
                return;
            }
            socket->disconnectFromHost();
        });
        return;
    }
    case Mode::RawResponse:
        if (socket->write(response.body) != response.body.size()) {
            socket->abort();
            return;
        }
        socket->disconnectFromHost();
        return;
    case Mode::RawResponseKeepOpen:
        if (socket->write(response.body) != response.body.size())
            socket->abort();
        return;
    case Mode::FragmentedRawResponse: {
        auto *timer = new QTimer(socket);
        timer->setTimerType(Qt::PreciseTimer);
        timer->setInterval(qMax(0, response.intervalMs));
        const auto index = std::make_shared<qsizetype>(0);
        const QList<QByteArray> fragments = response.fragments;
        connect(timer, &QTimer::timeout, socket,
                [socket, timer, fragments, index] {
            if (*index >= fragments.size()) {
                timer->stop();
                socket->disconnectFromHost();
                return;
            }
            const QByteArray &fragment = fragments.at((*index)++);
            if (socket->write(fragment) != fragment.size()) {
                timer->stop();
                socket->abort();
            }
        });
        timer->start();
        return;
    }
    case Mode::Complete:
        {
            const QByteArray wire =
                responseHead(response, response.body.size()) + response.body;
            if (socket->write(wire) != wire.size()) {
                socket->abort();
                return;
            }
        }
        socket->disconnectFromHost();
        return;
    }
}
