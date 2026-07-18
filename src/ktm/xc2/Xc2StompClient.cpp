#include "Xc2StompClient.h"

#include "Xc2RestClient.h"
#include "Xc2StompCodec.h"

#include <QAbstractEventDispatcher>
#include <QAbstractSocket>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QPointer>
#include <QSet>
#include <QStringDecoder>
#include <QTimer>
#include <QThread>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>
#include <QWebSocketProtocol>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace ktm::xc2 {
namespace {

constexpr quint64 kCodecMaximumBytes = 8 * 1024 * 1024;

class StompSocketRetirementOwner final : public QObject {
public:
    explicit StompSocketRetirementOwner(QAbstractEventDispatcher *dispatcher)
        : QObject(dispatcher)
    {
    }
};

QObject *socketRetirementOwner(QWebSocket *socket)
{
    if (!socket || socket->thread() != QThread::currentThread())
        return nullptr;
    QAbstractEventDispatcher *const dispatcher =
        QAbstractEventDispatcher::instance(socket->thread());
    if (!dispatcher || dispatcher->thread() != socket->thread())
        return nullptr;
    for (QObject *child : dispatcher->children()) {
        if (auto *owner =
                dynamic_cast<StompSocketRetirementOwner *>(child)) {
            return owner;
        }
    }
    return new StompSocketRetirementOwner(dispatcher);
}

Xc2Error contractError(QString message, QByteArray payload = {})
{
    Xc2Error error;
    error.category = Xc2ErrorCategory::Contract;
    error.message = std::move(message);
    error.rawPayload = std::move(payload);
    error.endpoint = Xc2ContractProfile::approved().webSocketPath();
    return error;
}

Xc2Error transportError(Xc2TransportReason reason, QString message)
{
    Xc2Error error;
    error.category = Xc2ErrorCategory::Transport;
    error.transportReason = reason;
    error.message = std::move(message);
    error.endpoint = Xc2ContractProfile::approved().webSocketPath();
    return error;
}

Xc2Error stompError(const Xc2StompFrame &frame)
{
    Xc2Error error;
    error.category = Xc2ErrorCategory::Backend;
    error.message = QString::fromUtf8(
        frame.headers.value(QByteArrayLiteral("message")));
    if (error.message.isEmpty())
        error.message = QStringLiteral("XC2 STOMP ERROR");
    error.rawPayload = frame.body;
    error.endpoint = Xc2ContractProfile::approved().webSocketPath();
    return error;
}

bool isKnownTopic(Topic topic)
{
    return Xc2ContractProfile::allTopics().contains(topic);
}

QString subscriptionId(Topic topic)
{
    switch (topic) {
    case Topic::VciStatus:
        return QStringLiteral("vci-status-subscription");
    case Topic::VehicleInfo:
        return QStringLiteral("vehicle-info-subscription");
    case Topic::Ecu:
        return QStringLiteral("ecu-subscription");
    case Topic::Progress:
        return QStringLiteral("progress-subscription");
    case Topic::MeasurementValues:
        return QStringLiteral("measurement-values-subscription");
    case Topic::FlowGui:
        return QStringLiteral("flow-gui-subscription");
    case Topic::FlowProgress:
        return QStringLiteral("flow-progress-subscription");
    case Topic::Login:
        return QStringLiteral("login-subscription");
    }
    return {};
}

int timerInterval(qint64 milliseconds)
{
    return int(std::clamp<qint64>(
        milliseconds, 1, std::numeric_limits<int>::max()));
}

bool parseNonNegativeDecimal(const QByteArray &text, qint64 *value)
{
    if (text.isEmpty())
        return false;
    for (const char byte : text) {
        if (byte < '0' || byte > '9')
            return false;
    }
    bool ok = false;
    const qint64 parsed = text.toLongLong(&ok, 10);
    if (!ok || parsed < 0)
        return false;
    *value = parsed;
    return true;
}

bool parseHeartbeat(const QByteArray &text, qint64 *first, qint64 *second)
{
    const qsizetype comma = text.indexOf(',');
    if (comma <= 0 || comma != text.lastIndexOf(',')
        || comma + 1 >= text.size()) {
        return false;
    }
    return parseNonNegativeDecimal(text.left(comma), first)
        && parseNonNegativeDecimal(text.mid(comma + 1), second);
}

int firstFrameHeaderCount(const QByteArray &wire, const QByteArray &name)
{
    qsizetype position = 0;
    while (position < wire.size()) {
        if (wire.at(position) == '\n') {
            ++position;
            continue;
        }
        if (wire.at(position) == '\r' && position + 1 < wire.size()
            && wire.at(position + 1) == '\n') {
            position += 2;
            continue;
        }
        break;
    }

    auto nextLine = [&wire](qsizetype *cursor,
                            QByteArray *line) -> bool {
        const qsizetype lf = wire.indexOf('\n', *cursor);
        if (lf < 0)
            return false;
        qsizetype end = lf;
        if (end > *cursor && wire.at(end - 1) == '\r')
            --end;
        *line = wire.mid(*cursor, end - *cursor);
        *cursor = lf + 1;
        return true;
    };

    QByteArray command;
    if (!nextLine(&position, &command)
        || command != QByteArrayLiteral("CONNECTED")) {
        return -1;
    }

    int count = 0;
    while (true) {
        QByteArray line;
        if (!nextLine(&position, &line))
            return -1;
        if (line.isEmpty())
            return count;
        const qsizetype colon = line.indexOf(':');
        if (colon >= 0 && line.left(colon) == name)
            ++count;
    }
}

} // namespace

struct Xc2StompClient::Private {
    enum class SendResult { Queued, Failed, Superseded, OwnerDeleted };
    enum class ConnectDecision {
        Inactive,
        Pending,
        TimeoutLatched,
        Connected
    };
    enum class DisconnectDecision { Inactive, Pending, CloseObserved };

    explicit Private(Xc2StompClient *owner, Xc2StompClientOptions value)
        : q(owner), options(value)
    {
    }

    ~Private()
    {
        destroyTimer(connectTimer);
        destroyTimer(disconnectTimer);
        destroyTimer(outgoingTimer);
        destroyTimer(incomingTimer);
        if (socket) {
            socket->disconnect(q);
            socket->abort();
            delete socket;
        }
    }

    static void destroyTimer(QTimer *&timer)
    {
        if (!timer)
            return;
        timer->stop();
        delete timer;
        timer = nullptr;
    }

    void retireTimer(QTimer *&timer)
    {
        if (!timer)
            return;
        timer->stop();
        timer->disconnect(q);
        timer->deleteLater();
        timer = nullptr;
    }

