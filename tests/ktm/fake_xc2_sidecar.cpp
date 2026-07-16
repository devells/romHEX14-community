#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QPointer>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstdio>
#include <memory>

namespace {

struct Options {
    quint16 port = 0;
    int readyDelayMs = 0;
    int healthDelayMs = 0;
    int currentUserDelayMs = 0;
    int lateAliveMs = 0;
    bool neverHealth = false;
    bool exitBeforeReady = false;
    bool ignoreShutdown = false;
    bool fragmentOutput = false;
    bool oversizeOutput = false;
    bool sensitiveOutput = false;
    bool spawnDescendant = false;
    bool descendant = false;
    QString holdLockPath;
    QString userMode = QStringLiteral("valid");
    QString releaseTriggerPath;
    QString eventLogPath;
};

bool takeValue(const QStringList &arguments, int &index, QString &value)
{
    if (index + 1 >= arguments.size())
        return false;
    value = arguments.at(++index);
    return true;
}

bool takeInteger(const QStringList &arguments, int &index, int &value)
{
    QString text;
    if (!takeValue(arguments, index, text))
        return false;
    bool ok = false;
    const int parsed = text.toInt(&ok);
    if (!ok || parsed < 0)
        return false;
    value = parsed;
    return true;
}

bool parseOptions(const QStringList &arguments, Options &options)
{
    for (int i = 1; i < arguments.size(); ++i) {
        const QString argument = arguments.at(i);
        if (argument == QStringLiteral("--port")) {
            int port = 0;
            if (!takeInteger(arguments, i, port) || port > 65535)
                return false;
            options.port = static_cast<quint16>(port);
        } else if (argument == QStringLiteral("--ready-delay-ms")) {
            if (!takeInteger(arguments, i, options.readyDelayMs))
                return false;
        } else if (argument == QStringLiteral("--health-delay-ms")) {
            if (!takeInteger(arguments, i, options.healthDelayMs))
                return false;
        } else if (argument == QStringLiteral("--current-user-delay-ms")) {
            if (!takeInteger(arguments, i, options.currentUserDelayMs))
                return false;
        } else if (argument == QStringLiteral("--late-alive-ms")) {
            if (!takeInteger(arguments, i, options.lateAliveMs))
                return false;
        } else if (argument == QStringLiteral("--never-health")) {
            options.neverHealth = true;
        } else if (argument == QStringLiteral("--exit-before-ready")) {
            options.exitBeforeReady = true;
        } else if (argument == QStringLiteral("--ignore-shutdown")) {
            options.ignoreShutdown = true;
        } else if (argument == QStringLiteral("--fragment-output")) {
            options.fragmentOutput = true;
        } else if (argument == QStringLiteral("--oversize-output")) {
            options.oversizeOutput = true;
        } else if (argument == QStringLiteral("--sensitive-output")) {
            options.sensitiveOutput = true;
        } else if (argument == QStringLiteral("--spawn-descendant")) {
            options.spawnDescendant = true;
        } else if (argument == QStringLiteral("--descendant")) {
            options.descendant = true;
        } else if (argument == QStringLiteral("--hold-lock")) {
            if (!takeValue(arguments, i, options.holdLockPath))
                return false;
        } else if (argument == QStringLiteral("--user-mode")) {
            if (!takeValue(arguments, i, options.userMode))
                return false;
        } else if (argument
                   == QStringLiteral("--release-listener-after-ready")) {
            if (!takeValue(arguments, i, options.releaseTriggerPath))
                return false;
        } else if (argument == QStringLiteral("--event-log")) {
            if (!takeValue(arguments, i, options.eventLogPath))
                return false;
        } else {
            return false;
        }
    }

    static const QStringList userModes{
        QStringLiteral("valid"),
        QStringLiteral("empty-permissions"),
        QStringLiteral("blank-login"),
        QStringLiteral("blank-name"),
        QStringLiteral("missing-permissions"),
        QStringLiteral("malformed"),
    };
    return userModes.contains(options.userMode);
}

class EventLog final {
public:
    explicit EventLog(const QString &path)
        : m_file(path)
    {
        if (!path.isEmpty())
            m_file.open(QIODevice::WriteOnly | QIODevice::Append);
    }

