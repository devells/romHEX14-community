#include "ktm/xc2/Xc2ContractProfile.h"
#include "ktm/xc2/Xc2RestClient.h"
#include "ktm/xc2/Xc2StompClient.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QTextStream>
#include <QTimer>

#include <limits>
#include <optional>

using namespace ktm::xc2;

namespace {

constexpr int kDefaultTimeoutMs = 30000;
constexpr int kMinimumTimeoutMs = 100;
constexpr int kMaximumTimeoutMs = 120000;

struct ParsedCommandLine {
    QByteArray rawBaseUrlBytes;
    int timeoutMs = kDefaultTimeoutMs;
    bool help = false;
    QString helpText;
};

QString categoryName(Xc2ErrorCategory category)
{
    switch (category) {
    case Xc2ErrorCategory::None:
        return QStringLiteral("none");
    case Xc2ErrorCategory::Prerequisite:
        return QStringLiteral("prerequisite");
    case Xc2ErrorCategory::Backend:
        return QStringLiteral("backend");
    case Xc2ErrorCategory::Contract:
        return QStringLiteral("contract");
    case Xc2ErrorCategory::Session:
        return QStringLiteral("session");
    case Xc2ErrorCategory::Vci:
        return QStringLiteral("vci");
    case Xc2ErrorCategory::Vehicle:
        return QStringLiteral("vehicle");
    case Xc2ErrorCategory::Job:
        return QStringLiteral("job");
    case Xc2ErrorCategory::FlashCritical:
        return QStringLiteral("flash-critical");
    case Xc2ErrorCategory::Transport:
        return QStringLiteral("transport");
    }
    return QStringLiteral("unknown");
}

void writeDiagnostic(const QString &stage,
                     const Xc2Error &error,
                     const QString &summary)
{
    QTextStream stream(stderr);
    stream << "xc2-contract-probe stage=" << stage
           << " category=" << categoryName(error.category)
           << " http=" << error.httpStatus
           << " xc2=" << error.xc2Code
           << " summary=" << summary << '\n';
    stream.flush();
}

bool parseDecimalTimeout(const QString &text, int *timeoutMs)
{
    if (text.isEmpty())
        return false;
    qint64 value = 0;
    for (const QChar character : text) {
        const ushort code = character.unicode();
        if (code < '0' || code > '9')
            return false;
        const int digit = code - '0';
        if (value > (std::numeric_limits<qint64>::max() - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    if (value < kMinimumTimeoutMs || value > kMaximumTimeoutMs)
        return false;
    *timeoutMs = int(value);
    return true;
}

std::optional<ParsedCommandLine> parseCommandLine(
    const QCoreApplication &application)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Read-only XC2 foundation contract probe"));
    parser.addHelpOption();
    const QCommandLineOption baseOption(
        QStringLiteral("base-url"),
        QStringLiteral("Exact loopback XC2 REST base URL"),
        QStringLiteral("raw-string"));
    const QCommandLineOption timeoutOption(
        QStringLiteral("timeout-ms"),
        QStringLiteral("Overall wall-clock deadline in milliseconds"),
        QStringLiteral("100..120000"));
    parser.addOption(baseOption);
    parser.addOption(timeoutOption);

    const QStringList arguments = application.arguments();
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QString &argument = arguments.at(index);
        if (argument.startsWith(QStringLiteral("--base-url="))
            || argument.startsWith(QStringLiteral("--timeout-ms="))) {
            return std::nullopt;
        }
    }
    if (!parser.parse(arguments))
        return std::nullopt;

    ParsedCommandLine parsed;
    if (parser.isSet(QStringLiteral("help"))) {
        if (arguments.size() != 2
            || (arguments.at(1) != QStringLiteral("--help")
                && arguments.at(1) != QStringLiteral("-h"))) {
            return std::nullopt;
        }
        parsed.help = true;
        parsed.helpText = parser.helpText();
        return parsed;
    }
    if (!parser.positionalArguments().isEmpty())
        return std::nullopt;

    const QStringList baseValues = parser.values(baseOption);
    const QStringList timeoutValues = parser.values(timeoutOption);
    if (baseValues.size() != 1 || timeoutValues.size() > 1)
        return std::nullopt;

    const QString &rawBase = baseValues.constFirst();
    if (rawBase.isEmpty())
        return std::nullopt;
    parsed.rawBaseUrlBytes.reserve(rawBase.size());
    for (const QChar character : rawBase) {
        const ushort code = character.unicode();
        if (code < 0x21 || code > 0x7e)
            return std::nullopt;
        parsed.rawBaseUrlBytes.append(char(code));
    }

    if (!timeoutValues.isEmpty()
        && !parseDecimalTimeout(timeoutValues.constFirst(),
                                &parsed.timeoutMs)) {
        return std::nullopt;
    }
    return parsed;
}

int backstopDeadlineMs(int outerDeadlineMs)
{
    return outerDeadlineMs > kMaximumTimeoutMs - 1000
        ? kMaximumTimeoutMs : outerDeadlineMs + 1000;
}

Xc2RestClientOptions restOptions(int outerDeadlineMs)
{
    const int backstop = backstopDeadlineMs(outerDeadlineMs);
    return {backstop, backstop};
}

Xc2StompClientOptions stompOptions(int outerDeadlineMs)
{
    Xc2StompClientOptions options;
    const int backstop = backstopDeadlineMs(outerDeadlineMs);
    options.connectDeadlineMs = backstop;
    options.disconnectDeadlineMs = backstop;
    return options;
}

class ProbeController final : public QObject {
public:
    ProbeController(QByteArray rawBaseUrlBytes,
                    int timeoutMs,
                    QObject *parent = nullptr)
        : QObject(parent),
          m_rawBaseUrlBytes(std::move(rawBaseUrlBytes)),
          m_timeoutMs(timeoutMs),
          m_rest(restOptions(timeoutMs), this),
          m_stomp(stompOptions(timeoutMs), this)
    {
        connect(&m_outerTimer, &QTimer::timeout,
                this, [this] { onOuterDeadline(); });
        connect(&m_rest, &Xc2RestClient::serviceStatusFinished,
                this, [this](Xc2RequestId id,
                             const Xc2Result<Xc2ServiceStatus> &result) {
            onServiceStatus(id, result);
        });
        connect(&m_rest, &Xc2RestClient::currentUserFinished,
                this, [this](Xc2RequestId id,
                             const Xc2Result<Xc2CurrentUser> &result) {
            onCurrentUser(id, result);
        });
        connect(&m_stomp, &Xc2StompClient::connected,
                this, [this](const Xc2StompSession &session) {
            onStompConnected(session);
        });
        connect(&m_stomp, &Xc2StompClient::subscriptionSent,
                this, [this](Topic topic, const QString &) {
            onSubscriptionSent(topic);
        });
        connect(&m_stomp, &Xc2StompClient::unsubscriptionSent,
                this, [this](Topic, const QString &) {
            if (m_completed || m_stomp.generation() != m_generation
                || !stompCallbackIsCurrent(m_generation)) {
                return;
            }
            failStomp(QStringLiteral("unexpected-unsubscribe"));
        });
        connect(&m_stomp, &Xc2StompClient::messageReceived,
                this, [this](const Xc2StompMessage &message) {
            if (!stompCallbackIsCurrent(message.generation))
                return;
            failStomp(QStringLiteral("unexpected-message"));
        });
        connect(&m_stomp, &Xc2StompClient::errorOccurred,
                this, [this](Xc2StompGeneration generation,
                             const Xc2Error &error) {
            onStompError(generation, error);
        });
        connect(&m_stomp, &Xc2StompClient::visibilityLost,
                this, [this](Xc2StompGeneration generation,
                             const Xc2Error &error) {
            if (!stompCallbackIsCurrent(generation))
                return;
            finishOnce(4, QStringLiteral("stomp"), error,
                       QStringLiteral("visibility-lost"));
        });
        connect(&m_stomp, &Xc2StompClient::disconnected,
                this, [this](Xc2StompGeneration generation) {
            onStompDisconnected(generation);
        });

        m_outerTimer.setSingleShot(true);
        m_outerTimer.setTimerType(Qt::PreciseTimer);
        m_baseValid = m_rest.setBaseUrl(m_rawBaseUrlBytes, &m_baseError);
    }

    bool baseValid() const { return m_baseValid; }
    const Xc2Error &baseError() const { return m_baseError; }

    void start()
    {
        if (m_completed || !m_baseValid)
            return;
        m_deadline = QDeadlineTimer(m_timeoutMs, Qt::PreciseTimer);
        m_outerTimer.start(int(qMax<qint64>(1, m_deadline.remainingTime())));
        m_stage = Stage::Health;
        m_requestId = m_rest.requestServiceStatus();
        if (m_requestId == 0) {
            Xc2Error error;
            error.category = Xc2ErrorCategory::Transport;
            finishOnce(2, QStringLiteral("health"), error,
                       QStringLiteral("request-start-failed"));
        }
    }

private:
    enum class Stage {
        Idle,
        Health,
        CurrentUser,
        Stomp,
        Disconnecting,
        Complete
    };

    bool callbackIsCurrent(Xc2RequestId id, Stage expected,
                           int deadlineExit, const QString &stage)
    {
        if (m_completed || m_stage != expected || id != m_requestId)
            return false;
        if (m_deadline.hasExpired()) {
            Xc2Error error;
            error.category = Xc2ErrorCategory::Transport;
            error.transportReason = Xc2TransportReason::Timeout;
            finishOnce(deadlineExit, stage, error,
                       QStringLiteral("overall-deadline"));
            return false;
        }
        return true;
    }

    bool stompCallbackIsCurrent(Xc2StompGeneration generation)
    {
        if (m_completed || generation != m_generation)
            return false;
        if (m_deadline.hasExpired()) {
            Xc2Error error;
            error.category = Xc2ErrorCategory::Transport;
            error.transportReason = Xc2TransportReason::Timeout;
            finishOnce(4, QStringLiteral("stomp"), error,
                       QStringLiteral("overall-deadline"));
            return false;
        }
        return true;
    }

    void onServiceStatus(
        Xc2RequestId id,
        const Xc2Result<Xc2ServiceStatus> &result)
    {
        if (!callbackIsCurrent(id, Stage::Health, 2,
                               QStringLiteral("health"))) {
            return;
        }
        m_requestId = 0;
        if (!result.ok()) {
            const int code = result.error.category
                    == Xc2ErrorCategory::Contract
                ? 3 : 2;
            finishOnce(code, QStringLiteral("health"), result.error,
                       code == 3 ? QStringLiteral("contract-failure")
                                 : QStringLiteral("backend-unavailable"));
            return;
        }
        if (!result.value->alive) {
            Xc2Error error;
            error.category = Xc2ErrorCategory::Contract;
            finishOnce(3, QStringLiteral("health"), error,
                       QStringLiteral("not-alive"));
            return;
        }
        if (m_deadline.hasExpired()) {
            Xc2Error error;
            error.category = Xc2ErrorCategory::Transport;
            error.transportReason = Xc2TransportReason::Timeout;
            finishOnce(2, QStringLiteral("health"), error,
                       QStringLiteral("overall-deadline"));
            return;
        }
        m_stage = Stage::CurrentUser;
        m_requestId = m_rest.requestCurrentUser();
        if (m_requestId == 0) {
            Xc2Error error;
            error.category = Xc2ErrorCategory::Transport;
            finishOnce(2, QStringLiteral("current-user"), error,
                       QStringLiteral("request-start-failed"));
        }
    }

    void onCurrentUser(
        Xc2RequestId id,
        const Xc2Result<Xc2CurrentUser> &result)
    {
        if (!callbackIsCurrent(id, Stage::CurrentUser, 2,
                               QStringLiteral("current-user"))) {
            return;
        }
        m_requestId = 0;
        if (!result.ok()) {
            const Xc2Error &error = result.error;
            if (error.category == Xc2ErrorCategory::Contract) {
                const bool unauthenticated204 = error.httpStatus == 204
                    && error.rawPayload.isEmpty();
                finishOnce(unauthenticated204 ? 5 : 3,
                           QStringLiteral("current-user"), error,
                           unauthenticated204
                               ? QStringLiteral("not-authenticated")
                               : QStringLiteral("contract-failure"));
                return;
            }
            const bool decodedAuthError =
                error.category == Xc2ErrorCategory::Backend
                && (error.httpStatus == 401 || error.httpStatus == 403);
            finishOnce(decodedAuthError ? 5 : 2,
                       QStringLiteral("current-user"), error,
                       decodedAuthError
                           ? QStringLiteral("not-authenticated")
                           : QStringLiteral("backend-unavailable"));
            return;
        }

        const Xc2CurrentUser &user = *result.value;
        if (user.loginName.trimmed().isEmpty()
            || user.name.trimmed().isEmpty()) {
            Xc2Error error;
            error.category = Xc2ErrorCategory::Contract;
            error.httpStatus = 200;
            finishOnce(3, QStringLiteral("current-user"), error,
                       QStringLiteral("blank-identity"));
            return;
        }
        const QStringList requiredPermissions =
            Xc2ContractProfile::approved().foundationProbePermissions();
        for (const QString &permission : requiredPermissions) {
            if (!user.permissions.contains(permission, Qt::CaseSensitive)) {
                Xc2Error error;
                error.category = Xc2ErrorCategory::Session;
                error.httpStatus = 200;
                finishOnce(5, QStringLiteral("permission"), error,
                           QStringLiteral("permission-missing"));
                return;
            }
        }
        beginStomp();
    }

    void beginStomp()
    {
        if (m_deadline.hasExpired()) {
            Xc2Error error;
            error.category = Xc2ErrorCategory::Transport;
            error.transportReason = Xc2TransportReason::Timeout;
            finishOnce(2, QStringLiteral("current-user"), error,
                       QStringLiteral("overall-deadline"));
            return;
        }
        m_stage = Stage::Stomp;
        Xc2Error error;
        for (const Topic topic
             : Xc2ContractProfile::approved().foundationProbeTopics()) {
            if (!m_stomp.subscribe(topic, &error)) {
                finishOnce(4, QStringLiteral("stomp"), error,
                           QStringLiteral("subscription-queue-failed"));
                return;
            }
        }
        if (m_deadline.hasExpired()) {
            error.category = Xc2ErrorCategory::Transport;
            error.transportReason = Xc2TransportReason::Timeout;
            finishOnce(4, QStringLiteral("stomp"), error,
                       QStringLiteral("overall-deadline"));
            return;
        }
        if (!m_stomp.connectToBackend(m_rest, &error)) {
            finishOnce(4, QStringLiteral("stomp"), error,
                       QStringLiteral("connect-start-failed"));
            return;
        }
        m_generation = m_stomp.generation();
        if (m_generation == 0) {
            error.category = Xc2ErrorCategory::Contract;
            finishOnce(4, QStringLiteral("stomp"), error,
                       QStringLiteral("invalid-generation"));
        }
    }

    void onStompConnected(const Xc2StompSession &session)
    {
        if (!stompCallbackIsCurrent(session.generation))
            return;
        if (m_stage != Stage::Stomp || session.version != QStringLiteral("1.2")
            || m_connected) {
            failStomp(QStringLiteral("invalid-connected"));
            return;
        }
        m_connected = true;
    }

    void onSubscriptionSent(Topic topic)
    {
        if (m_completed || m_stomp.generation() != m_generation)
            return;
        if (!stompCallbackIsCurrent(m_generation))
            return;
        const QList<Topic> expected =
            Xc2ContractProfile::approved().foundationProbeTopics();
        if (m_stage != Stage::Stomp || !m_connected
            || m_subscriptionCount >= expected.size()
            || topic != expected.at(m_subscriptionCount)) {
            failStomp(QStringLiteral("subscription-order"));
            return;
        }
        ++m_subscriptionCount;
        if (m_subscriptionCount != expected.size())
            return;
        m_stage = Stage::Disconnecting;
        m_stomp.disconnectFromBackend();
    }

    void onStompError(Xc2StompGeneration generation,
                      const Xc2Error &error)
    {
        if (!stompCallbackIsCurrent(generation))
            return;
        m_stompErrorSeen = true;
        finishOnce(4, QStringLiteral("stomp"), error,
                   QStringLiteral("transport-or-contract-failure"));
    }

    void onStompDisconnected(Xc2StompGeneration generation)
    {
        if (!stompCallbackIsCurrent(generation))
            return;
        const qsizetype expectedSubscriptions =
            Xc2ContractProfile::approved().foundationProbeTopics().size();
        if (m_stage != Stage::Disconnecting || !m_connected
            || m_stompErrorSeen
            || m_subscriptionCount != expectedSubscriptions) {
            failStomp(QStringLiteral("disconnect-without-receipt-close"));
            return;
        }
        Xc2Error success;
        finishOnce(0, QStringLiteral("complete"), success,
                   QStringLiteral("compatible-read-only-surface"));
    }

    void failStomp(const QString &summary)
    {
        if (m_completed)
            return;
        Xc2Error error;
        error.category = Xc2ErrorCategory::Contract;
        finishOnce(4, QStringLiteral("stomp"), error, summary);
    }

    void onOuterDeadline()
    {
        if (m_completed)
            return;
        if (!m_deadline.hasExpired()) {
            m_outerTimer.start(
                int(qMax<qint64>(1, m_deadline.remainingTime())));
            return;
        }
        Xc2Error error;
        error.category = Xc2ErrorCategory::Transport;
        error.transportReason = Xc2TransportReason::Timeout;
        const bool stompStarted = m_stage == Stage::Stomp
            || m_stage == Stage::Disconnecting;
        finishOnce(stompStarted ? 4 : 2,
                   stompStarted ? QStringLiteral("stomp")
                                : m_stage == Stage::CurrentUser
                                    ? QStringLiteral("current-user")
                                    : QStringLiteral("health"),
                   error, QStringLiteral("overall-deadline"));
    }

    void finishOnce(int code,
                    const QString &stage,
                    const Xc2Error &error,
                    const QString &summary)
    {
        if (m_completed)
            return;
        m_completed = true;
        m_stage = Stage::Complete;
        m_outerTimer.stop();

        const Xc2RequestId pendingRequest = m_requestId;
        m_requestId = 0;
        if (pendingRequest != 0)
            m_rest.abort(pendingRequest);
        if (m_generation != 0
            && m_stomp.state() != Xc2StompState::Disconnected
            && m_stomp.state() != Xc2StompState::Failed) {
            m_stomp.abortCurrentGeneration();
        }

        if (code == 0) {
            QTextStream stream(stdout);
            stream << "xc2-contract-probe compatible-read-only-surface\n";
            stream.flush();
        } else {
            writeDiagnostic(stage, error, summary);
        }
        QCoreApplication::exit(code);
    }

    QByteArray m_rawBaseUrlBytes;
    int m_timeoutMs = kDefaultTimeoutMs;
    Xc2RestClient m_rest;
    Xc2StompClient m_stomp;
    QDeadlineTimer m_deadline;
    QTimer m_outerTimer;
    Xc2Error m_baseError;
    Xc2RequestId m_requestId = 0;
    Xc2StompGeneration m_generation = 0;
    Stage m_stage = Stage::Idle;
    qsizetype m_subscriptionCount = 0;
    bool m_baseValid = false;
    bool m_connected = false;
    bool m_stompErrorSeen = false;
    bool m_completed = false;
};

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("xc2_contract_probe"));

    const std::optional<ParsedCommandLine> parsed =
        parseCommandLine(application);
    if (!parsed.has_value()) {
        Xc2Error error;
        error.category = Xc2ErrorCategory::Contract;
        writeDiagnostic(QStringLiteral("cli"), error,
                        QStringLiteral("invalid-arguments"));
        return 2;
    }
    if (parsed->help) {
        QTextStream stream(stdout);
        stream << parsed->helpText;
        stream.flush();
        return 0;
    }

    ProbeController controller(parsed->rawBaseUrlBytes, parsed->timeoutMs);
    if (!controller.baseValid()) {
        writeDiagnostic(QStringLiteral("base-url"), controller.baseError(),
                        QStringLiteral("invalid-base-url"));
        return 2;
    }
    QTimer::singleShot(0, &controller, [&controller] { controller.start(); });
    return application.exec();
}