    bool optionsAreValid(Xc2Error *error) const
    {
        const auto fail = [error](const QString &message) {
            if (error)
                *error = contractError(message);
            return false;
        };
        if (options.connectDeadlineMs <= 0)
            return fail(QStringLiteral("STOMP connect deadline must be positive"));
        if (options.disconnectDeadlineMs <= 0) {
            return fail(QStringLiteral(
                "STOMP disconnect deadline must be positive"));
        }
        if (options.clientOutgoingHeartbeatMs < 0
            || options.clientIncomingHeartbeatMs < 0) {
            return fail(QStringLiteral(
                "STOMP heartbeat offers must be non-negative"));
        }
        if (options.heartbeatGraceMultiplier <= 0) {
            return fail(QStringLiteral(
                "STOMP heartbeat grace multiplier must be positive"));
        }
        if (options.maximumIncomingMessageBytes == 0
            || options.maximumIncomingMessageBytes > kCodecMaximumBytes) {
            return fail(QStringLiteral(
                "STOMP incoming message limit must be within the 8 MiB codec cap"));
        }
        return true;
    }

    bool current(Xc2StompGeneration candidateGeneration,
                 const QWebSocket *candidateSocket) const
    {
        return !terminal && generation == candidateGeneration
            && socket == candidateSocket;
    }

    bool changeState(Xc2StompState next)
    {
        if (state == next)
            return true;
        state = next;
        QPointer<Xc2StompClient> owner(q);
        emit owner->stateChanged(generation, state);
        return !owner.isNull();
    }

    void clearGenerationState()
    {
        codec.reset();
        negotiationWire.clear();
        activeById.clear();
        activeByTopic.clear();
        outgoingIntervalMs = 0;
        incomingIntervalMs = 0;
        incomingGraceMs = 0;
        outgoingActivity.invalidate();
        incomingActivity.invalidate();
        disconnectReceipt.clear();
        disconnectReceiptMatched = false;
        disconnectDecision = DisconnectDecision::Inactive;
        disconnectFrameQueued = false;
        disconnectDeadlineActive = false;
        disconnectSocketErrorSeen = false;
        disconnectLocalCloseStarted = false;
        disconnectToken = 0;
        connectDecision = ConnectDecision::Inactive;
        connectToken = 0;
        connectFrameQueued = false;
        incomingDeadlineActive = false;
        subscriptionWritesDeferred = false;
        subscriptionFlushRunning = false;
        subscriptionFlushScheduled = false;
        established = false;
        intentional = false;
        canonicalHost.clear();
        retireTimer(connectTimer);
        retireTimer(disconnectTimer);
        retireTimer(outgoingTimer);
        retireTimer(incomingTimer);
    }

    void finishOnce(Xc2StompGeneration candidateGeneration,
                    QWebSocket *candidateSocket,
                    std::optional<Xc2Error> error,
                    bool forceAbort)
    {
        if (!current(candidateGeneration, candidateSocket))
            return;

        if (connectToken == candidateGeneration
            && connectDecision == ConnectDecision::TimeoutLatched) {
            error = transportError(
                Xc2TransportReason::Timeout,
                QStringLiteral("XC2 STOMP connection deadline expired"));
            forceAbort = true;
        }

        terminal = true;
        const bool lostEstablishedVisibility = established && !intentional;
        QPointer<QWebSocket> retiredSocket = socket;
        socket = nullptr;

        retireTimer(connectTimer);
        retireTimer(disconnectTimer);
        retireTimer(outgoingTimer);
        retireTimer(incomingTimer);
        codec.reset();
        negotiationWire.clear();
        activeById.clear();
        activeByTopic.clear();
        outgoingActivity.invalidate();
        incomingActivity.invalidate();
        outgoingIntervalMs = 0;
        incomingIntervalMs = 0;
        incomingGraceMs = 0;
        established = false;
        disconnectReceipt.clear();
        disconnectReceiptMatched = false;
        disconnectDecision = DisconnectDecision::Inactive;
        disconnectFrameQueued = false;
        disconnectDeadlineActive = false;
        disconnectSocketErrorSeen = false;
        disconnectLocalCloseStarted = false;
        disconnectToken = 0;
        connectDecision = ConnectDecision::Inactive;
        connectToken = 0;
        connectFrameQueued = false;
        incomingDeadlineActive = false;
        subscriptionWritesDeferred = false;
        subscriptionFlushRunning = false;
        subscriptionFlushScheduled = false;

        if (retiredSocket) {
            retiredSocket->disconnect(q);
            QObject *const retirementOwner =
                socketRetirementOwner(retiredSocket);
            Q_ASSERT(retirementOwner);
            retiredSocket->setParent(retirementOwner);
            if (forceAbort)
                retiredSocket->abort();
            retiredSocket->deleteLater();
        }

        if (!changeState(error.has_value()
                             ? Xc2StompState::Failed
                             : Xc2StompState::Disconnected)) {
            return;
        }
        QPointer<Xc2StompClient> owner(q);
        if (error.has_value()) {
            emit owner->errorOccurred(candidateGeneration, *error);
            if (!owner)
                return;
        }
        if (lostEstablishedVisibility) {
            Xc2Error visibility = transportError(
                Xc2TransportReason::Network,
                QStringLiteral("XC2 STOMP visibility lost"));
            emit owner->visibilityLost(candidateGeneration, visibility);
            if (!owner)
                return;
        }
        emit owner->disconnected(candidateGeneration);
    }

    void reportContract(QString message, QByteArray payload = {})
    {
        emit q->errorOccurred(generation,
                              contractError(std::move(message),
                                            std::move(payload)));
    }

    SendResult sendText(const QByteArray &payload)
    {
        if (!socket)
            return SendResult::Failed;
        const Xc2StompGeneration candidateGeneration = generation;
        QWebSocket *const candidateSocket = socket;
        QPointer<Xc2StompClient> owner(q);
        const qint64 accepted = candidateSocket->sendTextMessage(
            QString::fromUtf8(payload));
        if (!owner)
            return SendResult::OwnerDeleted;
        if (!current(candidateGeneration, candidateSocket))
            return SendResult::Superseded;
        if (accepted != payload.size())
            return SendResult::Failed;
        noteOutgoingActivity();
        return SendResult::Queued;
    }

    SendResult sendFrame(const Xc2StompFrame &frame)
    {
        return sendText(Xc2StompCodec::encode(frame));
    }

    void failWrite(const QString &operation)
    {
        if (!socket)
            return;
        finishOnce(generation, socket,
                   transportError(
                       Xc2TransportReason::Network,
                       QStringLiteral("Failed to queue STOMP %1").arg(operation)),
                   true);
    }

    bool requireConnectWindow(Xc2StompGeneration candidateGeneration,
                              QWebSocket *candidateSocket)
    {
        if (!current(candidateGeneration, candidateSocket)
            || connectToken != candidateGeneration
            || connectDecision == ConnectDecision::Inactive
            || connectDecision == ConnectDecision::Connected) {
            return false;
        }
        if (connectDecision == ConnectDecision::TimeoutLatched
            || connectDeadline.hasExpired()) {
            connectDecision = ConnectDecision::TimeoutLatched;
            finishOnce(candidateGeneration, candidateSocket,
                       transportError(
                           Xc2TransportReason::Timeout,
                           QStringLiteral(
                               "XC2 STOMP connection deadline expired")),
                       true);
            return false;
        }
        return true;
    }

