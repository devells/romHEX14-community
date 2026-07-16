#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include "Xc2BackendManager.h"

#include "Xc2InstallationProbe.h"
#include "Xc2RestClient.h"
#include "internal/Xc2ProcessOutput.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileInfo>
#include <QLockFile>
#include <QNetworkProxy>
#include <QPointer>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace ktm::xc2 {
namespace {

constexpr quint16 kForbiddenVendorPort = 8082;
constexpr int kHealthRetryMs = 50;
constexpr int kOwnershipRetryMs = 10;
constexpr int kMaximumTimeoutMs = 120000;
constexpr int kMaximumKillAttempts = 3;
constexpr int kMaximumQtReconcileAttempts = 2;
constexpr qsizetype kProcessReadBlockBytes = 4096;

struct ProductionInspectionTestHooks {
    std::function<void(const QString &)> observer;
    int delayMs = 0;
};

ProductionInspectionTestHooks &productionInspectionTestHooks()
{
    static ProductionInspectionTestHooks hooks;
    return hooks;
}

std::function<void()> &reaperRepostTestHook()
{
    static std::function<void()> hook;
    return hook;
}

Xc2Error backendError(const QString &message)
{
    Xc2Error error;
    error.category = Xc2ErrorCategory::Backend;
    error.message = message;
    return error;
}

Xc2Error prerequisiteError(const QString &message)
{
    Xc2Error error;
    error.category = Xc2ErrorCategory::Prerequisite;
    error.message = message;
    return error;
}

QString nativeFailure(const QString &operation, DWORD code = GetLastError())
{
    return QStringLiteral("%1 failed with Windows error %2")
        .arg(operation)
        .arg(static_cast<qulonglong>(code));
}

bool processHandleIsLive(HANDLE handle)
{
    return handle != nullptr
        && WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
}

bool processHandleIsSignaled(HANDLE handle)
{
    return handle != nullptr
        && WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
}

QString processImagePath(HANDLE handle)
{
    std::vector<wchar_t> buffer(32768);
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(handle, 0, buffer.data(), &size))
        return {};
    return QDir::cleanPath(
        QDir::fromNativeSeparators(QString::fromWCharArray(
            buffer.data(), static_cast<qsizetype>(size))));
}

QString comparablePath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(QDir::fromNativeSeparators(
        canonical.isEmpty() ? info.absoluteFilePath() : canonical));
}

struct TcpRows {
    std::unique_ptr<unsigned char[]> storage;
    const MIB_TCPTABLE_OWNER_PID *table = nullptr;
};

std::optional<TcpRows> queryTcpRows(TCP_TABLE_CLASS tableClass,
                                    QString &failure)
{
    ULONG size = 0;
    DWORD result = GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                                       tableClass, 0);
    if (result != ERROR_INSUFFICIENT_BUFFER && result != NO_ERROR) {
        failure = nativeFailure(QStringLiteral("GetExtendedTcpTable(size)"),
                                result);
        return std::nullopt;
    }

    for (int attempt = 0; attempt < 4; ++attempt) {
        const ULONG allocation = qMax<ULONG>(
            size, static_cast<ULONG>(sizeof(MIB_TCPTABLE_OWNER_PID)));
        TcpRows rows;
        rows.storage = std::make_unique<unsigned char[]>(allocation);
        ULONG actualSize = allocation;
        result = GetExtendedTcpTable(rows.storage.get(), &actualSize, FALSE,
                                     AF_INET, tableClass, 0);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            size = actualSize;
            continue;
        }
        if (result != NO_ERROR) {
            failure = nativeFailure(QStringLiteral("GetExtendedTcpTable"),
                                    result);
            return std::nullopt;
        }
        rows.table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID *>(
            rows.storage.get());
        const quint64 required = sizeof(DWORD)
            + static_cast<quint64>(rows.table->dwNumEntries)
                * sizeof(MIB_TCPROW_OWNER_PID);
        if (required > actualSize) {
            failure = QStringLiteral("GetExtendedTcpTable returned a truncated table");
            return std::nullopt;
        }
        return rows;
    }

    failure = QStringLiteral("TCP owner table kept growing");
    return std::nullopt;
}

quint16 decodedPort(DWORD value)
{
    return ntohs(static_cast<u_short>(value & 0xffffU));
}

quint32 decodedAddress(DWORD value)
{
    return ntohl(static_cast<u_long>(value));
}

struct RawOwnerRow {
    quint32 localAddress = 0;
    quint32 localPort = 0;
    quint32 owningPid = 0;
};

bool listenerRowsOwned(const QList<RawOwnerRow> &rows, quint16 port,
                       quint32 expectedPid)
{
    qsizetype matching = 0;
    for (const RawOwnerRow &row : rows) {
        if (decodedPort(row.localPort) != port)
            continue;
        ++matching;
        if (decodedAddress(row.localAddress) != 0x7f000001U
            || row.owningPid != expectedPid) {
            return false;
        }
    }
    return matching == 1;
}

struct RawConnectionRow {
    quint32 state = 0;
    quint32 localAddress = 0;
    quint32 localPort = 0;
    quint32 remoteAddress = 0;
    quint32 remotePort = 0;
    quint32 owningPid = 0;
};

bool shutdownRowsOwned(const QList<RawConnectionRow> &rows,
                       quint16 serverPort, quint16 clientPort,
                       quint32 expectedPid)
{
    qsizetype matching = 0;
    for (const RawConnectionRow &row : rows) {
        if (row.state != MIB_TCP_STATE_ESTAB
            || decodedPort(row.localPort) != serverPort
            || decodedPort(row.remotePort) != clientPort) {
            continue;
        }
        ++matching;
        if (decodedAddress(row.localAddress) != 0x7f000001U
            || decodedAddress(row.remoteAddress) != 0x7f000001U
            || row.owningPid != expectedPid) {
            return false;
        }
    }
    return matching == 1;
}

enum class ListenerProof {
    Owned,
    NotPresent,
    ProvenCollision,
    Invalid,
};

ListenerProof proveListener(quint16 port, DWORD expectedPid, HANDLE handle,
                            QString &failure,
                            const std::function<void()> &afterSnapshot = {})
{
    if (!processHandleIsLive(handle)) {
        failure = QStringLiteral("The captured child process HANDLE is signaled");
        return ListenerProof::Invalid;
    }

    const auto rows = queryTcpRows(TCP_TABLE_OWNER_PID_LISTENER, failure);
    if (!rows.has_value())
        return ListenerProof::Invalid;

    const MIB_TCPROW_OWNER_PID *matching = nullptr;
    qsizetype count = 0;
    QList<RawOwnerRow> rawRows;
    rawRows.reserve(static_cast<qsizetype>(rows->table->dwNumEntries));
    for (DWORD index = 0; index < rows->table->dwNumEntries; ++index) {
        const MIB_TCPROW_OWNER_PID &row = rows->table->table[index];
        rawRows.append({row.dwLocalAddr, row.dwLocalPort, row.dwOwningPid});
        if (decodedPort(row.dwLocalPort) != port)
            continue;
        ++count;
        matching = &row;
    }
    if (afterSnapshot)
        afterSnapshot();
    if (!processHandleIsLive(handle)) {
        failure = QStringLiteral("The captured child process HANDLE became signaled during listener proof");
        return ListenerProof::Invalid;
    }
    if (count == 0) {
        failure = QStringLiteral("No IPv4 listener owns the selected port");
        return ListenerProof::NotPresent;
    }
    if (count != 1 || matching == nullptr) {
        failure = QStringLiteral("The selected port has duplicate IPv4 owner rows");
        return ListenerProof::Invalid;
    }
    if (decodedAddress(matching->dwLocalAddr) != 0x7f000001U) {
        failure = QStringLiteral("The selected listener is not exact IPv4 localhost");
        return ListenerProof::Invalid;
    }
    if (matching->dwOwningPid != expectedPid) {
        failure = QStringLiteral("The selected listener belongs to another process");
        return ListenerProof::ProvenCollision;
    }
    if (!listenerRowsOwned(rawRows, port, expectedPid)) {
        failure = QStringLiteral("The selected listener owner row is invalid");
        return ListenerProof::Invalid;
    }
    return ListenerProof::Owned;
}

enum class ShutdownProof {
    Owned,
    NotPresent,
    Invalid,
};

