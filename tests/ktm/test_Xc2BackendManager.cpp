#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include "ktm/xc2/Xc2BackendManager.h"
#include "ktm/xc2/Xc2InstallationProbe.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPointer>
#include <QSet>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef FAKE_XC2_SIDECAR_PATH
#error FAKE_XC2_SIDECAR_PATH must be supplied by CMake
#endif

namespace ktm::xc2 {

class Xc2BackendManagerTestAccess final {
public:
    using RawTcpOwnerRow = Xc2BackendManager::RawTcpOwnerRow;
    using RawTcpConnectionRow = Xc2BackendManager::RawTcpConnectionRow;
    using ShutdownProofMutation =
        Xc2BackendManager::PrivateLaunchPlan::ShutdownProofMutation;

    enum class InvalidPlan {
        RelativeProgram,
        MissingProgram,
        RelativeWorkingDirectory,
        MissingWorkingDirectory,
        RelativeLock,
        NonLocalBind,
        ZeroStartupDeadline,
        ExcessiveShutdownDeadline,
        ZeroTerminateDeadline,
        ZeroPortAttempts,
        ExcessivePortAttempts,
        MissingArgumentsBuilder,
        MissingCandidateAllocator,
    };

    static QStringList productionArguments(const Xc2InstallLayout &layout,
                                           quint16 port)
    {
        return Xc2BackendManager::productionArguments(layout, port);
    }

    static QProcessEnvironment sanitizedEnvironment()
    {
        return Xc2BackendManager::sanitizedEnvironment();
    }

    static QString productionProgram(const Xc2InstallLayout &layout)
    {
        return Xc2BackendManager::productionProgram(layout);
    }

    static QString productionWorkingDirectory(
        const Xc2InstallLayout &layout)
    {
        return Xc2BackendManager::productionWorkingDirectory(layout);
    }

    static QString productionLockPath(const QString &appDataPath)
    {
        return Xc2BackendManager::productionLockPath(appDataPath);
    }

    static bool acceptsBindAddress(const QHostAddress &address)
    {
        return Xc2BackendManager::acceptsBindAddress(address);
    }

    static bool startFake(Xc2BackendManager &manager,
                          const QString &lockPath,
                          const QList<quint16> &candidates,
                          const QStringList &extraArguments,
                          const QString &eventLog,
                          int startupDeadlineMs = 6000,
                          int shutdownDeadlineMs = 1000,
                          int terminateDeadlineMs = 200,
                          int maxPortAttempts = 3,
                          std::shared_ptr<int> argumentCalls = {},
                          std::shared_ptr<int> candidateCalls = {},
                          Xc2Error *error = nullptr,
                          std::shared_ptr<QStringList> lifecycleTrace = {},
                          bool invalidateShutdownCookieTarget = false,
                          bool failJobSetup = false,
                          std::function<void()> afterListenerSnapshot = {},
                          std::function<void()> afterShutdownSnapshot = {},
                          bool failNativeTermination = false,
                          int finishedNotificationDelayMs = 0,
                          ShutdownProofMutation shutdownProofMutation =
                              ShutdownProofMutation::None,
                          int errorNotificationDelayMs = 0,
                          std::function<void(std::function<void()>)>
                              shutdownDeadlineEarlyWakeupHook = {},
                          std::function<void(const QString &)>
                              lifecycleObserver = {},
                          std::function<bool()>
                              outputFinalizationAllowed = {})
    {
        Xc2BackendManager::PrivateLaunchPlan plan;
        plan.program = QString::fromUtf8(FAKE_XC2_SIDECAR_PATH);
        plan.workingDirectory = QFileInfo(plan.program).absolutePath();
        plan.environment = Xc2BackendManager::sanitizedEnvironment();
        plan.bindAddress = QHostAddress::LocalHost;
        plan.lockPath = lockPath;
        plan.startupDeadlineMs = startupDeadlineMs;
        plan.shutdownDeadlineMs = shutdownDeadlineMs;
        plan.terminateDeadlineMs = terminateDeadlineMs;
        plan.maxPortAttempts = maxPortAttempts;
        plan.invalidateShutdownCookieTargetForTest =
            invalidateShutdownCookieTarget;
        plan.failJobSetupForTest = failJobSetup;
        plan.afterListenerSnapshotForTest =
            std::move(afterListenerSnapshot);
        plan.afterShutdownSnapshotForTest =
            std::move(afterShutdownSnapshot);
        plan.failNativeTerminationForTest = failNativeTermination;
        plan.finishedNotificationDelayMsForTest =
            finishedNotificationDelayMs;
        plan.shutdownProofMutationForTest = shutdownProofMutation;
        plan.errorNotificationDelayMsForTest = errorNotificationDelayMs;
        plan.shutdownDeadlineEarlyWakeupHookForTest =
            std::move(shutdownDeadlineEarlyWakeupHook);
        plan.outputFinalizationAllowedForTest =
            std::move(outputFinalizationAllowed);
        if (lifecycleTrace || lifecycleObserver) {
            plan.lifecycleObserver = [lifecycleTrace,
                                      lifecycleObserver =
                                          std::move(lifecycleObserver)](
                                         const QString &event) {
                if (lifecycleTrace)
                    lifecycleTrace->append(event);
                if (lifecycleObserver)
                    lifecycleObserver(event);
            };
        }
        auto index = std::make_shared<int>(0);
        plan.candidateAllocator = [candidates, index, candidateCalls] {
            if (candidateCalls)
                ++*candidateCalls;
            if (*index >= candidates.size())
                return quint16(0);
            return candidates.at((*index)++);
        };
        plan.argumentsBuilder = [extraArguments, eventLog,
                                 argumentCalls](quint16 port) {
            if (argumentCalls)
                ++*argumentCalls;
            QStringList arguments{
                QStringLiteral("--port"), QString::number(port),
                QStringLiteral("--event-log"), eventLog,
            };
            arguments.append(extraArguments);
            return arguments;
        };
        return manager.startPrivateForTest(std::move(plan), error);
    }

    static bool startStagedOutputFake(
        Xc2BackendManager &manager, const QString &lockPath,
        quint16 candidate, const QString &stagedOutputDirectory,
        const QString &eventLog,
        std::shared_ptr<QStringList> lifecycleTrace,
        std::function<void(const QString &)> lifecycleObserver,
        std::function<bool()> outputFinalizationAllowed = {})
    {
        return startFake(
            manager, lockPath, {candidate},
            {QStringLiteral("--staged-output-dir"),
             stagedOutputDirectory},
            eventLog, 5000, 500, 100, 1, {}, {}, nullptr,
            std::move(lifecycleTrace), false, false, {}, {}, false, 0,
            ShutdownProofMutation::None, 0, {},
            std::move(lifecycleObserver),
            std::move(outputFinalizationAllowed));
    }

    static bool startInvalid(Xc2BackendManager &manager,
                             InvalidPlan invalid,
                             const QString &absoluteLockPath,
                             std::shared_ptr<int> argumentCalls,
                             std::shared_ptr<int> candidateCalls,
                             Xc2Error *error = nullptr)
    {
        Xc2BackendManager::PrivateLaunchPlan plan;
        plan.program = QString::fromUtf8(FAKE_XC2_SIDECAR_PATH);
        plan.workingDirectory = QFileInfo(plan.program).absolutePath();
        plan.environment = Xc2BackendManager::sanitizedEnvironment();
        plan.bindAddress = QHostAddress::LocalHost;
        plan.lockPath = absoluteLockPath;
        plan.startupDeadlineMs = 5000;
        plan.shutdownDeadlineMs = 500;
        plan.terminateDeadlineMs = 100;
        plan.maxPortAttempts = 1;
        plan.argumentsBuilder = [argumentCalls](quint16 port) {
            ++*argumentCalls;
            return QStringList{QStringLiteral("--port"),
                               QString::number(port)};
        };
        plan.candidateAllocator = [candidateCalls] {
            ++*candidateCalls;
            return quint16(49123);
        };

        switch (invalid) {
        case InvalidPlan::RelativeProgram:
            plan.program = QStringLiteral("fake_xc2_sidecar.exe");
            break;
        case InvalidPlan::MissingProgram:
            plan.program = QFileInfo(absoluteLockPath).absoluteDir()
                               .filePath(QStringLiteral("missing.exe"));
            break;
        case InvalidPlan::RelativeWorkingDirectory:
            plan.workingDirectory = QStringLiteral("relative");
            break;
        case InvalidPlan::MissingWorkingDirectory:
            plan.workingDirectory = QFileInfo(absoluteLockPath).absoluteDir()
                                        .filePath(QStringLiteral("missing-dir"));
            break;
        case InvalidPlan::RelativeLock:
            plan.lockPath = QStringLiteral("relative.lock");
            break;
        case InvalidPlan::NonLocalBind:
            plan.bindAddress = QHostAddress::AnyIPv4;
            break;
        case InvalidPlan::ZeroStartupDeadline:
            plan.startupDeadlineMs = 0;
            break;
        case InvalidPlan::ExcessiveShutdownDeadline:
            plan.shutdownDeadlineMs = 120001;
            break;
        case InvalidPlan::ZeroTerminateDeadline:
            plan.terminateDeadlineMs = 0;
            break;
        case InvalidPlan::ZeroPortAttempts:
            plan.maxPortAttempts = 0;
            break;
        case InvalidPlan::ExcessivePortAttempts:
            plan.maxPortAttempts = 4;
            break;
        case InvalidPlan::MissingArgumentsBuilder:
            plan.argumentsBuilder = {};
            break;
        case InvalidPlan::MissingCandidateAllocator:
            plan.candidateAllocator = {};
            break;
        }
        return manager.startPrivateForTest(std::move(plan), error);
    }

    static bool startProgram(Xc2BackendManager &manager,
                              const QString &program,
                              const QString &lockPath,
                              int errorNotificationDelayMs = 0,
                              Xc2Error *error = nullptr)
    {
        Xc2BackendManager::PrivateLaunchPlan plan;
        plan.program = program;
        plan.workingDirectory = QFileInfo(program).absolutePath();
        plan.environment = Xc2BackendManager::sanitizedEnvironment();
        plan.bindAddress = QHostAddress::LocalHost;
        plan.lockPath = lockPath;
        plan.startupDeadlineMs = 3000;
        plan.shutdownDeadlineMs = 300;
        plan.terminateDeadlineMs = 100;
        plan.maxPortAttempts = 1;
        plan.errorNotificationDelayMsForTest = errorNotificationDelayMs;
        plan.argumentsBuilder = [](quint16) { return QStringList{}; };
        plan.candidateAllocator = [] { return quint16(49123); };
        return manager.startPrivateForTest(std::move(plan), error);
    }

    static quint16 allocateCandidate()
    {
        return Xc2BackendManager::allocateLoopbackCandidate();
    }

    static bool listenerRowsOwned(const QList<RawTcpOwnerRow> &rows,
                                  quint16 port, quint32 processId)
    {
        return Xc2BackendManager::listenerRowsOwnedForTest(
            rows, port, processId);
    }

    static bool shutdownRowsOwned(const QList<RawTcpConnectionRow> &rows,
                                  quint16 serverPort, quint16 clientPort,
                                  quint32 processId)
    {
        return Xc2BackendManager::shutdownRowsOwnedForTest(
            rows, serverPort, clientPort, processId);
    }

    static void setProductionInspectionHooks(
        std::function<void(const QString &)> observer, int delayMs)
    {
        Xc2BackendManager::setProductionInspectionTestHooks(
            std::move(observer), delayMs);
    }

    static void resetProductionInspectionHooks()
    {
        Xc2BackendManager::resetProductionInspectionTestHooks();
    }

    static void setReaperRepostHook(std::function<void()> hook)
    {
        Xc2BackendManager::setReaperRepostTestHook(std::move(hook));
    }
};

} // namespace ktm::xc2

using namespace ktm::xc2;

namespace {

class EnvironmentRestore final {
public:
    explicit EnvironmentRestore(QList<QByteArray> names)
    {
        for (QByteArray &name : names) {
            const bool wasSet = qEnvironmentVariableIsSet(name.constData());
            const QByteArray value = qgetenv(name.constData());
            m_entries.append({std::move(name), wasSet, value});
        }
    }

    ~EnvironmentRestore()
    {
        for (const Entry &entry : std::as_const(m_entries)) {
            if (entry.wasSet)
                qputenv(entry.name.constData(), entry.value);
            else
                qunsetenv(entry.name.constData());
        }
    }

private:
    struct Entry {
        QByteArray name;
        bool wasSet = false;
        QByteArray value;
    };
    QList<Entry> m_entries;
};

class ProductionInspectionHooksReset final {
public:
    ~ProductionInspectionHooksReset()
    {
        Xc2BackendManagerTestAccess::resetProductionInspectionHooks();
    }
};

class ReaperRepostHookReset final {
public:
    ~ReaperRepostHookReset()
    {
        Xc2BackendManagerTestAccess::setReaperRepostHook({});
    }
};

QString privateLockPath(const QTemporaryDir &directory,
                        const QString &name = QStringLiteral("manager.lock"))
{
    return QDir::cleanPath(directory.filePath(name));
}

QList<QJsonObject> eventObjects(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<QJsonObject> events;
    while (!file.atEnd()) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(
            file.readLine().trimmed(), &error);
        if (error.error == QJsonParseError::NoError && document.isObject())
            events.append(document.object());
    }
    return events;
}

int eventCount(const QString &path, const QString &event)
{
    int count = 0;
    for (const QJsonObject &object : eventObjects(path)) {
        if (object.value(QStringLiteral("event")).toString() == event)
            ++count;
    }
    return count;
}

QList<QByteArray> eventBytes(const QString &path, const QString &event)
{
    QList<QByteArray> values;
    for (const QJsonObject &object : eventObjects(path)) {
        if (object.value(QStringLiteral("event")).toString() != event)
            continue;
        values.append(QByteArray::fromBase64(
            object.value(QStringLiteral("bytesBase64"))
                .toString().toLatin1()));
    }
    return values;
}

QList<qint64> connectionClosedByteTotals(const QString &path)
{
    QList<qint64> totals;
    for (const QJsonObject &object : eventObjects(path)) {
        if (object.value(QStringLiteral("event")).toString()
            != QStringLiteral("CONNECTION_CLOSED")) {
            continue;
        }
        totals.append(static_cast<qint64>(
            object.value(QStringLiteral("totalBytes")).toDouble()));
    }
    return totals;
}

QList<qint64> eventPids(const QString &path, const QString &event)
{
    QList<qint64> values;
    for (const QJsonObject &object : eventObjects(path)) {
        if (object.value(QStringLiteral("event")).toString() != event)
            continue;
        values.append(static_cast<qint64>(
            object.value(QStringLiteral("pid")).toDouble()));
    }
    return values;
}

QStringList requestTargets(const QString &path)
{
    QStringList targets;
    for (const QJsonObject &object : eventObjects(path)) {
        if (object.value(QStringLiteral("event")).toString()
            == QStringLiteral("REQUEST")) {
            targets.append(object.value(QStringLiteral("target")).toString());
        }
    }
    return targets;
}

qint64 descendantPid(const QString &path)
{
    for (const QJsonObject &object : eventObjects(path)) {
        if (object.value(QStringLiteral("event")).toString()
            == QStringLiteral("DESCENDANT_STARTED")) {
            return static_cast<qint64>(object.value(
                QStringLiteral("descendantPid")).toDouble());
        }
    }
    return 0;
}

int traceIndex(const QStringList &trace, const QString &prefix)
{
    for (qsizetype index = 0; index < trace.size(); ++index) {
        if (trace.at(index).startsWith(prefix))
            return static_cast<int>(index);
    }
    return -1;
}

QString queriedImagePath(HANDLE handle)
{
    std::vector<wchar_t> buffer(32768);
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(handle, 0, buffer.data(), &size))
        return {};
    return QDir::cleanPath(QDir::fromNativeSeparators(
        QString::fromWCharArray(buffer.data(), size)));
}

class QuitSink final : public QObject {
public:
    ~QuitSink() override
    {
        if (qApp != nullptr)
            qApp->removeEventFilter(this);
    }

    int count = 0;
    std::function<void()> onQuit;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == qApp && event->type() == QEvent::Quit) {
            ++count;
            if (onQuit)
                onQuit();
            return true;
        }
        return false;
    }
};

class QueuedQuitEmitter final : public QObject {
public:
    static constexpr QEvent::Type EventType =
        static_cast<QEvent::Type>(QEvent::User + 41);

protected:
    bool event(QEvent *event) override
    {
        if (event->type() != EventType)
            return QObject::event(event);
        QEvent quit(QEvent::Quit);
        QCoreApplication::sendEvent(qApp, &quit);
        return true;
    }
};

bool startStandaloneFake(QProcess &process, quint16 port,
                         const QString &eventLog,
                         QStringList extraArguments = {})
{
    process.setProgram(QString::fromUtf8(FAKE_XC2_SIDECAR_PATH));
    QStringList arguments{
        QStringLiteral("--port"), QString::number(port),
        QStringLiteral("--event-log"), eventLog,
    };
    arguments.append(extraArguments);
    process.setArguments(arguments);
    process.setWorkingDirectory(QFileInfo(process.program()).absolutePath());
    process.start();
    return process.waitForStarted(3000);
}

void stopStandaloneProcess(QProcess &process)
{
    if (process.state() == QProcess::NotRunning)
        return;
    process.kill();
    process.waitForFinished(5000);
}

HANDLE openStableHandle(qint64 processId)
{
    return OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                       FALSE, static_cast<DWORD>(processId));
}

bool handleIsLive(HANDLE handle)
{
    return handle != nullptr && WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
}

bool handleIsSignaled(HANDLE handle)
{
    return handle != nullptr
        && WaitForSingleObject(handle, 0) == WAIT_OBJECT_0;
}

bool terminateProcess(qint64 processId, UINT exitCode = 9)
{
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                 static_cast<DWORD>(processId));
    if (process == nullptr)
        return false;
    const bool terminated = TerminateProcess(process, exitCode)
        && WaitForSingleObject(process, 3000) == WAIT_OBJECT_0;
    CloseHandle(process);
    return terminated;
}

bool lockIsAvailable(const QString &path)
{
    QLockFile probe(path);
    probe.setStaleLockTime(0);
    if (!probe.tryLock(0))
        return false;
    probe.unlock();
    return true;
}

bool writeStagedOutput(const QString &directory, int stage,
                       bool standardError, const QByteArray &bytes,
                       bool exitAfter = false)
{
    const QString suffix = standardError ? QStringLiteral("stderr")
                                         : QStringLiteral("stdout");
    const QString path = QDir(directory).filePath(
        QStringLiteral("stage-%1.%2%3")
            .arg(stage)
            .arg(suffix, exitAfter ? QStringLiteral(".exit") : QString()));
    const QString temporaryPath = path + QStringLiteral(".tmp");
    QFile::remove(temporaryPath);
    QFile staged(temporaryPath);
    if (!staged.open(QIODevice::WriteOnly) || staged.write(bytes) != bytes.size()
        || !staged.flush()) {
        return false;
    }
    staged.close();
    return QFile::rename(temporaryPath, path);
}