    bool claimConnected(Xc2StompGeneration candidateGeneration,
                        QWebSocket *candidateSocket)
    {
        if (!requireConnectWindow(candidateGeneration, candidateSocket))
            return false;
        connectDecision = ConnectDecision::Connected;
        retireTimer(connectTimer);
        return current(candidateGeneration, candidateSocket);
    }

    bool requireDisconnectWindow(Xc2StompGeneration candidateGeneration,
                                 QWebSocket *candidateSocket)
    {
        if (!current(candidateGeneration, candidateSocket)
            || state != Xc2StompState::Disconnecting
            || !disconnectDeadlineActive
            || disconnectToken != candidateGeneration
            || disconnectDecision != DisconnectDecision::Pending) {
            return false;
        }
        if (!disconnectDeadline.hasExpired())
            return true;
        finishOnce(candidateGeneration, candidateSocket,
                   transportError(
                       Xc2TransportReason::Timeout,
                       QStringLiteral(
                           "XC2 STOMP disconnect deadline expired")),
                   true);
        return false;
    }

    bool requireIncomingWindow(Xc2StompGeneration candidateGeneration,
                               QWebSocket *candidateSocket)
    {
        if (!current(candidateGeneration, candidateSocket))
            return false;
        if (state != Xc2StompState::Connected || !incomingDeadlineActive)
            return true;
        if (!incomingDeadline.hasExpired())
            return true;
        finishOnce(candidateGeneration, candidateSocket,
                   transportError(
                       Xc2TransportReason::Timeout,
                       QStringLiteral("XC2 STOMP heartbeat timed out")),
                   true);
        return false;
    }

    void startConnectTimer(Xc2StompGeneration candidateGeneration,
                           QWebSocket *candidateSocket)
    {
        connectTimer = new QTimer(q);
        connectTimer->setSingleShot(true);
        connectTimer->setTimerType(Qt::PreciseTimer);
        connectTimer->setInterval(timerInterval(
            std::max<qint64>(1, connectDeadline.remainingTime())));
        connect(connectTimer, &QTimer::timeout, q,
                [this, candidateGeneration,
                 guarded = QPointer<QWebSocket>(candidateSocket)] {
            if (!guarded || !current(candidateGeneration, guarded)
                || connectToken != candidateGeneration
                || connectDecision != ConnectDecision::Pending) {
                return;
            }
            if (!connectDeadline.hasExpired()) {
                connectTimer->start(timerInterval(
                    std::max<qint64>(1, connectDeadline.remainingTime())));
                return;
            }
            connectDecision = ConnectDecision::TimeoutLatched;
        }, Qt::DirectConnection);
        connect(connectTimer, &QTimer::timeout, q,
                [this, candidateGeneration,
                 guarded = QPointer<QWebSocket>(candidateSocket)] {
            if (!guarded || !current(candidateGeneration, guarded)
                || connectToken != candidateGeneration
                || connectDecision != ConnectDecision::TimeoutLatched) {
                return;
            }
            finishOnce(candidateGeneration, guarded,
                       transportError(
                           Xc2TransportReason::Timeout,
                           QStringLiteral(
                               "XC2 STOMP connection deadline expired")),
                       true);
        }, Qt::QueuedConnection);
        connectTimer->start();
    }

    void startDisconnectTimer(Xc2StompGeneration candidateGeneration,
                              QWebSocket *candidateSocket)
    {
        disconnectTimer = new QTimer(q);
        disconnectTimer->setSingleShot(true);
        disconnectTimer->setTimerType(Qt::PreciseTimer);
        disconnectTimer->setInterval(timerInterval(
            std::max<qint64>(1, disconnectDeadline.remainingTime())));
        connect(disconnectTimer, &QTimer::timeout, q,
                [this, candidateGeneration,
                 guarded = QPointer<QWebSocket>(candidateSocket)] {
            if (!guarded || !current(candidateGeneration, guarded)
                || state != Xc2StompState::Disconnecting
                || disconnectToken != candidateGeneration
                || !disconnectDeadlineActive
                || disconnectDecision != DisconnectDecision::Pending) {
                return;
            }
            if (!disconnectDeadline.hasExpired()) {
                disconnectTimer->start(timerInterval(
                    std::max<qint64>(1, disconnectDeadline.remainingTime())));
                return;
            }
            requireDisconnectWindow(candidateGeneration, guarded);
        });
        disconnectTimer->start();
    }

    void scheduleOutgoingTimer(qint64 delayMs)
    {
        if (!outgoingTimer || outgoingIntervalMs <= 0
            || state != Xc2StompState::Connected) {
            return;
        }
        outgoingTimer->start(timerInterval(delayMs));
    }

    void scheduleIncomingTimer(qint64 delayMs)
    {
        if (!incomingTimer || incomingIntervalMs <= 0
            || state != Xc2StompState::Connected) {
            return;
        }
        incomingTimer->start(timerInterval(delayMs));
    }

    void noteOutgoingActivity()
    {
        if (state != Xc2StompState::Connected || outgoingIntervalMs <= 0)
            return;
        outgoingActivity.restart();
        scheduleOutgoingTimer(outgoingIntervalMs);
    }

    bool noteIncomingActivity(Xc2StompGeneration candidateGeneration,
                              QWebSocket *candidateSocket)
    {
        if (state == Xc2StompState::WebSocketConnecting
            || state == Xc2StompState::StompConnecting) {
            return requireConnectWindow(candidateGeneration, candidateSocket);
        }
        if (state == Xc2StompState::Disconnecting) {
            return requireDisconnectWindow(candidateGeneration,
                                           candidateSocket);
        }
        if (state != Xc2StompState::Connected || incomingIntervalMs <= 0)
            return current(candidateGeneration, candidateSocket);
        if (!requireIncomingWindow(candidateGeneration, candidateSocket))
            return false;
        incomingActivity.restart();
        incomingDeadline.setRemainingTime(incomingGraceMs, Qt::PreciseTimer);
        scheduleIncomingTimer(std::max<qint64>(
            1, incomingDeadline.remainingTime()));
        return current(candidateGeneration, candidateSocket);
    }