ShutdownProof proveShutdownTuple(quint16 serverPort, quint16 clientPort,
                                 DWORD expectedPid, HANDLE handle,
                                 QString &failure,
                                 const std::function<void()> &afterSnapshot = {},
                                 int proofMutation = 0)
{
    if (!processHandleIsLive(handle)) {
        failure = QStringLiteral("The captured child process HANDLE is signaled");
        return ShutdownProof::Invalid;
    }
    if (clientPort == 0) {
        failure = QStringLiteral("The shutdown socket has no local port");
        return ShutdownProof::Invalid;
    }

    const auto rows = queryTcpRows(TCP_TABLE_OWNER_PID_ALL, failure);
    if (!rows.has_value())
        return ShutdownProof::Invalid;

    const MIB_TCPROW_OWNER_PID *matching = nullptr;
    qsizetype count = 0;
    QList<RawConnectionRow> rawRows;
    rawRows.reserve(static_cast<qsizetype>(rows->table->dwNumEntries));
    for (DWORD index = 0; index < rows->table->dwNumEntries; ++index) {
        const MIB_TCPROW_OWNER_PID &row = rows->table->table[index];
        rawRows.append({row.dwState, row.dwLocalAddr, row.dwLocalPort,
                        row.dwRemoteAddr, row.dwRemotePort,
                        row.dwOwningPid});
        if (row.dwState != MIB_TCP_STATE_ESTAB
            || decodedPort(row.dwLocalPort) != serverPort
            || decodedPort(row.dwRemotePort) != clientPort) {
            continue;
        }
        ++count;
        matching = &row;
    }
    if (afterSnapshot)
        afterSnapshot();
    if (!processHandleIsLive(handle)) {
        failure = QStringLiteral("The captured child process HANDLE became signaled during shutdown proof");
        return ShutdownProof::Invalid;
    }
    if (proofMutation == 1 && count == 1)
        ++count;
    if (proofMutation == 2 && count == 1) {
        failure = QStringLiteral("The shutdown server four-tuple has conflicting ownership");
        return ShutdownProof::Invalid;
    }
    if (count == 0 || matching == nullptr) {
        failure = QStringLiteral(
            "The shutdown server four-tuple is not established");
        return ShutdownProof::NotPresent;
    }
    if (count != 1) {
        failure = QStringLiteral(
            "The shutdown server four-tuple is duplicated");
        return ShutdownProof::Invalid;
    }
    if (decodedAddress(matching->dwLocalAddr) != 0x7f000001U
        || decodedAddress(matching->dwRemoteAddr) != 0x7f000001U
        || matching->dwOwningPid != expectedPid) {
        failure = QStringLiteral("The shutdown server four-tuple has the wrong owner");
        return ShutdownProof::Invalid;
    }
    if (!shutdownRowsOwned(rawRows, serverPort, clientPort, expectedPid)) {
        failure = QStringLiteral("The shutdown server owner row is invalid");
        return ShutdownProof::Invalid;
    }
    return ShutdownProof::Owned;
}

class ApplicationReaper final : public QObject {
public:
    struct Item {
        QPointer<QObject> context;
        std::function<void()> beginStop;
    };

    void adopt(QObject *context, std::function<void()> beginStop)
    {
        if (context == nullptr)
            return;
        if (m_items.isEmpty()) {
            m_quitPending = false;
            m_repostedQuitEvent = nullptr;
            if (!m_filterInstalled && qApp != nullptr) {
                qApp->installEventFilter(this);
                m_filterInstalled = true;
            }
        }
        context->setParent(this);
        m_items.append({context, std::move(beginStop)});
        const std::function<void()> stop = m_items.constLast().beginStop;
        stop();
    }

    void finished(QObject *context)
    {
        for (qsizetype index = 0; index < m_items.size(); ++index) {
            if (m_items.at(index).context == context) {
                m_items.removeAt(index);
                break;
            }
        }
        if (context != nullptr) {
            context->setParent(nullptr);
            context->deleteLater();
        }
        if (!m_items.isEmpty())
            return;

        if (m_quitPending && m_repostedQuitEvent == nullptr
            && qApp != nullptr) {
            m_repostedQuitEvent = new QEvent(QEvent::Quit);
            QCoreApplication::postEvent(qApp, m_repostedQuitEvent);
            if (reaperRepostTestHook())
                reaperRepostTestHook()();
            return;
        }
        removeFilter();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != qApp || event->type() != QEvent::Quit)
            return QObject::eventFilter(watched, event);
        if (event == m_repostedQuitEvent) {
            m_repostedQuitEvent = nullptr;
            m_quitPending = false;
            removeFilter();
            return false;
        }
        if (!m_items.isEmpty() || m_quitPending) {
            m_quitPending = true;
            const QList<Item> snapshot = m_items;
            for (const Item &item : snapshot) {
                if (item.context)
                    item.beginStop();
            }
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void removeFilter()
    {
        if (m_filterInstalled && qApp != nullptr)
            qApp->removeEventFilter(this);
        m_filterInstalled = false;
    }

    QList<Item> m_items;
    bool m_filterInstalled = false;
    bool m_quitPending = false;
    QEvent *m_repostedQuitEvent = nullptr;
};

ApplicationReaper *applicationReaper()
{
    // Intentionally process-lifetime: forced exit relies on the OS closing the
    // Job Object instead of synchronously destroying QProcess on the UI thread.
    static ApplicationReaper *reaper = new ApplicationReaper;
    return reaper;
}

} // namespace

struct Xc2BackendManager::RunContext final : QObject {
    struct Attempt {
        explicit Attempt(RunContext *runContext)
            : run(runContext)
        {
        }

        ~Attempt()
        {
            delete shutdownSocket;
            delete restClient;
            delete process;
            if (processHandle != nullptr)
                CloseHandle(processHandle);
            if (jobHandle != nullptr)
                CloseHandle(jobHandle);
        }

        RunContext *run = nullptr;
        quint64 attemptId = 0;
        quint16 port = 0;
        QProcess *process = nullptr;
        Xc2RestClient *restClient = nullptr;
        QTcpSocket *shutdownSocket = nullptr;
        Xc2ProcessOutput output;
        Xc2RequestId healthRequestId = 0;
        Xc2RequestId userRequestId = 0;
        DWORD capturedPid = 0;
        HANDLE processHandle = nullptr;
        HANDLE jobHandle = nullptr;
        bool started = false;
        bool processStartOutcomeObserved = false;
        bool mayBecomeReady = true;
        bool networkAuthorized = false;
        bool ownsChildForCleanup = false;
        bool outputFinalized = false;
        bool cleanupCompleted = false;
        bool shutdownAttempted = false;
        bool shutdownWritten = false;
        int killAttempts = 0;
        int finalReapPolls = 0;
        int qtReconcileAttempts = 0;
        bool finalKillBoundaryReached = false;
        bool finalReapExhausted = false;
        bool qtReconcileScheduled = false;
        bool collisionCleanup = false;
        bool collisionExhausted = false;
        bool finishedObserved = false;
        int exitCode = -1;
        QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    };

    enum class CleanupPhase {
        None,
        ShutdownConnect,
        ShutdownProof,
        ShutdownWait,
        Terminating,
        Killing,
        Finalizing,
    };

    RunContext(Xc2BackendManager *manager, quint64 id,
               PrivateLaunchPlan launchPlan)
        : owner(manager), runId(id), plan(std::move(launchPlan))
    {
        totalDeadline.setSingleShot(true);
        totalDeadline.setTimerType(Qt::PreciseTimer);
        connect(&totalDeadline, &QTimer::timeout, this, [this] {
            if (completed)
                return;
            if (!startupDeadline.hasExpired()) {
                totalDeadline.start(static_cast<int>(qMax<qint64>(
                    1, startupDeadline.remainingTime())));
                return;
            }
            fail(backendError(QStringLiteral("XC2 startup deadline expired")));
        });
    }

    class CallbackScope final {
    public:
        explicit CallbackScope(RunContext *context)
            : run(context)
        {
            ++run->callbackDepth;
        }

        ~CallbackScope()
        {
            run->leaveCallback();
        }

    private:
        RunContext *run;
    };

    void leaveCallback()
    {
        Q_ASSERT(callbackDepth > 0);
        --callbackDepth;
        if (callbackDepth != 0 || !pendingCleanup || cleanupQueued)
            return;
        cleanupQueued = true;
        QTimer::singleShot(0, this, [this] {
            cleanupQueued = false;
            if (completed) {
                pendingCleanup = false;
                pendingFinish = false;
                return;
            }
            if (callbackDepth != 0)
                return;
            pendingCleanup = false;
            if (attempt) {
                finalizeAttemptWhenSafe(attempt->attemptId);
                return;
            }
            if (pendingFinish) {
                pendingFinish = false;
                finishRun(deferredExitCode, deferredExitStatus);
            }
        });
    }

    template <typename Callback>
    bool invokeManager(Callback callback)
    {
        QPointer<Xc2BackendManager> target = owner;
        if (!target)
            return false;
        CallbackScope scope(this);
        callback(target.data());
        return target && owner == target && !reaperOwned && !completed;
    }

    void observe(const QString &event) const
    {
        if (plan.lifecycleObserver)
            plan.lifecycleObserver(event);
    }

    void begin(qint64 elapsedBeforeRunMs)
    {
        const qint64 remaining = static_cast<qint64>(plan.startupDeadlineMs)
            - elapsedBeforeRunMs;
        if (remaining <= 0) {
            fail(backendError(QStringLiteral("XC2 startup deadline expired")));
            return;
        }
        totalDeadline.start(static_cast<int>(qMin<qint64>(
            remaining, std::numeric_limits<int>::max())));
        startupDeadline.setRemainingTime(remaining, Qt::PreciseTimer);

        const QFileInfo lockInfo(plan.lockPath);
        if (!QDir().mkpath(lockInfo.absolutePath())) {
            fail(backendError(QStringLiteral("Cannot create the XC2 lock directory")));
            return;
        }
        lock = std::make_unique<QLockFile>(plan.lockPath);
        lock->setStaleLockTime(0);
        if (!lock->tryLock(0)) {
            fail(backendError(QStringLiteral("The XC2 backend is already owned")));
            return;
        }
        startAttempt();
    }

