#pragma once

#include "Xc2Models.h"

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <optional>

namespace ktm::xc2 {

struct Xc2InstallLayout;

enum class Xc2BackendState {
    Stopped,
    Starting,
    Probing,
    Ready,
    Stopping,
    Failed,
};

struct Xc2BackendEndpoints {
    QUrl restBaseUrl;
    QUrl webSocketUrl;
};

class Xc2BackendManagerTestAccess;

class Xc2BackendManager final : public QObject {
    Q_OBJECT

public:
    explicit Xc2BackendManager(QObject *parent = nullptr);
    ~Xc2BackendManager() override;

    bool startProduction(const QString &installRoot,
                         Xc2Error *error = nullptr);
    void stop();

    Xc2BackendState state() const;
    bool ownsProcess() const;
    qint64 ownedProcessId() const;
    Xc2BackendEndpoints endpoints() const;
    std::optional<Xc2CurrentUser> currentUser() const;
    QStringList recentOutput(bool standardError) const;

signals:
    void stateChanged(ktm::xc2::Xc2BackendState state);
    void ready(const ktm::xc2::Xc2BackendEndpoints &endpoints);
    void outputLine(bool standardError, const QString &line);
    void failed(const ktm::xc2::Xc2Error &error);
    void stopped(int exitCode, QProcess::ExitStatus exitStatus);

private:
    friend class Xc2BackendManagerTestAccess;

    struct PrivateLaunchPlan {
        enum class ShutdownProofMutation {
            None,
            Duplicate,
            Conflicting,
        };

        QString program;
        QString workingDirectory;
        QProcessEnvironment environment;
        QHostAddress bindAddress = QHostAddress::LocalHost;
        QString lockPath;
        int startupDeadlineMs = 15000;
        int shutdownDeadlineMs = 1500;
        int terminateDeadlineMs = 500;
        int maxPortAttempts = 3;
        std::function<QStringList(quint16)> argumentsBuilder;
        std::function<quint16()> candidateAllocator;
        std::function<void(const QString &)> lifecycleObserver;
        std::function<void()> afterListenerSnapshotForTest;
        std::function<void()> afterShutdownSnapshotForTest;
        bool invalidateShutdownCookieTargetForTest = false;
        bool failJobSetupForTest = false;
        bool failNativeTerminationForTest = false;
        int finishedNotificationDelayMsForTest = 0;
        int errorNotificationDelayMsForTest = 0;
        std::function<void(std::function<void()>)>
            shutdownDeadlineEarlyWakeupHookForTest;
        std::function<bool()> outputFinalizationAllowedForTest;
        ShutdownProofMutation shutdownProofMutationForTest =
            ShutdownProofMutation::None;
    };

    struct RawTcpOwnerRow {
        quint32 localAddress = 0;
        quint32 localPort = 0;
        quint32 owningPid = 0;
    };

    struct RawTcpConnectionRow {
        quint32 state = 0;
        quint32 localAddress = 0;
        quint32 localPort = 0;
        quint32 remoteAddress = 0;
        quint32 remotePort = 0;
        quint32 owningPid = 0;
    };

    struct RunContext;

    bool startPrivateForTest(PrivateLaunchPlan plan,
                             Xc2Error *error = nullptr);
    bool startAcceptedPlan(PrivateLaunchPlan plan, qint64 elapsedBeforeRunMs,
                           Xc2Error *error);
    bool startAcceptedFailure(const Xc2Error &failure,
                              qint64 elapsedBeforeRunMs,
                              Xc2Error *error);

    static QStringList productionArguments(const Xc2InstallLayout &layout,
                                           quint16 port);
    static QString productionProgram(const Xc2InstallLayout &layout);
    static QString productionWorkingDirectory(
        const Xc2InstallLayout &layout);
    static QString productionLockPath(const QString &appDataPath);
    static QProcessEnvironment sanitizedEnvironment();
    static bool acceptsBindAddress(const QHostAddress &address);
    static quint16 allocateLoopbackCandidate();
    static bool listenerRowsOwnedForTest(
        const QList<RawTcpOwnerRow> &rows, quint16 port,
        quint32 processId);
    static bool shutdownRowsOwnedForTest(
        const QList<RawTcpConnectionRow> &rows, quint16 serverPort,
        quint16 clientPort, quint32 processId);
    static void setProductionInspectionTestHooks(
        std::function<void(const QString &)> observer, int delayMs);
    static void resetProductionInspectionTestHooks();
    static void setReaperRepostTestHook(std::function<void()> hook);

    void setBackendState(Xc2BackendState state);
    void publishOwnedProcess(qint64 processId, bool owned);
    void publishReady(RunContext *run,
                      const Xc2BackendEndpoints &endpoints,
                      const Xc2CurrentUser &user);
    void clearNetworkPublication();
    bool publishOutputLine(RunContext *run, bool standardError,
                           const QString &line);
    void publishRecentOutput(const QStringList &standardOutput,
                             const QStringList &standardError);
    void publishFailure(RunContext *run, const Xc2Error &error);
    void publishStopped(RunContext *run, int exitCode,
                        QProcess::ExitStatus exitStatus);

    Xc2BackendState m_state = Xc2BackendState::Stopped;
    RunContext *m_run = nullptr;
    quint64 m_nextRunId = 1;
    bool m_ownsProcess = false;
    qint64 m_ownedProcessId = 0;
    Xc2BackendEndpoints m_endpoints;
    std::optional<Xc2CurrentUser> m_currentUser;
    QStringList m_recentStandardOutput;
    QStringList m_recentStandardError;
};

} // namespace ktm::xc2

Q_DECLARE_METATYPE(ktm::xc2::Xc2BackendState)
Q_DECLARE_METATYPE(ktm::xc2::Xc2BackendEndpoints)
