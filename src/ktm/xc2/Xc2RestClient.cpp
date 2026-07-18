#include "Xc2RestClient.h"

#include "Xc2ContractProfile.h"
#include "Xc2JsonCodec.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaObject>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkProxy>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

#include <utility>

namespace ktm::xc2 {
namespace {

constexpr qsizetype kMaximumHeaderBytes = 64 * 1024;
constexpr qsizetype kMaximumBodyBytes = 8 * 1024 * 1024;
constexpr qint64 kSocketReadBufferBytes = 16 * 1024;
constexpr qint64 kSocketReadChunkBytes = 4 * 1024;

Xc2Error contractError(QString message)
{
    Xc2Error error;
    error.category = Xc2ErrorCategory::Contract;
    error.message = std::move(message);
    return error;
}

Xc2Error transportError(Xc2TransportReason reason,
                        int httpStatus,
                        QByteArray rawPayload,
                        const QString &endpoint,
                        QString message)
{
    Xc2Error error;
    error.category = Xc2ErrorCategory::Transport;
    error.transportReason = reason;
    error.httpStatus = httpStatus;
    error.rawPayload = std::move(rawPayload);
    error.endpoint = endpoint;
    error.message = std::move(message);
    return error;
}

bool headerNameEquals(const QByteArray &left, const QByteArray &right)
{
    return left.compare(right, Qt::CaseInsensitive) == 0;
}

bool isTokenCharacter(char byte)
{
    const uchar value = static_cast<uchar>(byte);
    static const QByteArray separators("()<>@,;:\\\"/[]?={} \t");
    return value > 0x20 && value < 0x7f && !separators.contains(byte);
}

bool isHeaderName(const QByteArray &name)
{
    if (name.isEmpty())
        return false;
    for (const char byte : name) {
        if (!isTokenCharacter(byte))
            return false;
    }
    return true;
}

bool isHeaderValue(const QByteArray &value)
{
    for (const char byte : value) {
        const uchar character = static_cast<uchar>(byte);
        if ((character <= 0x1f && character != '\t')
            || character == 0x7f) {
            return false;
        }
    }
    return true;
}

void skipOptionalWhitespace(const QByteArray &line, qsizetype *position)
{
    while (*position < line.size()
           && (line.at(*position) == ' ' || line.at(*position) == '\t')) {
        ++*position;
    }
}

bool consumeToken(const QByteArray &line, qsizetype *position)
{
    const qsizetype start = *position;
    while (*position < line.size()
           && isTokenCharacter(line.at(*position))) {
        ++*position;
    }
    return *position > start;
}

bool isQuotedTextCharacter(char byte)
{
    const uchar value = static_cast<uchar>(byte);
    return value == '\t' || value == ' ' || value == 0x21
        || (value >= 0x23 && value <= 0x5b)
        || (value >= 0x5d && value <= 0x7e) || value >= 0x80;
}

bool isQuotedPairCharacter(char byte)
{
    const uchar value = static_cast<uchar>(byte);
    return value == '\t' || value == ' '
        || (value >= 0x21 && value <= 0x7e) || value >= 0x80;
}

bool consumeQuotedString(const QByteArray &line, qsizetype *position)
{
    if (*position >= line.size() || line.at(*position) != '"')
        return false;
    ++*position;
    while (*position < line.size()) {
        const char byte = line.at(*position);
        if (byte == '"') {
            ++*position;
            return true;
        }
        if (byte == '\\') {
            ++*position;
            if (*position >= line.size()
                || !isQuotedPairCharacter(line.at(*position))) {
                return false;
            }
            ++*position;
            continue;
        }
        if (!isQuotedTextCharacter(byte))
            return false;
        ++*position;
    }
    return false;
}

bool parseChunkSizeLine(const QByteArray &line, QByteArray *sizeText)
{
    qsizetype position = 0;
    while (position < line.size()) {
        const char byte = line.at(position);
        if (!((byte >= '0' && byte <= '9')
              || (byte >= 'a' && byte <= 'f')
              || (byte >= 'A' && byte <= 'F'))) {
            break;
        }
        ++position;
    }
    if (position == 0)
        return false;
    *sizeText = line.left(position);

    while (position < line.size()) {
        skipOptionalWhitespace(line, &position);
        if (position == line.size())
            return false;
        if (line.at(position) != ';')
            return false;
        ++position;
        skipOptionalWhitespace(line, &position);
        if (!consumeToken(line, &position))
            return false;
        const qsizetype afterName = position;
        skipOptionalWhitespace(line, &position);
        if (position == line.size())
            return position == afterName;
        if (position < line.size() && line.at(position) == '=') {
            ++position;
            skipOptionalWhitespace(line, &position);
            if (position >= line.size())
                return false;
            if (line.at(position) == '"') {
                if (!consumeQuotedString(line, &position))
                    return false;
            } else if (!consumeToken(line, &position)) {
                return false;
            }
            const qsizetype afterValue = position;
            skipOptionalWhitespace(line, &position);
            if (position == line.size())
                return position == afterValue;
        }
        if (position < line.size() && line.at(position) != ';')
            return false;
    }
    return true;
}

bool isApprovedSuccessStatus(Endpoint endpoint, int httpStatus)
{
    switch (endpoint) {
    case Endpoint::ServiceStatus:
    case Endpoint::CurrentUser:
    case Endpoint::DeviceLookup:
    case Endpoint::DeviceGet:
        return httpStatus == 200;
    case Endpoint::Shutdown:
    case Endpoint::DeviceGetSelected:
    case Endpoint::DeviceApply:
    case Endpoint::DeviceClose:
        return httpStatus == 200 || httpStatus == 204;
    default:
        return false;
    }
}

bool isSupportedEndpoint(Endpoint endpoint)
{
    switch (endpoint) {
    case Endpoint::ServiceStatus:
    case Endpoint::Shutdown:
    case Endpoint::CurrentUser:
    case Endpoint::DeviceLookup:
    case Endpoint::DeviceGet:
    case Endpoint::DeviceGetSelected:
    case Endpoint::DeviceApply:
    case Endpoint::DeviceClose:
        return true;
    default:
        return false;
    }
}

bool isDeviceOperation(Endpoint endpoint)
{
    switch (endpoint) {
    case Endpoint::DeviceLookup:
    case Endpoint::DeviceGet:
    case Endpoint::DeviceGetSelected:
    case Endpoint::DeviceApply:
    case Endpoint::DeviceClose:
        return true;
    default:
        return false;
    }
}

bool isSingleJsonValue(const QByteArray &body)
{
    QByteArray json = body;
    if (json.startsWith(QByteArrayLiteral("\xEF\xBB\xBF")))
        json.remove(0, 3);

    QByteArray wrapper;
    wrapper.reserve(json.size() + 2);
    wrapper += '[';
    wrapper += json;
    wrapper += ']';
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(wrapper, &parseError);
    return parseError.error == QJsonParseError::NoError
        && document.isArray() && document.array().size() == 1;
}

bool isDecimal(const QByteArray &value)
{
    if (value.isEmpty())
        return false;
    for (const char byte : value) {
        if (byte < '0' || byte > '9')
            return false;
    }
    return true;
}

QByteArray cookieHeader(const QNetworkCookieJar *jar, const QUrl &url)
{
    QByteArrayList values;
    const QList<QNetworkCookie> cookies = jar->cookiesForUrl(url);
    values.reserve(cookies.size());
    for (const QNetworkCookie &cookie : cookies)
        values.append(cookie.toRawForm(QNetworkCookie::NameAndValueOnly));
    return values.join(QByteArrayLiteral("; "));
}

} // namespace

struct Xc2RestClient::PendingRequest {
    enum class BodyMode {
        Undetermined,
        NoBody,
        ContentLength,
        Chunked,
        CloseDelimited,
    };