    void beginFailure(Xc2Error error)
    {
        if (!plannedFailure.has_value())
            plannedFailure = std::move(error);
        if (plannedFailureScheduled)
            return;
        plannedFailureScheduled = true;
        QTimer::singleShot(0, this, [this] {
            if (plannedFailure.has_value())
                fail(plannedFailure.value());
        });
    }

    void requestStop()
    {
        if (completed || stopRequested)
            return;
        if (plannedFailure.has_value()) {
            beginFailure(plannedFailure.value());
            return;
        }
        if (failureEmitted)
            return;
        stopRequested = true;
        totalDeadline.stop();
        const bool wasAuthorized = attempt && attempt->networkAuthorized;
        revokeReadinessAndRequests();
        if (owner)
            invokeManager([](Xc2BackendManager *manager) {
                manager->setBackendState(Xc2BackendState::Stopping);
            });
        if (reaperOwned || !owner || completed)
            return;
        if (!attempt) {
            finishRun(-1, QProcess::CrashExit);
            return;
        }
        if (wasAuthorized)
            beginShutdown();
        else
            beginTermination();
    }

    void beginReaperStop()
    {
        if (completed)
            return;
        reaperOwned = true;
        owner.clear();
        if (plannedFailure.has_value()) {
            completed = true;
            applicationReaper()->finished(this);
            return;
        }
        stopRequested = true;
        totalDeadline.stop();
        revokeReadinessAndRequests();
        if (!attempt) {
            finishRun(-1, QProcess::CrashExit);
            return;
        }
        beginTermination();
    }

    void startAttempt()
    {
        if (completed || stopRequested)
            return;

        quint16 port = 0;
        for (int allocation = 0; allocation < 32; ++allocation) {
            const quint16 candidate = plan.candidateAllocator();
            observe(QStringLiteral("candidate:%1").arg(candidate));
            if (candidate == 0)
                break;
            if (candidate == kForbiddenVendorPort
                || attemptedPorts.contains(candidate)) {
                observe(QStringLiteral("candidate-rejected:%1")
                            .arg(candidate));
                continue;
            }
            port = candidate;
            break;
        }
        if (port == 0 || port == kForbiddenVendorPort) {
            fail(backendError(QStringLiteral("No valid XC2 loopback port was allocated")));
            return;
        }
        attemptedPorts.append(port);

        ++spawnedAttempts;
        finalReapDeadlineStarted = false;
        attempt = std::make_unique<Attempt>(this);
        attempt->attemptId = nextAttemptId++;
        attempt->port = port;
        const quint64 attemptId = attempt->attemptId;
        observe(QStringLiteral("attempt-start:%1:%2")
                    .arg(attemptId)
                    .arg(port));

        attempt->restClient = new Xc2RestClient(
            {qMin(plan.startupDeadlineMs, 1000),
             qMin(plan.startupDeadlineMs, 750)});
        const QByteArray base = canonicalRestBase(port);
        observe(QStringLiteral("rest-base:%1:%2")
                    .arg(attemptId).arg(port));
        Xc2Error baseError;
        if (!attempt->restClient->setBaseUrl(base, &baseError)) {
            fail(baseError);
            return;
        }

        const quint64 callbackRunId = runId;
        connect(attempt->restClient, &Xc2RestClient::serviceStatusFinished,
                this,
                [this, callbackRunId, attemptId,
                 client = attempt->restClient](
                    Xc2RequestId requestId,
                    const Xc2Result<Xc2ServiceStatus> &result) {
                    CallbackScope callback(this);
                    onHealthFinished(callbackRunId, attemptId, client,
                                     requestId, result);
                });
        connect(attempt->restClient, &Xc2RestClient::currentUserFinished,
                this,
                [this, callbackRunId, attemptId,
                 client = attempt->restClient](
                    Xc2RequestId requestId,
                    const Xc2Result<Xc2CurrentUser> &result) {
                    CallbackScope callback(this);
                    onCurrentUserFinished(callbackRunId, attemptId, client,
                                          requestId, result);
                });

        attempt->process = new QProcess;
        attempt->process->setProcessChannelMode(QProcess::SeparateChannels);
        attempt->process->setProgram(plan.program);
        const QStringList arguments = plan.argumentsBuilder(port);
        observe(QStringLiteral("arguments-built:%1:%2")
                    .arg(attemptId).arg(port));
        attempt->process->setArguments(arguments);
        attempt->process->setWorkingDirectory(plan.workingDirectory);
        attempt->process->setProcessEnvironment(plan.environment);

        connect(attempt->process, &QProcess::started, this,
                [this, attemptId] {
                    CallbackScope callback(this);
                    onProcessStarted(attemptId);
                });
        connect(attempt->process, &QProcess::errorOccurred, this,
                [this, attemptId](QProcess::ProcessError error) {
                    CallbackScope callback(this);
                    onProcessError(attemptId, error);
                });
        connect(attempt->process,
                qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                this, [this, attemptId](int exitCode,
                                        QProcess::ExitStatus exitStatus) {
                    const auto deliver = [this, attemptId, exitCode,
                                          exitStatus] {
                        CallbackScope callback(this);
                        onProcessFinished(attemptId, exitCode, exitStatus);
                    };
                    if (plan.finishedNotificationDelayMsForTest > 0) {
                        QTimer::singleShot(
                            plan.finishedNotificationDelayMsForTest,
                            this, deliver);
                    } else {
                        deliver();
                    }
                });
        connect(attempt->process, &QProcess::readyReadStandardOutput, this,
                [this, attemptId] {
                    CallbackScope callback(this);
                    drainProcessOutput(attemptId,
                                       Xc2ProcessOutput::Stream::StandardOutput);
                });
        connect(attempt->process, &QProcess::readyReadStandardError, this,
                [this, attemptId] {
                    CallbackScope callback(this);
                    drainProcessOutput(attemptId,
                                       Xc2ProcessOutput::Stream::StandardError);
                });
        attempt->process->start(QIODevice::ReadOnly);
    }

    static QByteArray canonicalRestBase(quint16 port)
    {
        QUrl url;
        url.setScheme(QStringLiteral("http"));
        url.setHost(QStringLiteral("127.0.0.1"));
        url.setPort(port);
        url.setPath(QStringLiteral("/xc2/1.0"));
        return url.toEncoded(QUrl::FullyEncoded);
    }

    bool isCurrentAttempt(quint64 attemptId) const
    {
        return attempt && attempt->attemptId == attemptId;
    }

    bool isCurrentAttempt(quint64 expectedRunId,
                          quint64 attemptId) const
    {
        return expectedRunId == runId && isCurrentAttempt(attemptId);
    }

    bool startupExpired(const QString &phase)
    {
        if (!startupDeadline.hasExpired())
            return false;
        observe(QStringLiteral("startup-deadline-expired:%1").arg(phase));
        fail(backendError(QStringLiteral("XC2 startup deadline expired")));
        return true;
    }

    void onProcessStarted(quint64 attemptId)
    {
        if (!isCurrentAttempt(attemptId) || completed)
            return;
        Attempt &current = *attempt;
        current.processStartOutcomeObserved = true;
        current.started = true;
        current.ownsChildForCleanup = true;
        current.capturedPid = static_cast<DWORD>(current.process->processId());
        if (current.capturedPid == 0) {
            fail(backendError(QStringLiteral("QProcess did not expose the child PID")));
            return;
        }
        if (owner)
            owner->publishOwnedProcess(current.capturedPid, true);
        observe(QStringLiteral("process-started:%1:%2")
                    .arg(attemptId)
                    .arg(current.capturedPid));

        constexpr DWORD rights = SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION
            | PROCESS_TERMINATE | PROCESS_SET_QUOTA;
        current.processHandle = OpenProcess(rights, FALSE,
                                            current.capturedPid);
        if (current.processHandle == nullptr) {
            fail(backendError(nativeFailure(QStringLiteral("OpenProcess"))));
            return;
        }
        const QString actualImage = processImagePath(current.processHandle);
        if (actualImage.isEmpty()
            || actualImage.compare(comparablePath(plan.program),
                                   Qt::CaseInsensitive) != 0) {
            fail(backendError(QStringLiteral("The captured PID is not the direct configured executable")));
            return;
        }
        observe(QStringLiteral("direct-executable-proven:%1")
                    .arg(attemptId));

        if (plan.failJobSetupForTest) {
            fail(backendError(QStringLiteral(
                "Injected XC2 Job Object setup failure")));
            return;
        }

        current.jobHandle = CreateJobObjectW(nullptr, nullptr);
        if (current.jobHandle == nullptr) {
            fail(backendError(nativeFailure(QStringLiteral("CreateJobObject"))));
            return;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(current.jobHandle,
                                     JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits))) {
            fail(backendError(nativeFailure(
                QStringLiteral("SetInformationJobObject"))));
            return;
        }
        if (!AssignProcessToJobObject(current.jobHandle,
                                      current.processHandle)) {
            fail(backendError(nativeFailure(
                QStringLiteral("AssignProcessToJobObject"))));
            return;
        }
        observe(QStringLiteral("job-assigned:%1").arg(attemptId));

