#include "ktm/xc2/Xc2BackendManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QTimer>

#ifndef FAKE_XC2_SIDECAR_PATH
#error FAKE_XC2_SIDECAR_PATH must be supplied by CMake
#endif

namespace ktm::xc2 {

class Xc2BackendManagerTestAccess final {
public:
    static bool startExitHelper(Xc2BackendManager &manager,
                                const QString &lockPath,
                                const QString &eventLog,
                                bool spawnDescendant,
                                Xc2Error *error)
    {
        Xc2BackendManager::PrivateLaunchPlan plan;
        plan.program = QString::fromUtf8(FAKE_XC2_SIDECAR_PATH);
        plan.workingDirectory = QFileInfo(plan.program).absolutePath();
        plan.environment = Xc2BackendManager::sanitizedEnvironment();
        plan.bindAddress = QHostAddress::LocalHost;
        plan.lockPath = lockPath;
        plan.startupDeadlineMs = 8000;
        plan.shutdownDeadlineMs = 600;
        plan.terminateDeadlineMs = 250;
        plan.maxPortAttempts = 1;
        plan.candidateAllocator = [] {
            return Xc2BackendManager::allocateLoopbackCandidate();
        };
        plan.argumentsBuilder = [eventLog](quint16 port) {
            QStringList arguments{
                QStringLiteral("--port"), QString::number(port),
                QStringLiteral("--ignore-shutdown"),
                QStringLiteral("--event-log"), eventLog,
            };
            return arguments;
        };
        if (spawnDescendant) {
            plan.argumentsBuilder = [eventLog](quint16 port) {
                return QStringList{
                    QStringLiteral("--port"), QString::number(port),
                    QStringLiteral("--ignore-shutdown"),
                    QStringLiteral("--spawn-descendant"),
                    QStringLiteral("--event-log"), eventLog,
                };
            };
        }
        return manager.startPrivateForTest(std::move(plan), error);
    }
};

} // namespace ktm::xc2

namespace {

QString optionValue(const QStringList &arguments, const QString &name)
{
    const int index = arguments.indexOf(name);
    if (index < 0 || index + 1 >= arguments.size())
        return {};
    return arguments.at(index + 1);
}

qint64 descendantPidFromLog(const QString &eventLog)
{
    QFile file(eventLog);
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    while (!file.atEnd()) {
        const QJsonObject object = QJsonDocument::fromJson(
            file.readLine().trimmed()).object();
        if (object.value(QStringLiteral("event")).toString()
            == QStringLiteral("DESCENDANT_STARTED")) {
            return static_cast<qint64>(object.value(
                QStringLiteral("descendantPid")).toDouble());
        }
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    const QString proofPath = optionValue(arguments, QStringLiteral("--proof"));
    const QString lockPath = optionValue(arguments, QStringLiteral("--lock"));
    const QString eventLog = optionValue(arguments,
                                         QStringLiteral("--event-log"));
    const bool forced = arguments.contains(QStringLiteral("--forced"));
    if (proofPath.isEmpty() || lockPath.isEmpty() || eventLog.isEmpty())
        return 2;

    auto *manager = new ktm::xc2::Xc2BackendManager;
    QObject::connect(manager, &ktm::xc2::Xc2BackendManager::ready,
                     &application, [=](const ktm::xc2::Xc2BackendEndpoints &) {
        QSaveFile proof(proofPath);
        if (!proof.open(QIODevice::WriteOnly)) {
            QCoreApplication::exit(3);
            return;
        }
        const QJsonObject object{
            {QStringLiteral("pid"), manager->ownedProcessId()},
            {QStringLiteral("lockPath"), lockPath},
            {QStringLiteral("descendantPid"),
             descendantPidFromLog(eventLog)},
        };
        proof.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
        proof.write("\n");
        proof.flush();
        if (!proof.commit()) {
            QCoreApplication::exit(3);
            return;
        }

        if (forced) {
            auto *acknowledgment = new QTimer(qApp);
            acknowledgment->setInterval(10);
            QObject::connect(acknowledgment, &QTimer::timeout, qApp,
                             [acknowledgment, proofPath] {
                if (!QFileInfo::exists(proofPath
                                       + QStringLiteral(".ack"))) {
                    return;
                }
                acknowledgment->stop();
                QCoreApplication::exit(23);
            });
            acknowledgment->start();
            return;
        }
        delete manager;
        QCoreApplication::quit();
        QCoreApplication::quit();
    });
    QObject::connect(manager, &ktm::xc2::Xc2BackendManager::failed,
                     &application, [](const ktm::xc2::Xc2Error &) {
                         QCoreApplication::exit(4);
                     });

    ktm::xc2::Xc2Error error;
    if (!ktm::xc2::Xc2BackendManagerTestAccess::startExitHelper(
            *manager, lockPath, eventLog, forced, &error)) {
        delete manager;
        return 5;
    }
    QTimer::singleShot(15000, &application,
                       [] { QCoreApplication::exit(6); });
    return application.exec();
}