    void startHeartbeatTimers(Xc2StompGeneration candidateGeneration,
                              QWebSocket *candidateSocket)
    {
        if (outgoingIntervalMs > 0) {
            outgoingTimer = new QTimer(q);
            outgoingTimer->setSingleShot(true);
            outgoingTimer->setTimerType(Qt::PreciseTimer);
            connect(outgoingTimer, &QTimer::timeout, q,
                    [this, candidateGeneration,
                     guarded = QPointer<QWebSocket>(candidateSocket)] {
                if (!guarded || !current(candidateGeneration, guarded)
                    || state != Xc2StompState::Connected) {
                    return;
                }
                const qint64 elapsed = outgoingActivity.elapsed();
                if (elapsed < outgoingIntervalMs) {
                    scheduleOutgoingTimer(outgoingIntervalMs - elapsed);
                    return;
                }
                const SendResult result = sendText(QByteArrayLiteral("\n"));
                if (result == SendResult::Failed)
                    failWrite(QStringLiteral("heartbeat"));
            });
            outgoingActivity.start();
            scheduleOutgoingTimer(outgoingIntervalMs);
        }

        if (incomingIntervalMs > 0) {
            incomingTimer = new QTimer(q);
            incomingTimer->setSingleShot(true);
            incomingTimer->setTimerType(Qt::PreciseTimer);
            connect(incomingTimer, &QTimer::timeout, q,
                    [this, candidateGeneration,
                     guarded = QPointer<QWebSocket>(candidateSocket)] {
                if (!guarded || !current(candidateGeneration, guarded)
                    || state != Xc2StompState::Connected) {
                    return;
                }
                if (!incomingDeadline.hasExpired()) {
                    scheduleIncomingTimer(std::max<qint64>(
                        1, incomingDeadline.remainingTime()));
                    return;
                }
                requireIncomingWindow(candidateGeneration, guarded);
            });
            incomingActivity.start();
            incomingDeadlineActive = true;
            incomingDeadline.setRemainingTime(incomingGraceMs,
                                              Qt::PreciseTimer);
            scheduleIncomingTimer(std::max<qint64>(
                1, incomingDeadline.remainingTime()));
        }
    }

    void stopHeartbeatTimers()
    {
        retireTimer(outgoingTimer);
        retireTimer(incomingTimer);
        outgoingActivity.invalidate();
        incomingActivity.invalidate();
        incomingDeadlineActive = false;
    }

    void connectSocketSignals(Xc2StompGeneration candidateGeneration,
                              QWebSocket *candidateSocket)
    {
        const QPointer<QWebSocket> guarded(candidateSocket);
        connect(candidateSocket, &QWebSocket::connected, q,
                [this, candidateGeneration, guarded] {
            if (guarded && current(candidateGeneration, guarded))
                onWebSocketConnected(candidateGeneration, guarded);
        });
        connect(candidateSocket, &QWebSocket::textFrameReceived, q,
                [this, candidateGeneration, guarded](const QString &, bool) {
            if (guarded && current(candidateGeneration, guarded))
                noteIncomingActivity(candidateGeneration, guarded);
        });
        connect(candidateSocket, &QWebSocket::binaryFrameReceived, q,
                [this, candidateGeneration, guarded](const QByteArray &, bool) {
            if (guarded && current(candidateGeneration, guarded))
                noteIncomingActivity(candidateGeneration, guarded);
        });
        connect(candidateSocket, &QWebSocket::textMessageReceived, q,
                [this, candidateGeneration, guarded](const QString &message) {
            if (guarded && current(candidateGeneration, guarded))
                onPayload(candidateGeneration, guarded, message.toUtf8());
        });
        connect(candidateSocket, &QWebSocket::binaryMessageReceived, q,
                [this, candidateGeneration, guarded](const QByteArray &message) {
            if (guarded && current(candidateGeneration, guarded))
                onPayload(candidateGeneration, guarded, message);
        });
        connect(candidateSocket, &QWebSocket::disconnected, q,
                [this, candidateGeneration, guarded] {
            if (guarded && current(candidateGeneration, guarded))
                onSocketDisconnected(candidateGeneration, guarded);
        });
        connect(candidateSocket, &QWebSocket::errorOccurred, q,
                [this, candidateGeneration, guarded](QAbstractSocket::SocketError) {
            if (guarded && current(candidateGeneration, guarded))
                onSocketError(candidateGeneration, guarded);
        });
    }

    void onWebSocketConnected(Xc2StompGeneration candidateGeneration,
                              QWebSocket *candidateSocket)
    {
        QPointer<Xc2StompClient> owner(q);
        if (!requireConnectWindow(candidateGeneration, candidateSocket)
            || !owner) {
            return;
        }
        if (candidateSocket->subprotocol() != QStringLiteral("v12.stomp")) {
            finishOnce(candidateGeneration, candidateSocket,
                       contractError(QStringLiteral(
                           "WebSocket server did not negotiate v12.stomp")),
                       true);
            return;
        }

        Xc2StompFrame frame;
        frame.command = QByteArrayLiteral("CONNECT");
        frame.headers.insert(QByteArrayLiteral("accept-version"),
                             QByteArrayLiteral("1.2"));
        frame.headers.insert(QByteArrayLiteral("host"),
                             canonicalHost.toUtf8());
        frame.headers.insert(
            QByteArrayLiteral("heart-beat"),
            QByteArray::number(options.clientOutgoingHeartbeatMs)
                + ',' + QByteArray::number(options.clientIncomingHeartbeatMs));
        if (!changeState(Xc2StompState::StompConnecting))
            return;
        if (!current(candidateGeneration, candidateSocket)
            || state != Xc2StompState::StompConnecting) {
            return;
        }
        if (!requireConnectWindow(candidateGeneration, candidateSocket)
            || !owner) {
            return;
        }
        const SendResult result = sendFrame(frame);
        if (!owner)
            return;
        if (result == SendResult::Queued
            && current(candidateGeneration, candidateSocket)
            && state == Xc2StompState::StompConnecting) {
            connectFrameQueued = true;
        } else if (result == SendResult::Failed) {
            failWrite(QStringLiteral("CONNECT"));
        }
    }

    void onPayload(Xc2StompGeneration candidateGeneration,
                   QWebSocket *candidateSocket,
                   const QByteArray &payload)
    {
        QPointer<Xc2StompClient> owner(q);
        if (!noteIncomingActivity(candidateGeneration, candidateSocket)
            || !owner) {
            return;
        }
        if (state == Xc2StompState::StompConnecting
            && !connectFrameQueued) {
            finishOnce(candidateGeneration, candidateSocket,
                       contractError(QStringLiteral(
                           "STOMP response arrived before CONNECT was queued")),
                       true);
            return;
        }
        if (state == Xc2StompState::StompConnecting
            && negotiationWire.size() <= qsizetype(kCodecMaximumBytes)
            && payload.size() <= qsizetype(kCodecMaximumBytes)
            && negotiationWire.size()
                <= qsizetype(kCodecMaximumBytes) - payload.size()) {
            negotiationWire += payload;
        }

        const Xc2StompDecodeResult decoded = codec.feed(payload);
        if (!decoded.errors.isEmpty()) {
            finishOnce(candidateGeneration, candidateSocket,
                       decoded.errors.constFirst(), true);
            return;
        }
        for (const Xc2StompFrame &frame : decoded.frames) {
            if (!current(candidateGeneration, candidateSocket))
                return;
            QPointer<Xc2StompClient> owner(q);
            dispatchFrame(candidateGeneration, candidateSocket, frame);
            if (!owner)
                return;
        }
    }