        if (stopRequested) {
            beginTermination();
            return;
        }
        if (owner)
            invokeManager([](Xc2BackendManager *manager) {
                manager->setBackendState(Xc2BackendState::Probing);
            });
        if (reaperOwned || !owner || completed || stopRequested)
            return;
        requestHealth(attemptId);
    }

    void requestHealth(quint64 attemptId)
    {
        if (!isCurrentAttempt(attemptId) || completed || stopRequested)
            return;
        if (startupExpired(QStringLiteral("before-health-request")))
            return;
        Attempt &current = *attempt;
        if (!current.mayBecomeReady || current.healthRequestId != 0
            || current.userRequestId != 0 || !current.restClient)
            return;
        observe(QStringLiteral("health-request:%1:%2")
                    .arg(attemptId).arg(current.port));
        current.healthRequestId = current.restClient->requestServiceStatus();
        if (current.healthRequestId == 0) {
            fail(backendError(QStringLiteral("Cannot start the XC2 health request")));
        }
    }

    void onHealthFinished(quint64 expectedRunId, quint64 attemptId,
                          Xc2RestClient *client,
                          Xc2RequestId requestId,
                          const Xc2Result<Xc2ServiceStatus> &result)
    {
        if (!isCurrentAttempt(expectedRunId, attemptId) || completed)
            return;
        Attempt &current = *attempt;
        if (client != current.restClient
            || requestId == 0 || requestId != current.healthRequestId)
            return;
        current.healthRequestId = 0;
        if (!owner || owner->state() != Xc2BackendState::Probing
            || !current.mayBecomeReady || stopRequested)
            return;
        if (!result.ok() || !result.value->alive) {
            QTimer::singleShot(kHealthRetryMs, this,
                               [this, attemptId] { requestHealth(attemptId); });
            return;
        }
        if (startupExpired(QStringLiteral("before-listener-proof")))
            return;

        QString proofFailure;
        const ListenerProof proof = proveListener(
            current.port, current.capturedPid, current.processHandle,
            proofFailure);
        if (startupExpired(QStringLiteral("after-listener-proof")))
            return;
        if (proof == ListenerProof::ProvenCollision) {
            beginCollisionCleanup();
            return;
        }
        if (proof != ListenerProof::Owned) {
            fail(backendError(proofFailure));
            return;
        }

        observe(QStringLiteral("current-user-request:%1:%2")
                    .arg(attemptId).arg(current.port));
        current.userRequestId = current.restClient->requestCurrentUser();
        if (current.userRequestId == 0) {
            fail(backendError(QStringLiteral("Cannot start the XC2 current-user request")));
        }
    }

    void onCurrentUserFinished(quint64 expectedRunId, quint64 attemptId,
                               Xc2RestClient *client,
                               Xc2RequestId requestId,
                               const Xc2Result<Xc2CurrentUser> &result)
    {
        if (!isCurrentAttempt(expectedRunId, attemptId) || completed)
            return;
        Attempt &current = *attempt;
        if (client != current.restClient || requestId == 0
            || requestId != current.userRequestId)
            return;
        current.userRequestId = 0;
        if (!owner || owner->state() != Xc2BackendState::Probing
            || !current.mayBecomeReady || stopRequested)
            return;
        if (!result.ok()) {
            fail(result.error);
            return;
        }
        if (result.value->loginName.trimmed().isEmpty()
            || result.value->name.trimmed().isEmpty()) {
            fail(backendError(QStringLiteral("XC2 current user identity is blank")));
            return;
        }
        if (startupExpired(QStringLiteral("before-ready-proof")))
            return;

        QString proofFailure;
        const ListenerProof proof = proveListener(
            current.port, current.capturedPid, current.processHandle,
            proofFailure, plan.afterListenerSnapshotForTest);
        if (startupExpired(QStringLiteral("after-ready-proof")))
            return;
        if (proof == ListenerProof::ProvenCollision) {
            beginCollisionCleanup();
            return;
        }
        if (proof != ListenerProof::Owned) {
            fail(backendError(proofFailure));
            return;
        }
        if (startupExpired(QStringLiteral("before-ready")))
            return;
        if (!processHandleIsLive(current.processHandle)) {
            fail(backendError(QStringLiteral(
                "The captured child process HANDLE is signaled before Ready")));
            return;
        }

        totalDeadline.stop();
        current.networkAuthorized = true;
        observe(QStringLiteral("network-authorized:%1").arg(attemptId));
        Xc2BackendEndpoints endpoints;
        endpoints.restBaseUrl = QUrl::fromEncoded(
            canonicalRestBase(current.port), QUrl::StrictMode);
        endpoints.webSocketUrl.setScheme(QStringLiteral("ws"));
        endpoints.webSocketUrl.setHost(QStringLiteral("127.0.0.1"));
        endpoints.webSocketUrl.setPort(current.port);
        endpoints.webSocketUrl.setPath(QStringLiteral("/xc2-websocket"));
        if (startupExpired(QStringLiteral("publish-ready")))
            return;
        if (!processHandleIsLive(current.processHandle)) {
            fail(backendError(QStringLiteral(
                "The captured child process HANDLE is signaled at Ready publication")));
            return;
        }
        if (owner)
            invokeManager([this, &endpoints, &result](
                              Xc2BackendManager *manager) {
                manager->publishReady(this, endpoints, result.value.value());
            });
    }

    void onProcessError(quint64 attemptId, QProcess::ProcessError processError)
    {
        if (!isCurrentAttempt(attemptId) || completed)
            return;
        attempt->processStartOutcomeObserved = true;
        revokeReadinessAndRequests();

        if (attempt->collisionCleanup
            || (stopRequested && processError != QProcess::FailedToStart)) {
            QTimer::singleShot(0, this, [this, attemptId] {
                finalizeAttemptWhenSafe(attemptId);
            });
            return;
        }
        const QString message = processError == QProcess::FailedToStart
            ? QStringLiteral("The XC2 backend process failed to start")
            : QStringLiteral("The XC2 backend process reported an error: %1")
                  .arg(attempt->process->errorString());
        fail(backendError(message));
    }

    void onProcessFinished(quint64 attemptId, int exitCode,
                           QProcess::ExitStatus exitStatus)
    {
        if (!isCurrentAttempt(attemptId) || completed)
            return;
        Attempt &current = *attempt;
        current.processStartOutcomeObserved = true;
        current.finishedObserved = true;
        current.exitCode = exitCode;
        current.exitStatus = exitStatus;
        const bool unexpected = !current.collisionCleanup
            && !stopRequested && !failureEmitted;
        if (unexpected) {
            totalDeadline.stop();
            revokeReadinessAndRequests();
            markFailure(backendError(QStringLiteral(
                "The XC2 backend exited before stop")));
        }
        drainProcessOutput(attemptId,
                           Xc2ProcessOutput::Stream::StandardOutput);
        drainProcessOutput(attemptId,
                           Xc2ProcessOutput::Stream::StandardError);
        finalizeOutput(current);
        if (owner)
            owner->publishOwnedProcess(current.capturedPid, false);

        cleanupPhase = CleanupPhase::Finalizing;
        QTimer::singleShot(0, this, [this, attemptId] {
            finalizeAttemptWhenSafe(attemptId);
        });
    }

    void drainProcessOutput(quint64 attemptId,
                            Xc2ProcessOutput::Stream stream)
    {
        if (!isCurrentAttempt(attemptId) || !attempt->process
            || attempt->outputFinalized)
            return;
        QProcess *process = attempt->process;
        process->setReadChannel(stream == Xc2ProcessOutput::Stream::StandardOutput
                                    ? QProcess::StandardOutput
                                    : QProcess::StandardError);
        std::array<char, kProcessReadBlockBytes> buffer{};
        while (process->bytesAvailable() > 0) {
            const qint64 count = process->read(buffer.data(), buffer.size());
            if (count <= 0)
                break;
            const QStringList lines = attempt->output.feed(
                stream, QByteArrayView(buffer.data(), count));
            emitOutputLines(stream, lines);
        }
    }

    void finalizeOutput(Attempt &current)
    {
        if (current.outputFinalized)
            return;
        const Xc2ProcessOutput::FinishedLines lines = current.output.finish();
        current.outputFinalized = true;
        emitOutputLines(Xc2ProcessOutput::Stream::StandardOutput,
                        lines.standardOutput);
        emitOutputLines(Xc2ProcessOutput::Stream::StandardError,
                        lines.standardError);
    }

    void emitOutputLines(Xc2ProcessOutput::Stream stream,
                         const QStringList &lines)
    {
        const bool standardError =
            stream == Xc2ProcessOutput::Stream::StandardError;
        for (const QString &line : lines) {
            bool published = false;
            const bool stillOwned = invokeManager(
                [this, standardError, &line, &published](
                    Xc2BackendManager *manager) {
                    published = manager->publishOutputLine(
                        this, standardError, line);
                });
            if (!published || !stillOwned)
                return;
        }
    }

    void revokeReadinessAndRequests()
    {
        if (!attempt)
            return;
        Attempt &current = *attempt;
        current.mayBecomeReady = false;
        current.networkAuthorized = false;
        const Xc2RequestId healthId = current.healthRequestId;
        const Xc2RequestId userId = current.userRequestId;
        current.healthRequestId = 0;
        current.userRequestId = 0;
        if (owner)
            owner->clearNetworkPublication();
        if (current.restClient) {
            if (healthId != 0)
                current.restClient->abort(healthId);
            if (userId != 0)
                current.restClient->abort(userId);
        }
    }

    void beginCollisionCleanup()
    {
        if (!attempt || attempt->collisionCleanup)
            return;
        attempt->collisionCleanup = true;
        attempt->collisionExhausted = spawnedAttempts >= plan.maxPortAttempts;
        revokeReadinessAndRequests();
        beginTermination();
    }

    void beginShutdown()
    {
        if (!attempt || attempt->shutdownAttempted)
            return;
        Attempt &current = *attempt;
        current.shutdownAttempted = true;
        shutdownDeadline.setRemainingTime(plan.shutdownDeadlineMs,
                                          Qt::PreciseTimer);

        QUrl shutdownUrl = current.restClient->baseUrl();
        shutdownUrl.setPath(QStringLiteral("/xc2/1.0/serviceStatus/shutdown"));
        const Xc2Result<QByteArray> cookie =
            current.restClient->shutdownCookieHeaderFor(
                plan.invalidateShutdownCookieTargetForTest
                    ? shutdownUrl.toEncoded(QUrl::FullyEncoded)
                        + QByteArrayLiteral("#invalid")
                    : shutdownUrl.toEncoded(QUrl::FullyEncoded));
        if (!cookie.ok()) {
            shutdownFailed(cookie.error.message);
            return;
        }
        shutdownCookie = cookie.value.value();

        current.shutdownSocket = new QTcpSocket;
        current.shutdownSocket->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        const quint64 attemptId = current.attemptId;
        connect(current.shutdownSocket, &QTcpSocket::connected, this,
                [this, attemptId] {
                    CallbackScope callback(this);
                    if (!isCurrentAttempt(attemptId) || completed)
                        return;
                    cleanupPhase = CleanupPhase::ShutdownProof;
                    proveAndWriteShutdown(attemptId);
                });
        connect(current.shutdownSocket, &QTcpSocket::errorOccurred, this,
                [this, attemptId](QAbstractSocket::SocketError) {
                    CallbackScope callback(this);
                    if (!isCurrentAttempt(attemptId) || completed
                        || attempt->shutdownWritten)
                        return;
                    shutdownFailed(QStringLiteral("The XC2 shutdown socket failed"));
                });
        cleanupPhase = CleanupPhase::ShutdownConnect;
        current.shutdownSocket->connectToHost(
            QHostAddress(QStringLiteral("127.0.0.1")), current.port,
            QIODevice::ReadWrite);
        QTimer::singleShot(plan.shutdownDeadlineMs, this,
                           [this, attemptId] {
            if (!isCurrentAttempt(attemptId) || completed
                || cleanupPhase == CleanupPhase::Terminating
                || cleanupPhase == CleanupPhase::Killing
                || cleanupPhase == CleanupPhase::Finalizing)
                return;
            if (shutdownDeadline.hasExpired())
                shutdownFailed(QStringLiteral("The XC2 shutdown deadline expired"));
        });
    }

    bool shutdownExpired(const QString &phase)
    {
        if (!shutdownDeadline.hasExpired())
            return false;
        observe(QStringLiteral("shutdown-deadline-expired:%1").arg(phase));
        shutdownFailed(QStringLiteral("The XC2 shutdown deadline expired"));
        return true;
    }

    void proveAndWriteShutdown(quint64 attemptId)
    {
        if (!isCurrentAttempt(attemptId) || completed || !attempt->shutdownSocket)
            return;
        if (shutdownExpired(QStringLiteral("before-proof")))
            return;
        Attempt &current = *attempt;
        QString proofFailure;
        const ShutdownProof proof = proveShutdownTuple(
            current.port, current.shutdownSocket->localPort(),
            current.capturedPid, current.processHandle, proofFailure,
            plan.afterShutdownSnapshotForTest,
            static_cast<int>(plan.shutdownProofMutationForTest));
        if (shutdownExpired(QStringLiteral("after-proof")))
            return;
        if (proof != ShutdownProof::Owned) {
            if (proof == ShutdownProof::NotPresent
                && current.shutdownSocket->state()
                    == QAbstractSocket::ConnectedState) {
                QTimer::singleShot(kOwnershipRetryMs, this,
                                   [this, attemptId] {
                    proveAndWriteShutdown(attemptId);
                });
                return;
            }
            shutdownFailed(proofFailure);
            return;
        }
        QByteArray request = QByteArrayLiteral(
            "POST /xc2/1.0/serviceStatus/shutdown HTTP/1.1\r\nHost: 127.0.0.1:")
            + QByteArray::number(current.port)
            + QByteArrayLiteral(
                "\r\nContent-Type: application/x-www-form-urlencoded"
                "\r\nContent-Length: 0\r\nConnection: close\r\n");
        if (!shutdownCookie.isEmpty())
            request += QByteArrayLiteral("Cookie: ") + shutdownCookie + "\r\n";
        request += "\r\n";
        if (shutdownExpired(QStringLiteral("before-write")))
            return;
        if (!processHandleIsLive(current.processHandle)) {
            shutdownFailed(QStringLiteral(
                "The captured child process HANDLE is signaled before shutdown write"));
            return;
        }
        const qint64 accepted = current.shutdownSocket->write(request);
        if (accepted != request.size()) {
            current.shutdownSocket->abort();
            shutdownFailed(QStringLiteral("The XC2 shutdown write was partial"));
            return;
        }
        observe(QStringLiteral("shutdown-write:%1:%2")
                    .arg(attemptId).arg(current.port));
        current.shutdownWritten = true;
        cleanupPhase = CleanupPhase::ShutdownWait;
    }

    void shutdownFailed(const QString &message)
    {
        if (!attempt || completed)
            return;
        if (attempt->shutdownSocket)
            attempt->shutdownSocket->abort();
        if (!failureEmitted) {
            const Xc2Error error = backendError(
                message.isEmpty()
                    ? QStringLiteral("The XC2 shutdown proof failed")
                    : message);
            markFailure(error);
        }
        beginTermination();
    }

    void beginTermination()
    {
        if (!attempt) {
            finishRun(-1, QProcess::CrashExit);
            return;
        }
        Attempt &current = *attempt;
        if (current.shutdownSocket)
            current.shutdownSocket->abort();
        if (!current.process
            || current.process->state() == QProcess::NotRunning) {
            if (current.process && !current.started
                && !current.processStartOutcomeObserved) {
                const quint64 attemptId = current.attemptId;
                QTimer::singleShot(plan.terminateDeadlineMs, this,
                                   [this, attemptId] {
                    if (!isCurrentAttempt(attemptId) || completed)
                        return;
                    if (!attempt->processStartOutcomeObserved) {
                        attempt->processStartOutcomeObserved = true;
                        fail(backendError(QStringLiteral(
                            "The XC2 backend process failed before a start outcome")));
                        return;
                    }
                    beginTermination();
                });
                return;
            }
            if (!current.outputFinalized)
                finalizeOutput(current);
            QTimer::singleShot(0, this, [this, id = current.attemptId] {
                finalizeAttemptWhenSafe(id);
            });
            return;
        }
        if (cleanupPhase == CleanupPhase::Terminating
            || cleanupPhase == CleanupPhase::Killing
            || cleanupPhase == CleanupPhase::Finalizing)
            return;
        cleanupPhase = CleanupPhase::Terminating;
        current.process->terminate();
        const quint64 attemptId = current.attemptId;
        QTimer::singleShot(plan.terminateDeadlineMs, this,
                           [this, attemptId] {
            if (!isCurrentAttempt(attemptId) || completed)
                return;
            if (attempt->process->state() == QProcess::NotRunning) {
                finalizeAttemptWhenSafe(attemptId);
                return;
            }
            forceKill(attemptId);
        });
    }

    void forceKill(quint64 attemptId)
    {
        if (!isCurrentAttempt(attemptId) || completed || !attempt->process)
            return;
        if (attempt->process->state() == QProcess::NotRunning) {
            finalizeAttemptWhenSafe(attemptId);
            return;
        }
        if (attempt->finalKillBoundaryReached) {
            finalizeAttemptWhenSafe(attemptId);
            return;
        }
        cleanupPhase = CleanupPhase::Killing;
        ++attempt->killAttempts;
        bool processTerminationRequested = false;
        bool jobTerminationRequested = false;
        if (!plan.failNativeTerminationForTest) {
            attempt->process->kill();
            processTerminationRequested =
                !processHandleIsLive(attempt->processHandle)
                || TerminateProcess(attempt->processHandle, 1);
            jobTerminationRequested = attempt->jobHandle == nullptr
                || TerminateJobObject(attempt->jobHandle, 1);
        }
        observe(QStringLiteral("force-kill:%1:%2:%3:%4")
                    .arg(attemptId)
                    .arg(attempt->killAttempts)
                    .arg(processTerminationRequested)
                    .arg(jobTerminationRequested));
        if (attempt->killAttempts >= kMaximumKillAttempts) {
            reachFinalKillBoundary(attemptId);
            return;
        }
        QTimer::singleShot(plan.terminateDeadlineMs, this,
                           [this, attemptId] { forceKill(attemptId); });
    }

    void reachFinalKillBoundary(quint64 attemptId)
    {
        if (!isCurrentAttempt(attemptId) || completed
            || attempt->finalKillBoundaryReached) {
            return;
        }
        attempt->finalKillBoundaryReached = true;
        cleanupPhase = CleanupPhase::Finalizing;
        if (attempt->jobHandle != nullptr) {
            CloseHandle(attempt->jobHandle);
            attempt->jobHandle = nullptr;
        }
        observe(QStringLiteral("final-kill-boundary:%1:%2")
                    .arg(attemptId).arg(attempt->killAttempts));
        finalReapDeadline.setRemainingTime(
            qMax(500, plan.terminateDeadlineMs * 4), Qt::PreciseTimer);
        finalReapDeadlineStarted = true;
        QTimer::singleShot(kOwnershipRetryMs, this,
                           [this, attemptId] {
            finalizeAttemptWhenSafe(attemptId);
        });
    }

    void fail(const Xc2Error &error)
    {
        if (completed)
            return;
        totalDeadline.stop();
        revokeReadinessAndRequests();
        markFailure(error);
        if (!attempt) {
            QTimer::singleShot(0, this, [this] {
                finishRun(-1, QProcess::CrashExit);
            });
            return;
        }
        beginTermination();
    }

    void markFailure(const Xc2Error &error)
    {
        if (failureEmitted)
            return;
        failureEmitted = true;
        invokeManager([this, &error](Xc2BackendManager *manager) {
            manager->clearNetworkPublication();
            QPointer<Xc2BackendManager> guard(manager);
            manager->setBackendState(Xc2BackendState::Failed);
            if (guard)
                guard->publishFailure(this, error);
        });
    }

    bool reconcileFinishedAfterNativeSignal(Attempt &current)
    {
        if (!processHandleIsSignaled(current.processHandle))
            return false;
        if (!current.process)
            return processHandleIsSignaled(current.processHandle);
        if (current.process->state() == QProcess::NotRunning)
            return processHandleIsSignaled(current.processHandle);
        if (current.qtReconcileAttempts >= kMaximumQtReconcileAttempts)
            return false;
        ++current.qtReconcileAttempts;
        current.process->waitForFinished(0);
        const bool handleStillSignaled =
            processHandleIsSignaled(current.processHandle);
        const bool reconciled = handleStillSignaled
            && current.process->state() == QProcess::NotRunning;
        observe(QStringLiteral("qt-finished-reconcile:%1:%2:%3:%4")
                    .arg(current.attemptId)
                    .arg(current.qtReconcileAttempts)
                    .arg(handleStillSignaled)
                    .arg(reconciled));
        return reconciled;
    }

    void scheduleFinishedReconcile(quint64 attemptId)
    {
        if (!isCurrentAttempt(attemptId) || completed
            || attempt->qtReconcileScheduled) {
            return;
        }
        attempt->qtReconcileScheduled = true;
        QTimer::singleShot(kOwnershipRetryMs, this, [this, attemptId] {
            if (!isCurrentAttempt(attemptId) || completed)
                return;
            Attempt &current = *attempt;
            current.qtReconcileScheduled = false;
            if (!current.finalKillBoundaryReached
                || !processHandleIsSignaled(current.processHandle)) {
                current.finalReapExhausted = true;
            } else if (reconcileFinishedAfterNativeSignal(current)) {
                finalizeAttemptWhenSafe(attemptId);
                return;
            } else if (current.qtReconcileAttempts
                       < kMaximumQtReconcileAttempts) {
                scheduleFinishedReconcile(attemptId);
                return;
            } else {
                current.finalReapExhausted = true;
            }
            observe(QStringLiteral("qt-finished-reconcile-exhausted:%1:%2")
                        .arg(attemptId)
                        .arg(current.qtReconcileAttempts));
            if (!failureEmitted) {
                markFailure(backendError(QStringLiteral(
                    "QProcess could not reconcile the terminated XC2 child")));
            }
        });
    }

    void finalizeAttemptWhenSafe(quint64 attemptId)
    {
        if (!isCurrentAttempt(attemptId) || completed)
            return;
        if (callbackDepth != 0) {
            pendingCleanup = true;
            return;
        }
        Attempt &current = *attempt;
        if (current.process && current.process->state() != QProcess::NotRunning) {
            if (current.process->state() != QProcess::NotRunning) {
                if (current.finalKillBoundaryReached) {
                    ++current.finalReapPolls;
                    if (finalReapDeadlineStarted
                        && finalReapDeadline.hasExpired()) {
                        const bool liveHandle =
                            processHandleIsLive(current.processHandle);
                        if (!liveHandle
                            && processHandleIsSignaled(
                                current.processHandle)) {
                            scheduleFinishedReconcile(attemptId);
                            return;
                        }
                        current.finalReapExhausted = true;
                        observe(QStringLiteral(
                            "final-reap-qprocess-running:%1:%2:%3")
                                    .arg(attemptId)
                                    .arg(current.finalReapPolls)
                                    .arg(liveHandle));
                        if (!failureEmitted) {
                            markFailure(backendError(liveHandle
                                ? QStringLiteral(
                                    "XC2 final process reap could not prove termination")
                                : QStringLiteral(
                                    "QProcess remained Running after native XC2 termination")));
                        }
                        return;
                    }
                    QTimer::singleShot(kOwnershipRetryMs, this,
                                       [this, attemptId] {
                        finalizeAttemptWhenSafe(attemptId);
                    });
                    return;
                } else {
                    beginTermination();
                    return;
                }
            }
        }
        if (current.process
            && (current.process->state() == QProcess::NotRunning
                || processHandleIsSignaled(current.processHandle))
            && !current.outputFinalized) {
            drainProcessOutput(attemptId,
                               Xc2ProcessOutput::Stream::StandardOutput);
            drainProcessOutput(attemptId,
                               Xc2ProcessOutput::Stream::StandardError);
        }
        if (!current.outputFinalized)
            finalizeOutput(current);
        if (current.started && !current.finishedObserved) {
            if (!finalReapDeadlineStarted) {
                finalReapDeadline.setRemainingTime(
                    qMax(500, plan.terminateDeadlineMs * 4),
                    Qt::PreciseTimer);
                finalReapDeadlineStarted = true;
            }
            if (!finalReapDeadline.hasExpired()) {
                QTimer::singleShot(kOwnershipRetryMs, this,
                                   [this, attemptId] {
                    finalizeAttemptWhenSafe(attemptId);
                });
                return;
            }
            if (processHandleIsSignaled(current.processHandle)) {
                current.finishedObserved = true;
                current.exitCode = current.process
                    ? current.process->exitCode() : -1;
                current.exitStatus = current.process
                    ? current.process->exitStatus() : QProcess::CrashExit;
                observe(QStringLiteral("finished-notifier-lag-bypassed:%1")
                            .arg(attemptId));
            } else if (!current.finalKillBoundaryReached) {
                reachFinalKillBoundary(attemptId);
                return;
            } else {
                current.finalReapExhausted = true;
                observe(QStringLiteral("final-reap-live-handle:%1")
                            .arg(attemptId));
                if (!failureEmitted) {
                    markFailure(backendError(QStringLiteral(
                        "XC2 final process reap could not prove termination")));
                }
                return;
            }
        }
        if (current.processHandle != nullptr
            && !processHandleIsSignaled(current.processHandle)) {
            if (!finalReapDeadlineStarted) {
                finalReapDeadline.setRemainingTime(
                    qMax(500, plan.terminateDeadlineMs * 4),
                    Qt::PreciseTimer);
                finalReapDeadlineStarted = true;
            }
            if (finalReapDeadline.hasExpired()) {
                if (!current.finalKillBoundaryReached) {
                    reachFinalKillBoundary(attemptId);
                    return;
                }
                current.finalReapExhausted = true;
                observe(QStringLiteral("final-reap-live-handle:%1")
                            .arg(attemptId));
                if (!failureEmitted) {
                    markFailure(backendError(QStringLiteral(
                        "XC2 final process reap could not prove termination")));
                }
                return;
            }
            QTimer::singleShot(kOwnershipRetryMs, this,
                               [this, attemptId] {
                finalizeAttemptWhenSafe(attemptId);
            });
            return;
        }

        current.cleanupCompleted = true;
        observe(QStringLiteral("attempt-cleanup-complete:%1")
                    .arg(attemptId));
        const bool collision = current.collisionCleanup;
        const bool exhausted = current.collisionExhausted;
        const int exitCode = current.exitCode;
        const QProcess::ExitStatus exitStatus = current.exitStatus;
        if (current.processHandle != nullptr) {
            CloseHandle(current.processHandle);
            current.processHandle = nullptr;
        }
        if (current.jobHandle != nullptr) {
            CloseHandle(current.jobHandle);
            current.jobHandle = nullptr;
        }
        attempt.reset();
        cleanupPhase = CleanupPhase::None;

        if (collision && !stopRequested && !failureEmitted) {
            if (exhausted) {
                fail(backendError(QStringLiteral(
                    "XC2 listener ownership collisions exhausted all attempts")));
            } else {
                startAttempt();
            }
            return;
        }
        finishRun(exitCode, exitStatus);
    }

    void finishRun(int exitCode, QProcess::ExitStatus exitStatus)
    {
        if (completed)
            return;
        if (attempt) {
            finalizeAttemptWhenSafe(attempt->attemptId);
            return;
        }
        if (callbackDepth != 0) {
            pendingCleanup = true;
            pendingFinish = true;
            deferredExitCode = exitCode;
            deferredExitStatus = exitStatus;
            return;
        }
        completed = true;
        totalDeadline.stop();
        if (lock) {
            lock->unlock();
            lock.reset();
        }
        if (owner) {
            invokeManager([this, exitCode, exitStatus](
                              Xc2BackendManager *manager) {
                manager->publishStopped(this, exitCode, exitStatus);
            });
            owner.clear();
            deleteLater();
            return;
        }
        if (reaperOwned) {
            applicationReaper()->finished(this);
            return;
        }
        deleteLater();
    }

    QPointer<Xc2BackendManager> owner;
    const quint64 runId;
    PrivateLaunchPlan plan;
    std::unique_ptr<QLockFile> lock;
    std::unique_ptr<Attempt> attempt;
    QTimer totalDeadline;
    QDeadlineTimer startupDeadline;
    QDeadlineTimer shutdownDeadline;
    QDeadlineTimer finalReapDeadline;
    QByteArray shutdownCookie;
    QList<quint16> attemptedPorts;
    quint64 nextAttemptId = 1;
    int spawnedAttempts = 0;
    CleanupPhase cleanupPhase = CleanupPhase::None;
    bool stopRequested = false;
    bool failureEmitted = false;
    std::optional<Xc2Error> plannedFailure;
    bool plannedFailureScheduled = false;
    bool completed = false;
    bool reaperOwned = false;
    int callbackDepth = 0;
    bool pendingCleanup = false;
    bool cleanupQueued = false;
    bool pendingFinish = false;
    bool finalReapDeadlineStarted = false;
    int deferredExitCode = -1;
    QProcess::ExitStatus deferredExitStatus = QProcess::CrashExit;
};

