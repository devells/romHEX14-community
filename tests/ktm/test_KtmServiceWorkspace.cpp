#include "ktm/ui/KtmServiceWorkspace.h"

#include "ktm/ui/KtmConnectionPage.h"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QtTest>

using namespace ktm;
using namespace ktm::xc2;

namespace ktm {

class KtmSessionControllerTestAccess final {
public:
    static QPointer<Xc2BackendManager> backendGuard(
        KtmSessionController &controller)
    {
        return controller.m_backend;
    }

    static void prepareActiveForStop(KtmSessionController &controller,
                                     int &stopCount)
    {
        controller.m_state = KtmSessionState::SessionReady;
        controller.m_operation = KtmSessionOperation::None;
        controller.m_testOps.backendState = [] {
            return Xc2BackendState::Ready;
        };
        controller.m_testOps.disconnectStomp = [] {};
        controller.m_testOps.abortStomp = [] {};
        controller.m_testOps.stopBackend = [&controller, &stopCount] {
            ++stopCount;
            controller.handleBackendStateChanged(Xc2BackendState::Stopped);
        };
    }

    static void installRejectedStart(KtmSessionController &controller,
                                     QString &observedRoot)
    {
        controller.m_testOps.backendState = [] {
            return Xc2BackendState::Stopped;
        };
        controller.m_testOps.startBackend =
            [&observedRoot](const QString &root, Xc2Error *error) {
            observedRoot = root;
            if (error) {
                error->category = Xc2ErrorCategory::Prerequisite;
                error->message = QStringLiteral("Synthetic start rejection");
            }
            return false;
        };
        controller.m_testOps.disconnectStomp = [] {};
        controller.m_testOps.abortStomp = [] {};
        controller.m_testOps.stopBackend = [] {};
    }
};

} // namespace ktm

namespace {

template<typename T>
T *requiredChild(QObject &owner, const char *name)
{
    T *const child = owner.findChild<T *>(QString::fromLatin1(name));
    if (!child)
        qFatal("Missing required child: %s", name);
    return child;
}

int actionOccurrences(const QMenu &menu, const QAction *action)
{
    return menu.actions().count(const_cast<QAction *>(action));
}

} // namespace

class KtmServiceWorkspaceTest final : public QObject {
    Q_OBJECT

private slots:
    void workspaceOwnsControllerBeforePageAndStartsStopped()
    {
        QMainWindow owner;
        KtmServiceWorkspace workspace(&owner);
        QCOMPARE(workspace.objectName(),
                 QStringLiteral("ktmServiceWorkspace"));
        QVERIFY(!workspace.testAttribute(Qt::WA_DeleteOnClose));
        KtmSessionController *const controller =
            requiredChild<KtmSessionController>(
                workspace, "ktmSessionController");
        KtmConnectionPage *const page = requiredChild<KtmConnectionPage>(
            workspace, "ktmConnectionPage");
        const QObjectList children = workspace.children();
        QVERIFY(children.indexOf(controller) >= 0);
        QVERIFY(children.indexOf(page) > children.indexOf(controller));
        QCOMPARE(requiredChild<QLabel>(
                     *page, "ktmSessionStateLabel")->text(),
                 QStringLiteral("Stopped"));
        QProgressBar *const progress = requiredChild<QProgressBar>(
            *page, "ktmVciProgressBar");
        QCOMPARE(progress->minimum(), 0);
        QCOMPARE(progress->maximum(), 1);
        QVERIFY(requiredChild<QPushButton>(
            *page, "ktmStartSessionButton")->isEnabled());
        QVERIFY(!requiredChild<QPushButton>(
            *page, "ktmStopSessionButton")->isEnabled());
        workspace.shutdown();
    }