    void dispatchFrame(Xc2StompGeneration candidateGeneration,
                       QWebSocket *candidateSocket,
                       const Xc2StompFrame &frame)
    {
        if (frame.command == QByteArrayLiteral("ERROR")) {
            finishOnce(candidateGeneration, candidateSocket,
                       stompError(frame), true);
            return;
        }

        if (state == Xc2StompState::StompConnecting) {
            if (frame.command != QByteArrayLiteral("CONNECTED")) {
                finishOnce(candidateGeneration, candidateSocket,
                           contractError(QStringLiteral(
                               "Unexpected STOMP command before CONNECTED"),
                               frame.command),
                           true);
                return;
            }
            establishSession(candidateGeneration, candidateSocket, frame);
            return;
        }

        if (state != Xc2StompState::Connected
            && state != Xc2StompState::Disconnecting) {
            finishOnce(candidateGeneration, candidateSocket,
                       contractError(QStringLiteral(
                           "STOMP frame arrived in an invalid state"),
                           frame.command),
                       true);
            return;
        }

        if (frame.command == QByteArrayLiteral("CONNECTED")) {
            finishOnce(candidateGeneration, candidateSocket,
                       contractError(QStringLiteral(
                           "Duplicate or misplaced CONNECTED frame")),
                       true);
        } else if (frame.command == QByteArrayLiteral("MESSAGE")) {
            routeMessage(frame);
        } else if (frame.command == QByteArrayLiteral("RECEIPT")) {
            handleReceipt(candidateSocket, frame);
        } else {
            finishOnce(candidateGeneration, candidateSocket,
                       contractError(QStringLiteral(
                           "Unknown STOMP server command"), frame.command),
                       true);
        }
    }

    void establishSession(Xc2StompGeneration candidateGeneration,
                          QWebSocket *candidateSocket,
                          const Xc2StompFrame &frame)
    {
        QPointer<Xc2StompClient> owner(q);
        if (!requireConnectWindow(candidateGeneration, candidateSocket)
            || !owner) {
            return;
        }
        const int versionHeaders = firstFrameHeaderCount(
            negotiationWire, QByteArrayLiteral("version"));
        if (versionHeaders != 1) {
            finishOnce(candidateGeneration, candidateSocket,
                       contractError(QStringLiteral(
                           "CONNECTED must contain exactly one version header")),
                       true);
            return;
        }
        if (!frame.body.isEmpty()) {
            finishOnce(candidateGeneration, candidateSocket,
                       contractError(QStringLiteral(
                           "CONNECTED body must be empty"), frame.body),
                       true);
            return;
        }
        if (frame.headers.value(QByteArrayLiteral("version"))
            != QByteArrayLiteral("1.2")) {
            finishOnce(candidateGeneration, candidateSocket,
                       contractError(QStringLiteral(
                           "CONNECTED version must equal 1.2")),
                       true);
            return;
        }

        qint64 serverOutgoing = 0;
        qint64 serverIncoming = 0;
        if (frame.headers.contains(QByteArrayLiteral("heart-beat"))
            && !parseHeartbeat(
                frame.headers.value(QByteArrayLiteral("heart-beat")),
                &serverOutgoing, &serverIncoming)) {
            finishOnce(candidateGeneration, candidateSocket,
                       contractError(QStringLiteral(
                           "CONNECTED heart-beat must contain two non-negative integers")),
                       true);
            return;
        }

        outgoingIntervalMs = options.clientOutgoingHeartbeatMs == 0
                || serverIncoming == 0
            ? 0
            : std::max(options.clientOutgoingHeartbeatMs, serverIncoming);
        incomingIntervalMs = serverOutgoing == 0
                || options.clientIncomingHeartbeatMs == 0
            ? 0
            : std::max(serverOutgoing, options.clientIncomingHeartbeatMs);
        if (incomingIntervalMs > 0
            && incomingIntervalMs
                > std::numeric_limits<qint64>::max()
                    / options.heartbeatGraceMultiplier) {
            finishOnce(candidateGeneration, candidateSocket,
                       contractError(QStringLiteral(
                           "Negotiated heartbeat grace interval overflows")),
                       true);
            return;
        }
        incomingGraceMs = incomingIntervalMs
            * options.heartbeatGraceMultiplier;

        if (!claimConnected(candidateGeneration, candidateSocket)
            || !owner) {
            return;
        }

        negotiationWire.clear();
        established = true;
        subscriptionWritesDeferred = true;
        state = Xc2StompState::Connected;
        startHeartbeatTimers(candidateGeneration, candidateSocket);
        emit owner->stateChanged(candidateGeneration,
                                 Xc2StompState::Connected);
        if (!owner)
            return;
        if (!current(candidateGeneration, candidateSocket)
            || state != Xc2StompState::Connected) {
            return;
        }
        if (!requireIncomingWindow(candidateGeneration, candidateSocket)
            || !owner) {
            return;
        }

        Xc2StompSession session;
        session.generation = candidateGeneration;
        session.version = QStringLiteral("1.2");
        session.outgoingHeartbeatMs = outgoingIntervalMs;
        session.incomingHeartbeatMs = incomingIntervalMs;
        emit owner->connected(session);
        if (!owner || !current(candidateGeneration, candidateSocket)
            || state != Xc2StompState::Connected)
            return;
        if (!requireIncomingWindow(candidateGeneration, candidateSocket)
            || !owner)
            return;
        flushSubscriptions(candidateGeneration, candidateSocket);
    }

    bool subscriptionsMatchDesired() const
    {
        for (const Topic topic : Xc2ContractProfile::allTopics()) {
            const int key = int(topic);
            if (desired.contains(key) != activeByTopic.contains(key))
                return false;
        }
        return true;
    }

    void scheduleSubscriptionFlush(
        Xc2StompGeneration candidateGeneration,
        QWebSocket *candidateSocket)
    {
        if (subscriptionFlushScheduled
            || !current(candidateGeneration, candidateSocket)
            || state != Xc2StompState::Connected) {
            return;
        }
        subscriptionFlushScheduled = true;
        const QPointer<QWebSocket> guarded(candidateSocket);
        QMetaObject::invokeMethod(q, [this, candidateGeneration, guarded] {
            if (!guarded || !current(candidateGeneration, guarded))
                return;
            subscriptionFlushScheduled = false;
            if (state != Xc2StompState::Connected)
                return;
            QPointer<Xc2StompClient> owner(q);
            if (!requireIncomingWindow(candidateGeneration, guarded)
                || !owner) {
                return;
            }
            flushSubscriptions(candidateGeneration, guarded);
        }, Qt::QueuedConnection);
    }

