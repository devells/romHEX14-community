#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QQueue>
#include <QUrl>

class QTcpServer;
class QTcpSocket;

struct FakeHttpRequest {
    QByteArray method;
    QByteArray target;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;

    QByteArray headerValue(const QByteArray &name) const;
    QList<QByteArray> headerValues(const QByteArray &name) const;
};

struct FakeHttpResponse {
    enum class Mode {
        Complete,
        CloseBeforeStatus,
        TruncateBody,
        DripWithoutCompletion,
        NeverRespond,
        DelayedComplete,
        RawResponse,
        RawResponseKeepOpen,
        FragmentedRawResponse,
    };

    Mode mode = Mode::Complete;
    int status = 200;
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
    qint64 declaredContentLength = -1;
    int intervalMs = 0;
    QByteArray dripChunk;
    QList<QByteArray> fragments;
};

class FakeHttpServer final : public QObject {
    Q_OBJECT

public:
    explicit FakeHttpServer(const QHostAddress &address,
                            QObject *parent = nullptr);

    bool isListening() const;
    QString errorString() const;
    QHostAddress address() const;
    quint16 port() const;
    int connectionCount() const;
    int requestCount() const;
    const QList<FakeHttpRequest> &requests() const;

    void enqueueResponse(FakeHttpResponse response);

    static FakeHttpResponse complete(
        int status,
        QByteArray body = {},
        QList<QPair<QByteArray, QByteArray>> headers = {});
    static FakeHttpResponse closeBeforeStatus();
    static FakeHttpResponse truncated(int status,
                                      QByteArray partialBody,
                                      qint64 declaredContentLength);
    static FakeHttpResponse drip(int status,
                                int intervalMs,
                                QByteArray chunk = QByteArrayLiteral("x"));
    static FakeHttpResponse neverRespond();
    static FakeHttpResponse delayedComplete(int status,
                                            QByteArray body,
                                            int delayMs);
    static FakeHttpResponse rawResponse(QByteArray wire);
    static FakeHttpResponse rawResponseKeepOpen(QByteArray wire);
    static FakeHttpResponse fragmentedRawResponse(
        QList<QByteArray> fragments,
        int intervalMs = 1);
    static FakeHttpResponse redirect(int status, const QUrl &location,
                                     QByteArray body = {});

signals:
    void requestCaptured();

private:
    struct ConnectionState {
        QByteArray bytes;
        qsizetype headerEnd = -1;
        qint64 contentLength = 0;
        bool captured = false;
    };

    void acceptConnections();
    void consume(QTcpSocket *socket);
    void sendResponse(QTcpSocket *socket, const FakeHttpResponse &response);

    QTcpServer *m_server = nullptr;
    QHash<QTcpSocket *, ConnectionState> m_connections;
    QQueue<FakeHttpResponse> m_responses;
    QList<FakeHttpRequest> m_requests;
    int m_connectionCount = 0;
};