Xc2BackendManager::Xc2BackendManager(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<Xc2BackendState>();
    qRegisterMetaType<Xc2BackendEndpoints>();
    qRegisterMetaType<Xc2Error>();
    qRegisterMetaType<QProcess::ExitStatus>();
}

Xc2BackendManager::~Xc2BackendManager()
{
    if (!m_run)
        return;
    RunContext *run = m_run;
    m_run = nullptr;
    run->owner.clear();
    applicationReaper()->adopt(run, [run] { run->beginReaperStop(); });
}

bool Xc2BackendManager::startProduction(const QString &installRoot,
                                        Xc2Error *error)
{
    if (m_state != Xc2BackendState::Stopped) {
        if (error)
            *error = backendError(QStringLiteral("An XC2 backend run is already active"));
        return false;
    }

    if (error)
        *error = {};
    m_recentStandardOutput.clear();
    m_recentStandardError.clear();
    clearNetworkPublication();
    QElapsedTimer startupClock;
    startupClock.start();
    const ProductionInspectionTestHooks inspectionHooks =
        productionInspectionTestHooks();
    if (inspectionHooks.observer)
        inspectionHooks.observer(QStringLiteral("deadline-start"));

    const QString cleanedRoot = QDir::cleanPath(
        QDir::fromNativeSeparators(installRoot));
    if (installRoot.trimmed().isEmpty()
        || !QDir::isAbsolutePath(cleanedRoot)) {
        return startAcceptedFailure(
            prerequisiteError(QStringLiteral("The XC2 install root must be an absolute path")),
            startupClock.elapsed(), error);
    }

    if (inspectionHooks.observer)
        inspectionHooks.observer(QStringLiteral("inspect-start"));
    if (inspectionHooks.delayMs > 0)
        QThread::msleep(static_cast<unsigned long>(inspectionHooks.delayMs));
    const Xc2PrerequisiteReport report =
        Xc2InstallationProbe::inspectProduction(cleanedRoot);
    if (startupClock.elapsed() >= 15000) {
        if (inspectionHooks.observer)
            inspectionHooks.observer(QStringLiteral("inspect-fail:deadline"));
        return startAcceptedFailure(
            backendError(QStringLiteral("XC2 startup deadline expired during inspection")),
            startupClock.elapsed(), error);
    }
    if (!report.ok()) {
        if (inspectionHooks.observer)
            inspectionHooks.observer(QStringLiteral("inspect-fail:prerequisite"));
        const QString message = report.issues.isEmpty()
            ? QStringLiteral("XC2 production inspection failed")
            : report.issues.constFirst().message;
        return startAcceptedFailure(prerequisiteError(message),
                                    startupClock.elapsed(), error);
    }

    const Xc2InstallLayout layout = report.layout;
    const QString appData = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    if (appData.isEmpty()) {
        return startAcceptedFailure(
            backendError(QStringLiteral("The per-user application data path is unavailable")),
            startupClock.elapsed(), error);
    }

    PrivateLaunchPlan plan;
    plan.program = productionProgram(layout);
    plan.workingDirectory = productionWorkingDirectory(layout);
    plan.environment = sanitizedEnvironment();
    plan.bindAddress = QHostAddress::LocalHost;
    plan.lockPath = productionLockPath(appData);
    plan.startupDeadlineMs = 15000;
    plan.shutdownDeadlineMs = 1500;
    plan.terminateDeadlineMs = 500;
    plan.maxPortAttempts = 3;
    plan.lifecycleObserver = inspectionHooks.observer;
    plan.candidateAllocator = [] { return allocateLoopbackCandidate(); };
    plan.argumentsBuilder = [layout](quint16 port) {
        return productionArguments(layout, port);
    };
    return startAcceptedPlan(std::move(plan), startupClock.elapsed(), error);
}