std::optional<DWORD> exactListenerOwner(quint16 port)
{
    ULONG size = 0;
    DWORD result = GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                                       TCP_TABLE_OWNER_PID_LISTENER, 0);
    if (result != ERROR_INSUFFICIENT_BUFFER)
        return std::nullopt;
    std::unique_ptr<unsigned char[]> bytes(new unsigned char[size]);
    result = GetExtendedTcpTable(bytes.get(), &size, FALSE, AF_INET,
                                 TCP_TABLE_OWNER_PID_LISTENER, 0);
    if (result != NO_ERROR)
        return std::nullopt;
    const auto *table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID *>(
        bytes.get());
    std::optional<DWORD> owner;
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const MIB_TCPROW_OWNER_PID &row = table->table[index];
        if (ntohs(static_cast<u_short>(row.dwLocalPort & 0xffffU)) != port)
            continue;
        if (ntohl(row.dwLocalAddr) != 0x7f000001U || owner.has_value())
            return std::nullopt;
        owner = row.dwOwningPid;
    }
    return owner;
}

} // namespace

class Xc2BackendManagerTest final : public QObject {
    Q_OBJECT

private slots:
    void managerRegistersPublicSignalMetatypes()
    {
        Xc2BackendManager manager;
        QSignalSpy state(&manager, &Xc2BackendManager::stateChanged);
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(state.isValid());
        QVERIFY(ready.isValid());
        QVERIFY(failed.isValid());
        QVERIFY(stopped.isValid());
    }

    void productionArgumentsOverrideEveryEmbedded8082()
    {
        const Xc2InstallLayout layout{
            QStringLiteral("C:/XC2/resources/app"),
            QStringLiteral("C:/XC2/resources/app/jre/bin/java.exe"),
            QStringLiteral("C:/XC2/resources/app/xc2_backend_patched.jar"),
            QStringLiteral("C:/XC2/resources/app/config"),
            QStringLiteral("C:/XC2/resources/app/config/log.xml"),
            QStringLiteral("C:/XC2/resources/app/config/custom-cloud.properties"),
            QString(), QString(),
        };
        const QStringList arguments =
            Xc2BackendManagerTestAccess::productionArguments(layout, 49123);
        const QString joined = arguments.join(QLatin1Char(' '));
        QVERIFY2(!joined.contains(QStringLiteral(":8082")),
                 qPrintable(joined));
        const QList<QPair<QString, QString>> overrides{
            {QStringLiteral("-Donelogin.saml2.sp.assertion_consumer_service.url="),
             QStringLiteral("http://127.0.0.1:49123/xc2/1.0/auth/samlACS")},
            {QStringLiteral("-Donelogin.saml2.sp.single_logout_service.url="),
             QStringLiteral("http://127.0.0.1:49123/xc2/1.0/auth/samlLogoutSLO")},
            {QStringLiteral("-Donelogin.saml2.idp.single_sign_on_service.url="),
             QStringLiteral("http://127.0.0.1:49123/xc2/1.0/auth/mock")},
            {QStringLiteral("-Donelogin.saml2.idp.single_logout_service.url="),
             QStringLiteral("http://127.0.0.1:49123/xc2/1.0/auth/mock")},
            {QStringLiteral("-Dcom.avl.ditest.xc2.gripsresource.prefix="),
             QStringLiteral("http://127.0.0.1:49123/xc2/1.0/mock/streamer?file={0}")},
        };
        for (const auto &override : overrides) {
            int occurrences = 0;
            for (const QString &argument : arguments) {
                if (argument.startsWith(override.first)) {
                    ++occurrences;
                    QCOMPARE(argument,
                             override.first + override.second);
                }
            }
            QCOMPARE(occurrences, 1);
        }
    }

    void productionArgumentsKeepRequiredJavaOrderingAndProfile()
    {
        Xc2InstallLayout layout;
        layout.root = QStringLiteral("C:/XC2/resources/app");
        layout.backendJar =
            QStringLiteral("C:/XC2/resources/app/xc2_backend_patched.jar");
        const QStringList arguments =
            Xc2BackendManagerTestAccess::productionArguments(layout, 49123);
        const QStringList expected{
            QStringLiteral("-Dssc.includezip=true"),
            QStringLiteral("-Dlogging.config=config/log.xml"),
            QStringLiteral("-Dspring.config.additional-location=file:./config/custom-cloud.properties"),
            QStringLiteral("-Dserver.address=127.0.0.1"),
            QStringLiteral("-Dserver.port=49123"),
            QStringLiteral("-Donelogin.saml2.sp.assertion_consumer_service.url=http://127.0.0.1:49123/xc2/1.0/auth/samlACS"),
            QStringLiteral("-Donelogin.saml2.sp.single_logout_service.url=http://127.0.0.1:49123/xc2/1.0/auth/samlLogoutSLO"),
            QStringLiteral("-Donelogin.saml2.idp.single_sign_on_service.url=http://127.0.0.1:49123/xc2/1.0/auth/mock"),
            QStringLiteral("-Donelogin.saml2.idp.single_logout_service.url=http://127.0.0.1:49123/xc2/1.0/auth/mock"),
            QStringLiteral("-Dcom.avl.ditest.xc2.gripsresource.prefix=http://127.0.0.1:49123/xc2/1.0/mock/streamer?file={0}"),
            QStringLiteral("-Dloader.main=com.avl.ditest.xc2.Xc2NgApplication"),
            QStringLiteral("-Dspring.profiles.active=dev"),
            QStringLiteral("-Dcom.avl.ditest.xc2.developer=true"),
            QStringLiteral("-jar"),
            layout.backendJar,
        };
        QCOMPARE(arguments, expected);
        QCOMPARE(arguments.count(QStringLiteral("-jar")), 1);
        const int jarIndex = arguments.indexOf(QStringLiteral("-jar"));
        QCOMPARE(jarIndex, arguments.size() - 2);
        QCOMPARE(arguments.constLast(), layout.backendJar);
        const QStringList required{
            QStringLiteral("-Dssc.includezip=true"),
            QStringLiteral("-Dlogging.config=config/log.xml"),
            QStringLiteral("-Dspring.config.additional-location=file:./config/custom-cloud.properties"),
            QStringLiteral("-Dloader.main=com.avl.ditest.xc2.Xc2NgApplication"),
            QStringLiteral("-Dspring.profiles.active=dev"),
            QStringLiteral("-Dcom.avl.ditest.xc2.developer=true"),
            QStringLiteral("-Dserver.address=127.0.0.1"),
            QStringLiteral("-Dserver.port=49123"),
        };
        for (const QString &argument : required) {
            const int index = arguments.indexOf(argument);
            QVERIFY2(index >= 0 && index < jarIndex, qPrintable(argument));
            QCOMPARE(arguments.count(argument), 1);
        }
        QSet<QString> propertyKeys;
        for (int index = 0; index < jarIndex; ++index) {
            const QString &argument = arguments.at(index);
            if (!argument.startsWith(QStringLiteral("-D")))
                continue;
            const QString key = argument.left(argument.indexOf('='));
            QVERIFY2(!propertyKeys.contains(key), qPrintable(key));
            propertyKeys.insert(key);
        }
    }

    void productionStartCannotConsumeForgedOrTestReports()
    {
        using Start = bool (Xc2BackendManager::*)(const QString &, Xc2Error *);
        static_assert(std::is_same_v<decltype(&Xc2BackendManager::startProduction),
                                     Start>);
        QVERIFY(true);
    }

    void productionStartRunsFreshInspectionBeforeAnySideEffect()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString appRoot = directory.filePath("resources/app");
        QVERIFY(QDir().mkpath(appRoot));
        const QString productionLock =
            Xc2BackendManagerTestAccess::productionLockPath(
                QStandardPaths::writableLocation(
                    QStandardPaths::AppLocalDataLocation));
        const QFileInfo lockBefore(productionLock);
        const bool existedBefore = lockBefore.exists();
        const qint64 sizeBefore = lockBefore.size();
        const QDateTime modifiedBefore = lockBefore.lastModified();

        Xc2BackendManager manager;
        QSignalSpy states(&manager, &Xc2BackendManager::stateChanged);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        Xc2Error supplied;
        supplied.message = QStringLiteral("must be cleared");
        QVERIFY(manager.startProduction(appRoot, &supplied));
        QVERIFY(supplied.message.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
        QCOMPARE(manager.state(), Xc2BackendState::Stopped);
        QVERIFY(!manager.ownsProcess());
        QCOMPARE(failed.size(), 1);
        QCOMPARE(qvariant_cast<Xc2Error>(failed.constFirst().constFirst())
                     .category,
                 Xc2ErrorCategory::Prerequisite);
        QCOMPARE(stopped.constFirst().at(0).toInt(), -1);
        QCOMPARE(qvariant_cast<QProcess::ExitStatus>(
                     stopped.constFirst().at(1)),
                 QProcess::CrashExit);
        QCOMPARE(states.size(), 3);
        QCOMPARE(qvariant_cast<Xc2BackendState>(states.at(0).at(0)),
                 Xc2BackendState::Starting);
        QCOMPARE(qvariant_cast<Xc2BackendState>(states.at(1).at(0)),
                 Xc2BackendState::Failed);
        QCOMPARE(qvariant_cast<Xc2BackendState>(states.at(2).at(0)),
                 Xc2BackendState::Stopped);
        const QFileInfo lockAfter(productionLock);
        QCOMPARE(lockAfter.exists(), existedBefore);
        if (existedBefore) {
            QCOMPARE(lockAfter.size(), sizeBefore);
            QCOMPARE(lockAfter.lastModified(), modifiedBefore);
        }
    }

