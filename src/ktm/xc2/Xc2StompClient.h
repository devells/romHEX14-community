#pragma once

#include "Xc2ContractProfile.h"
#include "Xc2Models.h"

#include <QMetaType>
#include <QObject>

namespace ktm::xc2 {

class Xc2RestClient;

using Xc2StompGeneration = quint64;

enum class Xc2StompState {
    Disconnected,
    WebSocketConnecting,
    StompConnecting,
    Connected,
    Disconnecting,
    Failed
};

struct Xc2StompClientOptions {
    int connectDeadlineMs = 10000;
    int disconnectDeadlineMs = 2000;
    qint64 clientOutgoingHeartbeatMs = 10000;
    qint64 clientIncomingHeartbeatMs = 10000;
    int heartbeatGraceMultiplier = 2;
    quint64 maximumIncomingMessageBytes = 8 * 1024 * 1024;
};

struct Xc2StompSession {
    Xc2StompGeneration generation = 0;
    QString version;
    qint64 outgoingHeartbeatMs = 0;
    qint64 incomingHeartbeatMs = 0;
};

struct Xc2StompMessage {
    Xc2StompGeneration generation = 0;
    Topic topic = Topic::VciStatus;
    QString destination;
    QString subscriptionId;
    QString messageId;
    QByteArray body;
};

class Xc2StompClient final : public QObject {
    Q_OBJECT

public:
    explicit Xc2StompClient(Xc2StompClientOptions options = {},
                            QObject *parent = nullptr);
    ~Xc2StompClient() override;

    bool connectToBackend(const Xc2RestClient &rest,
                          Xc2Error *error = nullptr);
    bool subscribe(Topic topic, Xc2Error *error = nullptr);
    bool unsubscribe(Topic topic, Xc2Error *error = nullptr);
    void disconnectFromBackend();
    void abortCurrentGeneration();
    Xc2StompState state() const;
    Xc2StompGeneration generation() const;

signals:
    void stateChanged(Xc2StompGeneration, Xc2StompState);
    void connected(const Xc2StompSession &);
    void subscriptionSent(Topic, const QString &subscriptionId);
    void unsubscriptionSent(Topic, const QString &subscriptionId);
    void messageReceived(const Xc2StompMessage &);
    void errorOccurred(Xc2StompGeneration, const Xc2Error &);
    void visibilityLost(Xc2StompGeneration, const Xc2Error &);
    void disconnected(Xc2StompGeneration);

private:
    struct Private;
    Private *d = nullptr;
};

} // namespace ktm::xc2

Q_DECLARE_METATYPE(ktm::xc2::Topic)
Q_DECLARE_METATYPE(ktm::xc2::Xc2StompState)
Q_DECLARE_METATYPE(ktm::xc2::Xc2StompSession)
Q_DECLARE_METATYPE(ktm::xc2::Xc2StompMessage)