bool Xc2BackendManager::startPrivateForTest(PrivateLaunchPlan plan,
                                            Xc2Error *error)
{
    if (m_state != Xc2BackendState::Stopped) {
        if (error)
            *error = backendError(QStringLiteral("An XC2 backend run is already active"));
        return false;
    }
    if (error)
        *error = {};
    m_recentStandardOutput.clear();
    m_recentStandardError.clear();
    clearNetworkPublication();
    return startAcceptedPlan(std::move(plan), 0, error);
}

bool Xc2BackendManager::startAcceptedPlan(PrivateLaunchPlan plan,
                                          qint64 elapsedBeforeRunMs,
                                          Xc2Error *error)
{
    QElapsedTimer validationClock;
    validationClock.start();
    QString validationFailure;
    if (plan.program.isEmpty() || !QDir::isAbsolutePath(plan.program)
        || !QFileInfo(plan.program).isFile()) {
        validationFailure = QStringLiteral("The direct XC2 executable must be an existing absolute file");
    } else if (plan.workingDirectory.isEmpty()
               || !QDir::isAbsolutePath(plan.workingDirectory)
               || !QFileInfo(plan.workingDirectory).isDir()) {
        validationFailure = QStringLiteral("The XC2 working directory must be an existing absolute directory");
    } else if (plan.lockPath.isEmpty()
               || !QDir::isAbsolutePath(plan.lockPath)) {
        validationFailure = QStringLiteral("The XC2 lock path must be absolute");
    } else if (!acceptsBindAddress(plan.bindAddress)) {
        validationFailure = QStringLiteral("XC2 may bind only exact IPv4 localhost");
    } else if (plan.startupDeadlineMs <= 0
               || plan.startupDeadlineMs > kMaximumTimeoutMs
               || plan.shutdownDeadlineMs <= 0
               || plan.shutdownDeadlineMs > kMaximumTimeoutMs
               || plan.terminateDeadlineMs <= 0
               || plan.terminateDeadlineMs > kMaximumTimeoutMs) {
        validationFailure = QStringLiteral("XC2 lifecycle deadlines are out of bounds");
    } else if (plan.maxPortAttempts < 1 || plan.maxPortAttempts > 3) {
        validationFailure = QStringLiteral("XC2 port attempts must be between one and three");
    } else if (!plan.argumentsBuilder || !plan.candidateAllocator) {
        validationFailure = QStringLiteral("XC2 private launch callbacks are required");
    }
    if (!validationFailure.isEmpty()) {
        return startAcceptedFailure(backendError(validationFailure),
                                    elapsedBeforeRunMs, error);
    }

    RunContext *run = new RunContext(this, m_nextRunId++, std::move(plan));
    m_run = run;
    QPointer<Xc2BackendManager> guard(this);
    setBackendState(Xc2BackendState::Starting);
    if (!guard || m_run != run)
        return true;
    run->begin(elapsedBeforeRunMs + validationClock.elapsed());
    return true;
}