    Xc2RequestId id = 0;
    Endpoint endpoint = Endpoint::ServiceStatus;
    QPointer<QTcpSocket> socket;
    QTimer *totalDeadline = nullptr;
    QTimer *inactivityDeadline = nullptr;
    Xc2TransportReason forcedReason = Xc2TransportReason::None;
    QAbstractSocket::SocketError observedError =
        QAbstractSocket::UnknownSocketError;
    bool hasObservedError = false;
    bool completed = false;
    bool completionScheduled = false;
    bool requestWritten = false;
    QByteArray requestBytes;
    QByteArray receiveBuffer;
    QByteArray rawPayload;
    bool headersParsed = false;
    bool responseComplete = false;
    bool protocolFailure = false;
    QString protocolMessage;
    int httpStatus = 0;
    QList<QPair<QByteArray, QByteArray>> responseHeaders;
    BodyMode bodyMode = BodyMode::Undetermined;
    qint64 contentRemaining = 0;
    qint64 chunkRemaining = -1;
    bool expectChunkTerminator = false;
    bool readingTrailers = false;
    qsizetype trailerBytes = 0;
};

Xc2RestClient::Xc2RestClient(Xc2RestClientOptions options, QObject *parent)
    : QObject(parent),
      m_options(options),
      m_cookieJar(new QNetworkCookieJar(this))
{
}

Xc2RestClient::~Xc2RestClient()
{
    for (PendingRequest *pending : std::as_const(m_pending)) {
        pending->totalDeadline->stop();
        pending->inactivityDeadline->stop();
        if (pending->socket) {
            pending->socket->disconnect(this);
            pending->socket->abort();
        }
        delete pending;
    }
    m_pending.clear();
}

bool Xc2RestClient::setBaseUrl(const QByteArray &encodedLoopbackRestBase,
                              Xc2Error *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = contractError(message);
        return false;
    };

    if (!m_pending.isEmpty()) {
        return fail(QStringLiteral(
            "REST base cannot change while requests are pending"));
    }
    if (encodedLoopbackRestBase.isEmpty())
        return fail(QStringLiteral("REST base must be an absolute URL"));
    for (const char byte : encodedLoopbackRestBase) {
        const uchar value = static_cast<uchar>(byte);
        if (value <= 0x20 || value >= 0x7f) {
            return fail(QStringLiteral(
                "REST base must use visible ASCII URL syntax"));
        }
    }
    if (encodedLoopbackRestBase.size() < 8
        || encodedLoopbackRestBase.left(7).toLower()
            != QByteArrayLiteral("http://")) {
        return fail(QStringLiteral("REST base scheme must be http"));
    }
    if (encodedLoopbackRestBase.contains('?')
        || encodedLoopbackRestBase.contains('#')) {
        return fail(QStringLiteral(
            "REST base must not contain a query or fragment"));
    }

    const qsizetype pathStart = encodedLoopbackRestBase.indexOf('/', 7);
    if (pathStart < 0)
        return fail(QStringLiteral("REST base requires the approved context"));
    const QByteArray rawAuthority =
        encodedLoopbackRestBase.mid(7, pathStart - 7);
    const QByteArray rawPath = encodedLoopbackRestBase.mid(pathStart);
    const QByteArray restContext =
        Xc2ContractProfile::approved().restContext().toLatin1();
    if (rawPath != restContext
        && rawPath != restContext + QByteArrayLiteral("/")) {
        return fail(QStringLiteral("REST base path is not the approved context"));
    }
    if (rawAuthority.isEmpty() || rawAuthority.contains('@'))
        return fail(QStringLiteral("REST base must not contain user info"));