    void flushSubscriptions(Xc2StompGeneration candidateGeneration,
                            QWebSocket *candidateSocket)
    {
        if (!current(candidateGeneration, candidateSocket)
            || state != Xc2StompState::Connected
            || subscriptionFlushRunning) {
            return;
        }
        QPointer<Xc2StompClient> owner(q);
        if (!requireIncomingWindow(candidateGeneration, candidateSocket)
            || !owner) {
            return;
        }
        subscriptionFlushRunning = true;
        subscriptionWritesDeferred = true;
        const QSet<int> desiredSnapshot = desired;
        QSet<int> activeSnapshot;
        for (auto it = activeByTopic.cbegin();
             it != activeByTopic.cend(); ++it) {
            activeSnapshot.insert(it.key());
        }
        for (const Topic topic : Xc2ContractProfile::allTopics()) {
            if (!current(candidateGeneration, candidateSocket)
                || state != Xc2StompState::Connected) {
                break;
            }
            if (!requireIncomingWindow(candidateGeneration, candidateSocket)
                || !owner) {
                return;
            }
            const int key = int(topic);
            const bool wanted = desiredSnapshot.contains(key);
            const bool active = activeSnapshot.contains(key);
            if (wanted == active)
                continue;
            const bool sent = wanted
                ? sendSubscription(topic)
                : sendUnsubscription(topic);
            if (!owner)
                return;
            if (!current(candidateGeneration, candidateSocket))
                return;
            if (!requireIncomingWindow(candidateGeneration, candidateSocket)
                || !owner) {
                return;
            }
            if (!sent || state != Xc2StompState::Connected)
                break;
        }
        if (!owner)
            return;
        if (!current(candidateGeneration, candidateSocket))
            return;
        subscriptionFlushRunning = false;
        if (state != Xc2StompState::Connected)
            return;
        if (subscriptionsMatchDesired()) {
            subscriptionWritesDeferred = false;
            return;
        }
        scheduleSubscriptionFlush(candidateGeneration, candidateSocket);
    }

    bool sendSubscription(Topic topic)
    {
        const int key = int(topic);
        if (activeByTopic.contains(key))
            return true;
        const QString id = subscriptionId(topic);
        Xc2StompFrame frame;
        frame.command = QByteArrayLiteral("SUBSCRIBE");
        frame.headers.insert(QByteArrayLiteral("id"), id.toUtf8());
        frame.headers.insert(
            QByteArrayLiteral("destination"),
            Xc2ContractProfile::approved().topic(topic).toUtf8());
        frame.headers.insert(QByteArrayLiteral("ack"),
                             QByteArrayLiteral("auto"));
        const SendResult result = sendFrame(frame);
        if (result != SendResult::Queued) {
            if (result == SendResult::Failed)
                failWrite(QStringLiteral("SUBSCRIBE"));
            return false;
        }
        activeByTopic.insert(key, id);
        activeById.insert(id.toUtf8(), topic);
        emit q->subscriptionSent(topic, id);
        return true;
    }

    bool sendUnsubscription(Topic topic)
    {
        const int key = int(topic);
        const auto active = activeByTopic.constFind(key);
        if (active == activeByTopic.cend())
            return true;
        const QString id = active.value();
        Xc2StompFrame frame;
        frame.command = QByteArrayLiteral("UNSUBSCRIBE");
        frame.headers.insert(QByteArrayLiteral("id"), id.toUtf8());
        const SendResult result = sendFrame(frame);
        if (result != SendResult::Queued) {
            if (result == SendResult::Failed)
                failWrite(QStringLiteral("UNSUBSCRIBE"));
            return false;
        }
        activeByTopic.remove(key);
        activeById.remove(id.toUtf8());
        emit q->unsubscriptionSent(topic, id);
        return true;
    }