bool Xc2BackendManager::startAcceptedFailure(const Xc2Error &failure,
                                             qint64 elapsedBeforeRunMs,
                                             Xc2Error *error)
{
    Q_UNUSED(elapsedBeforeRunMs);
    if (error)
        *error = {};
    PrivateLaunchPlan plan;
    RunContext *run = new RunContext(this, m_nextRunId++, std::move(plan));
    run->beginFailure(failure);
    m_run = run;
    QPointer<Xc2BackendManager> guard(this);
    setBackendState(Xc2BackendState::Starting);
    if (!guard || m_run != run)
        return true;
    return true;
}

void Xc2BackendManager::stop()
{
    if (m_run)
        m_run->requestStop();
}

Xc2BackendState Xc2BackendManager::state() const
{
    return m_state;
}

bool Xc2BackendManager::ownsProcess() const
{
    return m_ownsProcess;
}

qint64 Xc2BackendManager::ownedProcessId() const
{
    return m_ownedProcessId;
}

Xc2BackendEndpoints Xc2BackendManager::endpoints() const
{
    return m_endpoints;
}

std::optional<Xc2CurrentUser> Xc2BackendManager::currentUser() const
{
    return m_currentUser;
}

QStringList Xc2BackendManager::recentOutput(bool standardError) const
{
    return standardError ? m_recentStandardError : m_recentStandardOutput;
}