    QByteArray rawHost;
    QByteArray rawPort;
    bool bracketedHost = false;
    if (rawAuthority.startsWith('[')) {
        const qsizetype closeBracket = rawAuthority.indexOf(']');
        if (closeBracket <= 1
            || closeBracket + 1 >= rawAuthority.size()
            || rawAuthority.at(closeBracket + 1) != ':') {
            return fail(QStringLiteral("REST base has an invalid IPv6 authority"));
        }
        bracketedHost = true;
        rawHost = rawAuthority.mid(1, closeBracket - 1);
        rawPort = rawAuthority.mid(closeBracket + 2);
    } else {
        const qsizetype colon = rawAuthority.indexOf(':');
        if (colon <= 0 || colon != rawAuthority.lastIndexOf(':')) {
            return fail(QStringLiteral("REST base requires an explicit port"));
        }
        rawHost = rawAuthority.left(colon);
        rawPort = rawAuthority.mid(colon + 1);
    }
    if (!isDecimal(rawPort)) {
        return fail(QStringLiteral(
            "REST base requires an explicit valid port"));
    }
    bool portOk = false;
    const int port = rawPort.toInt(&portOk);
    if (!portOk || port <= 0 || port > 65535
        || rawPort != QByteArray::number(port)) {
        return fail(QStringLiteral("REST base requires an explicit valid port"));
    }

    const QString inputHost = QString::fromLatin1(rawHost);
    QString canonicalHost;
    if (inputHost.compare(QStringLiteral("localhost"),
                          Qt::CaseInsensitive) == 0) {
        if (bracketedHost)
            return fail(QStringLiteral("localhost must not use IPv6 brackets"));
        canonicalHost = QStringLiteral("127.0.0.1");
    } else {
        QHostAddress numericHost;
        if (!numericHost.setAddress(inputHost) || !numericHost.isLoopback()) {
            return fail(QStringLiteral(
                "REST base host must be a numeric loopback address or localhost"));
        }
        canonicalHost = numericHost.toString();
        const bool isIpv6 =
            numericHost.protocol() == QAbstractSocket::IPv6Protocol;
        if (bracketedHost != isIpv6
            || rawHost != canonicalHost.toLatin1()) {
            return fail(QStringLiteral(
                "REST base numeric address must use canonical syntax"));
        }
    }

    const QUrl parsed = QUrl::fromEncoded(
        encodedLoopbackRestBase, QUrl::StrictMode);
    if (!parsed.isValid() || parsed.isRelative())
        return fail(QStringLiteral("REST base must be an absolute URL"));

    QUrl canonicalBase;
    canonicalBase.setScheme(QStringLiteral("http"));
    canonicalBase.setHost(canonicalHost);
    canonicalBase.setPort(port);
    canonicalBase.setPath(QString::fromLatin1(restContext));

    QUrl canonicalWebSocket;
    canonicalWebSocket.setScheme(QStringLiteral("ws"));
    canonicalWebSocket.setHost(canonicalHost);
    canonicalWebSocket.setPort(port);
    canonicalWebSocket.setPath(
        Xc2ContractProfile::approved().webSocketPath());

    if (!m_baseUrl.isEmpty() && canonicalBase != m_baseUrl) {
        QNetworkCookieJar *oldJar = m_cookieJar;
        m_cookieJar = new QNetworkCookieJar(this);
        oldJar->deleteLater();
    }
    m_baseUrl = canonicalBase;
    m_webSocketUrl = canonicalWebSocket;
    m_canonicalHost = canonicalHost;
    m_explicitPort = port;
    if (error)
        *error = {};
    return true;
}

QUrl Xc2RestClient::baseUrl() const
{
    return m_baseUrl;
}

QUrl Xc2RestClient::webSocketUrl() const
{
    return m_webSocketUrl;
}

Xc2RequestId Xc2RestClient::requestServiceStatus()
{
    return startRequest(Endpoint::ServiceStatus);
}

Xc2RequestId Xc2RestClient::requestCurrentUser()
{
    return startRequest(Endpoint::CurrentUser);
}

Xc2RequestId Xc2RestClient::requestShutdown()
{
    return startRequest(Endpoint::Shutdown);
}

Xc2RequestId Xc2RestClient::requestDeviceLookup()
{
    return startRequest(Endpoint::DeviceLookup);
}

Xc2RequestId Xc2RestClient::requestDevices()
{
    return startRequest(Endpoint::DeviceGet);
}

Xc2RequestId Xc2RestClient::requestSelectedDevice()
{
    return startRequest(Endpoint::DeviceGetSelected);
}

Xc2RequestId Xc2RestClient::requestApplyDevice(
    const Xc2VciDevice &device)
{
    const QByteArray body = QJsonDocument(
        Xc2JsonCodec::vciDeviceJson(device)).toJson(QJsonDocument::Compact);
    return startRequest(Endpoint::DeviceApply, body);
}

Xc2RequestId Xc2RestClient::requestCloseDevice(
    const Xc2VciDevice &device)
{
    const QByteArray body = QJsonDocument(
        Xc2JsonCodec::vciDeviceJson(device)).toJson(QJsonDocument::Compact);
    return startRequest(Endpoint::DeviceClose, body);
}

Xc2Result<QByteArray> Xc2RestClient::cookieHeaderFor(
    const QByteArray &encodedUrl) const
{
    const auto fail = [](const QString &message) {
        return Xc2Result<QByteArray>::failure(contractError(message));
    };

    if (m_webSocketUrl.isEmpty())
        return fail(QStringLiteral("REST base has not been configured"));
    const QByteArray canonical =
        m_webSocketUrl.toEncoded(QUrl::FullyEncoded);
    if (encodedUrl != canonical) {
        return fail(QStringLiteral(
            "Cookie export URL must equal the derived WebSocket URL"));
    }

    return Xc2Result<QByteArray>::success(
        cookieHeader(m_cookieJar, m_webSocketUrl));
}

Xc2Result<QByteArray> Xc2RestClient::shutdownCookieHeaderFor(
    const QByteArray &encodedShutdownUrl) const
{
    const auto fail = [](const QString &message) {
        return Xc2Result<QByteArray>::failure(contractError(message));
    };

    if (m_baseUrl.isEmpty())
        return fail(QStringLiteral("REST base has not been configured"));
    const QUrl shutdownUrl = endpointUrl(Endpoint::Shutdown);
    const QByteArray canonical =
        shutdownUrl.toEncoded(QUrl::FullyEncoded);
    if (encodedShutdownUrl != canonical) {
        return fail(QStringLiteral(
            "Cookie export URL must equal the derived shutdown URL"));
    }

    return Xc2Result<QByteArray>::success(
        cookieHeader(m_cookieJar, shutdownUrl));
}