    void write(QString event, QJsonObject details = {})
    {
        details.insert(QStringLiteral("event"), std::move(event));
        details.insert(QStringLiteral("pid"),
                       static_cast<qint64>(QCoreApplication::applicationPid()));
        if (!m_file.isOpen())
            return;
        m_file.write(QJsonDocument(details).toJson(QJsonDocument::Compact));
        m_file.write("\n");
        m_file.flush();
    }

private:
    QFile m_file;
};

QByteArray currentUserPayload(const QString &mode)
{
    if (mode == QStringLiteral("malformed"))
        return QByteArrayLiteral("{not-json");

    QJsonObject object{
        {QStringLiteral("loginName"), QStringLiteral("test.login")},
        {QStringLiteral("name"), QStringLiteral("Test User")},
    };
    if (mode == QStringLiteral("blank-login"))
        object.insert(QStringLiteral("loginName"), QStringLiteral(" \t"));
    if (mode == QStringLiteral("blank-name"))
        object.insert(QStringLiteral("name"), QStringLiteral("  "));
    if (mode != QStringLiteral("missing-permissions")) {
        QJsonArray permissions;
        if (mode != QStringLiteral("empty-permissions")) {
            permissions.append(QStringLiteral("diagnostics.read"));
            permissions.append(QStringLiteral("UNKNOWN.permission"));
            permissions.append(QStringLiteral("diagnostics.read"));
        }
        object.insert(QStringLiteral("permissions"), permissions);
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool isExactShutdownRequest(const QByteArray &request, quint16 port)
{
    if (!request.endsWith("\r\n\r\n"))
        return false;
    for (qsizetype index = 0; index < request.size(); ++index) {
        if (request.at(index) == '\n'
            && (index == 0 || request.at(index - 1) != '\r')) {
            return false;
        }
    }
    QList<QByteArray> lines = request.left(request.size() - 4).split('\n');
    if (lines.isEmpty())
        return false;
    QByteArray firstLine = lines.takeFirst();
    if (!firstLine.endsWith('\r'))
        return false;
    firstLine.chop(1);
    if (firstLine
        != QByteArrayLiteral(
            "POST /xc2/1.0/serviceStatus/shutdown HTTP/1.1")) {
        return false;
    }

    int hostCount = 0;
    int contentTypeCount = 0;
    int contentLengthCount = 0;
    int connectionCount = 0;
    int cookieCount = 0;
    for (QByteArray line : std::as_const(lines)) {
        if (line.endsWith('\r'))
            line.chop(1);
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0)
            return false;
        const QByteArray name = line.left(colon);
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (name.compare(QByteArrayLiteral("Host"),
                         Qt::CaseInsensitive) == 0) {
            ++hostCount;
            if (value != QByteArrayLiteral("127.0.0.1:")
                    + QByteArray::number(port))
                return false;
        } else if (name.compare(QByteArrayLiteral("Content-Type"),
                                Qt::CaseInsensitive) == 0) {
            ++contentTypeCount;
            if (value
                != QByteArrayLiteral("application/x-www-form-urlencoded"))
                return false;
        } else if (name.compare(QByteArrayLiteral("Content-Length"),
                                Qt::CaseInsensitive) == 0) {
            ++contentLengthCount;
            if (value != QByteArrayLiteral("0"))
                return false;
        } else if (name.compare(QByteArrayLiteral("Connection"),
                                Qt::CaseInsensitive) == 0) {
            ++connectionCount;
            if (value.compare(QByteArrayLiteral("close"),
                              Qt::CaseInsensitive) != 0)
                return false;
        } else if (name.compare(QByteArrayLiteral("Cookie"),
                                Qt::CaseInsensitive) == 0) {
            ++cookieCount;
            if (value.isEmpty())
                return false;
        } else {
            return false;
        }
    }
    return hostCount == 1 && contentTypeCount == 1
        && contentLengthCount == 1 && connectionCount == 1
        && cookieCount <= 1;
}

void writeProcessOutput(const Options &options)
{
    QFile standardOutput;
    QFile standardError;
    standardOutput.open(stdout, QIODevice::WriteOnly,
                        QFileDevice::DontCloseHandle);
    standardError.open(stderr, QIODevice::WriteOnly,
                       QFileDevice::DontCloseHandle);

    if (options.fragmentOutput) {
        standardOutput.write("stdout-frag");
        standardOutput.flush();
        standardOutput.write("ment\r");
        standardOutput.flush();
        standardOutput.write("\n");
        standardError.write("stderr-frag");
        standardError.flush();
        standardError.write("ment\n");
    }
    if (options.oversizeOutput) {
        for (int index = 0; index < 300; ++index) {
            const QByteArray line = QByteArrayLiteral("stdout-ring-")
                + QByteArray::number(index) + '\n';
            standardOutput.write(line);
            standardError.write(QByteArrayLiteral("stderr-ring-")
                                + QByteArray::number(index) + '\n');
        }
        const QByteArray line(20 * 1024, 'x');
        standardOutput.write(line);
        standardOutput.write("\n");
        standardError.write(line);
        standardError.write("\n");
    }
    if (options.sensitiveOutput) {
        static const QList<QByteArray> names{
            QByteArrayLiteral("Authorization"),
            QByteArrayLiteral("Proxy-Authorization"),
            QByteArrayLiteral("Cookie"),
            QByteArrayLiteral("Set-Cookie"),
            QByteArrayLiteral("password"),
            QByteArrayLiteral("token"),
            QByteArrayLiteral("sessionIndex"),
            QByteArrayLiteral("SAMLRequest"),
            QByteArrayLiteral("SAMLResponse"),
        };
        for (qsizetype index = 0; index < names.size(); ++index) {
            const QByteArray &name = names.at(index);
            QByteArray line;
            if (index % 3 == 0) {
                line = name + QByteArrayLiteral(": secret-")
                    + name.toLower();
            } else if (index % 3 == 1) {
                line = name + QByteArrayLiteral("=secret-")
                    + name.toLower();
            } else {
                line = QByteArrayLiteral("{\"") + name
                    + QByteArrayLiteral("\":\"secret-") + name.toLower()
                    + QByteArrayLiteral("\"}");
            }
            line.prepend(QByteArrayLiteral("\x1b[31m\x01"));
            line.append(QByteArrayLiteral("\x1b[0m\n"));
            standardOutput.write(line);
            standardError.write(line);
        }
        standardOutput.write("token=secret-eof-stdout");
        standardError.write("{\"password\":\"secret-eof-stderr\"}");
    }
    standardOutput.flush();
    standardError.flush();
}

class FakeServer final : public QObject {
public:
    FakeServer(Options options, EventLog *log, QObject *parent = nullptr)
        : QObject(parent), m_options(std::move(options)), m_log(log)
    {
        m_elapsed.start();
        connect(&m_server, &QTcpServer::newConnection, this,
                [this] { acceptConnections(); });
    }

    void start()
    {
        if (m_options.exitBeforeReady) {
            m_log->write(QStringLiteral("EXIT_BEFORE_READY"));
            QTimer::singleShot(m_options.readyDelayMs, qApp,
                               [] { QCoreApplication::exit(7); });
            return;
        }
        QTimer::singleShot(m_options.readyDelayMs, this,
                           [this] { beginListening(); });
    }

private:
    void beginListening()
    {
        if (!m_server.listen(QHostAddress::LocalHost, m_options.port)) {
            m_log->write(QStringLiteral("BIND_FAILED"),
                         {{QStringLiteral("port"), m_options.port},
                          {QStringLiteral("error"), m_server.errorString()}});
            return;
        }
        m_log->write(QStringLiteral("LISTENING"),
                     {{QStringLiteral("address"),
                       m_server.serverAddress().toString()},
                      {QStringLiteral("port"), m_server.serverPort()}});
        if (!m_options.releaseTriggerPath.isEmpty()) {
            auto *timer = new QTimer(this);
            timer->setInterval(10);
            connect(timer, &QTimer::timeout, this, [this, timer] {
                if (!QFileInfo::exists(m_options.releaseTriggerPath))
                    return;
                timer->stop();
                m_server.close();
                QFile::remove(m_options.releaseTriggerPath);
                m_log->write(QStringLiteral("LISTENER_RELEASED"),
                             {{QStringLiteral("port"), m_options.port}});
            });
            timer->start();
        }
    }

    void acceptConnections()
    {
        while (QTcpSocket *socket = m_server.nextPendingConnection()) {
            socket->setParent(this);
            auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, this,
                    [this, socket, buffer] {
                        buffer->append(socket->readAll());
                        if (buffer->size() > 64 * 1024) {
                            socket->abort();
                            return;
                        }
                        const qsizetype headerEnd = buffer->indexOf("\r\n\r\n");
                        if (headerEnd < 0)
                            return;
                        qsizetype contentLength = 0;
                        const QList<QByteArray> headers =
                            buffer->left(headerEnd).split('\n');
                        for (QByteArray header : headers) {
                            header = header.trimmed();
                            if (header.left(15).compare(
                                    QByteArrayLiteral("Content-Length:"),
                                    Qt::CaseInsensitive) == 0) {
                                bool ok = false;
                                contentLength = header.mid(15).trimmed()
                                                    .toLongLong(&ok);
                                if (!ok || contentLength < 0) {
                                    socket->abort();
                                    return;
                                }
                            }
                        }
                        const qsizetype total = headerEnd + 4 + contentLength;
                        if (buffer->size() < total)
                            return;
                        if (buffer->size() != total) {
                            socket->abort();
                            return;
                        }
                        const QByteArray request = buffer->left(total);
                        socket->disconnect(this);
                        handleRequest(socket, request);
                    });
        }
    }

    void handleRequest(QTcpSocket *socket, const QByteArray &request)
    {
        const qsizetype firstLineEnd = request.indexOf("\r\n");
        const QByteArray firstLine = firstLineEnd >= 0
            ? request.left(firstLineEnd) : request;
        const QList<QByteArray> parts = firstLine.split(' ');
        const QByteArray method = parts.value(0);
        const QByteArray target = parts.value(1);
        m_log->write(QStringLiteral("REQUEST"),
                     {{QStringLiteral("method"),
                       QString::fromLatin1(method)},
                      {QStringLiteral("target"),
                       QString::fromLatin1(target)},
                      {QStringLiteral("bytesBase64"),
                       QString::fromLatin1(request.toBase64())}});

        if (method == QByteArrayLiteral("GET")
            && target
                == QByteArrayLiteral("/xc2/1.0/serviceStatus/status")) {
            handleHealth(socket);
            return;
        }
        if (method == QByteArrayLiteral("GET")
            && target == QByteArrayLiteral("/xc2/1.0/auth/currentUser")) {
            const QByteArray extraHeaders =
                m_options.userMode == QStringLiteral("valid")
                ? QByteArrayLiteral(
                    "Set-Cookie: XC2SESSION=sidecar; Path=/xc2/1.0\r\n")
                : QByteArray();
            const QPointer<QTcpSocket> guarded(socket);
            QTimer::singleShot(m_options.currentUserDelayMs, this,
                               [this, guarded, extraHeaders] {
                if (!guarded)
                    return;
                sendResponse(guarded,
                             currentUserPayload(m_options.userMode),
                             QByteArrayLiteral("application/json"), 200,
                             QByteArrayLiteral("OK"), extraHeaders);
                m_log->write(QStringLiteral("CURRENT_USER_RESPONSE"));
            });
            return;
        }
        if (method == QByteArrayLiteral("POST")
            && target
                == QByteArrayLiteral("/xc2/1.0/serviceStatus/shutdown")) {
            if (!isExactShutdownRequest(request, m_options.port)) {
                sendResponse(socket, QByteArrayLiteral("invalid shutdown"),
                             QByteArrayLiteral("text/plain"), 400,
                             QByteArrayLiteral("Bad Request"));
                return;
            }
            m_log->write(QStringLiteral("SHUTDOWN"),
                         {{QStringLiteral("bytesBase64"),
                           QString::fromLatin1(request.toBase64())}});
            if (m_options.ignoreShutdown)
                return;
            sendResponse(socket, QByteArray(),
                         QByteArrayLiteral("text/plain"));
            QTimer::singleShot(0, qApp, [] { QCoreApplication::quit(); });
            return;
        }
        sendResponse(socket, QByteArrayLiteral("not found"),
                     QByteArrayLiteral("text/plain"), 404,
                     QByteArrayLiteral("Not Found"));
    }

    void handleHealth(QTcpSocket *socket)
    {
        if (m_options.spawnDescendant && !m_descendantStarted) {
            qint64 processId = 0;
            m_descendantStarted = QProcess::startDetached(
                QCoreApplication::applicationFilePath(),
                {QStringLiteral("--descendant")},
                QFileInfo(QCoreApplication::applicationFilePath())
                    .absolutePath(),
                &processId);
            m_log->write(QStringLiteral("DESCENDANT_STARTED"),
                         {{QStringLiteral("descendantPid"), processId},
                          {QStringLiteral("started"), m_descendantStarted}});
        }
        ++m_concurrentHealth;
        m_maxConcurrentHealth = qMax(m_maxConcurrentHealth,
                                     m_concurrentHealth);
        m_log->write(QStringLiteral("HEALTH_COUNTS"),
                     {{QStringLiteral("concurrent"), m_concurrentHealth},
                      {QStringLiteral("maximum"), m_maxConcurrentHealth}});
        const auto completed = std::make_shared<bool>(false);
        const auto finishCount = [this, completed] {
            if (*completed)
                return;
            *completed = true;
            --m_concurrentHealth;
            m_log->write(QStringLiteral("HEALTH_COUNTS"),
                         {{QStringLiteral("concurrent"), m_concurrentHealth},
                          {QStringLiteral("maximum"), m_maxConcurrentHealth}});
        };
        connect(socket, &QTcpSocket::destroyed, this, finishCount);
        connect(socket, &QTcpSocket::disconnected, this, finishCount);
        if (m_options.neverHealth)
            return;

        const int untilLateAlive = qMax(
            0, m_options.lateAliveMs - static_cast<int>(m_elapsed.elapsed()));
        const int delay = qMax(m_options.healthDelayMs, untilLateAlive);
        const QPointer<QTcpSocket> guarded(socket);
        QTimer::singleShot(delay, this, [this, guarded, finishCount] {
            m_log->write(QStringLiteral("HEALTH_RESPONSE"),
                         {{QStringLiteral("connected"),
                           guarded
                               && guarded->state()
                                   == QAbstractSocket::ConnectedState}});
            if (!guarded)
                return;
            sendResponse(guarded, QByteArrayLiteral("alive"),
                         QByteArrayLiteral("text/plain"));
            finishCount();
        });
    }

    static void sendResponse(QTcpSocket *socket, const QByteArray &body,
                             const QByteArray &contentType,
                             int status = 200,
                             const QByteArray &reason = QByteArrayLiteral("OK"),
                             const QByteArray &extraHeaders = {})
    {
        QByteArray response = QByteArrayLiteral("HTTP/1.1 ")
            + QByteArray::number(status) + ' ' + reason
            + QByteArrayLiteral("\r\nContent-Type: ") + contentType
            + QByteArrayLiteral("\r\n") + extraHeaders
            + QByteArrayLiteral("Content-Length: ")
            + QByteArray::number(body.size())
            + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    Options m_options;
    EventLog *m_log = nullptr;
    QTcpServer m_server;
    QElapsedTimer m_elapsed;
    int m_concurrentHealth = 0;
    int m_maxConcurrentHealth = 0;
    bool m_descendantStarted = false;
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    Options options;
    if (!parseOptions(application.arguments(), options))
        return 2;

    EventLog log(options.eventLogPath);
    if (options.descendant)
        return application.exec();
    QJsonArray injectionVariables;
    static const QStringList deniedEnvironment{
        QStringLiteral("JAVA_TOOL_OPTIONS"),
        QStringLiteral("_JAVA_OPTIONS"),
        QStringLiteral("JDK_JAVA_OPTIONS"),
        QStringLiteral("SPRING_APPLICATION_JSON"),
        QStringLiteral("SPRING_CONFIG_LOCATION"),
        QStringLiteral("SPRING_CONFIG_ADDITIONAL_LOCATION"),
        QStringLiteral("SPRING_PROFILES_ACTIVE"),
        QStringLiteral("SERVER_ADDRESS"),
        QStringLiteral("SERVER_PORT"),
    };
    for (const QString &name : deniedEnvironment) {
        if (qEnvironmentVariableIsSet(name.toLatin1().constData()))
            injectionVariables.append(name);
    }
    log.write(QStringLiteral("ENVIRONMENT"),
              {{QStringLiteral("injectionVariables"), injectionVariables}});
    QObject::connect(&application, &QCoreApplication::aboutToQuit,
                     &application, [&log] {
                         log.write(QStringLiteral("PROCESS_EXIT"));
                     });

    std::unique_ptr<QLockFile> heldLock;
    if (!options.holdLockPath.isEmpty()) {
        heldLock = std::make_unique<QLockFile>(options.holdLockPath);
        heldLock->setStaleLockTime(0);
        if (!heldLock->tryLock(0)) {
            log.write(QStringLiteral("LOCK_FAILED"));
            return 3;
        }
        log.write(QStringLiteral("LOCKED"),
                  {{QStringLiteral("path"), options.holdLockPath}});
    }

    writeProcessOutput(options);
    FakeServer server(options, &log);
    server.start();
    return application.exec();
}