    void routeMessage(const Xc2StompFrame &frame)
    {
        const QByteArray destination =
            frame.headers.value(QByteArrayLiteral("destination"));
        const QByteArray subscription =
            frame.headers.value(QByteArrayLiteral("subscription"));
        const QByteArray messageId =
            frame.headers.value(QByteArrayLiteral("message-id"));
        if (destination.isEmpty() || subscription.isEmpty()
            || messageId.isEmpty()) {
            reportContract(QStringLiteral(
                "MESSAGE requires destination, subscription, and message-id"));
            return;
        }

        const auto active = activeById.constFind(subscription);
        if (active == activeById.cend()) {
            reportContract(QStringLiteral(
                "MESSAGE subscription is not active in this generation"));
            return;
        }
        const Topic topic = active.value();
        const QByteArray expectedDestination =
            Xc2ContractProfile::approved().topic(topic).toUtf8();
        if (destination != expectedDestination) {
            reportContract(QStringLiteral(
                "MESSAGE destination does not match its active subscription"));
            return;
        }
        QStringDecoder messageIdDecoder(
            QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
        const QString decodedMessageId = messageIdDecoder.decode(messageId);
        if (messageIdDecoder.hasError()) {
            reportContract(QStringLiteral(
                "MESSAGE message-id must be valid UTF-8"), messageId);
            return;
        }

        Xc2StompMessage message;
        message.generation = generation;
        message.topic = topic;
        message.destination = QString::fromUtf8(destination);
        message.subscriptionId = QString::fromUtf8(subscription);
        message.messageId = decodedMessageId;
        message.body = frame.body;
        emit q->messageReceived(message);
    }

    void handleReceipt(QWebSocket *candidateSocket,
                       const Xc2StompFrame &frame)
    {
        const QByteArray receiptId =
            frame.headers.value(QByteArrayLiteral("receipt-id"));
        if (receiptId.isEmpty()) {
            reportContract(QStringLiteral("RECEIPT requires receipt-id"));
            return;
        }
        if (state != Xc2StompState::Disconnecting
            || receiptId != disconnectReceipt.toUtf8()) {
            reportContract(QStringLiteral("Unrelated STOMP RECEIPT"));
            return;
        }
        if (!disconnectFrameQueued) {
            finishOnce(generation, candidateSocket,
                       contractError(QStringLiteral(
                           "RECEIPT arrived before DISCONNECT was queued")),
                       true);
            return;
        }
        if (disconnectReceiptMatched)
            return;
        QPointer<Xc2StompClient> owner(q);
        if (!requireDisconnectWindow(generation, candidateSocket)
            || !owner) {
            return;
        }
        disconnectReceiptMatched = true;
        const Xc2StompGeneration candidateGeneration = generation;
        const QPointer<QWebSocket> guarded(candidateSocket);
        QMetaObject::invokeMethod(q, [this, candidateGeneration, guarded] {
            if (!guarded || !current(candidateGeneration, guarded)
                || state != Xc2StompState::Disconnecting) {
                return;
            }
            QPointer<Xc2StompClient> owner(q);
            if (!requireDisconnectWindow(candidateGeneration, guarded)
                || !owner) {
                return;
            }
            if (guarded->state() != QAbstractSocket::ConnectedState)
                return;
            disconnectLocalCloseStarted = true;
            guarded->close(QWebSocketProtocol::CloseCodeNormal,
                           QStringLiteral("XC2 STOMP disconnect"));
        }, Qt::QueuedConnection);
    }

    void onSocketError(Xc2StompGeneration candidateGeneration,
                       QWebSocket *candidateSocket)
    {
        if (state == Xc2StompState::Disconnecting) {
            QPointer<Xc2StompClient> owner(q);
            if (disconnectDecision == DisconnectDecision::CloseObserved) {
                if (!current(candidateGeneration, candidateSocket)
                    || disconnectToken != candidateGeneration) {
                    return;
                }
            } else {
                if (!requireDisconnectWindow(candidateGeneration,
                                             candidateSocket)
                    || !owner) {
                    return;
                }
            }
            disconnectSocketErrorSeen = true;
        } else if ((state == Xc2StompState::WebSocketConnecting
                    || state == Xc2StompState::StompConnecting)
                   && !requireConnectWindow(candidateGeneration,
                                            candidateSocket)) {
            return;
        }
        QString message = candidateSocket->errorString();
        if (message.isEmpty())
            message = QStringLiteral("XC2 WebSocket transport failed");
        finishOnce(candidateGeneration, candidateSocket,
                   transportError(Xc2TransportReason::Network,
                                  std::move(message)),
                   true);
    }

    void onSocketDisconnected(Xc2StompGeneration candidateGeneration,
                              QWebSocket *candidateSocket)
    {
        if (state == Xc2StompState::WebSocketConnecting
            || state == Xc2StompState::StompConnecting) {
            if (!requireConnectWindow(candidateGeneration, candidateSocket))
                return;
        }
        if (state == Xc2StompState::Disconnecting) {
            QPointer<Xc2StompClient> owner(q);
            if (!requireDisconnectWindow(candidateGeneration, candidateSocket)
                || !owner) {
                return;
            }
            if (!disconnectReceiptMatched) {
                finishOnce(candidateGeneration, candidateSocket,
                           transportError(
                               Xc2TransportReason::Network,
                               QStringLiteral(
                                   "XC2 WebSocket closed before DISCONNECT receipt")),
                           false);
                return;
            }
            disconnectDecision = DisconnectDecision::CloseObserved;
            disconnectDeadlineActive = false;
            retireTimer(disconnectTimer);
            const QPointer<QWebSocket> guarded(candidateSocket);
            QMetaObject::invokeMethod(q, [this, candidateGeneration, guarded] {
                if (!guarded || !current(candidateGeneration, guarded)
                    || state != Xc2StompState::Disconnecting
                    || disconnectDecision
                        != DisconnectDecision::CloseObserved) {
                    return;
                }
                if (!disconnectSocketErrorSeen
                    && disconnectLocalCloseStarted) {
                    finishOnce(candidateGeneration, guarded,
                               std::nullopt, false);
                    return;
                }
                finishOnce(candidateGeneration, guarded,
                           transportError(
                               Xc2TransportReason::Network,
                               QStringLiteral(
                                   "XC2 WebSocket disconnect was not graceful")),
                           false);
            }, Qt::QueuedConnection);
            return;
        }
        finishOnce(candidateGeneration, candidateSocket,
                   transportError(
                       Xc2TransportReason::Network,
                       QStringLiteral("XC2 WebSocket closed unexpectedly")),
                   false);
    }

    Xc2StompClient *q = nullptr;
    Xc2StompClientOptions options;
    Xc2StompState state = Xc2StompState::Disconnected;
    Xc2StompGeneration generation = 0;
    QPointer<QWebSocket> socket;
    QTimer *connectTimer = nullptr;
    QTimer *disconnectTimer = nullptr;
    QTimer *outgoingTimer = nullptr;
    QTimer *incomingTimer = nullptr;
    Xc2StompCodec codec;
    QByteArray negotiationWire;
    QSet<int> desired;
    QHash<QByteArray, Topic> activeById;
    QHash<int, QString> activeByTopic;
    QElapsedTimer outgoingActivity;
    QElapsedTimer incomingActivity;
    qint64 outgoingIntervalMs = 0;
    qint64 incomingIntervalMs = 0;
    qint64 incomingGraceMs = 0;
    QDeadlineTimer connectDeadline;
    QDeadlineTimer disconnectDeadline;
    QDeadlineTimer incomingDeadline;
    QString canonicalHost;
    QString disconnectReceipt;
    Xc2StompGeneration connectToken = 0;
    Xc2StompGeneration disconnectToken = 0;
    ConnectDecision connectDecision = ConnectDecision::Inactive;
    DisconnectDecision disconnectDecision = DisconnectDecision::Inactive;
    bool terminal = true;
    bool established = false;
    bool intentional = false;
    bool disconnectReceiptMatched = false;
    bool connectFrameQueued = false;
    bool disconnectFrameQueued = false;
    bool disconnectDeadlineActive = false;
    bool disconnectSocketErrorSeen = false;
    bool disconnectLocalCloseStarted = false;
    bool incomingDeadlineActive = false;
    bool subscriptionWritesDeferred = false;
    bool subscriptionFlushRunning = false;
    bool subscriptionFlushScheduled = false;
};

Xc2StompClient::Xc2StompClient(Xc2StompClientOptions options,
                               QObject *parent)
    : QObject(parent), d(new Private(this, options))
{
}

Xc2StompClient::~Xc2StompClient()
{
    delete d;
}

bool Xc2StompClient::connectToBackend(const Xc2RestClient &rest,
                                      Xc2Error *error)
{
    const QDeadlineTimer callDeadline(
        qMax(0, d->options.connectDeadlineMs), Qt::PreciseTimer);
    const bool inFlight = d->state == Xc2StompState::WebSocketConnecting
        || d->state == Xc2StompState::StompConnecting
        || d->state == Xc2StompState::Connected
        || d->state == Xc2StompState::Disconnecting;
    if (inFlight) {
        if (error) {
            *error = contractError(QStringLiteral(
                "XC2 STOMP connection is already active"));
        }
        return false;
    }
    if (!d->optionsAreValid(error))
        return false;
    if (d->generation == std::numeric_limits<Xc2StompGeneration>::max()) {
        if (error) {
            *error = contractError(QStringLiteral(
                "XC2 STOMP generation counter is exhausted"));
        }
        return false;
    }

    const QUrl url = rest.webSocketUrl();
    const Xc2Result<QByteArray> cookie = rest.cookieHeaderFor(
        url.toEncoded(QUrl::FullyEncoded));
    if (!cookie.ok()) {
        if (error)
            *error = cookie.error;
        return false;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setMaximumRedirectsAllowed(0);
    request.setRawHeader(QByteArrayLiteral("Cookie"), *cookie.value);
    QWebSocketHandshakeOptions handshake;
    handshake.setSubprotocols({QStringLiteral("v12.stomp")});

    d->clearGenerationState();
    const Xc2StompGeneration candidateGeneration = ++d->generation;
    d->terminal = false;
    d->connectDeadline = callDeadline;
    d->connectToken = candidateGeneration;
    d->connectDecision = Private::ConnectDecision::Pending;
    d->connectFrameQueued = false;
    d->canonicalHost = url.host();
    auto *socket = new QWebSocket(QString(),
                                  QWebSocketProtocol::VersionLatest, this);
    d->socket = socket;
    socket->setProxy(QNetworkProxy::NoProxy);
    socket->setMaxAllowedIncomingFrameSize(
        d->options.maximumIncomingMessageBytes);
    socket->setMaxAllowedIncomingMessageSize(
        d->options.maximumIncomingMessageBytes);
    d->connectSocketSignals(candidateGeneration, socket);
    d->startConnectTimer(candidateGeneration, socket);
    if (!d->changeState(Xc2StompState::WebSocketConnecting)) {
        if (error)
            *error = {};
        return true;
    }
    if (!d->current(candidateGeneration, socket)
        || d->state != Xc2StompState::WebSocketConnecting) {
        if (error)
            *error = {};
        return true;
    }
    QPointer<Xc2StompClient> owner(this);
    if (!d->requireConnectWindow(candidateGeneration, socket) || !owner) {
        if (error)
            *error = {};
        return true;
    }
    socket->open(request, handshake);
    if (error)
        *error = {};
    return true;
}

bool Xc2StompClient::subscribe(Topic topic, Xc2Error *error)
{
    if (!isKnownTopic(topic)) {
        if (error)
            *error = contractError(QStringLiteral("Topic is not approved"));
        return false;
    }
    const int key = int(topic);
    if (d->desired.contains(key)) {
        if (error)
            *error = {};
        return true;
    }
    d->desired.insert(key);
    if (d->state == Xc2StompState::Connected
        && !d->subscriptionWritesDeferred
        && !d->subscriptionFlushRunning) {
        const Xc2StompGeneration candidateGeneration = d->generation;
        QWebSocket *const candidateSocket = d->socket;
        QPointer<Xc2StompClient> owner(this);
        if (!d->requireIncomingWindow(candidateGeneration, candidateSocket)
            || !owner) {
            if (owner && error) {
                *error = transportError(
                    Xc2TransportReason::Timeout,
                    QStringLiteral("XC2 STOMP heartbeat timed out"));
            }
            return false;
        }
        const bool sent = d->sendSubscription(topic);
        if (!owner)
            return sent;
        if (!sent) {
            if (error) {
                *error = transportError(
                    Xc2TransportReason::Network,
                    QStringLiteral("Failed to queue STOMP SUBSCRIBE"));
            }
            return false;
        }
        if (d->current(candidateGeneration, candidateSocket)
            && (!d->requireIncomingWindow(candidateGeneration,
                                          candidateSocket)
                || !owner)) {
            if (owner && error) {
                *error = transportError(
                    Xc2TransportReason::Timeout,
                    QStringLiteral("XC2 STOMP heartbeat timed out"));
            }
            return false;
        }
    }
    if (error)
        *error = {};
    return true;
}

bool Xc2StompClient::unsubscribe(Topic topic, Xc2Error *error)
{
    if (!isKnownTopic(topic)) {
        if (error)
            *error = contractError(QStringLiteral("Topic is not approved"));
        return false;
    }
    const int key = int(topic);
    if (!d->desired.remove(key)) {
        if (error)
            *error = {};
        return true;
    }
    if (d->state == Xc2StompState::Connected
        && !d->subscriptionWritesDeferred
        && !d->subscriptionFlushRunning) {
        const Xc2StompGeneration candidateGeneration = d->generation;
        QWebSocket *const candidateSocket = d->socket;
        QPointer<Xc2StompClient> owner(this);
        if (!d->requireIncomingWindow(candidateGeneration, candidateSocket)
            || !owner) {
            if (owner && error) {
                *error = transportError(
                    Xc2TransportReason::Timeout,
                    QStringLiteral("XC2 STOMP heartbeat timed out"));
            }
            return false;
        }
        const bool sent = d->sendUnsubscription(topic);
        if (!owner)
            return sent;
        if (!sent) {
            if (error) {
                *error = transportError(
                    Xc2TransportReason::Network,
                    QStringLiteral("Failed to queue STOMP UNSUBSCRIBE"));
            }
            return false;
        }
        if (d->current(candidateGeneration, candidateSocket)
            && (!d->requireIncomingWindow(candidateGeneration,
                                          candidateSocket)
                || !owner)) {
            if (owner && error) {
                *error = transportError(
                    Xc2TransportReason::Timeout,
                    QStringLiteral("XC2 STOMP heartbeat timed out"));
            }
            return false;
        }
    }
    if (error)
        *error = {};
    return true;
}

void Xc2StompClient::disconnectFromBackend()
{
    const QDeadlineTimer callDeadline(
        qMax(0, d->options.disconnectDeadlineMs), Qt::PreciseTimer);
    if (d->state == Xc2StompState::Disconnected
        || d->state == Xc2StompState::Failed
        || d->state == Xc2StompState::Disconnecting) {
        return;
    }
    if (!d->socket)
        return;

    d->intentional = true;
    if (d->state == Xc2StompState::WebSocketConnecting
        || d->state == Xc2StompState::StompConnecting) {
        d->finishOnce(d->generation, d->socket,
                      transportError(Xc2TransportReason::Canceled,
                                     QStringLiteral(
                                         "XC2 STOMP connection canceled")),
                      true);
        return;
    }

    const Xc2StompGeneration candidateGeneration = d->generation;
    QWebSocket *const socket = d->socket;
    const QString receipt = QStringLiteral("disconnect-%1")
                                .arg(candidateGeneration);

    Xc2StompFrame frame;
    frame.command = QByteArrayLiteral("DISCONNECT");
    frame.headers.insert(QByteArrayLiteral("receipt"),
                         receipt.toUtf8());
    d->stopHeartbeatTimers();
    d->disconnectReceipt = receipt;
    d->disconnectDecision = Private::DisconnectDecision::Pending;
    d->disconnectFrameQueued = false;
    d->disconnectDeadline = callDeadline;
    d->disconnectToken = candidateGeneration;
    d->disconnectDeadlineActive = true;
    d->disconnectSocketErrorSeen = false;
    d->disconnectLocalCloseStarted = false;
    d->startDisconnectTimer(candidateGeneration, socket);
    if (!d->changeState(Xc2StompState::Disconnecting))
        return;
    if (!d->current(candidateGeneration, socket)
        || d->state != Xc2StompState::Disconnecting) {
        return;
    }
    QPointer<Xc2StompClient> owner(this);
    if (!d->requireDisconnectWindow(candidateGeneration, socket) || !owner)
        return;
    const Private::SendResult result = d->sendFrame(frame);
    if (!owner)
        return;
    if (result == Private::SendResult::Queued
        && d->current(candidateGeneration, socket)
        && d->state == Xc2StompState::Disconnecting) {
        d->disconnectFrameQueued = true;
    } else if (result == Private::SendResult::Failed) {
        d->failWrite(QStringLiteral("DISCONNECT"));
    }
}

void Xc2StompClient::abortCurrentGeneration()
{
    if (!d->socket || d->terminal)
        return;
    if (d->state == Xc2StompState::Disconnecting
        && d->disconnectDecision
            == Private::DisconnectDecision::CloseObserved) {
        return;
    }
    d->intentional = true;
    d->finishOnce(d->generation, d->socket,
                  transportError(Xc2TransportReason::Canceled,
                                 QStringLiteral(
                                     "XC2 STOMP generation canceled")),
                  true);
}

Xc2StompState Xc2StompClient::state() const
{
    return d->state;
}

Xc2StompGeneration Xc2StompClient::generation() const
{
    return d->generation;
}

} // namespace ktm::xc2