QStringList Xc2BackendManager::productionArguments(
    const Xc2InstallLayout &layout, quint16 port)
{
    const QString authority = QStringLiteral("http://127.0.0.1:%1")
        .arg(port);
    return {
        QStringLiteral("-Dssc.includezip=true"),
        QStringLiteral("-Dlogging.config=config/log.xml"),
        QStringLiteral("-Dspring.config.additional-location=file:./config/custom-cloud.properties"),
        QStringLiteral("-Dserver.address=127.0.0.1"),
        QStringLiteral("-Dserver.port=%1").arg(port),
        QStringLiteral("-Donelogin.saml2.sp.assertion_consumer_service.url=%1/xc2/1.0/auth/samlACS")
            .arg(authority),
        QStringLiteral("-Donelogin.saml2.sp.single_logout_service.url=%1/xc2/1.0/auth/samlLogoutSLO")
            .arg(authority),
        QStringLiteral("-Donelogin.saml2.idp.single_sign_on_service.url=%1/xc2/1.0/auth/mock")
            .arg(authority),
        QStringLiteral("-Donelogin.saml2.idp.single_logout_service.url=%1/xc2/1.0/auth/mock")
            .arg(authority),
        QStringLiteral("-Dcom.avl.ditest.xc2.gripsresource.prefix=%1/xc2/1.0/mock/streamer?file={0}")
            .arg(authority),
        QStringLiteral("-Dloader.main=com.avl.ditest.xc2.Xc2NgApplication"),
        QStringLiteral("-Dspring.profiles.active=dev"),
        QStringLiteral("-Dcom.avl.ditest.xc2.developer=true"),
        QStringLiteral("-jar"),
        layout.backendJar,
    };
}

QString Xc2BackendManager::productionProgram(const Xc2InstallLayout &layout)
{
    return layout.javaExe;
}

QString Xc2BackendManager::productionWorkingDirectory(
    const Xc2InstallLayout &layout)
{
    return layout.root;
}

QString Xc2BackendManager::productionLockPath(const QString &appDataPath)
{
    if (appDataPath.isEmpty())
        return {};
    return QDir::cleanPath(QDir(appDataPath).filePath(
        QStringLiteral("ktm-xc2-vci2k.lock")));
}

QProcessEnvironment Xc2BackendManager::sanitizedEnvironment()
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    static const QStringList denied{
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
    const QStringList keys = environment.keys();
    for (const QString &key : keys) {
        const bool remove = std::any_of(
            denied.cbegin(), denied.cend(), [&key](const QString &candidate) {
                return key.compare(candidate, Qt::CaseInsensitive) == 0;
            });
        if (remove)
            environment.remove(key);
    }
    return environment;
}

bool Xc2BackendManager::acceptsBindAddress(const QHostAddress &address)
{
    return address.protocol() == QAbstractSocket::IPv4Protocol
        && address.toIPv4Address() == 0x7f000001U;
}

quint16 Xc2BackendManager::allocateLoopbackCandidate()
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        QTcpServer reservation;
        if (!reservation.listen(QHostAddress::LocalHost, 0))
            return 0;
        const quint16 port = reservation.serverPort();
        reservation.close();
        if (port != 0 && port != kForbiddenVendorPort)
            return port;
    }
    return 0;
}

bool Xc2BackendManager::listenerRowsOwnedForTest(
    const QList<RawTcpOwnerRow> &rows, quint16 port, quint32 processId)
{
    QList<RawOwnerRow> nativeRows;
    nativeRows.reserve(rows.size());
    for (const RawTcpOwnerRow &row : rows) {
        nativeRows.append(
            {row.localAddress, row.localPort, row.owningPid});
    }
    return listenerRowsOwned(nativeRows, port, processId);
}

bool Xc2BackendManager::shutdownRowsOwnedForTest(
    const QList<RawTcpConnectionRow> &rows, quint16 serverPort,
    quint16 clientPort, quint32 processId)
{
    QList<RawConnectionRow> nativeRows;
    nativeRows.reserve(rows.size());
    for (const RawTcpConnectionRow &row : rows) {
        nativeRows.append({row.state, row.localAddress, row.localPort,
                           row.remoteAddress, row.remotePort,
                           row.owningPid});
    }
    return shutdownRowsOwned(nativeRows, serverPort, clientPort, processId);
}

void Xc2BackendManager::setProductionInspectionTestHooks(
    std::function<void(const QString &)> observer, int delayMs)
{
    ProductionInspectionTestHooks &hooks = productionInspectionTestHooks();
    hooks.observer = std::move(observer);
    hooks.delayMs = qMax(0, delayMs);
}

void Xc2BackendManager::resetProductionInspectionTestHooks()
{
    productionInspectionTestHooks() = {};
}

void Xc2BackendManager::setReaperRepostTestHook(std::function<void()> hook)
{
    reaperRepostTestHook() = std::move(hook);
}

void Xc2BackendManager::setBackendState(Xc2BackendState state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

void Xc2BackendManager::publishOwnedProcess(qint64 processId, bool owned)
{
    m_ownsProcess = owned;
    m_ownedProcessId = owned ? processId : 0;
}

void Xc2BackendManager::publishReady(RunContext *run,
                                     const Xc2BackendEndpoints &endpoints,
                                     const Xc2CurrentUser &user)
{
    if (run != m_run)
        return;
    m_endpoints = endpoints;
    m_currentUser = user;
    QPointer<Xc2BackendManager> guard(this);
    setBackendState(Xc2BackendState::Ready);
    if (!guard || m_state != Xc2BackendState::Ready || m_run != run)
        return;
    emit ready(endpoints);
}

void Xc2BackendManager::clearNetworkPublication()
{
    m_endpoints = {};
    m_currentUser.reset();
}

bool Xc2BackendManager::publishOutputLine(RunContext *run,
                                          bool standardError,
                                          const QString &line)
{
    if (run != m_run)
        return false;
    QStringList &ring = standardError
        ? m_recentStandardError : m_recentStandardOutput;
    ring.append(line);
    while (ring.size() > Xc2ProcessOutput::MaximumRecentLines)
        ring.removeFirst();
    QPointer<Xc2BackendManager> guard(this);
    emit outputLine(standardError, line);
    return guard && m_run == run;
}

void Xc2BackendManager::publishRecentOutput(
    const QStringList &standardOutput, const QStringList &standardError)
{
    m_recentStandardOutput = standardOutput;
    m_recentStandardError = standardError;
}

void Xc2BackendManager::publishFailure(RunContext *run,
                                       const Xc2Error &error)
{
    if (run != m_run)
        return;
    emit failed(error);
}

void Xc2BackendManager::publishStopped(RunContext *run, int exitCode,
                                       QProcess::ExitStatus exitStatus)
{
    if (run != m_run)
        return;
    m_run = nullptr;
    m_ownsProcess = false;
    m_ownedProcessId = 0;
    clearNetworkPublication();
    QPointer<Xc2BackendManager> guard(this);
    emit stopped(exitCode, exitStatus);
    if (!guard)
        return;
    setBackendState(Xc2BackendState::Stopped);
}

} // namespace ktm::xc2