    void typedStartFailureProjectsOperatorError()
    {
        QMainWindow owner;
        KtmServiceWorkspace workspace(&owner);
        KtmSessionController *const controller =
            requiredChild<KtmSessionController>(
                workspace, "ktmSessionController");
        KtmConnectionPage *const page = requiredChild<KtmConnectionPage>(
            workspace, "ktmConnectionPage");
        QString observedRoot;
        KtmSessionControllerTestAccess::installRejectedStart(
            *controller, observedRoot);
        requiredChild<QLineEdit>(*page, "ktmInstallRootEdit")
            ->setText(QStringLiteral("C:/XC2/workspace"));
        requiredChild<QPushButton>(*page, "ktmStartSessionButton")->click();
        QCOMPARE(observedRoot, QStringLiteral("C:/XC2/workspace"));
        QCOMPARE(requiredChild<QLabel>(
                     *page, "ktmSessionErrorLabel")->text(),
                 QStringLiteral("Synthetic start rejection"));
        workspace.shutdown();
    }

    void hostReusesActionWorkspaceControllerAndSidecarAcrossClose()
    {
        QMainWindow owner;
        QMenu menu(&owner);
        QAction command(QStringLiteral("Command Palette"), &owner);
        QAction preferences(QStringLiteral("Preferences"), &owner);
        menu.addAction(&command);

        KtmServiceWorkspaceHost host(&owner);
        QAction *const action = requiredChild<QAction>(
            host, "actionKtmService");
        host.retranslate(QStringLiteral("KTM Service Test"));
        QCOMPARE(action->text(), QStringLiteral("KTM Service Test"));
        host.attachToMenu(&menu);
        host.attachToMenu(&menu);
        menu.addAction(&preferences);
        QCOMPARE(actionOccurrences(menu, action), 1);
        QCOMPARE(menu.actions().indexOf(action),
                 menu.actions().indexOf(&command) + 1);
        QVERIFY(menu.actions().indexOf(action)
                < menu.actions().indexOf(&preferences));

        action->trigger();
        KtmServiceWorkspace *workspace =
            owner.findChild<KtmServiceWorkspace *>(
                QStringLiteral("ktmServiceWorkspace"));
        QVERIFY(workspace);
        QTRY_VERIFY(workspace->isVisible());
        KtmSessionController *const controller =
            requiredChild<KtmSessionController>(
                *workspace, "ktmSessionController");
        QPointer<Xc2BackendManager> backend =
            KtmSessionControllerTestAccess::backendGuard(*controller);
        QVERIFY(backend);
        int stopCount = 0;
        KtmSessionControllerTestAccess::prepareActiveForStop(
            *controller, stopCount);

        workspace->close();
        QCoreApplication::processEvents();
        QTRY_VERIFY(!workspace->isVisible());
        QCOMPARE(stopCount, 0);
        QCOMPARE(owner.findChild<KtmServiceWorkspace *>(
                     QStringLiteral("ktmServiceWorkspace")), workspace);
        QCOMPARE(requiredChild<KtmSessionController>(
                     *workspace, "ktmSessionController"), controller);
        QCOMPARE(KtmSessionControllerTestAccess::backendGuard(*controller),
                 backend);

        action->trigger();
        QTRY_VERIFY(workspace->isVisible());
        QCOMPARE(owner.findChildren<KtmServiceWorkspace *>(
                     QStringLiteral("ktmServiceWorkspace")).size(), 1);
        QCOMPARE(requiredChild<KtmSessionController>(
                     *workspace, "ktmSessionController"), controller);

        menu.clear();
        menu.addAction(&command);
        host.attachToMenu(&menu);
        host.attachToMenu(&menu);
        menu.addAction(&preferences);
        QCOMPARE(actionOccurrences(menu, action), 1);
        QCOMPARE(menu.actions().indexOf(action),
                 menu.actions().indexOf(&command) + 1);
        QVERIFY(menu.actions().indexOf(action)
                < menu.actions().indexOf(&preferences));

        QVERIFY(QMetaObject::invokeMethod(
            QCoreApplication::instance(), "aboutToQuit",
            Qt::DirectConnection));
        host.shutdown();
        host.shutdown();
        QCOMPARE(stopCount, 1);
        workspace->close();
    }
};

QTEST_MAIN(KtmServiceWorkspaceTest)
#include "test_KtmServiceWorkspace.moc"