    void productionInspectionTracePrecedesAllLaunchSideEffects()
    {
        ProductionInspectionHooksReset reset;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString appRoot = directory.filePath("resources/app");
        QVERIFY(QDir().mkpath(appRoot));
        const QString productionLock =
            Xc2BackendManagerTestAccess::productionLockPath(
                QStandardPaths::writableLocation(
                    QStandardPaths::AppLocalDataLocation));
        const QFileInfo before(productionLock);
        const bool existedBefore = before.exists();
        const qint64 sizeBefore = before.size();
        const QDateTime modifiedBefore = before.lastModified();

        for (const int delayMs : {0, 15020}) {
            auto trace = std::make_shared<QStringList>();
            Xc2BackendManagerTestAccess::setProductionInspectionHooks(
                [trace](const QString &event) { trace->append(event); },
                delayMs);
            Xc2BackendManager manager;
            QSignalSpy failed(&manager, &Xc2BackendManager::failed);
            QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
            QElapsedTimer elapsed;
            elapsed.start();
            QVERIFY(manager.startProduction(appRoot));
            QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 18000);
            QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
            QCOMPARE(trace->size(), 3);
            QCOMPARE(trace->at(0), QStringLiteral("deadline-start"));
            QCOMPARE(trace->at(1), QStringLiteral("inspect-start"));
            QVERIFY(trace->at(2).startsWith(QStringLiteral("inspect-fail:")));
            for (const QString &event : std::as_const(*trace)) {
                QVERIFY(!event.startsWith(QStringLiteral("lock-")));
                QVERIFY(!event.startsWith(QStringLiteral("candidate:")));
                QVERIFY(!event.startsWith(QStringLiteral("arguments-built:")));
                QVERIFY(!event.startsWith(QStringLiteral("attempt-start:")));
                QVERIFY(!event.startsWith(QStringLiteral("process-started:")));
            }
            if (delayMs > 0) {
                QVERIFY(elapsed.elapsed() >= 15000);
                QCOMPARE(trace->at(2),
                         QStringLiteral("inspect-fail:deadline"));
            }
            const QFileInfo after(productionLock);
            QCOMPARE(after.exists(), existedBefore);
            if (existedBefore) {
                QCOMPARE(after.size(), sizeBefore);
                QCOMPARE(after.lastModified(), modifiedBefore);
            }
        }
    }

    void acceptedValidationFailureCannotBeOverwrittenByStop()
    {
        for (const bool stopFromStarting : {false, true}) {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            Xc2BackendManager manager;
            QList<Xc2BackendState> order;
            QObject::connect(&manager, &Xc2BackendManager::stateChanged,
                             &manager, [&](Xc2BackendState state) {
                order.append(state);
                if (stopFromStarting && state == Xc2BackendState::Starting)
                    manager.stop();
            });
            QSignalSpy failed(&manager, &Xc2BackendManager::failed);
            QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
            auto argumentCalls = std::make_shared<int>(0);
            auto candidateCalls = std::make_shared<int>(0);
            QVERIFY(Xc2BackendManagerTestAccess::startInvalid(
                manager,
                Xc2BackendManagerTestAccess::InvalidPlan::RelativeProgram,
                privateLockPath(directory), argumentCalls, candidateCalls));
            if (!stopFromStarting)
                manager.stop();
            QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
            QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
            QCOMPARE(stopped.constFirst().at(0).toInt(), -1);
            QCOMPARE(qvariant_cast<QProcess::ExitStatus>(
                         stopped.constFirst().at(1)),
                     QProcess::CrashExit);
            const QList<Xc2BackendState> expectedOrder{
                Xc2BackendState::Starting,
                Xc2BackendState::Failed,
                Xc2BackendState::Stopped,
            };
            QCOMPARE(order, expectedOrder);
            QCOMPARE(*argumentCalls, 0);
            QCOMPARE(*candidateCalls, 0);
        }
    }

    void acceptedPublicFailuresCannotBeOverwrittenByStop()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString incompleteRoot = directory.filePath("resources/app");
        QVERIFY(QDir().mkpath(incompleteRoot));
        const QList<QString> roots{
            QStringLiteral("relative/resources/app"),
            incompleteRoot,
        };
        for (qsizetype index = 0; index < roots.size(); ++index) {
            Xc2BackendManager manager;
            QList<Xc2BackendState> states;
            QObject::connect(&manager, &Xc2BackendManager::stateChanged,
                             &manager, [&](Xc2BackendState state) {
                states.append(state);
                if (index == 1 && state == Xc2BackendState::Starting)
                    manager.stop();
            });
            QSignalSpy failed(&manager, &Xc2BackendManager::failed);
            QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
            QVERIFY(manager.startProduction(roots.at(index)));
            if (index == 0)
                manager.stop();
            QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
            QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
            QCOMPARE(stopped.constFirst().at(0).toInt(), -1);
            QCOMPARE(qvariant_cast<QProcess::ExitStatus>(
                         stopped.constFirst().at(1)),
                     QProcess::CrashExit);
            const QList<Xc2BackendState> expected{
                Xc2BackendState::Starting,
                Xc2BackendState::Failed,
                Xc2BackendState::Stopped,
            };
            QCOMPARE(states, expected);
        }
    }

    void productionLockPathIsFixedAndRunIndependent()
    {
        const QString appData = QStringLiteral("C:/Users/test/AppData/Local/app");
        const QString expected = QDir::cleanPath(
            appData + QStringLiteral("/ktm-xc2-vci2k.lock"));
        QCOMPARE(Xc2BackendManagerTestAccess::productionLockPath(appData),
                 expected);
        QCOMPARE(Xc2BackendManagerTestAccess::productionLockPath(appData),
                 Xc2BackendManagerTestAccess::productionLockPath(appData));
        QVERIFY(!expected.contains(QStringLiteral("8082")));
        QVERIFY(Xc2BackendManagerTestAccess::productionLockPath({}).isEmpty());
    }

    void productionBuilderUsesExactApprovedPaths()
    {
        Xc2InstallLayout layout;
        layout.root = QStringLiteral("C:/Program Files/XC2/resources/app");
        layout.javaExe = layout.root + QStringLiteral("/jre/bin/java.exe");
        layout.backendJar =
            layout.root + QStringLiteral("/xc2_backend_patched.jar");
        const QString program =
            Xc2BackendManagerTestAccess::productionProgram(layout);
        const QString workingDirectory =
            Xc2BackendManagerTestAccess::productionWorkingDirectory(layout);
        const QStringList arguments =
            Xc2BackendManagerTestAccess::productionArguments(layout, 49123);
        QCOMPARE(program, layout.javaExe);
        QCOMPARE(workingDirectory, layout.root);
        QCOMPARE(arguments.constLast(), layout.backendJar);
        QVERIFY(QDir::isAbsolutePath(program));
        QVERIFY(QDir::isAbsolutePath(workingDirectory));
        QVERIFY(QDir::isAbsolutePath(arguments.constLast()));
        const QString executableName = QFileInfo(program).fileName();
        QCOMPARE(executableName.compare(QStringLiteral("java.exe"),
                                        Qt::CaseInsensitive), 0);
        for (const QString &forbidden : {
                 QStringLiteral("cmd.exe"), QStringLiteral("powershell.exe"),
                 QStringLiteral("javaw.exe"), QStringLiteral("start")}) {
            QVERIFY(program.compare(forbidden, Qt::CaseInsensitive) != 0);
        }
    }

    void productionProcessEnvironmentRemovesInjectionVariables()
    {
        const QList<QByteArray> names{
            QByteArrayLiteral("JAVA_TOOL_OPTIONS"),
            QByteArrayLiteral("_java_options"),
            QByteArrayLiteral("Jdk_Java_Options"),
            QByteArrayLiteral("SPRING_APPLICATION_JSON"),
            QByteArrayLiteral("spring_config_location"),
            QByteArrayLiteral("SPRING_CONFIG_ADDITIONAL_LOCATION"),
            QByteArrayLiteral("Spring_Profiles_Active"),
            QByteArrayLiteral("SERVER_ADDRESS"),
            QByteArrayLiteral("server_port"),
        };
        EnvironmentRestore restore(names);
        for (const QByteArray &name : names)
            qputenv(name.constData(), QByteArrayLiteral("injected"));
        const QProcessEnvironment environment =
            Xc2BackendManagerTestAccess::sanitizedEnvironment();
        for (const QString &key : environment.keys()) {
            for (const QByteArray &name : names) {
                QVERIFY(key.compare(QString::fromLatin1(name),
                                    Qt::CaseInsensitive) != 0);
            }
        }
    }

    void privateFakeArgumentsReceiveManagerSelectedPort()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const quint16 candidate = Xc2BackendManagerTestAccess::allocateCandidate();
        QVERIFY(candidate > 0 && candidate != 8082);
        auto argumentCalls = std::make_shared<int>(0);
        const QList<QByteArray> injectedNames{
            QByteArrayLiteral("JAVA_TOOL_OPTIONS"),
            QByteArrayLiteral("_JAVA_OPTIONS"),
            QByteArrayLiteral("JDK_JAVA_OPTIONS"),
            QByteArrayLiteral("SPRING_APPLICATION_JSON"),
            QByteArrayLiteral("SPRING_CONFIG_LOCATION"),
            QByteArrayLiteral("SPRING_CONFIG_ADDITIONAL_LOCATION"),
            QByteArrayLiteral("SPRING_PROFILES_ACTIVE"),
            QByteArrayLiteral("SERVER_ADDRESS"),
            QByteArrayLiteral("SERVER_PORT"),
        };
        EnvironmentRestore restore(injectedNames);
        for (const QByteArray &name : injectedNames)
            qputenv(name.constData(), QByteArrayLiteral("injected"));

        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        const QString eventLog = directory.filePath("events.jsonl");
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory), {candidate}, {},
            eventLog, 6000, 1000, 200, 1,
            argumentCalls));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 8000);
        QCOMPARE(manager.endpoints().restBaseUrl.port(), candidate);
        QCOMPARE(*argumentCalls, 1);
        bool sawEnvironment = false;
        for (const QJsonObject &event : eventObjects(eventLog)) {
            if (event.value(QStringLiteral("event")).toString()
                != QStringLiteral("ENVIRONMENT"))
                continue;
            sawEnvironment = true;
            QVERIFY(event.value(QStringLiteral("injectionVariables"))
                        .toArray().isEmpty());
        }
        QVERIFY(sawEnvironment);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
    }

    void fakeCliRejectsEveryAmbiguousOrUnsafeInvocation()
    {
        const QList<QStringList> invalidArguments{
            {},
            {QStringLiteral("--port"), QStringLiteral("0")},
            {QStringLiteral("--port")},
            {QStringLiteral("--port"), QStringLiteral("49123"),
             QStringLiteral("--port"), QStringLiteral("49124")},
            {QStringLiteral("--port"), QStringLiteral("49123"),
             QStringLiteral("--never-health"),
             QStringLiteral("--never-health")},
            {QStringLiteral("--port"), QStringLiteral("49123"),
             QStringLiteral("--hold-lock"), QStringLiteral("relative.lock")},
            {QStringLiteral("--port"), QStringLiteral("49123"),
             QStringLiteral("--release-listener-after-ready"),
             QStringLiteral("relative.trigger")},
            {QStringLiteral("--port"), QStringLiteral("49123"),
             QStringLiteral("--event-log"), QStringLiteral("relative.jsonl")},
            {QStringLiteral("--port"), QStringLiteral("49123"),
             QStringLiteral("--unknown")},
            {QStringLiteral("--port"), QStringLiteral("49123"),
             QStringLiteral("--event-log"),
             QStringLiteral("--never-health")},
        };
        for (const QStringList &arguments : invalidArguments) {
            QProcess process;
            process.setProgram(QString::fromUtf8(FAKE_XC2_SIDECAR_PATH));
            process.setArguments(arguments);
            process.setWorkingDirectory(
                QFileInfo(process.program()).absolutePath());
            process.start();
            QVERIFY2(process.waitForStarted(3000),
                     qPrintable(arguments.join(QLatin1Char(' '))));
            QVERIFY(process.waitForFinished(3000));
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QCOMPARE(process.exitCode(), 2);
        }
    }

    void privateLaunchValidationFailsBeforeLockAllocatorOrBuilder()
    {
        using Invalid = Xc2BackendManagerTestAccess::InvalidPlan;
        const QList<Invalid> cases{
            Invalid::RelativeProgram,
            Invalid::MissingProgram,
            Invalid::RelativeWorkingDirectory,
            Invalid::MissingWorkingDirectory,
            Invalid::RelativeLock,
            Invalid::NonLocalBind,
            Invalid::ZeroStartupDeadline,
            Invalid::ExcessiveShutdownDeadline,
            Invalid::ZeroTerminateDeadline,
            Invalid::ZeroPortAttempts,
            Invalid::ExcessivePortAttempts,
            Invalid::MissingArgumentsBuilder,
            Invalid::MissingCandidateAllocator,
        };
        for (qsizetype index = 0; index < cases.size(); ++index) {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const QString lockPath = directory.filePath(
                QStringLiteral("uncreated-%1/manager.lock").arg(index));
            auto argumentCalls = std::make_shared<int>(0);
            auto candidateCalls = std::make_shared<int>(0);
            Xc2BackendManager manager;
            QSignalSpy failed(&manager, &Xc2BackendManager::failed);
            QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
            Xc2Error acceptedError;
            acceptedError.message = QStringLiteral("clear me");
            QVERIFY(Xc2BackendManagerTestAccess::startInvalid(
                manager, cases.at(index), lockPath, argumentCalls,
                candidateCalls, &acceptedError));
            QVERIFY(acceptedError.message.isEmpty());
            QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 2000);
            manager.stop();
            manager.stop();
            QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 2000);
            QCOMPARE(*argumentCalls, 0);
            QCOMPARE(*candidateCalls, 0);
            QVERIFY(!QFileInfo::exists(QFileInfo(lockPath).absolutePath()));
            QCOMPARE(qvariant_cast<Xc2Error>(
                         failed.constFirst().constFirst()).category,
                     Xc2ErrorCategory::Backend);
            QCOMPARE(stopped.constFirst().at(0).toInt(), -1);
            QCOMPARE(qvariant_cast<QProcess::ExitStatus>(
                         stopped.constFirst().at(1)),
                     QProcess::CrashExit);
        }
    }

    void invalidCandidatePortsNeverBuildArgumentsOrSpawn()
    {
        for (const QList<quint16> candidates : {
                 QList<quint16>{0},
                 QList<quint16>{8082, 0},
             }) {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const QString lockPath = privateLockPath(directory);
            const QString eventLog = directory.filePath("invalid-port.jsonl");
            auto argumentCalls = std::make_shared<int>(0);
            auto candidateCalls = std::make_shared<int>(0);
            Xc2BackendManager manager;
            QSignalSpy failed(&manager, &Xc2BackendManager::failed);
            QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
            QVERIFY(Xc2BackendManagerTestAccess::startFake(
                manager, lockPath, candidates, {}, eventLog,
                3000, 300, 100, 3, argumentCalls, candidateCalls));
            QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 2000);
            QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 2000);
            QCOMPARE(*argumentCalls, 0);
            QVERIFY(*candidateCalls >= 1);
            QVERIFY(!QFileInfo::exists(eventLog));
            QVERIFY(lockIsAvailable(lockPath));
        }
    }

    void readyRequiresExactIpv4ListenerOwnedByStableChildHandle()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const quint16 port = Xc2BackendManagerTestAccess::allocateCandidate();
        QVERIFY(port > 0 && port != 8082);

        Xc2BackendManager manager;
        auto trace = std::make_shared<QStringList>();
        bool publicationWasClearBeforeReady = true;
        QObject::connect(&manager, &Xc2BackendManager::stateChanged,
                         &manager, [&manager, &publicationWasClearBeforeReady](
                                       Xc2BackendState state) {
            if (state == Xc2BackendState::Starting
                || state == Xc2BackendState::Probing) {
                publicationWasClearBeforeReady =
                    publicationWasClearBeforeReady
                    && manager.endpoints().restBaseUrl.isEmpty()
                    && manager.endpoints().webSocketUrl.isEmpty()
                    && !manager.currentUser().has_value();
            }
        });
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory), {port}, {},
            directory.filePath("owned.jsonl"), 6000, 1000, 200, 1,
            {}, {}, nullptr, trace));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 8000);
        QVERIFY(publicationWasClearBeforeReady);
        QVERIFY(manager.ownsProcess());
        QVERIFY(manager.ownedProcessId() > 0);
        HANDLE child = openStableHandle(manager.ownedProcessId());
        QVERIFY(child != nullptr);
        QVERIFY(handleIsLive(child));
        QCOMPARE(queriedImagePath(child).compare(
                     QDir::cleanPath(QString::fromUtf8(
                         FAKE_XC2_SIDECAR_PATH)),
                     Qt::CaseInsensitive),
                 0);
        const int direct = traceIndex(
            *trace, QStringLiteral("direct-executable-proven:1"));
        const int lockDirectory = traceIndex(
            *trace, QStringLiteral("lock-directory:"));
        const int lockAttempt = traceIndex(
            *trace, QStringLiteral("lock-attempt:"));
        const int lockAcquired = traceIndex(
            *trace, QStringLiteral("lock-acquired:"));
        const int candidate = traceIndex(*trace, QStringLiteral("candidate:"));
        const int job = traceIndex(*trace, QStringLiteral("job-assigned:1"));
        const int health = traceIndex(
            *trace, QStringLiteral("health-request:1"));
        const int authorized = traceIndex(
            *trace, QStringLiteral("network-authorized:1"));
        QVERIFY(lockDirectory >= 0 && lockAttempt > lockDirectory
                && lockAcquired > lockAttempt && candidate > lockAcquired);
        QVERIFY(direct > candidate && job > direct && health > job
                && authorized > health);
        QTRY_VERIFY_WITH_TIMEOUT(exactListenerOwner(port).has_value(), 2000);
        QCOMPARE(exactListenerOwner(port).value(),
                 static_cast<DWORD>(manager.ownedProcessId()));
        QCOMPARE(manager.endpoints().restBaseUrl,
                 QUrl(QStringLiteral("http://127.0.0.1:%1/xc2/1.0")
                          .arg(port)));
        QCOMPARE(manager.endpoints().webSocketUrl,
                 QUrl(QStringLiteral("ws://127.0.0.1:%1/xc2-websocket")
                          .arg(port)));

        manager.stop();
        QVERIFY(manager.endpoints().restBaseUrl.isEmpty());
        QVERIFY(manager.endpoints().webSocketUrl.isEmpty());
        QVERIFY(!manager.currentUser().has_value());
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        const int lockReleased = traceIndex(
            *trace, QStringLiteral("lock-released:"));
        QVERIFY(lockReleased > authorized);
        QVERIFY(handleIsSignaled(child));
        CloseHandle(child);
    }

    void ownerTableUsesNetworkByteOrderAndRejectsDuplicateRows()
    {
        using Row = Xc2BackendManagerTestAccess::RawTcpOwnerRow;
        const quint16 port = 49123;
        const quint32 pid = 4242;
        const Row owned{
            htonl(0x7f000001U),
            static_cast<quint32>(htons(port)),
            pid,
        };
        QVERIFY(Xc2BackendManagerTestAccess::listenerRowsOwned(
            {owned}, port, pid));
        QVERIFY(!Xc2BackendManagerTestAccess::listenerRowsOwned(
            {{0x7f000001U, owned.localPort, pid}}, port, pid));
        QVERIFY(!Xc2BackendManagerTestAccess::listenerRowsOwned(
            {{htonl(0x7f000002U), owned.localPort, pid}}, port, pid));
        QVERIFY(!Xc2BackendManagerTestAccess::listenerRowsOwned(
            {{owned.localAddress, port, pid}}, port, pid));
        QVERIFY(!Xc2BackendManagerTestAccess::listenerRowsOwned(
            {{owned.localAddress, owned.localPort, pid + 1}}, port, pid));
        QVERIFY(!Xc2BackendManagerTestAccess::listenerRowsOwned(
            {owned, owned}, port, pid));
        QVERIFY(!Xc2BackendManagerTestAccess::listenerRowsOwned(
            {{htonl(0U), owned.localPort, pid}}, port, pid));
        QVERIFY(!Xc2BackendManagerTestAccess::listenerRowsOwned(
            {{owned.localAddress, 0, pid}}, port, pid));
        const Row conflicting{owned.localAddress, owned.localPort, pid + 1};
        QVERIFY(!Xc2BackendManagerTestAccess::listenerRowsOwned(
            {owned, conflicting}, port, pid));
        const Row unrelated{htonl(0x7f000001U),
                            static_cast<quint32>(htons(port + 1)), pid + 1};
        QVERIFY(Xc2BackendManagerTestAccess::listenerRowsOwned(
            {owned, unrelated}, port, pid));
    }

    void validAliveDecoyOn8082IsIgnoredAndSurvives()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QTcpServer availability;
        const QString decoyLog = directory.filePath("decoy-8082.jsonl");
        QProcess decoy;
        const bool testOwnsDecoy = availability.listen(
            QHostAddress::LocalHost, 8082);
        if (testOwnsDecoy) {
            availability.close();
            QVERIFY(startStandaloneFake(decoy, 8082, decoyLog));
            QTRY_COMPARE_WITH_TIMEOUT(
                eventCount(decoyLog, QStringLiteral("LISTENING")),
                1, 3000);
        }

        const quint16 ownedPort =
            Xc2BackendManagerTestAccess::allocateCandidate();
        QVERIFY(ownedPort > 0 && ownedPort != 8082);
        auto candidateCalls = std::make_shared<int>(0);
        auto argumentCalls = std::make_shared<int>(0);
        auto trace = std::make_shared<QStringList>();
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory), {8082, ownedPort}, {},
            directory.filePath("owned.jsonl"), 6000, 1000, 200, 1,
            argumentCalls, candidateCalls, nullptr, trace));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 8000);
        QCOMPARE(*candidateCalls, 2);
        QCOMPARE(*argumentCalls, 1);
        QCOMPARE(manager.endpoints().restBaseUrl.port(), ownedPort);
        QVERIFY(ownedPort != 8082);
        QVERIFY(trace->contains(QStringLiteral("candidate:8082")));
        QVERIFY(trace->contains(QStringLiteral("candidate-rejected:8082")));
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        for (const QString &prefix : {
                 QStringLiteral("attempt-start:"),
                 QStringLiteral("arguments-built:"),
                 QStringLiteral("rest-base:"),
                 QStringLiteral("health-request:"),
                 QStringLiteral("current-user-request:"),
                 QStringLiteral("shutdown-write:"),
             }) {
            int selectedEvents = 0;
            for (const QString &event : std::as_const(*trace)) {
                if (!event.startsWith(prefix))
                    continue;
                QVERIFY2(!event.endsWith(QStringLiteral(":8082")),
                         qPrintable(event));
                QVERIFY2(event.endsWith(QStringLiteral(":%1").arg(ownedPort)),
                         qPrintable(event));
                ++selectedEvents;
            }
            QVERIFY2(selectedEvents >= 1, qPrintable(prefix));
        }

        if (testOwnsDecoy) {
            QCOMPARE(decoy.state(), QProcess::Running);
            QCOMPARE(eventCount(decoyLog, QStringLiteral("REQUEST")), 0);
            QCOMPARE(eventCount(decoyLog, QStringLiteral("SHUTDOWN")), 0);
            QCOMPARE(eventCount(decoyLog, QStringLiteral("RAW_BYTES")), 0);
            QCOMPARE(eventCount(decoyLog,
                                QStringLiteral("CONNECTION_ACCEPTED")), 0);
            stopStandaloneProcess(decoy);
        } else {
            QTcpServer stillOccupied;
            QVERIFY(!stillOccupied.listen(QHostAddress::LocalHost, 8082));
        }
    }

    void injectedCandidate8082IsRejectedBeforeArgumentsOrSpawn()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const quint16 selected =
            Xc2BackendManagerTestAccess::allocateCandidate();
        QVERIFY(selected > 0 && selected != 8082);
        auto argumentCalls = std::make_shared<int>(0);
        auto candidateCalls = std::make_shared<int>(0);

        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory), {8082, selected}, {},
            directory.filePath("skip.jsonl"), 6000, 1000, 200, 1,
            argumentCalls, candidateCalls));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 8000);
        QCOMPARE(*candidateCalls, 2);
        QCOMPARE(*argumentCalls, 1);
        QCOMPARE(manager.endpoints().restBaseUrl.port(), selected);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
    }

    void portStealRaceCleansChildAndRetriesANewPort()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const quint16 stolen = Xc2BackendManagerTestAccess::allocateCandidate();
        const QString decoyLog = directory.filePath("stealer.jsonl");
        const QString managedLog = directory.filePath("managed.jsonl");
        QProcess stealer;
        QVERIFY(startStandaloneFake(
            stealer, stolen, decoyLog,
            {QStringLiteral("--health-delay-ms"), QStringLiteral("300")}));
        QTRY_COMPARE_WITH_TIMEOUT(eventCount(decoyLog, QStringLiteral("LISTENING")),
                                  1, 3000);
        const quint16 replacement =
            Xc2BackendManagerTestAccess::allocateCandidate();
        QVERIFY(replacement > 0 && replacement != stolen);
        auto argumentCalls = std::make_shared<int>(0);

        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory), {stolen, replacement}, {},
            managedLog, 8000, 1000, 150, 2,
            argumentCalls));
        QTRY_COMPARE_WITH_TIMEOUT(
            eventCount(managedLog, QStringLiteral("BIND_FAILED")), 1, 3000);
        const QList<qint64> failedPids = eventPids(
            managedLog, QStringLiteral("BIND_FAILED"));
        QCOMPARE(failedPids.size(), 1);
        HANDLE failedAttempt = openStableHandle(failedPids.constFirst());
        QVERIFY(failedAttempt != nullptr && handleIsLive(failedAttempt));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
        QVERIFY(handleIsSignaled(failedAttempt));
        QCOMPARE(*argumentCalls, 2);
        QCOMPARE(manager.endpoints().restBaseUrl.port(), replacement);
        QVERIFY(eventCount(decoyLog, QStringLiteral("REQUEST")) >= 1);
        QCOMPARE(eventCount(decoyLog, QStringLiteral("SHUTDOWN")), 0);
        QCOMPARE(stealer.state(), QProcess::Running);

        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        CloseHandle(failedAttempt);
        stopStandaloneProcess(stealer);
    }

    void collisionRetrySkipsPreviouslyAttemptedPorts()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const quint16 stolen = Xc2BackendManagerTestAccess::allocateCandidate();
        const quint16 replacement =
            Xc2BackendManagerTestAccess::allocateCandidate();
        QVERIFY(stolen > 0 && replacement > 0 && stolen != replacement);
        QProcess stealer;
        const QString stealerLog = directory.filePath("duplicate-stealer.jsonl");
        QVERIFY(startStandaloneFake(
            stealer, stolen, stealerLog,
            {QStringLiteral("--health-delay-ms"), QStringLiteral("300")}));
        QTRY_COMPARE_WITH_TIMEOUT(eventCount(
            stealerLog, QStringLiteral("LISTENING")), 1, 3000);

        auto candidateCalls = std::make_shared<int>(0);
        auto argumentCalls = std::make_shared<int>(0);
        auto trace = std::make_shared<QStringList>();
        const QString managedLog = directory.filePath("duplicate-managed.jsonl");
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {stolen, stolen, replacement}, {}, managedLog,
            8000, 700, 100, 2, argumentCalls, candidateCalls, nullptr,
            trace));
        QTRY_COMPARE_WITH_TIMEOUT(eventCount(
            managedLog, QStringLiteral("BIND_FAILED")), 1, 3000);
        const qint64 failedPid = eventPids(
            managedLog, QStringLiteral("BIND_FAILED")).constFirst();
        HANDLE failedChild = openStableHandle(failedPid);
        QVERIFY(failedChild != nullptr && handleIsLive(failedChild));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 10000);
        QVERIFY(handleIsSignaled(failedChild));
        QCOMPARE(*candidateCalls, 3);
        QCOMPARE(*argumentCalls, 2);
        QCOMPARE(eventCount(managedLog, QStringLiteral("BIND_FAILED")), 1);
        QCOMPARE(manager.endpoints().restBaseUrl.port(), replacement);
        const int cleaned = traceIndex(
            *trace, QStringLiteral("attempt-cleanup-complete:1"));
        const int second = traceIndex(
            *trace, QStringLiteral("attempt-start:2:"));
        QVERIFY(cleaned >= 0 && second > cleaned);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        CloseHandle(failedChild);
        stopStandaloneProcess(stealer);
    }

    void secondListenerProofRetriesOnlyProvenCollision()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const quint16 firstPort =
            Xc2BackendManagerTestAccess::allocateCandidate();
        const quint16 secondPort =
            Xc2BackendManagerTestAccess::allocateCandidate();
        QVERIFY(firstPort > 0 && secondPort > 0 && firstPort != secondPort);
        const QString trigger = directory.filePath("second-proof.trigger");
        const QString managedLog = directory.filePath("second-proof.jsonl");
        const QString decoyLog = directory.filePath("second-proof-decoy.jsonl");
        auto trace = std::make_shared<QStringList>();

        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory), {firstPort, secondPort},
            {QStringLiteral("--current-user-delay-ms"),
             QStringLiteral("500"),
             QStringLiteral("--release-listener-after-ready"), trigger},
            managedLog, 10000, 700, 100, 2, {}, {}, nullptr, trace));
        QTRY_VERIFY_WITH_TIMEOUT(requestTargets(managedLog).contains(
            QStringLiteral("/xc2/1.0/auth/currentUser")), 4000);
        const QList<qint64> firstListeners = eventPids(
            managedLog, QStringLiteral("LISTENING"));
        QVERIFY(!firstListeners.isEmpty());
        HANDLE firstChild = openStableHandle(firstListeners.constFirst());
        QVERIFY(firstChild != nullptr && handleIsLive(firstChild));
        QFile triggerFile(trigger);
        QVERIFY(triggerFile.open(QIODevice::WriteOnly));
        triggerFile.write("release");
        triggerFile.close();
        QTRY_COMPARE_WITH_TIMEOUT(eventCount(
            managedLog, QStringLiteral("LISTENER_RELEASED")), 1, 3000);

        QProcess decoy;
        QVERIFY(startStandaloneFake(decoy, firstPort, decoyLog));
        QTRY_COMPARE_WITH_TIMEOUT(eventCount(
            decoyLog, QStringLiteral("LISTENING")), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 12000);
        QCOMPARE(manager.endpoints().restBaseUrl.port(), secondPort);
        QVERIFY(handleIsSignaled(firstChild));
        QCOMPARE(decoy.state(), QProcess::Running);
        QCOMPARE(eventCount(decoyLog, QStringLiteral("REQUEST")), 0);
        QCOMPARE(eventCount(decoyLog, QStringLiteral("SHUTDOWN")), 0);
        const int cleaned = traceIndex(
            *trace, QStringLiteral("attempt-cleanup-complete:1"));
        const int second = traceIndex(
            *trace, QStringLiteral("attempt-start:2:"));
        QVERIFY(cleaned >= 0 && second > cleaned);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        CloseHandle(firstChild);
        stopStandaloneProcess(decoy);
    }

    void threePortCollisionsExhaustWithoutResidue()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        std::array<QProcess, 3> stealers;
        QList<quint16> ports;
        for (qsizetype index = 0; index < 3; ++index) {
            const quint16 port =
                Xc2BackendManagerTestAccess::allocateCandidate();
            QVERIFY(port > 0 && !ports.contains(port));
            ports.append(port);
            const QString log = directory.filePath(
                QStringLiteral("stealer-%1.jsonl").arg(index));
            QVERIFY(startStandaloneFake(
                stealers[index], port, log,
                {QStringLiteral("--health-delay-ms"),
                 QStringLiteral("400")}));
            QTRY_COMPARE_WITH_TIMEOUT(eventCount(log, QStringLiteral("LISTENING")),
                                      1, 3000);
        }
        auto argumentCalls = std::make_shared<int>(0);

        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        const QString managedLog = directory.filePath("managed.jsonl");
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory), ports, {},
            managedLog, 10000, 800, 100, 3,
            argumentCalls));
        std::array<HANDLE, 3> failedAttempts{};
        for (qsizetype index = 0; index < 3; ++index) {
            QTRY_COMPARE_WITH_TIMEOUT(
                eventCount(managedLog, QStringLiteral("BIND_FAILED")),
                static_cast<int>(index + 1), 5000);
            const QList<qint64> pids = eventPids(
                managedLog, QStringLiteral("BIND_FAILED"));
            QCOMPARE(pids.size(), index + 1);
            failedAttempts[index] = openStableHandle(pids.at(index));
            QVERIFY(failedAttempts[index] != nullptr);
            QVERIFY(handleIsLive(failedAttempts[index]));
        }
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 12000);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(*argumentCalls, 3);
        QVERIFY(!manager.ownsProcess());
        for (qsizetype index = 0; index < 3; ++index) {
            QVERIFY(handleIsSignaled(failedAttempts[index]));
            CloseHandle(failedAttempts[index]);
            QCOMPARE(stealers[index].state(), QProcess::Running);
            QCOMPARE(eventCount(directory.filePath(
                         QStringLiteral("stealer-%1.jsonl").arg(index)),
                     QStringLiteral("SHUTDOWN")), 0);
            stopStandaloneProcess(stealers[index]);
        }
    }

    void shutdownConnectionProvesServerFourTupleBeforeWriting()
    {
        using Row = Xc2BackendManagerTestAccess::RawTcpConnectionRow;
        const quint16 tableServerPort = 49123;
        const quint16 tableClientPort = 53111;
        const quint32 tablePid = 4242;
        const Row ownedRow{
            MIB_TCP_STATE_ESTAB,
            htonl(0x7f000001U),
            static_cast<quint32>(htons(tableServerPort)),
            htonl(0x7f000001U),
            static_cast<quint32>(htons(tableClientPort)),
            tablePid,
        };
        QVERIFY(Xc2BackendManagerTestAccess::shutdownRowsOwned(
            {ownedRow}, tableServerPort, tableClientPort, tablePid));
        QVERIFY(!Xc2BackendManagerTestAccess::shutdownRowsOwned(
            {ownedRow, ownedRow}, tableServerPort, tableClientPort,
            tablePid));
        Row invalid = ownedRow;
        invalid.localAddress = htonl(0x7f000002U);
        QVERIFY(!Xc2BackendManagerTestAccess::shutdownRowsOwned(
            {invalid}, tableServerPort, tableClientPort, tablePid));
        invalid = ownedRow;
        invalid.remotePort = ownedRow.localPort;
        invalid.localPort = ownedRow.remotePort;
        QVERIFY(!Xc2BackendManagerTestAccess::shutdownRowsOwned(
            {invalid}, tableServerPort, tableClientPort, tablePid));
        invalid = ownedRow;
        invalid.localPort = tableServerPort;
        QVERIFY(!Xc2BackendManagerTestAccess::shutdownRowsOwned(
            {invalid}, tableServerPort, tableClientPort, tablePid));
        invalid = ownedRow;
        invalid.owningPid = tablePid + 1;
        QVERIFY(!Xc2BackendManagerTestAccess::shutdownRowsOwned(
            {invalid}, tableServerPort, tableClientPort, tablePid));
        invalid = ownedRow;
        invalid.remoteAddress = htonl(0x7f000002U);
        QVERIFY(!Xc2BackendManagerTestAccess::shutdownRowsOwned(
            {invalid}, tableServerPort, tableClientPort, tablePid));
        invalid = ownedRow;
        invalid.localAddress = htonl(0U);
        QVERIFY(!Xc2BackendManagerTestAccess::shutdownRowsOwned(
            {invalid}, tableServerPort, tableClientPort, tablePid));
        invalid = ownedRow;
        invalid.state = MIB_TCP_STATE_SYN_RCVD;
        QVERIFY(!Xc2BackendManagerTestAccess::shutdownRowsOwned(
            {invalid}, tableServerPort, tableClientPort, tablePid));
        Row conflicting = ownedRow;
        conflicting.owningPid = tablePid + 1;
        QVERIFY(!Xc2BackendManagerTestAccess::shutdownRowsOwned(
            {ownedRow, conflicting}, tableServerPort, tableClientPort,
            tablePid));
        Row unrelated = ownedRow;
        unrelated.localPort = static_cast<quint32>(
            htons(static_cast<quint16>(tableServerPort + 1)));
        QVERIFY(Xc2BackendManagerTestAccess::shutdownRowsOwned(
            {ownedRow, unrelated}, tableServerPort, tableClientPort,
            tablePid));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const quint16 port = Xc2BackendManagerTestAccess::allocateCandidate();
        const QString eventLog = directory.filePath("shutdown.jsonl");
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory), {port}, {}, eventLog,
            6000, 1200, 200, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 8000);
        HANDLE child = openStableHandle(manager.ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);

        QTRY_COMPARE_WITH_TIMEOUT(eventCount(eventLog, QStringLiteral("SHUTDOWN")),
                                  1, 2000);
        const QList<QByteArray> requests = eventBytes(
            eventLog, QStringLiteral("SHUTDOWN"));
        QCOMPARE(requests.size(), 1);
        const QByteArray expected = QByteArrayLiteral(
            "POST /xc2/1.0/serviceStatus/shutdown HTTP/1.1\r\nHost: 127.0.0.1:")
            + QByteArray::number(port)
            + QByteArrayLiteral(
                "\r\nContent-Type: application/x-www-form-urlencoded"
                "\r\nContent-Length: 0\r\nConnection: close"
                "\r\nCookie: XC2SESSION=sidecar\r\n\r\n");
        QCOMPARE(requests.constFirst(), expected);
        QVERIFY(handleIsSignaled(child));
        CloseHandle(child);
    }

    void stableHandleIsRecheckedAfterReadyAndShutdownSnapshots()
    {
        {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            Xc2BackendManager manager;
            auto hookCalled = std::make_shared<int>(0);
            QSignalSpy ready(&manager, &Xc2BackendManager::ready);
            QSignalSpy failed(&manager, &Xc2BackendManager::failed);
            QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
            const auto terminateAfterSnapshot = [&manager, hookCalled] {
                ++*hookCalled;
                QVERIFY(terminateProcess(manager.ownedProcessId(), 31));
            };
            QVERIFY(Xc2BackendManagerTestAccess::startFake(
                manager, privateLockPath(directory),
                {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
                directory.filePath("ready-handle.jsonl"),
                5000, 500, 100, 1, {}, {}, nullptr, {}, false, false,
                terminateAfterSnapshot));
            QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
            QCOMPARE(*hookCalled, 1);
            QCOMPARE(ready.count(), 0);
            QCOMPARE(failed.count(), 1);
        }

        {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const QString eventLog =
                directory.filePath("shutdown-handle.jsonl");
            Xc2BackendManager manager;
            auto hookCalled = std::make_shared<int>(0);
            QSignalSpy ready(&manager, &Xc2BackendManager::ready);
            QSignalSpy failed(&manager, &Xc2BackendManager::failed);
            QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
            const auto terminateAfterSnapshot = [&manager, hookCalled] {
                ++*hookCalled;
                QVERIFY(terminateProcess(manager.ownedProcessId(), 32));
            };
            QVERIFY(Xc2BackendManagerTestAccess::startFake(
                manager, privateLockPath(directory),
                {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
                eventLog, 5000, 500, 100, 1, {}, {}, nullptr, {},
                false, false, {}, terminateAfterSnapshot));
            QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
            const int rawBefore = eventCount(
                eventLog, QStringLiteral("RAW_BYTES"));
            const int acceptedBefore = eventCount(
                eventLog, QStringLiteral("CONNECTION_ACCEPTED"));
            manager.stop();
            QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
            QCOMPARE(*hookCalled, 1);
            QCOMPARE(failed.count(), 1);
            QCOMPARE(eventCount(eventLog, QStringLiteral("SHUTDOWN")), 0);
            QCOMPARE(eventCount(eventLog, QStringLiteral("RAW_BYTES")),
                     rawBefore);
            QCOMPARE(eventCount(eventLog,
                                QStringLiteral("CONNECTION_ACCEPTED")),
                     acceptedBefore + 1);
        }
    }

    void absoluteDeadlinesRejectLateReadyAndShutdownWrite()
    {
        {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            Xc2BackendManager manager;
            QSignalSpy ready(&manager, &Xc2BackendManager::ready);
            QSignalSpy failed(&manager, &Xc2BackendManager::failed);
            QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
            const auto blockPastDeadline = [] { QThread::msleep(350); };
            QVERIFY(Xc2BackendManagerTestAccess::startFake(
                manager, privateLockPath(directory),
                {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
                directory.filePath("late-ready.jsonl"),
                300, 300, 100, 1, {}, {}, nullptr, {}, false, false,
                blockPastDeadline));
            QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
            QCOMPARE(ready.count(), 0);
            QCOMPARE(failed.count(), 1);
        }

        {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const QString eventLog =
                directory.filePath("late-shutdown.jsonl");
            Xc2BackendManager manager;
            QSignalSpy ready(&manager, &Xc2BackendManager::ready);
            QSignalSpy failed(&manager, &Xc2BackendManager::failed);
            QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
            const auto blockPastDeadline = [] { QThread::msleep(250); };
            QVERIFY(Xc2BackendManagerTestAccess::startFake(
                manager, privateLockPath(directory),
                {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
                eventLog, 5000, 150, 100, 1, {}, {}, nullptr, {},
                false, false, {}, blockPastDeadline));
            QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
            const int rawBefore = eventCount(
                eventLog, QStringLiteral("RAW_BYTES"));
            const int closedBefore = connectionClosedByteTotals(eventLog).size();
            manager.stop();
            QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
            QCOMPARE(failed.count(), 1);
            QCOMPARE(eventCount(eventLog, QStringLiteral("SHUTDOWN")), 0);
            QCOMPARE(eventCount(eventLog, QStringLiteral("RAW_BYTES")),
                     rawBefore);
            QTRY_COMPARE_WITH_TIMEOUT(
                connectionClosedByteTotals(eventLog).size(),
                closedBefore + 1, 3000);
            QCOMPARE(connectionClosedByteTotals(eventLog).constLast(), 0);
        }
    }

    void duplicateAndConflictingShutdownProofsWriteZeroRawBytes()
    {
        using Mutation = Xc2BackendManagerTestAccess::ShutdownProofMutation;
        for (const Mutation mutation : {
                 Mutation::Duplicate,
                 Mutation::Conflicting,
             }) {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const QString eventLog = directory.filePath(
                mutation == Mutation::Duplicate
                    ? "duplicate-proof.jsonl"
                    : "conflicting-proof.jsonl");
            Xc2BackendManager manager;
            QSignalSpy ready(&manager, &Xc2BackendManager::ready);
            QSignalSpy failed(&manager, &Xc2BackendManager::failed);
            QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
            QVERIFY(Xc2BackendManagerTestAccess::startFake(
                manager, privateLockPath(directory),
                {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
                eventLog, 5000, 500, 100, 1, {}, {}, nullptr, {},
                false, false, {}, {}, false, 0, mutation));
            QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
            const int rawBefore = eventCount(
                eventLog, QStringLiteral("RAW_BYTES"));
            const int closedBefore = connectionClosedByteTotals(eventLog).size();
            manager.stop();
            QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
            QCOMPARE(failed.count(), 1);
            QCOMPARE(eventCount(eventLog, QStringLiteral("SHUTDOWN")), 0);
            QCOMPARE(eventCount(eventLog, QStringLiteral("RAW_BYTES")),
                     rawBefore);
            QTRY_COMPARE_WITH_TIMEOUT(
                connectionClosedByteTotals(eventLog).size(),
                closedBefore + 1, 3000);
            QCOMPARE(connectionClosedByteTotals(eventLog).constLast(), 0);
        }
    }

    void shutdownCookieExportFailureWritesZeroBytes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString eventLog = directory.filePath("cookie-failure.jsonl");
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
            eventLog, 5000, 500, 100, 1, {}, {}, nullptr, {}, true));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        HANDLE child = openStableHandle(manager.ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 4000);
        QCOMPARE(eventCount(eventLog, QStringLiteral("SHUTDOWN")), 0);
        QCOMPARE(requestTargets(eventLog).count(
                     QStringLiteral("/xc2/1.0/serviceStatus/shutdown")),
                 0);
        QVERIFY(handleIsSignaled(child));
        CloseHandle(child);
    }

    void perUserLockRejectsSecondManagerUntilChildFinished()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const quint16 firstPort =
            Xc2BackendManagerTestAccess::allocateCandidate();
        const quint16 secondPort =
            Xc2BackendManagerTestAccess::allocateCandidate();

        Xc2BackendManager first;
        QSignalSpy firstReady(&first, &Xc2BackendManager::ready);
        QSignalSpy firstStopped(&first, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            first, lockPath, {firstPort}, {}, directory.filePath("first.jsonl"),
            6000, 1000, 200, 1));
        QTRY_COMPARE_WITH_TIMEOUT(firstReady.count(), 1, 8000);
        HANDLE child = openStableHandle(first.ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));

        auto secondCandidates = std::make_shared<int>(0);
        Xc2BackendManager second;
        QSignalSpy secondFailed(&second, &Xc2BackendManager::failed);
        QSignalSpy secondStopped(&second, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            second, lockPath, {secondPort}, {}, directory.filePath("second.jsonl"),
            3000, 500, 100, 1, {}, secondCandidates));
        QTRY_COMPARE_WITH_TIMEOUT(secondFailed.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(secondStopped.count(), 1, 3000);
        QCOMPARE(*secondCandidates, 0);
        QVERIFY(handleIsLive(child));

        first.stop();
        QTRY_COMPARE_WITH_TIMEOUT(firstStopped.count(), 1, 5000);
        QVERIFY(handleIsSignaled(child));
        CloseHandle(child);

        QLockFile recovered(lockPath);
        recovered.setStaleLockTime(0);
        QVERIFY(recovered.tryLock(0));
        recovered.unlock();
    }

    void listenerHandoffWritesZeroBytesToDecoy()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const quint16 port = Xc2BackendManagerTestAccess::allocateCandidate();
        const QString trigger = directory.filePath("release.trigger");
        const QString ownedLog = directory.filePath("owned-handoff.jsonl");
        const QString decoyLog = directory.filePath("handoff-decoy.jsonl");

        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory), {port},
            {QStringLiteral("--release-listener-after-ready"), trigger},
            ownedLog, 6000, 700, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 8000);
        HANDLE child = openStableHandle(manager.ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));

        QFile triggerFile(trigger);
        QVERIFY(triggerFile.open(QIODevice::WriteOnly));
        triggerFile.write("release");
        triggerFile.close();
        QTRY_COMPARE_WITH_TIMEOUT(
            eventCount(ownedLog, QStringLiteral("LISTENER_RELEASED")),
            1, 3000);

        QProcess decoy;
        QVERIFY(startStandaloneFake(decoy, port, decoyLog));
        QTRY_COMPARE_WITH_TIMEOUT(eventCount(decoyLog, QStringLiteral("LISTENING")),
                                  1, 3000);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QCOMPARE(eventCount(ownedLog, QStringLiteral("SHUTDOWN")), 0);
        QCOMPARE(eventCount(decoyLog, QStringLiteral("REQUEST")), 0);
        QCOMPARE(eventCount(decoyLog, QStringLiteral("SHUTDOWN")), 0);
        QCOMPARE(eventCount(decoyLog, QStringLiteral("RAW_BYTES")), 0);
        QTRY_COMPARE_WITH_TIMEOUT(
            connectionClosedByteTotals(decoyLog),
            QList<qint64>({0}), 3000);
        QCOMPARE(decoy.state(), QProcess::Running);
        QVERIFY(handleIsSignaled(child));
        CloseHandle(child);
        stopStandaloneProcess(decoy);
    }

    void ignoreShutdownIsTerminatedThenKilledWithoutResidue()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const quint16 port = Xc2BackendManagerTestAccess::allocateCandidate();
        const QString eventLog = directory.filePath("ignore.jsonl");

        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory), {port},
            {QStringLiteral("--ignore-shutdown")}, eventLog,
            6000, 350, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 8000);
        HANDLE child = openStableHandle(manager.ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));
        QElapsedTimer stopElapsed;
        stopElapsed.start();
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 4000);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QCOMPARE(eventCount(eventLog, QStringLiteral("SHUTDOWN")), 1);
        QVERIFY(handleIsSignaled(child));
        QVERIFY(stopElapsed.elapsed() >= 400);
        QVERIFY(!manager.ownsProcess());
        QLockFile lock(privateLockPath(directory));
        lock.setStaleLockTime(0);
        QVERIFY(lock.tryLock(0));
        lock.unlock();
        QCOMPARE(qvariant_cast<Xc2Error>(
                     failed.constFirst().constFirst()).category,
                 Xc2ErrorCategory::Backend);
        CloseHandle(child);
    }

    void earlyShutdownDeadlineWakeReschedulesToAbsoluteDeadline()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const QString eventLog = directory.filePath("early-wakeup.jsonl");
        auto trace = std::make_shared<QStringList>();
        auto earlyWakeups = std::make_shared<int>(0);
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        const auto earlyWakeupHook =
            [earlyWakeups](std::function<void()> wakeup) {
                ++*earlyWakeups;
                wakeup();
            };
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, lockPath,
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--ignore-shutdown")}, eventLog,
            6000, 250, 100, 1, {}, {}, nullptr, trace,
            false, false, {}, {}, false, 0,
            Xc2BackendManagerTestAccess::ShutdownProofMutation::None,
            0, earlyWakeupHook));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 8000);
        HANDLE child = openStableHandle(manager.ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QCOMPARE(*earlyWakeups, 1);
        QCOMPARE(eventCount(eventLog, QStringLiteral("SHUTDOWN")), 1);
        QVERIFY(traceIndex(*trace,
                           QStringLiteral("shutdown-deadline-rescheduled:"))
                >= 0);
        QVERIFY(traceIndex(*trace,
                           QStringLiteral("shutdown-deadline-expired:wakeup"))
                >= 0);
        QVERIFY(traceIndex(*trace, QStringLiteral("force-kill:")) >= 0);
        QVERIFY(handleIsSignaled(child));
        QVERIFY(lockIsAvailable(lockPath));
        QCOMPARE(manager.state(), Xc2BackendState::Stopped);
        CloseHandle(child);
    }

    void nativeTerminationFailureAndNotifierLagConvergeAtJobBoundary()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const QString eventLog = directory.filePath("bounded-reap.jsonl");
        auto trace = std::make_shared<QStringList>();
        Xc2BackendManager manager;
        HANDLE child = nullptr;
        bool stoppedAfterDeathBeforeUnlock = false;
        QObject::connect(&manager, &Xc2BackendManager::stopped,
                         &manager, [&] {
            stoppedAfterDeathBeforeUnlock = child != nullptr
                && handleIsSignaled(child)
                && !manager.ownsProcess()
                && lockIsAvailable(lockPath);
        });
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, lockPath,
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--ignore-shutdown")}, eventLog,
            5000, 120, 40, 1, {}, {}, nullptr, trace,
            false, false, {}, {}, true, 1500));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        child = openStableHandle(manager.ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));
        QElapsedTimer elapsed;
        elapsed.start();
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 4000);
        QCOMPARE(failed.count(), 1);
        QVERIFY(elapsed.elapsed() < 2500);
        QVERIFY(stoppedAfterDeathBeforeUnlock);
        QCOMPARE(eventCount(eventLog, QStringLiteral("SHUTDOWN")), 1);
        int killEvents = 0;
        int boundaryEvents = 0;
        int lagBypassEvents = 0;
        for (const QString &event : std::as_const(*trace)) {
            if (event.startsWith(QStringLiteral("force-kill:")))
                ++killEvents;
            if (event.startsWith(QStringLiteral("final-kill-boundary:")))
                ++boundaryEvents;
            if (event.startsWith(
                    QStringLiteral("finished-notifier-lag-bypassed:"))) {
                ++lagBypassEvents;
            }
        }
        QCOMPARE(killEvents, 3);
        QCOMPARE(boundaryEvents, 1);
        QCOMPARE(lagBypassEvents, 1);
        QVERIFY(handleIsSignaled(child));
        CloseHandle(child);
    }

    void livePerUserLockOlderThanThirtySecondsIsNeverRemoved()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const QString lockerLog = directory.filePath("live-lock.jsonl");
        const quint16 lockerPort =
            Xc2BackendManagerTestAccess::allocateCandidate();
        QProcess locker;
        QVERIFY(startStandaloneFake(
            locker, lockerPort, lockerLog,
            {QStringLiteral("--hold-lock"), lockPath}));
        QTRY_COMPARE_WITH_TIMEOUT(eventCount(lockerLog, QStringLiteral("LOCKED")),
                                  1, 3000);
        QVERIFY(QFileInfo::exists(lockPath));
        QTest::qWait(31000);

        auto candidateCalls = std::make_shared<int>(0);
        Xc2BackendManager manager;
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, lockPath,
            {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
            directory.filePath("blocked.jsonl"), 3000, 500, 100, 1,
            {}, candidateCalls));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
        QCOMPARE(*candidateCalls, 0);
        QCOMPARE(locker.state(), QProcess::Running);
        QVERIFY(QFileInfo::exists(lockPath));
        stopStandaloneProcess(locker);
    }

    void stalePerUserLockIsRecovered()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const QString lockerLog = directory.filePath("stale-lock.jsonl");
        QProcess locker;
        QVERIFY(startStandaloneFake(
            locker, Xc2BackendManagerTestAccess::allocateCandidate(),
            lockerLog, {QStringLiteral("--hold-lock"), lockPath}));
        QTRY_COMPARE_WITH_TIMEOUT(eventCount(lockerLog, QStringLiteral("LOCKED")),
                                  1, 3000);
        HANDLE lockerHandle = openStableHandle(locker.processId());
        QVERIFY(lockerHandle != nullptr && handleIsLive(lockerHandle));
        stopStandaloneProcess(locker);
        QVERIFY(handleIsSignaled(lockerHandle));

        const quint16 port = Xc2BackendManagerTestAccess::allocateCandidate();
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, lockPath, {port}, {},
            directory.filePath("recovered.jsonl"), 6000, 1000, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 8000);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        CloseHandle(lockerHandle);
    }

    void processErrorRevokesNetworkButRetainsCleanupOwnership()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        Xc2BackendManager manager;
        bool failedStateWasSynchronous = false;
        QObject::connect(&manager, &Xc2BackendManager::stateChanged,
                         &manager, [&](Xc2BackendState state) {
            if (state != Xc2BackendState::Failed)
                return;
            failedStateWasSynchronous =
                manager.endpoints().restBaseUrl.isEmpty()
                && manager.endpoints().webSocketUrl.isEmpty()
                && !manager.currentUser().has_value()
                && manager.ownsProcess()
                && !lockIsAvailable(lockPath);
        });
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, lockPath,
            {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
            directory.filePath("crash.jsonl"), 4000, 500, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        QVERIFY(!manager.endpoints().restBaseUrl.isEmpty());
        QVERIFY(manager.currentUser().has_value());
        QVERIFY(manager.ownsProcess());
        const qint64 childPid = manager.ownedProcessId();
        HANDLE child = openStableHandle(childPid);
        QVERIFY(child != nullptr && handleIsLive(child));
        HANDLE terminator = OpenProcess(PROCESS_TERMINATE, FALSE,
                                        static_cast<DWORD>(childPid));
        QVERIFY(terminator != nullptr);
        QVERIFY(TerminateProcess(terminator, 9));
        CloseHandle(terminator);
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 4000);
        QVERIFY(failedStateWasSynchronous);
        QVERIFY(manager.endpoints().restBaseUrl.isEmpty());
        QVERIFY(!manager.currentUser().has_value());
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 4000);
        QCOMPARE(ready.count(), 1);
        QVERIFY(handleIsSignaled(child));
        QLockFile recovered(lockPath);
        recovered.setStaleLockTime(0);
        QVERIFY(recovered.tryLock(0));
        recovered.unlock();
        CloseHandle(child);
    }

    void jobSetupFailureIsFatalBeforeNetworkAndNeverRetried()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto argumentCalls = std::make_shared<int>(0);
        auto candidateCalls = std::make_shared<int>(0);
        auto trace = std::make_shared<QStringList>();
        const QString eventLog = directory.filePath("job-failure.jsonl");
        Xc2BackendManager manager;
        HANDLE child = nullptr;
        QObject::connect(&manager, &Xc2BackendManager::stateChanged,
                         &manager, [&manager, &child](Xc2BackendState state) {
            if (state == Xc2BackendState::Failed
                && manager.ownedProcessId() > 0 && child == nullptr) {
                child = openStableHandle(manager.ownedProcessId());
            }
        });
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
            eventLog, 5000, 500, 100, 3,
            argumentCalls, candidateCalls, nullptr, trace, false, true));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 4000);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(*argumentCalls, 1);
        QCOMPARE(*candidateCalls, 1);
        QCOMPARE(traceIndex(*trace, QStringLiteral("attempt-start:2:")), -1);
        QVERIFY(traceIndex(*trace,
                           QStringLiteral("direct-executable-proven:1")) >= 0);
        QCOMPARE(traceIndex(*trace, QStringLiteral("job-assigned:1")), -1);
        QCOMPARE(traceIndex(*trace, QStringLiteral("health-request:1")), -1);
        QCOMPARE(eventCount(eventLog, QStringLiteral("REQUEST")), 0);
        QVERIFY(child != nullptr && handleIsSignaled(child));
        QCOMPARE(qvariant_cast<Xc2Error>(
                     failed.constFirst().constFirst()).category,
                 Xc2ErrorCategory::Backend);
        CloseHandle(child);
    }

    void healthPollingIsSingleFlightWithinOneTotalDeadline()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString eventLog = directory.filePath("never-health.jsonl");
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--never-health")}, eventLog,
            1400, 400, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
        QVERIFY(elapsed.elapsed() >= 1200);
        QVERIFY(elapsed.elapsed() < 2800);
        QCOMPARE(ready.count(), 0);
        int maximum = 0;
        int samples = 0;
        for (const QJsonObject &event : eventObjects(eventLog)) {
            if (event.value(QStringLiteral("event")).toString()
                != QStringLiteral("HEALTH_COUNTS"))
                continue;
            maximum = qMax(maximum,
                           event.value(QStringLiteral("maximum")).toInt());
            ++samples;
        }
        QVERIFY(samples >= 1);
        QCOMPARE(maximum, 1);
    }

    void startupTimeoutAbortsReplyAndLateAliveCannotReviveRun()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const quint16 stalePort =
            Xc2BackendManagerTestAccess::allocateCandidate();
        const QString decoyLog = directory.filePath("late-decoy.jsonl");
        const QString managedLog = directory.filePath("late-managed.jsonl");
        QProcess lateServer;
        QVERIFY(startStandaloneFake(
            lateServer, stalePort, decoyLog,
            {QStringLiteral("--late-alive-ms"), QStringLiteral("1400")}));
        QTRY_COMPARE_WITH_TIMEOUT(eventCount(
            decoyLog, QStringLiteral("LISTENING")), 1, 3000);
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);

        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, lockPath, {stalePort}, {}, managedLog,
            350, 300, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(eventCount(
            managedLog, QStringLiteral("BIND_FAILED")), 1, 2000);
        HANDLE staleChild = openStableHandle(eventPids(
            managedLog, QStringLiteral("BIND_FAILED")).constFirst());
        QVERIFY(staleChild != nullptr && handleIsLive(staleChild));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
        QCOMPARE(ready.count(), 0);
        QVERIFY(handleIsSignaled(staleChild));

        quint16 newPort = Xc2BackendManagerTestAccess::allocateCandidate();
        while (newPort == stalePort)
            newPort = Xc2BackendManagerTestAccess::allocateCandidate();
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, lockPath, {newPort}, {},
            directory.filePath("new-run.jsonl"), 5000, 700, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        QCOMPARE(manager.endpoints().restBaseUrl.port(), newPort);
        QTRY_COMPARE_WITH_TIMEOUT(eventCount(
            decoyLog, QStringLiteral("HEALTH_RESPONSE")), 1, 3000);
        QCOMPARE(ready.count(), 1);
        QCOMPARE(lateServer.state(), QProcess::Running);
        QCOMPARE(eventCount(decoyLog, QStringLiteral("SHUTDOWN")), 0);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 2, 5000);
        CloseHandle(staleChild);
        stopStandaloneProcess(lateServer);
    }

    void currentUserAndPermissionsAreValidatedBeforeReady()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        {
            Xc2BackendManager manager;
            QSignalSpy ready(&manager, &Xc2BackendManager::ready);
            QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
            const QString validLog = directory.filePath("valid-user.jsonl");
            QVERIFY(Xc2BackendManagerTestAccess::startFake(
                manager, privateLockPath(directory, "valid.lock"),
                {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
                validLog, 5000, 700, 100, 1));
            QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
            QVERIFY(manager.currentUser().has_value());
            QCOMPARE(manager.currentUser()->loginName,
                     QStringLiteral("test.login"));
            QCOMPARE(manager.currentUser()->name, QStringLiteral("Test User"));
            QCOMPARE(manager.currentUser()->permissions,
                     QStringList({QStringLiteral("diagnostics.read"),
                                  QStringLiteral("UNKNOWN.permission"),
                                  QStringLiteral("diagnostics.read")}));
            const QStringList targets = requestTargets(validLog);
            QVERIFY(targets.size() >= 2);
            QCOMPARE(targets.constLast(),
                     QStringLiteral("/xc2/1.0/auth/currentUser"));
            QCOMPARE(targets.count(
                         QStringLiteral("/xc2/1.0/auth/currentUser")),
                     1);
            for (qsizetype index = 0; index + 1 < targets.size(); ++index) {
                QCOMPARE(targets.at(index),
                         QStringLiteral(
                             "/xc2/1.0/serviceStatus/status"));
            }
            manager.stop();
            QVERIFY(manager.endpoints().restBaseUrl.isEmpty());
            QVERIFY(!manager.currentUser().has_value());
            QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        }

        const QStringList invalidModes{
            QStringLiteral("blank-login"),
            QStringLiteral("blank-name"),
            QStringLiteral("missing-permissions"),
            QStringLiteral("malformed"),
        };
        for (const QString &mode : invalidModes) {
            Xc2BackendManager manager;
            QSignalSpy ready(&manager, &Xc2BackendManager::ready);
            QSignalSpy failed(&manager, &Xc2BackendManager::failed);
            QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
            QVERIFY(Xc2BackendManagerTestAccess::startFake(
                manager, privateLockPath(directory, mode + ".lock"),
                {Xc2BackendManagerTestAccess::allocateCandidate()},
                {QStringLiteral("--user-mode"), mode},
                directory.filePath(mode + ".jsonl"), 5000, 700, 100, 1));
            QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 7000);
            QVERIFY(manager.endpoints().restBaseUrl.isEmpty());
            QVERIFY(manager.endpoints().webSocketUrl.isEmpty());
            QVERIFY(!manager.currentUser().has_value());
            QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
            QCOMPARE(ready.count(), 0);
            QVERIFY(manager.endpoints().restBaseUrl.isEmpty());
            QVERIFY(!manager.currentUser().has_value());
        }
    }

    void emptyPermissionsRemainReadyButAuthorizeNothing()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString eventLog = directory.filePath("empty.jsonl");
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--user-mode"),
             QStringLiteral("empty-permissions")},
            eventLog, 5000, 700, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        QVERIFY(manager.currentUser().has_value());
        QVERIFY(manager.currentUser()->permissions.isEmpty());
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        const QList<QByteArray> shutdown = eventBytes(
            eventLog, QStringLiteral("SHUTDOWN"));
        QCOMPARE(shutdown.size(), 1);
        QVERIFY(!shutdown.constFirst().contains(QByteArrayLiteral("Cookie:")));
    }

    void freshAttemptDoesNotReusePriorShutdownCookie()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const QString firstLog = directory.filePath("cookie-first.jsonl");
        const QString secondLog = directory.filePath("cookie-second.jsonl");
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);

        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, lockPath,
            {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
            firstLog, 5000, 700, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        const QList<QByteArray> firstShutdown = eventBytes(
            firstLog, QStringLiteral("SHUTDOWN"));
        QCOMPARE(firstShutdown.size(), 1);
        QCOMPARE(firstShutdown.constFirst().count(
                     QByteArrayLiteral("Cookie: XC2SESSION=sidecar\r\n")),
                 1);

        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, lockPath,
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--user-mode"),
             QStringLiteral("empty-permissions")},
            secondLog, 5000, 700, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 7000);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 2, 5000);
        const QList<QByteArray> secondShutdown = eventBytes(
            secondLog, QStringLiteral("SHUTDOWN"));
        QCOMPARE(secondShutdown.size(), 1);
        QVERIFY(!secondShutdown.constFirst().contains(
            QByteArrayLiteral("Cookie:")));
    }

    void earlyCrashReportsBackendErrorAndLeavesNoOwnedRun()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--exit-before-ready")},
            directory.filePath("early.jsonl"), 3000, 300, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 4000);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 4000);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(manager.state(), Xc2BackendState::Stopped);
        QVERIFY(!manager.ownsProcess());
        QVERIFY(manager.endpoints().restBaseUrl.isEmpty());
    }

    void unexpectedEofPartialRevokesBeforeOutputCallback()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString trigger = directory.filePath("exit.trigger");
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QStringList order;
        bool sawPartial = false;
        bool publicationCleared = false;
        bool failureWasImmutable = false;
        QObject::connect(&manager, &Xc2BackendManager::failed,
                         &manager, [&order] { order.append("failed"); });
        QObject::connect(&manager, &Xc2BackendManager::outputLine,
                         &manager, [&](bool standardError,
                                       const QString &line) {
            if (standardError
                || line != QStringLiteral("unexpected-eof-partial")) {
                return;
            }
            sawPartial = true;
            order.append("output");
            publicationCleared =
                manager.state() == Xc2BackendState::Failed
                && manager.endpoints().restBaseUrl.isEmpty()
                && manager.endpoints().webSocketUrl.isEmpty()
                && !manager.currentUser().has_value();
            manager.stop();
            failureWasImmutable = manager.state() == Xc2BackendState::Failed;
        });
        QObject::connect(&manager, &Xc2BackendManager::stopped,
                         &manager, [&order] { order.append("stopped"); });
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--exit-trigger"), trigger},
            directory.filePath("unexpected-exit.jsonl"),
            5000, 500, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        HANDLE child = openStableHandle(manager.ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));
        QFile triggerFile(trigger);
        QVERIFY(triggerFile.open(QIODevice::WriteOnly));
        triggerFile.write("exit");
        triggerFile.close();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QCOMPARE(failed.count(), 1);
        QVERIFY(sawPartial);
        QVERIFY(publicationCleared);
        QVERIFY(failureWasImmutable);
        QCOMPARE(order.count("failed"), 1);
        QCOMPARE(order.count("output"), 1);
        QCOMPARE(order.count("stopped"), 1);
        QVERIFY(order.indexOf("failed") < order.indexOf("output"));
        QVERIFY(order.indexOf("output") < order.indexOf("stopped"));
        QVERIFY(handleIsSignaled(child));
        CloseHandle(child);
    }

    void unexpectedNewlineOutputRevokesBeforeOutputCallbackAndDelete()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const QString trigger = directory.filePath("newline-exit.trigger");
        const QString release = directory.filePath("newline-exit.release");
        const auto lifecycleTrace = std::make_shared<QStringList>();
        auto *manager = new Xc2BackendManager;
        QPointer<Xc2BackendManager> guardedManager(manager);
        int readyCount = 0;
        int failedCount = 0;
        bool releaseIssued = false;
        bool sawRunningLine = false;
        bool runningLinePublishedWhileLive = false;
        bool sawFinalLine = false;
        bool publicationCleared = false;
        bool failedBeforeOutput = false;
        bool deletedFromOutput = false;
        HANDLE child = nullptr;
        QStringList order;
        QObject::connect(manager, &Xc2BackendManager::ready,
                         manager, [&] { ++readyCount; });
        QObject::connect(manager, &Xc2BackendManager::failed,
                         manager, [&] {
            ++failedCount;
            order.append(QStringLiteral("failed"));
        });
        QObject::connect(manager, &Xc2BackendManager::outputLine,
                         manager, [&](bool standardError,
                                      const QString &line) {
            if (standardError)
                return;
            if (line == QStringLiteral("running-before-final")) {
                sawRunningLine = true;
                runningLinePublishedWhileLive =
                    manager->state() == Xc2BackendState::Ready
                    && child != nullptr && handleIsLive(child)
                    && !manager->endpoints().restBaseUrl.isEmpty();
                order.append(QStringLiteral("running"));
                return;
            }
            if (line != QStringLiteral("unexpected-final-line"))
                return;
            sawFinalLine = true;
            order.append(QStringLiteral("final"));
            publicationCleared =
                manager->endpoints().restBaseUrl.isEmpty()
                && manager->endpoints().webSocketUrl.isEmpty()
                && !manager->currentUser().has_value();
            failedBeforeOutput = manager->state() == Xc2BackendState::Failed
                && failedCount == 1;
            if (releaseIssued) {
                deletedFromOutput = true;
                manager->stop();
                manager->deleteLater();
            }
        });
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            *manager, lockPath,
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--newline-exit-trigger"), trigger,
             QStringLiteral("--newline-exit-release-trigger"), release},
            directory.filePath("newline-exit.jsonl"),
            5000, 500, 100, 1, {}, {}, nullptr, lifecycleTrace));
        QTRY_COMPARE_WITH_TIMEOUT(readyCount, 1, 7000);
        child = openStableHandle(manager->ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));
        QFile triggerFile(trigger);
        QVERIFY(triggerFile.open(QIODevice::WriteOnly));
        triggerFile.write("exit");
        triggerFile.close();
        const auto tailHeldCount = [&](const QString &stream) {
            qsizetype count = 0;
            for (const QString &event : std::as_const(*lifecycleTrace)) {
                if (event.startsWith(QStringLiteral("output-tail-held:"))
                    && event.endsWith(QLatin1Char(':') + stream))
                    ++count;
            }
            return count;
        };
        QTRY_VERIFY_WITH_TIMEOUT(
            (sawRunningLine
             && tailHeldCount(QStringLiteral("stdout")) >= 2)
                || sawFinalLine,
            5000);
        const bool finalHeldBeforeRelease =
            tailHeldCount(QStringLiteral("stdout")) >= 2
            && sawRunningLine && !sawFinalLine
            && handleIsLive(child)
            && manager->state() == Xc2BackendState::Ready
            && !manager->endpoints().restBaseUrl.isEmpty()
            && manager->currentUser().has_value();
        releaseIssued = true;
        QFile releaseFile(release);
        QVERIFY(releaseFile.open(QIODevice::WriteOnly));
        releaseFile.write("exit");
        releaseFile.close();
        QTRY_VERIFY_WITH_TIMEOUT(sawFinalLine, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(handleIsSignaled(child), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(lockIsAvailable(lockPath), 3000);
        if (guardedManager)
            guardedManager->deleteLater();
        QTRY_VERIFY_WITH_TIMEOUT(guardedManager.isNull(), 5000);
        QVERIFY(finalHeldBeforeRelease);
        QVERIFY(sawRunningLine);
        QVERIFY(runningLinePublishedWhileLive);
        QVERIFY(publicationCleared);
        QVERIFY(failedBeforeOutput);
        QVERIFY(deletedFromOutput);
        QCOMPARE(failedCount, 1);
        QCOMPARE(order.count(QStringLiteral("running")), 1);
        QCOMPARE(order.count(QStringLiteral("failed")), 1);
        QCOMPARE(order.count(QStringLiteral("final")), 1);
        QVERIFY(order.indexOf(QStringLiteral("running"))
                < order.indexOf(QStringLiteral("failed")));
        QVERIFY(order.indexOf(QStringLiteral("failed"))
                < order.indexOf(QStringLiteral("final")));
        CloseHandle(child);
    }

    void deliberateStopFlushesHeldCompleteLineWithoutLoss()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const QString trigger = directory.filePath("running-output.trigger");
        const QString release = directory.filePath("unused-exit.release");
        const auto lifecycleTrace = std::make_shared<QStringList>();
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        HANDLE child = nullptr;
        QStringList standardOutputLines;
        QStringList standardErrorLines;
        QList<Xc2BackendState> standardOutputStates;
        QList<Xc2BackendState> standardErrorStates;
        QList<bool> standardOutputHandleLive;
        QList<bool> standardErrorHandleLive;
        QObject::connect(&manager, &Xc2BackendManager::outputLine,
                         &manager, [&](bool standardError,
                                       const QString &line) {
            QStringList &lines = standardError
                ? standardErrorLines : standardOutputLines;
            QList<Xc2BackendState> &states = standardError
                ? standardErrorStates : standardOutputStates;
            QList<bool> &handleLive = standardError
                ? standardErrorHandleLive : standardOutputHandleLive;
            lines.append(line);
            states.append(manager.state());
            handleLive.append(child != nullptr && handleIsLive(child));
        });
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, lockPath,
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--newline-exit-trigger"), trigger,
             QStringLiteral("--newline-exit-release-trigger"), release},
            directory.filePath("deliberate-stop-output.jsonl"),
            5000, 500, 100, 1, {}, {}, nullptr, lifecycleTrace));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        child = openStableHandle(manager.ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));
        QFile triggerFile(trigger);
        QVERIFY(triggerFile.open(QIODevice::WriteOnly));
        triggerFile.write("output");
        triggerFile.close();
        QTRY_COMPARE_WITH_TIMEOUT(standardOutputLines.size(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(standardErrorLines.size(), 1, 5000);
        QCOMPARE(standardOutputLines.constFirst(),
                 QStringLiteral("running-before-final"));
        QCOMPARE(standardErrorLines.constFirst(),
                 QStringLiteral("running-stderr-before-final"));
        QCOMPARE(standardOutputStates.constFirst(), Xc2BackendState::Ready);
        QCOMPARE(standardErrorStates.constFirst(), Xc2BackendState::Ready);
        QVERIFY(standardOutputHandleLive.constFirst());
        QVERIFY(standardErrorHandleLive.constFirst());
        QVERIFY(handleIsLive(child));
        QCOMPARE(manager.state(), Xc2BackendState::Ready);
        QVERIFY(!manager.recentOutput(false).contains(
            QStringLiteral("unexpected-final-line")));
        QVERIFY(!manager.recentOutput(true).contains(
            QStringLiteral("unexpected-stderr-final-line")));
        qsizetype stdoutHeldCount = 0;
        qsizetype stderrHeldCount = 0;
        for (const QString &event : std::as_const(*lifecycleTrace)) {
            if (!event.startsWith(QStringLiteral("output-tail-held:")))
                continue;
            if (event.endsWith(QStringLiteral(":stdout")))
                ++stdoutHeldCount;
            if (event.endsWith(QStringLiteral(":stderr")))
                ++stderrHeldCount;
        }
        QCOMPARE(stdoutHeldCount, 2);
        QCOMPARE(stderrHeldCount, 2);

        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(standardOutputLines,
                 QStringList({QStringLiteral("running-before-final"),
                              QStringLiteral("unexpected-final-line")}));
        QCOMPARE(standardErrorLines,
                 QStringList({
                     QStringLiteral("running-stderr-before-final"),
                     QStringLiteral("unexpected-stderr-final-line")}));
        QCOMPARE(standardOutputStates,
                 QList<Xc2BackendState>({Xc2BackendState::Ready,
                                         Xc2BackendState::Stopping}));
        QCOMPARE(standardErrorStates,
                 QList<Xc2BackendState>({Xc2BackendState::Ready,
                                         Xc2BackendState::Stopping}));
        QCOMPARE(manager.recentOutput(false), standardOutputLines);
        QCOMPARE(manager.recentOutput(true), standardErrorLines);
        QVERIFY(handleIsSignaled(child));
        QVERIFY(lockIsAvailable(lockPath));
        CloseHandle(child);
    }

    void nestedSameStreamCompleteOutputPreservesWireOrder_data()
    {
        QTest::addColumn<bool>("standardError");
        QTest::newRow("stdout") << false;
        QTest::newRow("stderr") << true;
    }

    void nestedSameStreamCompleteOutputPreservesWireOrder()
    {
        QFETCH(bool, standardError);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const QString streamName = standardError ? QStringLiteral("stderr")
                                                 : QStringLiteral("stdout");
        const QString heldSuffix = QLatin1Char(':') + streamName;
        const auto lifecycleTrace = std::make_shared<QStringList>();
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QStringList lines;
        QList<Xc2BackendState> states;
        QList<bool> handleLive;
        HANDLE child = nullptr;
        qsizetype heldCount = 0;
        qsizetype nestedHeldTarget = 0;
        QEventLoop *nestedLoop = nullptr;
        bool nestedTriggered = false;
        bool nestedCompleted = false;
        bool nestedTimedOut = false;
        bool nestedStageWritten = false;
        const auto lifecycleObserver = [&](const QString &event) {
            if (!event.startsWith(QStringLiteral("output-tail-held:"))
                || !event.endsWith(heldSuffix)) {
                return;
            }
            ++heldCount;
            if (nestedLoop != nullptr && heldCount >= nestedHeldTarget)
                nestedLoop->quit();
        };
        QObject::connect(&manager, &Xc2BackendManager::outputLine,
                         &manager, [&](bool emittedStandardError,
                                       const QString &line) {
            if (emittedStandardError != standardError)
                return;
            lines.append(line);
            states.append(manager.state());
            handleLive.append(child != nullptr && handleIsLive(child));
            if (line != QStringLiteral("P") || nestedTriggered)
                return;
            nestedTriggered = true;
            nestedHeldTarget = heldCount + 1;
            QEventLoop nested;
            nestedLoop = &nested;
            nestedStageWritten = writeStagedOutput(
                directory.path(), 3, standardError, QByteArrayLiteral("B\n"));
            if (nestedStageWritten) {
                QTimer::singleShot(5000, &nested, [&] {
                    nestedTimedOut = true;
                    nested.quit();
                });
                nested.exec();
            }
            nestedLoop = nullptr;
            nestedCompleted = nestedStageWritten && !nestedTimedOut;
        });
        QVERIFY(Xc2BackendManagerTestAccess::startStagedOutputFake(
            manager, lockPath,
            Xc2BackendManagerTestAccess::allocateCandidate(), directory.path(),
            directory.filePath("nested-complete.jsonl"), lifecycleTrace,
            lifecycleObserver));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        child = openStableHandle(manager.ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));
        QVERIFY(writeStagedOutput(directory.path(), 1, standardError,
                                  QByteArrayLiteral("P\n")));
        QTRY_COMPARE_WITH_TIMEOUT(heldCount, qsizetype(1), 5000);
        QCOMPARE(lines.size(), 0);
        QVERIFY(writeStagedOutput(directory.path(), 2, standardError,
                                  QByteArrayLiteral("A\n")));
        QTRY_VERIFY_WITH_TIMEOUT(nestedTriggered && nestedCompleted, 7000);
        const QStringList linesBeforeStop = lines;

        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QCOMPARE(failed.count(), 0);
        QVERIFY(nestedStageWritten);
        QVERIFY(!nestedTimedOut);
        QCOMPARE(lines,
                 QStringList({QStringLiteral("P"), QStringLiteral("A"),
                              QStringLiteral("B")}));
        QCOMPARE(linesBeforeStop,
                 QStringList({QStringLiteral("P"), QStringLiteral("A")}));
        QCOMPARE(states,
                 QList<Xc2BackendState>({Xc2BackendState::Ready,
                                         Xc2BackendState::Ready,
                                         Xc2BackendState::Stopping}));
        QCOMPARE(manager.recentOutput(standardError), lines);
        QCOMPARE(handleLive.size(), 3);
        QVERIFY(handleLive.at(0));
        QVERIFY(handleLive.at(1));
        QVERIFY(handleIsSignaled(child));
        QVERIFY(lockIsAvailable(lockPath));
        CloseHandle(child);
    }

    void nestedTerminalPreservesHeldCompleteBeforeEofPartial_data()
    {
        QTest::addColumn<bool>("standardError");
        QTest::newRow("stdout") << false;
        QTest::newRow("stderr") << true;
    }

    void nestedTerminalPreservesHeldCompleteBeforeEofPartial()
    {
        QFETCH(bool, standardError);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const QString streamName = standardError ? QStringLiteral("stderr")
                                                 : QStringLiteral("stdout");
        const QString heldSuffix = QLatin1Char(':') + streamName;
        const auto lifecycleTrace = std::make_shared<QStringList>();
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QStringList lines;
        QStringList order;
        QList<Xc2BackendState> states;
        HANDLE child = nullptr;
        qsizetype heldCount = 0;
        QEventLoop *nestedLoop = nullptr;
        bool nestedTriggered = false;
        bool nestedCompleted = false;
        bool nestedTimedOut = false;
        bool nestedStageWritten = false;
        bool outputFinalized = false;
        const auto lifecycleObserver = [&](const QString &event) {
            if (event.startsWith(QStringLiteral("output-tail-held:"))
                && event.endsWith(heldSuffix)) {
                ++heldCount;
            }
            if (!event.startsWith(QStringLiteral("output-finalized:")))
                return;
            outputFinalized = true;
            if (nestedLoop != nullptr)
                nestedLoop->quit();
        };
        QObject::connect(&manager, &Xc2BackendManager::failed,
                         &manager, [&] { order.append("failed"); });
        QObject::connect(&manager, &Xc2BackendManager::stopped,
                         &manager, [&] { order.append("stopped"); });
        QObject::connect(&manager, &Xc2BackendManager::outputLine,
                         &manager, [&](bool emittedStandardError,
                                       const QString &line) {
            if (emittedStandardError != standardError)
                return;
            lines.append(line);
            states.append(manager.state());
            order.append(QStringLiteral("output:") + line);
            if (line != QStringLiteral("P") || nestedTriggered)
                return;
            nestedTriggered = true;
            QEventLoop nested;
            nestedLoop = &nested;
            nestedStageWritten = writeStagedOutput(
                directory.path(), 3, standardError,
                QByteArrayLiteral("EOF-partial"), true);
            if (nestedStageWritten) {
                QTimer::singleShot(5000, &nested, [&] {
                    nestedTimedOut = true;
                    nested.quit();
                });
                nested.exec();
            }
            nestedLoop = nullptr;
            nestedCompleted = nestedStageWritten && outputFinalized
                && !nestedTimedOut;
        });
        QVERIFY(Xc2BackendManagerTestAccess::startStagedOutputFake(
            manager, lockPath,
            Xc2BackendManagerTestAccess::allocateCandidate(), directory.path(),
            directory.filePath("nested-terminal.jsonl"), lifecycleTrace,
            lifecycleObserver));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        child = openStableHandle(manager.ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));
        QVERIFY(writeStagedOutput(directory.path(), 1, standardError,
                                  QByteArrayLiteral("P\n")));
        QTRY_COMPARE_WITH_TIMEOUT(heldCount, qsizetype(1), 5000);
        QCOMPARE(lines.size(), 0);
        QVERIFY(writeStagedOutput(directory.path(), 2, standardError,
                                  QByteArrayLiteral("A\n")));
        QTRY_VERIFY_WITH_TIMEOUT(nestedTriggered && nestedCompleted, 7000);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);

        QCOMPARE(failed.count(), 1);
        QVERIFY(nestedStageWritten);
        QVERIFY(outputFinalized);
        QVERIFY(!nestedTimedOut);
        QCOMPARE(lines,
                 QStringList({QStringLiteral("P"), QStringLiteral("A"),
                              QStringLiteral("EOF-partial")}));
        QCOMPARE(states,
                 QList<Xc2BackendState>({Xc2BackendState::Ready,
                                         Xc2BackendState::Failed,
                                         Xc2BackendState::Failed}));
        QCOMPARE(manager.recentOutput(standardError), lines);
        QCOMPARE(order,
                 QStringList({QStringLiteral("output:P"),
                              QStringLiteral("failed"),
                              QStringLiteral("output:A"),
                              QStringLiteral("output:EOF-partial"),
                              QStringLiteral("stopped")}));
        QVERIFY(handleIsSignaled(child));
        QVERIFY(lockIsAvailable(lockPath));
        CloseHandle(child);
    }

    void ownerlessLargeOutputDrainsBeforeCleanup_data()
    {
        QTest::addColumn<bool>("standardError");
        QTest::newRow("stdout") << false;
        QTest::newRow("stderr") << true;
    }

    void ownerlessLargeOutputDrainsBeforeCleanup()
    {
        QFETCH(bool, standardError);
        ReaperRepostHookReset reset;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const QString eventLog = directory.filePath("ownerless-large.jsonl");
        const auto trace = std::make_shared<QStringList>();
        auto *manager = new Xc2BackendManager;
        QPointer<Xc2BackendManager> guardedManager(manager);
        QSignalSpy ready(manager, &Xc2BackendManager::ready);
        HANDLE child = nullptr;
        QStringList publicLines;
        bool deletedFromOutput = false;
        bool nestedCompleted = false;
        bool nestedTimedOut = false;
        QString nestedTerminalBoundary;
        QEventLoop *nestedLoop = nullptr;
        bool quitHeldSynchronously = false;
        bool childSignaledAtQuit = false;
        bool lockAvailableAtQuit = false;
        QuitSink sink;
        sink.onQuit = [&] {
            trace->append(QStringLiteral("downstream-quit"));
            childSignaledAtQuit = handleIsSignaled(child);
            lockAvailableAtQuit = lockIsAvailable(lockPath);
        };
        qApp->installEventFilter(&sink);
        const auto lifecycleObserver = [&](const QString &event) {
            if (nestedLoop == nullptr
                || (!event.startsWith(
                        QStringLiteral("output-raw-drain-blocked:"))
                    && !event.startsWith(
                        QStringLiteral("output-finalized:")))) {
                return;
            }
            nestedTerminalBoundary = event;
            nestedLoop->quit();
        };
        QObject::connect(manager, &Xc2BackendManager::outputLine,
                         qApp, [&](bool emittedStandardError,
                                   const QString &line) {
            if (emittedStandardError != standardError)
                return;
            publicLines.append(line);
            if (deletedFromOutput)
                return;
            deletedFromOutput = true;
            trace->append(QStringLiteral("manager-delete-from-output"));
            delete manager;
            manager = nullptr;
            QEvent quit(QEvent::Quit);
            QCoreApplication::sendEvent(qApp, &quit);
            quitHeldSynchronously = sink.count == 0;
            QEventLoop nested;
            nestedLoop = &nested;
            QTimer::singleShot(5000, &nested, [&] {
                nestedTimedOut = true;
                nested.quit();
            });
            nested.exec();
            nestedLoop = nullptr;
            nestedCompleted = !nestedTimedOut
                && !nestedTerminalBoundary.isEmpty();
        });
        QVERIFY(Xc2BackendManagerTestAccess::startStagedOutputFake(
            *manager, lockPath,
            Xc2BackendManagerTestAccess::allocateCandidate(), directory.path(),
            eventLog, trace, lifecycleObserver));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        child = openStableHandle(manager->ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));
        QByteArray payload;
        for (int index = 0; index < 400; ++index) {
            payload += QByteArrayLiteral("line-")
                + QByteArray::number(index).rightJustified(3, '0') + '\n';
        }
        payload += QByteArrayLiteral("terminal-partial");
        QVERIFY(writeStagedOutput(directory.path(), 1, standardError,
                                  payload, true));
        QTRY_VERIFY_WITH_TIMEOUT(guardedManager.isNull(), 5000);
        QTRY_VERIFY2_WITH_TIMEOUT(
            sink.count == 1,
            qPrintable(QStringLiteral("nestedCompleted=%1 nestedTimedOut=%2 "
                                      "boundary=%3 trace=%4")
                           .arg(nestedCompleted)
                           .arg(nestedTimedOut)
                           .arg(nestedTerminalBoundary,
                                trace->join(QStringLiteral(" | ")))),
            10000);
        qApp->removeEventFilter(&sink);

        const int deleteIndex = traceIndex(
            *trace, QStringLiteral("manager-delete-from-output"));
        const int rawDrainedIndex = traceIndex(
            *trace, QStringLiteral("output-raw-drain-complete:"));
        const int finalizedIndex = traceIndex(
            *trace, QStringLiteral("output-finalized:"));
        const int cleanupIndex = traceIndex(
            *trace, QStringLiteral("attempt-cleanup-complete:"));
        const int lockIndex = traceIndex(
            *trace, QStringLiteral("lock-released:"));
        const int reaperIndex = traceIndex(
            *trace, QStringLiteral("reaper-finished:"));
        const int quitIndex = traceIndex(
            *trace, QStringLiteral("downstream-quit"));
        QVERIFY(deletedFromOutput);
        QVERIFY(quitHeldSynchronously);
        QVERIFY2(nestedCompleted,
                 qPrintable(QStringLiteral("nestedTimedOut=%1 boundary=%2 "
                                           "trace=%3")
                                .arg(nestedTimedOut)
                                .arg(nestedTerminalBoundary,
                                     trace->join(QStringLiteral(" | ")))));
        QVERIFY(!nestedTimedOut);
        QVERIFY2(nestedTerminalBoundary.startsWith(
                     QStringLiteral("output-finalized:")),
                 qPrintable(trace->join(QStringLiteral(" | "))));
        QVERIFY2(traceIndex(*trace,
                            QStringLiteral("output-raw-drain-blocked:")) < 0,
                 qPrintable(trace->join(QStringLiteral(" | "))));
        QCOMPARE(publicLines.size(), 1);
        QCOMPARE(eventCount(eventLog, QStringLiteral("STAGED_OUTPUT")), 1);
        QVERIFY(deleteIndex >= 0);
        QVERIFY(rawDrainedIndex > deleteIndex);
        QVERIFY(finalizedIndex > rawDrainedIndex);
        QVERIFY(cleanupIndex > finalizedIndex);
        QVERIFY(lockIndex > cleanupIndex);
        QVERIFY(reaperIndex > lockIndex);
        QVERIFY(quitIndex > reaperIndex);
        QVERIFY(childSignaledAtQuit);
        QVERIFY(lockAvailableAtQuit);
        QVERIFY(handleIsSignaled(child));
        QVERIFY(lockIsAvailable(lockPath));
        CloseHandle(child);
    }

    void cleanupWaitsForDeferredOutputFinalization()
    {
        ReaperRepostHookReset reset;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const auto trace = std::make_shared<QStringList>();
        const auto allowFinalization = std::make_shared<bool>(false);
        auto *manager = new Xc2BackendManager;
        QSignalSpy ready(manager, &Xc2BackendManager::ready);
        HANDLE child = nullptr;
        bool releaseScheduled = false;
        bool gateHeldAtRelease = false;
        bool childSignaledAtQuit = false;
        bool lockAvailableAtQuit = false;
        QuitSink sink;
        sink.onQuit = [&] {
            trace->append(QStringLiteral("downstream-quit"));
            childSignaledAtQuit = handleIsSignaled(child);
            lockAvailableAtQuit = lockIsAvailable(lockPath);
        };
        qApp->installEventFilter(&sink);
        const auto lifecycleObserver = [&](const QString &event) {
            if (releaseScheduled
                || !event.startsWith(
                    QStringLiteral("output-finalization-deferred:"))) {
                return;
            }
            releaseScheduled = true;
            QTimer::singleShot(100, qApp, [&] {
                gateHeldAtRelease = !lockIsAvailable(lockPath)
                    && sink.count == 0
                    && traceIndex(*trace,
                                  QStringLiteral("attempt-cleanup-complete:"))
                        < 0
                    && traceIndex(*trace, QStringLiteral("lock-released:")) < 0
                    && traceIndex(*trace, QStringLiteral("reaper-finished:"))
                        < 0;
                *allowFinalization = true;
            });
        };
        QVERIFY(Xc2BackendManagerTestAccess::startStagedOutputFake(
            *manager, lockPath,
            Xc2BackendManagerTestAccess::allocateCandidate(), directory.path(),
            directory.filePath("deferred-finalization.jsonl"), trace,
            lifecycleObserver,
            [allowFinalization] { return *allowFinalization; }));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        child = openStableHandle(manager->ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));
        delete manager;
        manager = nullptr;
        QCoreApplication::postEvent(qApp, new QEvent(QEvent::Quit));
        QTRY_VERIFY_WITH_TIMEOUT(*allowFinalization, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(sink.count, 1, 10000);
        qApp->removeEventFilter(&sink);

        const int deferredIndex = traceIndex(
            *trace, QStringLiteral("output-finalization-deferred:"));
        const int rawDrainedIndex = traceIndex(
            *trace, QStringLiteral("output-raw-drain-complete:"));
        const int finalizedIndex = traceIndex(
            *trace, QStringLiteral("output-finalized:"));
        const int cleanupIndex = traceIndex(
            *trace, QStringLiteral("attempt-cleanup-complete:"));
        const int lockIndex = traceIndex(
            *trace, QStringLiteral("lock-released:"));
        const int reaperIndex = traceIndex(
            *trace, QStringLiteral("reaper-finished:"));
        const int quitIndex = traceIndex(
            *trace, QStringLiteral("downstream-quit"));
        QVERIFY(releaseScheduled);
        QVERIFY(gateHeldAtRelease);
        QVERIFY(deferredIndex >= 0);
        QVERIFY(rawDrainedIndex > deferredIndex);
        QVERIFY(finalizedIndex > rawDrainedIndex);
        QVERIFY(cleanupIndex > finalizedIndex);
        QVERIFY(lockIndex > cleanupIndex);
        QVERIFY(reaperIndex > lockIndex);
        QVERIFY(quitIndex > reaperIndex);
        QVERIFY(childSignaledAtQuit);
        QVERIFY(lockAvailableAtQuit);
        QVERIFY(handleIsSignaled(child));
        QVERIFY(lockIsAvailable(lockPath));
        CloseHandle(child);
    }

    void stopDuringStartNeverEmitsReady()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QObject::connect(&manager, &Xc2BackendManager::stateChanged,
                         &manager, [&manager](Xc2BackendState state) {
            if (state == Xc2BackendState::Starting) {
                manager.stop();
                manager.stop();
            }
        });
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--ready-delay-ms"), QStringLiteral("1200")},
            directory.filePath("stop-start.jsonl"), 4000, 400, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(manager.state(), Xc2BackendState::Stopped);
    }

    void stopFromProbingIsIdempotentAndNeverWritesShutdown()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString eventLog = directory.filePath("stop-probing.jsonl");
        Xc2BackendManager manager;
        HANDLE child = nullptr;
        QObject::connect(&manager, &Xc2BackendManager::stateChanged,
                         &manager, [&manager, &child](Xc2BackendState state) {
            if (state != Xc2BackendState::Probing)
                return;
            child = openStableHandle(manager.ownedProcessId());
            manager.stop();
            manager.stop();
        });
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--never-health")}, eventLog,
            5000, 500, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(eventCount(eventLog, QStringLiteral("SHUTDOWN")), 0);
        QVERIFY(child != nullptr && handleIsSignaled(child));
        CloseHandle(child);
    }

    void stopFromReadyStateSignalSuppressesReadyPublication()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString eventLog = directory.filePath("stop-ready-signal.jsonl");
        Xc2BackendManager manager;
        QObject::connect(&manager, &Xc2BackendManager::stateChanged,
                         &manager, [&manager](Xc2BackendState state) {
            if (state == Xc2BackendState::Ready)
                manager.stop();
        });
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
            eventLog, 5000, 700, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(eventCount(eventLog, QStringLiteral("SHUTDOWN")), 1);
    }

    void deleteFromFailedStateSuppressesStaleFailurePublication()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        auto *manager = new Xc2BackendManager;
        QPointer<Xc2BackendManager> guarded(manager);
        HANDLE child = nullptr;
        QObject::connect(manager, &Xc2BackendManager::stateChanged,
                         manager, [manager, &child](Xc2BackendState state) {
            if (state != Xc2BackendState::Failed)
                return;
            child = openStableHandle(manager->ownedProcessId());
            delete manager;
        });
        QSignalSpy failed(manager, &Xc2BackendManager::failed);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            *manager, lockPath,
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--user-mode"),
             QStringLiteral("blank-login")},
            directory.filePath("delete-failed.jsonl"),
            5000, 500, 100, 1));
        QTRY_VERIFY_WITH_TIMEOUT(guarded.isNull(), 7000);
        QCOMPARE(failed.count(), 0);
        QVERIFY(child != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(handleIsSignaled(child), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(lockIsAvailable(lockPath), 3000);
        CloseHandle(child);
    }

    void repeatedStartIsRejectedWithoutDisturbingCurrentRun()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
            directory.filePath("active.jsonl"), 5000, 700, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        const qint64 originalPid = manager.ownedProcessId();
        const Xc2BackendEndpoints originalEndpoints = manager.endpoints();
        Xc2Error rejection;
        QVERIFY(!Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory, "other.lock"),
            {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
            directory.filePath("rejected.jsonl"), 5000, 700, 100, 1,
            {}, {}, &rejection));
        QVERIFY(!rejection.message.isEmpty());
        QCOMPARE(manager.state(), Xc2BackendState::Ready);
        QCOMPARE(manager.ownedProcessId(), originalPid);
        QCOMPARE(manager.endpoints().restBaseUrl, originalEndpoints.restBaseUrl);
        QCOMPARE(failed.count(), 0);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
    }

    void repeatedStartIsRejectedInEveryActiveState()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString eventLog = directory.filePath("all-state-active.jsonl");
        auto argumentCalls = std::make_shared<int>(0);
        auto candidateCalls = std::make_shared<int>(0);
        QList<Xc2BackendState> rejectedStates;
        bool everyRejected = true;
        bool everyErrorSet = true;
        Xc2BackendManager manager;
        QObject::connect(&manager, &Xc2BackendManager::stateChanged,
                         &manager,
                         [&](Xc2BackendState state) {
            if (state == Xc2BackendState::Stopped)
                return;
            Xc2Error rejection;
            const bool accepted = Xc2BackendManagerTestAccess::startFake(
                manager, privateLockPath(directory, "rejected.lock"),
                {49123}, {}, directory.filePath("rejected.jsonl"),
                3000, 300, 100, 1, argumentCalls, candidateCalls,
                &rejection);
            everyRejected = everyRejected && !accepted;
            everyErrorSet = everyErrorSet && !rejection.message.isEmpty();
            rejectedStates.append(state);
            if (state == Xc2BackendState::Failed) {
                manager.stop();
                manager.stop();
            }
        });
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--ignore-shutdown")}, eventLog,
            5000, 300, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        manager.stop();
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 4000);
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QVERIFY(everyRejected);
        QVERIFY(everyErrorSet);
        QCOMPARE(*argumentCalls, 0);
        QCOMPARE(*candidateCalls, 0);
        for (const Xc2BackendState expected : {
                 Xc2BackendState::Starting,
                 Xc2BackendState::Probing,
                 Xc2BackendState::Ready,
                 Xc2BackendState::Stopping,
                 Xc2BackendState::Failed,
             }) {
            QVERIFY(rejectedStates.contains(expected));
        }
        QCOMPARE(eventCount(eventLog, QStringLiteral("SHUTDOWN")), 1);
        manager.stop();
        QCOMPARE(stopped.count(), 1);
    }

    void stopIsIdempotentAndRestartWaitsForStopped()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const QString firstLog = directory.filePath("first-stop.jsonl");
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, lockPath,
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--ignore-shutdown")},
            firstLog, 5000, 500, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        manager.stop();
        manager.stop();
        Xc2Error rejection;
        QVERIFY(!Xc2BackendManagerTestAccess::startFake(
            manager, lockPath,
            {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
            directory.filePath("too-early.jsonl"), 5000, 500, 100, 1,
            {}, {}, &rejection));
        QVERIFY(!rejection.message.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QCOMPARE(eventCount(firstLog, QStringLiteral("SHUTDOWN")), 1);

        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, lockPath,
            {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
            directory.filePath("restart.jsonl"), 5000, 700, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 7000);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 2, 5000);
    }

    void failedToStartNotificationAfterStoppingStillFinalizes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString invalidProgram = directory.filePath("invalid-program.txt");
        QFile file(invalidProgram);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a Windows executable\n");
        file.close();
        const QString lockPath = privateLockPath(directory);
        Xc2BackendManager manager;
        QList<Xc2BackendState> order;
        QObject::connect(&manager, &Xc2BackendManager::stateChanged,
                         &manager, [&](Xc2BackendState state) {
            order.append(state);
        });
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy failed(&manager, &Xc2BackendManager::failed);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startProgram(
            manager, invalidProgram, lockPath, 50));
        QCOMPARE(manager.state(), Xc2BackendState::Starting);
        manager.stop();
        QCOMPARE(manager.state(), Xc2BackendState::Stopping);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 4000);
        QCOMPARE(ready.count(), 0);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(qvariant_cast<Xc2Error>(
                     failed.constFirst().constFirst()).category,
                 Xc2ErrorCategory::Backend);
        QCOMPARE(stopped.constFirst().at(0).toInt(), -1);
        QCOMPARE(qvariant_cast<QProcess::ExitStatus>(
                     stopped.constFirst().at(1)), QProcess::CrashExit);
        const QList<Xc2BackendState> expectedOrder{
            Xc2BackendState::Starting,
            Xc2BackendState::Stopping,
            Xc2BackendState::Failed,
            Xc2BackendState::Stopped,
        };
        QCOMPARE(order, expectedOrder);
        QCOMPARE(manager.state(), Xc2BackendState::Stopped);
        QVERIFY(lockIsAvailable(lockPath));
    }

    void destroyAcceptedRunsBeforeChildOwnershipIsSafe()
    {
        {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const QString lockPath = privateLockPath(directory);
            const QString eventLog = directory.filePath("starting-delete.jsonl");
            auto *manager = new Xc2BackendManager;
            QPointer<Xc2BackendManager> guarded(manager);
            QObject::connect(manager, &Xc2BackendManager::stateChanged,
                             manager, [manager](Xc2BackendState state) {
                if (state == Xc2BackendState::Starting)
                    delete manager;
            });
            QVERIFY(Xc2BackendManagerTestAccess::startFake(
                *manager, lockPath,
                {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
                eventLog, 3000, 300, 100, 1));
            QVERIFY(guarded.isNull());
            QCoreApplication::sendPostedEvents(nullptr,
                                               QEvent::DeferredDelete);
            QVERIFY(lockIsAvailable(lockPath));
            QVERIFY(!QFileInfo::exists(eventLog));
        }

        {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const QString lockPath = privateLockPath(directory);
            auto *manager = new Xc2BackendManager;
            QSignalSpy failed(manager, &Xc2BackendManager::failed);
            QVERIFY(Xc2BackendManagerTestAccess::startFake(
                *manager, lockPath, {0}, {},
                directory.filePath("no-candidate.jsonl"),
                3000, 300, 100, 1));
            QCOMPARE(failed.count(), 1);
            QVERIFY(!lockIsAvailable(lockPath));
            delete manager;
            QTRY_VERIFY_WITH_TIMEOUT(lockIsAvailable(lockPath), 2000);
        }

        {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const QString invalidProgram = directory.filePath("invalid-program.txt");
            QFile file(invalidProgram);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("invalid\n");
            file.close();
            const QString lockPath = privateLockPath(directory);
            auto *manager = new Xc2BackendManager;
            QVERIFY(Xc2BackendManagerTestAccess::startProgram(
                *manager, invalidProgram, lockPath));
            delete manager;
            QTRY_VERIFY_WITH_TIMEOUT(lockIsAvailable(lockPath), 4000);
        }
    }

    void deleteFromStoppingUsesReaperAndWritesNoShutdown()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        const QString eventLog = directory.filePath("delete-stopping.jsonl");
        auto *manager = new Xc2BackendManager;
        QPointer<Xc2BackendManager> guarded(manager);
        QSignalSpy ready(manager, &Xc2BackendManager::ready);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            *manager, lockPath,
            {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
            eventLog, 5000, 700, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        HANDLE child = openStableHandle(manager->ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));
        QObject::connect(manager, &Xc2BackendManager::stateChanged,
                         manager, [manager](Xc2BackendState state) {
            if (state == Xc2BackendState::Stopping)
                delete manager;
        });
        manager->stop();
        QVERIFY(guarded.isNull());
        QTRY_VERIFY_WITH_TIMEOUT(handleIsSignaled(child), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(lockIsAvailable(lockPath), 3000);
        QCOMPARE(eventCount(eventLog, QStringLiteral("SHUTDOWN")), 0);
        CloseHandle(child);
    }

    void destroyRunningManagerReapsWithoutUiThreadWait()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString lockPath = privateLockPath(directory);
        auto *manager = new Xc2BackendManager;
        QSignalSpy ready(manager, &Xc2BackendManager::ready);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            *manager, lockPath,
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--ignore-shutdown")},
            directory.filePath("destroy.jsonl"), 5000, 500, 150, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        HANDLE child = openStableHandle(manager->ownedProcessId());
        QVERIFY(child != nullptr && handleIsLive(child));
        QElapsedTimer deletion;
        deletion.start();
        delete manager;
        QVERIFY(deletion.elapsed() < 100);
        QTRY_VERIFY_WITH_TIMEOUT(handleIsSignaled(child), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(lockIsAvailable(lockPath), 3000);
        CloseHandle(child);
    }

    void coordinatedQuitReapsChildBeforeUnlockAndExit()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString proofPath = directory.filePath("proof.json");
        const QString lockPath = privateLockPath(directory);
        QProcess helper;
        helper.setProgram(QString::fromUtf8(XC2_MANAGER_EXIT_HELPER_PATH));
        helper.setArguments({QStringLiteral("--proof"), proofPath,
                             QStringLiteral("--lock"), lockPath,
                             QStringLiteral("--event-log"),
                             directory.filePath("helper-events.jsonl")});
        helper.start();
        QVERIFY(helper.waitForStarted(3000));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(proofPath), 10000);
        QFile proofFile(proofPath);
        QVERIFY(proofFile.open(QIODevice::ReadOnly));
        const QJsonObject proof =
            QJsonDocument::fromJson(proofFile.readAll()).object();
        const qint64 childPid =
            static_cast<qint64>(proof.value(QStringLiteral("pid")).toDouble());
        HANDLE child = openStableHandle(childPid);
        QVERIFY(child != nullptr);
        QVERIFY(handleIsLive(child));
        QCOMPARE(helper.state(), QProcess::Running);
        QElapsedTimer reapDeadline;
        reapDeadline.start();
        bool helperExitedBeforeChild = false;
        bool lockReleasedBeforeChild = false;
        while (handleIsLive(child) && reapDeadline.elapsed() < 7000) {
            if (helper.state() != QProcess::Running
                && handleIsLive(child)) {
                helperExitedBeforeChild = true;
                break;
            }
            if (lockIsAvailable(lockPath) && handleIsLive(child)) {
                lockReleasedBeforeChild = true;
                break;
            }
            QTest::qWait(10);
        }
        QVERIFY(!helperExitedBeforeChild);
        QVERIFY(!lockReleasedBeforeChild);
        QVERIFY(handleIsSignaled(child));
        QTRY_VERIFY_WITH_TIMEOUT(lockIsAvailable(lockPath), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(helper.state(), QProcess::NotRunning, 7000);
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
        QCOMPARE(helper.exitCode(), 0);
        QLockFile recovered(lockPath);
        recovered.setStaleLockTime(0);
        QVERIFY(recovered.tryLock(0));
        recovered.unlock();
        CloseHandle(child);
    }

    void reaperHandlesMultipleContextsAndRepeatedQuitAcrossCycles()
    {
        ReaperRepostHookReset reset;
        QuitSink sink;
        QueuedQuitEmitter duplicateEmitter;
        qApp->installEventFilter(&sink);
        Xc2BackendManagerTestAccess::setReaperRepostHook([&duplicateEmitter] {
            QCoreApplication::postEvent(
                &duplicateEmitter, new QEvent(QueuedQuitEmitter::EventType),
                Qt::HighEventPriority);
            QCoreApplication::postEvent(
                &duplicateEmitter, new QEvent(QueuedQuitEmitter::EventType),
                Qt::NormalEventPriority);
            QCoreApplication::postEvent(
                &duplicateEmitter, new QEvent(QueuedQuitEmitter::EventType),
                Qt::LowEventPriority);
        });
        for (int cycle = 0; cycle < 2; ++cycle) {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            QList<HANDLE> children;
            QStringList lockPaths;
            QList<Xc2BackendManager *> managers;
            for (int index = 0; index < 2; ++index) {
                const QString lockPath = directory.filePath(
                    QStringLiteral("cycle-%1-%2.lock")
                        .arg(cycle).arg(index));
                auto *manager = new Xc2BackendManager;
                QSignalSpy ready(manager, &Xc2BackendManager::ready);
                QVERIFY(Xc2BackendManagerTestAccess::startFake(
                    *manager, lockPath,
                    {Xc2BackendManagerTestAccess::allocateCandidate()},
                    {QStringLiteral("--ignore-shutdown")},
                    directory.filePath(QStringLiteral("cycle-%1-%2.jsonl")
                                           .arg(cycle).arg(index)),
                    5000, 500, 150, 1));
                QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
                HANDLE child = openStableHandle(manager->ownedProcessId());
                QVERIFY(child != nullptr && handleIsLive(child));
                managers.append(manager);
                children.append(child);
                lockPaths.append(lockPath);
            }
            for (Xc2BackendManager *manager : std::as_const(managers))
                delete manager;
            QCoreApplication::postEvent(qApp, new QEvent(QEvent::Quit));
            QCoreApplication::postEvent(qApp, new QEvent(QEvent::Quit));
            for (HANDLE child : std::as_const(children))
                QTRY_VERIFY_WITH_TIMEOUT(handleIsSignaled(child), 5000);
            for (const QString &lockPath : std::as_const(lockPaths))
                QTRY_VERIFY_WITH_TIMEOUT(lockIsAvailable(lockPath), 3000);
            QTRY_COMPARE_WITH_TIMEOUT(sink.count, cycle + 1, 3000);
            QCoreApplication::sendPostedEvents();
            QCOMPARE(sink.count, cycle + 1);
            for (HANDLE child : std::as_const(children))
                CloseHandle(child);
        }
        qApp->removeEventFilter(&sink);
    }

    void taggedQuitWaitsForContextAdoptedByRepostHook()
    {
        ReaperRepostHookReset reset;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString firstLock = directory.filePath("first.lock");
        const QString secondLock = directory.filePath("second.lock");
        auto *first = new Xc2BackendManager;
        auto *second = new Xc2BackendManager;
        QSignalSpy firstReady(first, &Xc2BackendManager::ready);
        QSignalSpy secondReady(second, &Xc2BackendManager::ready);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            *first, firstLock,
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--ignore-shutdown")},
            directory.filePath("first.jsonl"), 5000, 200, 80, 1));
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            *second, secondLock,
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--ignore-shutdown")},
            directory.filePath("second.jsonl"), 5000, 200, 80, 1,
            {}, {}, nullptr, {}, false, false, {}, {}, false, 1000));
        QTRY_COMPARE_WITH_TIMEOUT(firstReady.count(), 1, 7000);
        QTRY_COMPARE_WITH_TIMEOUT(secondReady.count(), 1, 7000);
        HANDLE firstChild = openStableHandle(first->ownedProcessId());
        HANDLE secondChild = openStableHandle(second->ownedProcessId());
        QVERIFY(firstChild != nullptr && handleIsLive(firstChild));
        QVERIFY(secondChild != nullptr && handleIsLive(secondChild));

        QuitSink sink;
        bool secondReapedBeforeQuit = false;
        sink.onQuit = [&] {
            secondReapedBeforeQuit = handleIsSignaled(secondChild)
                && lockIsAvailable(secondLock);
        };
        qApp->installEventFilter(&sink);
        int repostCount = 0;
        bool secondAdopted = false;
        Xc2BackendManagerTestAccess::setReaperRepostHook([&] {
            ++repostCount;
            if (secondAdopted)
                return;
            secondAdopted = true;
            delete second;
            second = nullptr;
        });

        delete first;
        first = nullptr;
        QCoreApplication::postEvent(qApp, new QEvent(QEvent::Quit));
        QTRY_VERIFY_WITH_TIMEOUT(secondAdopted, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(handleIsSignaled(firstChild), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(handleIsSignaled(secondChild), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(lockIsAvailable(firstLock), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(lockIsAvailable(secondLock), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(sink.count, 1, 3000);
        QCoreApplication::sendPostedEvents();
        const int finalQuitCount = sink.count;
        const int finalRepostCount = repostCount;
        qApp->removeEventFilter(&sink);
        CloseHandle(firstChild);
        CloseHandle(secondChild);

        QCOMPARE(finalQuitCount, 1);
        QVERIFY(secondReapedBeforeQuit);
        QCOMPARE(finalRepostCount, 2);
    }

    void forcedApplicationExitJobObjectContainsChildTree()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString proofPath = directory.filePath("forced-proof.json");
        QProcess helper;
        helper.setProgram(QString::fromUtf8(XC2_MANAGER_EXIT_HELPER_PATH));
        const QString eventLog = directory.filePath("forced-events.jsonl");
        helper.setArguments({QStringLiteral("--proof"), proofPath,
                             QStringLiteral("--lock"),
                             privateLockPath(directory),
                             QStringLiteral("--event-log"),
                             eventLog,
                             QStringLiteral("--forced")});
        helper.start();
        QVERIFY(helper.waitForStarted(3000));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(proofPath), 10000);
        QFile proofFile(proofPath);
        QVERIFY(proofFile.open(QIODevice::ReadOnly));
        const QJsonObject proof =
            QJsonDocument::fromJson(proofFile.readAll()).object();
        HANDLE child = openStableHandle(static_cast<qint64>(
            proof.value(QStringLiteral("pid")).toDouble()));
        QVERIFY(child != nullptr);
        const qint64 descendantProcessId = static_cast<qint64>(
            proof.value(QStringLiteral("descendantPid")).toDouble());
        QVERIFY(descendantProcessId > 0);
        QCOMPARE(descendantPid(eventLog), descendantProcessId);
        HANDLE descendant = openStableHandle(descendantProcessId);
        QVERIFY(descendant != nullptr);
        QVERIFY(handleIsLive(child));
        QVERIFY(handleIsLive(descendant));
        QFile acknowledgment(proofPath + QStringLiteral(".ack"));
        QVERIFY(acknowledgment.open(QIODevice::WriteOnly));
        acknowledgment.write("captured\n");
        acknowledgment.close();
        QTRY_COMPARE_WITH_TIMEOUT(helper.state(), QProcess::NotRunning, 7000);
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
        QCOMPARE(helper.exitCode(), 23);
        QTRY_VERIFY_WITH_TIMEOUT(handleIsSignaled(child), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(handleIsSignaled(descendant), 5000);
        CloseHandle(child);
        CloseHandle(descendant);
    }

    void stdoutAndStderrUseIndependentFragmentBuffers()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--fragment-output")},
            directory.filePath("fragments.jsonl"), 5000, 700, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QVERIFY(manager.recentOutput(false).contains(
            QStringLiteral("stdout-fragment")));
        QVERIFY(manager.recentOutput(true).contains(
            QStringLiteral("stderr-fragment")));
        QVERIFY(!manager.recentOutput(false).contains(
            QStringLiteral("stderr-fragment")));
        QVERIFY(!manager.recentOutput(true).contains(
            QStringLiteral("stdout-fragment")));
    }

    void outputLinesAndRingsAreBounded()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Xc2BackendManager manager;
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--oversize-output")},
            directory.filePath("oversize.jsonl"), 5000, 700, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        for (const bool standardError : {false, true}) {
            const QStringList lines = manager.recentOutput(standardError);
            QCOMPARE(lines.size(), 256);
            for (const QString &line : lines)
                QVERIFY(line.toUtf8().size() <= 16 * 1024);
            QVERIFY(lines.constLast().endsWith(
                QStringLiteral("[truncated]")));
            QVERIFY(!lines.contains(QStringLiteral("%1-ring-0")
                .arg(standardError ? QStringLiteral("stderr")
                                   : QStringLiteral("stdout"))));
            QVERIFY(lines.contains(QStringLiteral("%1-ring-299")
                .arg(standardError ? QStringLiteral("stderr")
                                   : QStringLiteral("stdout"))));
        }
    }

    void outputControlsAndSensitiveFieldsAreRemoved()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        Xc2BackendManager manager;
        QStringList publicationOrder;
        QObject::connect(&manager, &Xc2BackendManager::outputLine,
                         &manager,
                         [&publicationOrder](bool, const QString &line) {
            publicationOrder.append(QStringLiteral("output:") + line);
        });
        QObject::connect(&manager, &Xc2BackendManager::stopped,
                         &manager, [&publicationOrder] {
            publicationOrder.append(QStringLiteral("stopped"));
        });
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy output(&manager, &Xc2BackendManager::outputLine);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory),
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--sensitive-output")},
            directory.filePath("sensitive.jsonl"), 5000, 700, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QVERIFY(!output.isEmpty());
        QCOMPARE(publicationOrder.constLast(), QStringLiteral("stopped"));
        static const QStringList names{
            QStringLiteral("Authorization"),
            QStringLiteral("Proxy-Authorization"),
            QStringLiteral("Cookie"),
            QStringLiteral("Set-Cookie"),
            QStringLiteral("password"),
            QStringLiteral("token"),
            QStringLiteral("sessionIndex"),
            QStringLiteral("SAMLRequest"),
            QStringLiteral("SAMLResponse"),
        };
        for (const bool standardError : {false, true}) {
            const QString joined = manager.recentOutput(standardError)
                                       .join(QLatin1Char('\n'));
            QVERIFY(!joined.contains(QStringLiteral("secret-"),
                                     Qt::CaseInsensitive));
            QVERIFY(joined.contains(QStringLiteral("[redacted]"),
                                    Qt::CaseInsensitive));
            for (const QString &name : names)
                QVERIFY2(joined.contains(name, Qt::CaseInsensitive),
                         qPrintable(name));
            QVERIFY(!joined.contains(QChar(0x1b)));
            QVERIFY(!joined.contains(QChar(0x01)));
            for (const QChar character : joined) {
                const ushort value = character.unicode();
                QVERIFY(value == '\n' || value == '\t' || value >= 0x20);
            }
        }
        for (const QList<QVariant> &emission : output) {
            const QString line = emission.at(1).toString();
            QVERIFY(!line.contains(QStringLiteral("secret-"),
                                   Qt::CaseInsensitive));
            QVERIFY(line.toUtf8().size() <= 16 * 1024);
        }

        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory, "clean-run.lock"),
            {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
            directory.filePath("clean-run.jsonl"), 5000, 700, 100, 1));
        QVERIFY(manager.recentOutput(false).isEmpty());
        QVERIFY(manager.recentOutput(true).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 7000);
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 2, 5000);
    }

    void outputSignalReentrancyCannotPolluteRestartedRun()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString firstLock = privateLockPath(directory, "output-first.lock");
        Xc2BackendManager manager;
        bool reentered = false;
        QObject::connect(&manager, &Xc2BackendManager::outputLine,
                         &manager, [&manager, &reentered](bool,
                                                          const QString &) {
            if (reentered)
                return;
            reentered = true;
            manager.stop();
            QElapsedTimer nested;
            nested.start();
            while (nested.elapsed() < 40) {
                QCoreApplication::processEvents(
                    QEventLoop::AllEvents, 5);
            }
        });
        QSignalSpy ready(&manager, &Xc2BackendManager::ready);
        QSignalSpy stopped(&manager, &Xc2BackendManager::stopped);
        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, firstLock,
            {Xc2BackendManagerTestAccess::allocateCandidate()},
            {QStringLiteral("--fragment-output")},
            directory.filePath("output-first.jsonl"),
            5000, 500, 100, 1));
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QVERIFY(reentered);
        QCOMPARE(ready.count(), 0);
        QVERIFY(lockIsAvailable(firstLock));

        QVERIFY(Xc2BackendManagerTestAccess::startFake(
            manager, privateLockPath(directory, "output-second.lock"),
            {Xc2BackendManagerTestAccess::allocateCandidate()}, {},
            directory.filePath("output-second.jsonl"),
            5000, 700, 100, 1));
        QVERIFY(manager.recentOutput(false).isEmpty());
        QVERIFY(manager.recentOutput(true).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 7000);
        QVERIFY(manager.recentOutput(false).isEmpty());
        QVERIFY(manager.recentOutput(true).isEmpty());
        manager.stop();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 2, 5000);
    }

    void rejectsEveryBindAddressExceptIpv4Localhost_data()
    {
        QTest::addColumn<QHostAddress>("address");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("localhost-v4")
            << QHostAddress(QHostAddress::LocalHost) << true;
        QTest::newRow("any") << QHostAddress(QHostAddress::Any) << false;
        QTest::newRow("any-v4")
            << QHostAddress(QHostAddress::AnyIPv4) << false;
        QTest::newRow("any-v6")
            << QHostAddress(QHostAddress::AnyIPv6) << false;
        QTest::newRow("localhost-v6")
            << QHostAddress(QHostAddress::LocalHostIPv6) << false;
        QTest::newRow("other-loopback")
            << QHostAddress(QStringLiteral("127.0.0.2")) << false;
        QTest::newRow("external")
            << QHostAddress(QStringLiteral("192.0.2.10")) << false;
    }

    void rejectsEveryBindAddressExceptIpv4Localhost()
    {
        QFETCH(QHostAddress, address);
        QFETCH(bool, accepted);
        QCOMPARE(Xc2BackendManagerTestAccess::acceptsBindAddress(address),
                 accepted);
    }
};

QTEST_GUILESS_MAIN(Xc2BackendManagerTest)
#include "test_Xc2BackendManager.moc"