void Xc2RestClient::abort(Xc2RequestId id)
{
    forceStop(id, Xc2TransportReason::Canceled);
}

Xc2RequestId Xc2RestClient::startRequest(Endpoint endpoint,
                                         QByteArray jsonBody)
{
    if (m_baseUrl.isEmpty() || !isSupportedEndpoint(endpoint))
        return 0;

    const Xc2ContractProfile &profile = Xc2ContractProfile::approved();
    const EndpointSpec spec = profile.endpoint(endpoint);
    if (spec.maxAutomaticRetries != 0)
        return 0;
    if (spec.method == HttpMethod::Get) {
        if (!jsonBody.isEmpty())
            return 0;
    } else if (spec.method == HttpMethod::PostForm) {
        if (endpoint != Endpoint::Shutdown || !jsonBody.isEmpty())
            return 0;
    } else if (spec.method == HttpMethod::PostJson) {
        if ((endpoint != Endpoint::DeviceApply
             && endpoint != Endpoint::DeviceClose)
            || jsonBody.isEmpty()) {
            return 0;
        }
    } else {
        return 0;
    }

    Xc2RequestId id = m_nextRequestId++;
    while (id == 0 || m_pending.contains(id))
        id = m_nextRequestId++;

    auto *pending = new PendingRequest;
    pending->id = id;
    pending->endpoint = endpoint;
    pending->socket = new QTcpSocket(this);
    pending->socket->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    pending->socket->setReadBufferSize(kSocketReadBufferBytes);
    pending->totalDeadline = new QTimer(this);
    pending->totalDeadline->setSingleShot(true);
    pending->totalDeadline->setTimerType(Qt::PreciseTimer);
    pending->inactivityDeadline = new QTimer(this);
    pending->inactivityDeadline->setSingleShot(true);
    pending->inactivityDeadline->setTimerType(Qt::PreciseTimer);

    QByteArray host = m_canonicalHost.toLatin1();
    if (host.contains(':'))
        host = QByteArrayLiteral("[") + host + QByteArrayLiteral("]");
    host += ':' + QByteArray::number(m_explicitPort);
    const QByteArray target = profile.restContext().toLatin1()
        + '/' + spec.path.toLatin1();
    pending->requestBytes = spec.method == HttpMethod::Get
        ? QByteArrayLiteral("GET ") : QByteArrayLiteral("POST ");
    pending->requestBytes += target + QByteArrayLiteral(" HTTP/1.1\r\nHost: ")
        + host
        + QByteArrayLiteral(
            "\r\nAccept: */*\r\nAccept-Encoding: identity\r\n"
            "Connection: close\r\n");
    const QByteArray cookies = cookieHeader(m_cookieJar, endpointUrl(endpoint));
    if (!cookies.isEmpty()) {
        pending->requestBytes += QByteArrayLiteral("Cookie: ") + cookies
            + QByteArrayLiteral("\r\n");
    }
    if (spec.method == HttpMethod::PostForm) {
        pending->requestBytes += QByteArrayLiteral(
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: 0\r\n");
    } else if (spec.method == HttpMethod::PostJson) {
        pending->requestBytes += QByteArrayLiteral(
            "Content-Type: application/json\r\nContent-Length: ")
            + QByteArray::number(jsonBody.size()) + QByteArrayLiteral("\r\n");
    }
    pending->requestBytes += QByteArrayLiteral("\r\n");
    pending->requestBytes += jsonBody;
    m_pending.insert(id, pending);

    connect(pending->socket, &QTcpSocket::connected, this, [this, id] {
        const auto iterator = m_pending.find(id);
        if (iterator == m_pending.end() || !iterator.value()->socket)
            return;
        PendingRequest *record = iterator.value();
        record->requestWritten = true;
        if (record->socket->write(record->requestBytes)
            != record->requestBytes.size()) {
            failProtocol(record, QStringLiteral("Failed to write HTTP request"));
        }
    });
    connect(pending->socket, &QIODevice::readyRead, this,
            [this, id] { readAvailable(id); });
    connect(pending->socket, &QAbstractSocket::errorOccurred, this,
            [this, id](QAbstractSocket::SocketError socketError) {
        const auto iterator = m_pending.find(id);
        if (iterator == m_pending.end())
            return;
        iterator.value()->hasObservedError = true;
        iterator.value()->observedError = socketError;
        scheduleCompletion(id);
    });
    connect(pending->socket, &QAbstractSocket::disconnected, this,
            [this, id] { scheduleCompletion(id); });
    connect(pending->socket, &QAbstractSocket::stateChanged, this,
            [this, id](QAbstractSocket::SocketState state) {
        if (state == QAbstractSocket::UnconnectedState)
            scheduleCompletion(id);
    });
    connect(pending->totalDeadline, &QTimer::timeout, this, [this, id] {
        forceStop(id, Xc2TransportReason::Timeout);
    });
    connect(pending->inactivityDeadline, &QTimer::timeout, this, [this, id] {
        forceStop(id, Xc2TransportReason::Timeout);
    });

    const int totalDeadlineMs = isDeviceOperation(endpoint)
        ? m_options.deviceOperationDeadlineMs : m_options.totalDeadlineMs;
    pending->totalDeadline->start(qMax(1, totalDeadlineMs));
    if (!pending->responseComplete && m_options.transferTimeoutMs > 0) {
        pending->inactivityDeadline->start(
            qMax(1, m_options.transferTimeoutMs));
    }
    pending->socket->connectToHost(
        QHostAddress(m_canonicalHost), quint16(m_explicitPort));
    return id;
}

QUrl Xc2RestClient::endpointUrl(Endpoint endpoint) const
{
    const Xc2ContractProfile &profile = Xc2ContractProfile::approved();
    QUrl url = m_baseUrl;
    url.setPath(profile.restContext() + QLatin1Char('/')
                + profile.endpoint(endpoint).path);
    return url;
}

void Xc2RestClient::readAvailable(Xc2RequestId id)
{
    const auto iterator = m_pending.find(id);
    if (iterator == m_pending.end() || !iterator.value()->socket)
        return;
    drainAvailable(iterator.value());
}

void Xc2RestClient::drainAvailable(PendingRequest *pending)
{
    while (!pending->protocolFailure && pending->socket
           && pending->socket->bytesAvailable() > 0) {
        if (pending->responseComplete) {
            failProtocol(pending,
                         QStringLiteral("Bytes follow framed HTTP response"));
            return;
        }
        const QByteArray bytes = pending->socket->read(
            qMin(kSocketReadChunkBytes,
                 pending->socket->bytesAvailable()));
        if (bytes.isEmpty())
            return;
        pending->receiveBuffer += bytes;
        if (m_options.transferTimeoutMs > 0) {
            pending->inactivityDeadline->start(
                qMax(1, m_options.transferTimeoutMs));
        }
        parseAvailable(pending);
    }
}

void Xc2RestClient::parseAvailable(PendingRequest *pending)
{
    if (pending->protocolFailure)
        return;
    if (pending->responseComplete) {
        if (!pending->receiveBuffer.isEmpty()) {
            failProtocol(pending,
                         QStringLiteral("Bytes follow framed HTTP response"));
        }
        return;
    }

    const auto finishResponse = [this, pending] {
        pending->responseComplete = true;
        pending->totalDeadline->stop();
        pending->inactivityDeadline->stop();
        const Xc2RequestId id = pending->id;
        QMetaObject::invokeMethod(this, [this, id] {
            const auto iterator = m_pending.find(id);
            if (iterator == m_pending.end())
                return;
            PendingRequest *record = iterator.value();
            drainAvailable(record);
            if (record->socket
                && record->socket->state()
                       != QAbstractSocket::UnconnectedState) {
                record->socket->abort();
            }
            scheduleCompletion(id);
        }, Qt::QueuedConnection);
    };

    if (!pending->headersParsed) {
        const qsizetype marker = pending->receiveBuffer.indexOf("\r\n\r\n");
        if (marker < 0) {
            if (pending->receiveBuffer.size() > kMaximumHeaderBytes) {
                failProtocol(pending,
                             QStringLiteral("HTTP response headers exceed limit"));
            }
            return;
        }
        if (marker > kMaximumHeaderBytes) {
            failProtocol(pending,
                         QStringLiteral("HTTP response headers exceed limit"));
            return;
        }

        const QByteArray block = pending->receiveBuffer.left(marker);
        pending->receiveBuffer.remove(0, marker + 4);
        for (qsizetype i = 0; i < block.size(); ++i) {
            if (block.at(i) == '\n'
                && (i == 0 || block.at(i - 1) != '\r')) {
                failProtocol(pending,
                             QStringLiteral("Malformed HTTP header line"));
                return;
            }
        }
        QList<QByteArray> lines = block.split('\n');
        if (lines.isEmpty()) {
            failProtocol(pending, QStringLiteral("Missing HTTP status line"));
            return;
        }
        QByteArray statusLine = lines.takeFirst();
        if (!statusLine.endsWith('\r')) {
            failProtocol(pending, QStringLiteral("Malformed HTTP status line"));
            return;
        }
        statusLine.chop(1);
        if (statusLine.size() < 13
            || !statusLine.startsWith(QByteArrayLiteral("HTTP/1.1 "))
            || statusLine.at(12) != ' '
            || !isDecimal(statusLine.mid(9, 3))) {
            failProtocol(pending, QStringLiteral("Malformed HTTP status line"));
            return;
        }
        pending->httpStatus = statusLine.mid(9, 3).toInt();
        if (pending->httpStatus < 100 || pending->httpStatus > 599) {
            failProtocol(pending, QStringLiteral("Invalid HTTP status code"));
            return;
        }
        if (!isHeaderValue(statusLine.mid(13))) {
            failProtocol(pending, QStringLiteral("Invalid HTTP reason phrase"));
            return;
        }
        if (pending->httpStatus >= 100 && pending->httpStatus < 200) {
            failProtocol(pending,
                         QStringLiteral("Interim HTTP responses are unsupported"));
            return;
        }

        for (QByteArray line : std::as_const(lines)) {
            if (line.endsWith('\r'))
                line.chop(1);
            const qsizetype colon = line.indexOf(':');
            if (colon <= 0) {
                failProtocol(pending,
                             QStringLiteral("Malformed HTTP header line"));
                return;
            }
            const QByteArray name = line.left(colon);
            if (!isHeaderName(name)) {
                failProtocol(pending,
                             QStringLiteral("Invalid HTTP header name"));
                return;
            }
            const QByteArray rawValue = line.mid(colon + 1);
            if (!isHeaderValue(rawValue)) {
                failProtocol(pending,
                             QStringLiteral("Invalid HTTP header value"));
                return;
            }
            pending->responseHeaders.append(
                {name, rawValue.trimmed()});
        }

        QList<QByteArray> contentLengths;
        QList<QByteArray> transferEncodings;
        for (const auto &header : std::as_const(pending->responseHeaders)) {
            if (headerNameEquals(header.first,
                                 QByteArrayLiteral("Content-Length"))) {
                contentLengths.append(header.second);
            } else if (headerNameEquals(
                           header.first,
                           QByteArrayLiteral("Transfer-Encoding"))) {
                transferEncodings.append(header.second);
            }
        }
        if (contentLengths.size() > 1 || transferEncodings.size() > 1
            || (!contentLengths.isEmpty()
                && !transferEncodings.isEmpty())) {
            failProtocol(pending,
                         QStringLiteral("Ambiguous HTTP response framing"));
            return;
        }

        qint64 contentLength = -1;
        if (!contentLengths.isEmpty()) {
            if (!isDecimal(contentLengths.front())) {
                failProtocol(pending,
                             QStringLiteral("Invalid Content-Length"));
                return;
            }
            bool lengthOk = false;
            contentLength = contentLengths.front().toLongLong(&lengthOk);
            if (!lengthOk || contentLength < 0) {
                failProtocol(pending,
                             QStringLiteral("Invalid Content-Length"));
                return;
            }
        }
        const bool chunked = !transferEncodings.isEmpty();
        if (chunked
            && transferEncodings.front().compare(
                   QByteArrayLiteral("chunked"), Qt::CaseInsensitive) != 0) {
            failProtocol(pending,
                         QStringLiteral("Unsupported Transfer-Encoding"));
            return;
        }

        pending->headersParsed = true;
        if (pending->httpStatus == 204) {
            if (!contentLengths.isEmpty() || !transferEncodings.isEmpty()
                || !pending->receiveBuffer.isEmpty()) {
                failProtocol(pending,
                             QStringLiteral("HTTP 204 carried forbidden framing"));
                return;
            }
            pending->bodyMode = PendingRequest::BodyMode::NoBody;
            finishResponse();
            return;
        }
        if (pending->httpStatus == 304) {
            if (chunked || contentLength > 0
                || !pending->receiveBuffer.isEmpty()) {
                failProtocol(pending,
                             QStringLiteral("No-body response carried a body"));
                return;
            }
            pending->bodyMode = PendingRequest::BodyMode::NoBody;
            finishResponse();
            return;
        }
        if (chunked) {
            pending->bodyMode = PendingRequest::BodyMode::Chunked;
        } else if (contentLength >= 0) {
            if (contentLength > kMaximumBodyBytes) {
                failProtocol(pending,
                             QStringLiteral("HTTP response body exceeds limit"));
                return;
            }
            pending->bodyMode = PendingRequest::BodyMode::ContentLength;
            pending->contentRemaining = contentLength;
            if (contentLength == 0) {
                finishResponse();
                return;
            }
        } else {
            pending->bodyMode = PendingRequest::BodyMode::CloseDelimited;
        }
    }

    if (pending->bodyMode == PendingRequest::BodyMode::ContentLength) {
        const qsizetype take = static_cast<qsizetype>(qMin<qint64>(
            pending->contentRemaining, pending->receiveBuffer.size()));
        pending->rawPayload += pending->receiveBuffer.left(take);
        pending->receiveBuffer.remove(0, take);
        pending->contentRemaining -= take;
        if (pending->rawPayload.size() > kMaximumBodyBytes) {
            failProtocol(pending,
                         QStringLiteral("HTTP response body exceeds limit"));
            return;
        }
        if (pending->contentRemaining == 0) {
            if (!pending->receiveBuffer.isEmpty()) {
                failProtocol(pending,
                             QStringLiteral("HTTP response exceeds Content-Length"));
                return;
            }
            finishResponse();
        }
        return;
    }

    if (pending->bodyMode == PendingRequest::BodyMode::CloseDelimited) {
        if (pending->receiveBuffer.size()
            > kMaximumBodyBytes - pending->rawPayload.size()) {
            failProtocol(pending,
                         QStringLiteral("HTTP response body exceeds limit"));
            return;
        }
        pending->rawPayload += pending->receiveBuffer;
        pending->receiveBuffer.clear();
        return;
    }

    if (pending->bodyMode != PendingRequest::BodyMode::Chunked)
        return;

    while (!pending->protocolFailure && !pending->responseComplete) {
        if (pending->readingTrailers) {
            const qsizetype lineEnd = pending->receiveBuffer.indexOf("\r\n");
            if (lineEnd < 0) {
                if (pending->trailerBytes + pending->receiveBuffer.size()
                    > kMaximumHeaderBytes) {
                    failProtocol(pending,
                                 QStringLiteral("HTTP trailers exceed limit"));
                }
                return;
            }
            QByteArray trailer = pending->receiveBuffer.left(lineEnd);
            pending->receiveBuffer.remove(0, lineEnd + 2);
            pending->trailerBytes += lineEnd + 2;
            if (pending->trailerBytes > kMaximumHeaderBytes) {
                failProtocol(pending,
                             QStringLiteral("HTTP trailers exceed limit"));
                return;
            }
            if (trailer.isEmpty()) {
                if (!pending->receiveBuffer.isEmpty()) {
                    failProtocol(pending,
                                 QStringLiteral("Bytes follow final HTTP chunk"));
                    return;
                }
                finishResponse();
                return;
            }
            const qsizetype colon = trailer.indexOf(':');
            if (colon <= 0 || !isHeaderName(trailer.left(colon))
                || !isHeaderValue(trailer.mid(colon + 1))) {
                failProtocol(pending, QStringLiteral("Malformed HTTP trailer"));
                return;
            }
            continue;
        }

        if (pending->expectChunkTerminator) {
            if (pending->receiveBuffer.size() < 2)
                return;
            if (!pending->receiveBuffer.startsWith("\r\n")) {
                failProtocol(pending,
                             QStringLiteral("Malformed HTTP chunk terminator"));
                return;
            }
            pending->receiveBuffer.remove(0, 2);
            pending->expectChunkTerminator = false;
            pending->chunkRemaining = -1;
            continue;
        }

        if (pending->chunkRemaining < 0) {
            const qsizetype lineEnd = pending->receiveBuffer.indexOf("\r\n");
            if (lineEnd < 0) {
                if (pending->receiveBuffer.size() > kMaximumHeaderBytes) {
                    failProtocol(pending,
                                 QStringLiteral("HTTP chunk line exceeds limit"));
                }
                return;
            }
            if (lineEnd > kMaximumHeaderBytes) {
                failProtocol(pending,
                             QStringLiteral("HTTP chunk line exceeds limit"));
                return;
            }
            const QByteArray chunkLine = pending->receiveBuffer.left(lineEnd);
            pending->receiveBuffer.remove(0, lineEnd + 2);
            QByteArray sizeText;
            if (!parseChunkSizeLine(chunkLine, &sizeText)) {
                failProtocol(
                    pending,
                    QStringLiteral("Invalid HTTP chunk size or extension"));
                return;
            }
            bool sizeOk = false;
            const qulonglong chunkSize = sizeText.toULongLong(&sizeOk, 16);
            if (!sizeOk
                || chunkSize > qulonglong(kMaximumBodyBytes
                                          - pending->rawPayload.size())) {
                failProtocol(pending,
                             QStringLiteral("HTTP response body exceeds limit"));
                return;
            }
            if (chunkSize == 0) {
                pending->readingTrailers = true;
                continue;
            }
            pending->chunkRemaining = static_cast<qint64>(chunkSize);
        }

        const qsizetype take = static_cast<qsizetype>(qMin<qint64>(
            pending->chunkRemaining, pending->receiveBuffer.size()));
        if (take > 0) {
            pending->rawPayload += pending->receiveBuffer.left(take);
            pending->receiveBuffer.remove(0, take);
            pending->chunkRemaining -= take;
        }
        if (pending->chunkRemaining > 0)
            return;
        pending->expectChunkTerminator = true;
    }
}

void Xc2RestClient::failProtocol(PendingRequest *pending,
                                 const QString &message)
{
    if (pending->protocolFailure || pending->completed)
        return;
    pending->protocolFailure = true;
    pending->protocolMessage = message;
    if (pending->socket
        && pending->socket->state() != QAbstractSocket::UnconnectedState) {
        pending->socket->abort();
    }
    scheduleCompletion(pending->id);
}

void Xc2RestClient::forceStop(Xc2RequestId id, Xc2TransportReason reason)
{
    const auto iterator = m_pending.find(id);
    if (iterator == m_pending.end())
        return;
    PendingRequest *pending = iterator.value();
    if (pending->completed)
        return;
    if (pending->forcedReason == Xc2TransportReason::None)
        pending->forcedReason = reason;
    if (pending->socket)
        pending->socket->abort();
    scheduleCompletion(id);
}

void Xc2RestClient::scheduleCompletion(Xc2RequestId id)
{
    const auto iterator = m_pending.find(id);
    if (iterator == m_pending.end())
        return;
    PendingRequest *pending = iterator.value();
    if (pending->completed || pending->completionScheduled)
        return;
    pending->completionScheduled = true;
    QMetaObject::invokeMethod(this, [this, id] { completeOnce(id); },
                              Qt::QueuedConnection);
}

void Xc2RestClient::completeOnce(Xc2RequestId id)
{
    const auto iterator = m_pending.find(id);
    if (iterator == m_pending.end())
        return;
    PendingRequest *pending = iterator.value();
    if (pending->completed)
        return;

    drainAvailable(pending);
    if (pending->forcedReason == Xc2TransportReason::None
        && !pending->protocolFailure && !pending->responseComplete
        && pending->socket
        && pending->socket->state() != QAbstractSocket::UnconnectedState) {
        pending->completionScheduled = false;
        return;
    }
    if (!pending->protocolFailure && pending->headersParsed
        && pending->bodyMode == PendingRequest::BodyMode::CloseDelimited) {
        const bool endedAtEof = !pending->hasObservedError
            || pending->observedError
                   == QAbstractSocket::RemoteHostClosedError;
        if (!endedAtEof) {
            pending->protocolFailure = true;
            pending->protocolMessage = pending->socket
                ? pending->socket->errorString()
                : QStringLiteral("HTTP connection failed before EOF");
        } else {
            if (pending->receiveBuffer.size()
                > kMaximumBodyBytes - pending->rawPayload.size()) {
                pending->protocolFailure = true;
                pending->protocolMessage =
                    QStringLiteral("HTTP response body exceeds limit");
            } else {
                pending->rawPayload += pending->receiveBuffer;
                pending->receiveBuffer.clear();
                pending->responseComplete = true;
            }
        }
    }

    pending->completed = true;
    pending->totalDeadline->stop();
    pending->inactivityDeadline->stop();
    const Endpoint endpoint = pending->endpoint;
    const EndpointSpec spec =
        Xc2ContractProfile::approved().endpoint(endpoint);
    const int httpStatus = pending->httpStatus;
    const QByteArray rawPayload = pending->rawPayload;
    const Xc2TransportReason forcedReason = pending->forcedReason;
    const bool protocolFailure = pending->protocolFailure;
    const bool responseComplete = pending->responseComplete;
    const bool unapprovedSuccessStatus = httpStatus >= 200 && httpStatus < 300
        && !isApprovedSuccessStatus(endpoint, httpStatus);
    QString networkMessage = pending->protocolMessage;
    if (networkMessage.isEmpty() && pending->socket)
        networkMessage = pending->socket->errorString();
    const QList<QPair<QByteArray, QByteArray>> responseHeaders =
        pending->responseHeaders;
    const QPointer<QTcpSocket> socket = pending->socket;
    QTimer *const totalDeadline = pending->totalDeadline;
    QTimer *const inactivityDeadline = pending->inactivityDeadline;

    m_pending.erase(iterator);

    if (forcedReason == Xc2TransportReason::None && !protocolFailure
        && responseComplete) {
        const QUrl responseUrl = endpointUrl(endpoint);
        for (const auto &header : responseHeaders) {
            if (!headerNameEquals(header.first,
                                  QByteArrayLiteral("Set-Cookie"))) {
                continue;
            }
            const QList<QNetworkCookie> cookies =
                QNetworkCookie::parseCookies(header.second);
            if (!cookies.isEmpty())
                m_cookieJar->setCookiesFromUrl(cookies, responseUrl);
        }
    }

    const auto cleanup = [this, pending, socket, totalDeadline,
                          inactivityDeadline] {
        if (socket) {
            socket->disconnect(this);
            socket->deleteLater();
        }
        totalDeadline->disconnect(this);
        inactivityDeadline->disconnect(this);
        totalDeadline->deleteLater();
        inactivityDeadline->deleteLater();
        delete pending;
    };

    const bool hasTransportFailure =
        forcedReason != Xc2TransportReason::None
        || protocolFailure || !responseComplete || httpStatus == 0;
    bool hasCommonFailure = false;
    Xc2Error commonError;
    if (hasTransportFailure) {
        const Xc2TransportReason reason =
            forcedReason == Xc2TransportReason::None
            ? Xc2TransportReason::Network : forcedReason;
        if (networkMessage.isEmpty()) {
            networkMessage = reason == Xc2TransportReason::Canceled
                ? QStringLiteral("Request canceled")
                : reason == Xc2TransportReason::Timeout
                    ? QStringLiteral("Request deadline expired")
                    : QStringLiteral("Incomplete HTTP response");
        }
        commonError = transportError(
            reason, httpStatus, rawPayload, spec.path, networkMessage);
        hasCommonFailure = true;
    } else if (unapprovedSuccessStatus) {
        commonError = contractError(QStringLiteral(
            "HTTP success status is not approved for this endpoint"));
        commonError.httpStatus = httpStatus;
        commonError.rawPayload = rawPayload;
        commonError.endpoint = spec.path;
        hasCommonFailure = true;
    } else if (httpStatus < 200 || httpStatus >= 300) {
        commonError = Xc2JsonCodec::error(
            rawPayload, httpStatus, spec.path);
        hasCommonFailure = true;
    }

    const auto addResponseContext = [&](Xc2Error &error) {
        error.httpStatus = httpStatus;
        error.endpoint = spec.path;
        error.rawPayload = rawPayload;
    };

    switch (endpoint) {
    case Endpoint::ServiceStatus: {
        Xc2Result<Xc2ServiceStatus> result = hasCommonFailure
            ? Xc2Result<Xc2ServiceStatus>::failure(commonError)
            : Xc2JsonCodec::serviceStatus(rawPayload);
        if (!result.ok() && !hasCommonFailure)
            addResponseContext(result.error);
        cleanup();
        emit serviceStatusFinished(id, result);
        return;
    }
    case Endpoint::CurrentUser: {
        Xc2Result<Xc2CurrentUser> result = hasCommonFailure
            ? Xc2Result<Xc2CurrentUser>::failure(commonError)
            : Xc2JsonCodec::currentUser(rawPayload);
        if (!result.ok() && !hasCommonFailure)
            addResponseContext(result.error);
        cleanup();
        emit currentUserFinished(id, result);
        return;
    }
    case Endpoint::Shutdown: {
        const Xc2Error result = hasCommonFailure
            ? commonError : Xc2Error{};
        cleanup();
        emit shutdownFinished(id, result);
        return;
    }
    case Endpoint::DeviceLookup: {
        Xc2Result<Xc2JobAccepted> result = hasCommonFailure
            ? Xc2Result<Xc2JobAccepted>::failure(commonError)
            : Xc2JsonCodec::jobAccepted(rawPayload);
        if (result.ok() && result.value->jobId.isEmpty()) {
            Xc2Error error = contractError(
                QStringLiteral("Field 'jobID' must not be empty"));
            addResponseContext(error);
            result = Xc2Result<Xc2JobAccepted>::failure(std::move(error));
        } else if (!result.ok() && !hasCommonFailure) {
            addResponseContext(result.error);
        }
        cleanup();
        emit deviceLookupFinished(id, result);
        return;
    }
    case Endpoint::DeviceGet: {
        Xc2Result<QList<Xc2VciDevice>> result = hasCommonFailure
            ? Xc2Result<QList<Xc2VciDevice>>::failure(commonError)
            : Xc2JsonCodec::vciDevices(rawPayload);
        if (!result.ok() && !hasCommonFailure)
            addResponseContext(result.error);
        cleanup();
        emit devicesFinished(id, result);
        return;
    }
    case Endpoint::DeviceGetSelected: {
        Xc2Result<Xc2SelectedVci> result = hasCommonFailure
            ? Xc2Result<Xc2SelectedVci>::failure(commonError)
            : rawPayload.isEmpty()
                ? Xc2Result<Xc2SelectedVci>::success({std::nullopt})
                : Xc2JsonCodec::selectedVci(rawPayload);
        if (!result.ok() && !hasCommonFailure)
            addResponseContext(result.error);
        cleanup();
        emit selectedDeviceFinished(id, result);
        return;
    }
    case Endpoint::DeviceApply: {
        Xc2Error result = hasCommonFailure ? commonError : Xc2Error{};
        if (!hasCommonFailure && !rawPayload.isEmpty()
            && !isSingleJsonValue(rawPayload)) {
            result = contractError(QStringLiteral(
                "Device apply response must contain one JSON value"));
            addResponseContext(result);
        }
        cleanup();
        emit applyDeviceFinished(id, result);
        return;
    }
    case Endpoint::DeviceClose: {
        Xc2Error result = hasCommonFailure ? commonError : Xc2Error{};
        if (!hasCommonFailure && !rawPayload.isEmpty()
            && !isSingleJsonValue(rawPayload)) {
            result = contractError(QStringLiteral(
                "Device close response must contain one JSON value"));
            addResponseContext(result);
        }
        cleanup();
        emit closeDeviceFinished(id, result);
        return;
    }
    case Endpoint::Login:
    case Endpoint::Logout:
    case Endpoint::SettingsGet:
    case Endpoint::SettingsSet:
    case Endpoint::VehicleDetect:
    case Endpoint::VehicleManufacturers:
    case Endpoint::VehicleSeries:
    case Endpoint::VehicleModels:
    case Endpoint::VehicleSelect:
    case Endpoint::VehicleInfo:
    case Endpoint::AutoScan:
    case Endpoint::EcuDomains:
    case Endpoint::EcuOpen:
    case Endpoint::EcuClose:
    case Endpoint::EcuScan:
    case Endpoint::EcuClearDtc:
    case Endpoint::EcuMeasurementsGet:
    case Endpoint::EcuMeasurementsStart:
    case Endpoint::EcuMeasurementsStop:
    case Endpoint::EcuExecuteFlow:
    case Endpoint::VehicleExecuteFlow:
    case Endpoint::FlowUpdateGui:
    case Endpoint::DownloadMapping:
    case Endpoint::FlashAutomatic:
    case Endpoint::FlashFile:
        cleanup();
        return;
    }
}

} // namespace ktm::xc2
