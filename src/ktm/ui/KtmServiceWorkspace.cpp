#include "ktm/ui/KtmServiceWorkspace.h"

#include "ktm/KtmSessionController.h"
#include "ktm/ui/KtmConnectionPage.h"

#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QMainWindow>
#include <QMenu>
#include <QVBoxLayout>

namespace ktm {

KtmServiceWorkspace::KtmServiceWorkspace(QWidget *owner)
    : QDialog(owner)
{
    setObjectName(QStringLiteral("ktmServiceWorkspace"));
    setWindowTitle(tr("KTM Service"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(760, 360);

    m_controller = new KtmSessionController(this);
    m_controller->setObjectName(QStringLiteral("ktmSessionController"));
    m_page = new KtmConnectionPage(this);
    m_page->projectState(KtmSessionState::Stopped);
    m_page->projectOperation(KtmSessionOperation::None);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_page);

    connect(m_page, &KtmConnectionPage::startRequested,
            this, [this](const QString &installRoot) {
        xc2::Xc2Error error;
        if (!m_controller->startProduction(installRoot, &error)
            && !error.message.isEmpty()) {
            m_page->projectError(error);
        }
    });
    connect(m_page, &KtmConnectionPage::stopRequested,
            m_controller, &KtmSessionController::stop);
    connect(m_page, &KtmConnectionPage::lookupRequested,
            this, [this] {
        xc2::Xc2Error error;
        if (!m_controller->lookupVci(&error) && !error.message.isEmpty())
            m_page->projectError(error);
    });
    connect(m_page, &KtmConnectionPage::applyRequested,
            this, [this](const xc2::Xc2VciDevice &device) {
        xc2::Xc2Error error;
        if (!m_controller->applyVci(device, &error)
            && !error.message.isEmpty()) {
            m_page->projectError(error);
        }
    });
    connect(m_page, &KtmConnectionPage::closeRequested,
            this, [this] {
        xc2::Xc2Error error;
        if (!m_controller->closeVci(&error) && !error.message.isEmpty())
            m_page->projectError(error);
    });

    connect(m_controller, &KtmSessionController::stateChanged,
            m_page, &KtmConnectionPage::projectState);
    connect(m_controller, &KtmSessionController::operationChanged,
            m_page, &KtmConnectionPage::projectOperation);
    connect(m_controller, &KtmSessionController::failed,
            m_page, &KtmConnectionPage::projectError);
    connect(m_controller, &KtmSessionController::vciLookupFinished,
            m_page, &KtmConnectionPage::projectDevices);
    connect(m_controller, &KtmSessionController::vciReady,
            this, [this](const xc2::Xc2VciDevice &,
                         const xc2::Xc2VciStatus &status) {
        m_page->projectStatus(status);
    });
    connect(m_controller, &KtmSessionController::vciStatusChanged,
            m_page, &KtmConnectionPage::projectStatus);
    connect(m_controller, &KtmSessionController::vciReadinessRevoked,
            m_page, &KtmConnectionPage::projectError);
    connect(m_controller, &KtmSessionController::vciClosed,
            this, [this] { m_page->projectStatus({}); });
}

void KtmServiceWorkspace::shutdown()
{
    if (m_shutdown)
        return;
    m_shutdown = true;
    m_controller->stop();
}

void KtmServiceWorkspace::closeEvent(QCloseEvent *event)
{
    hide();
    event->ignore();
}

KtmServiceWorkspaceHost::KtmServiceWorkspaceHost(QMainWindow *owner)
    : QObject(owner), m_owner(owner), m_action(new QAction(this))
{
    m_action->setObjectName(QStringLiteral("actionKtmService"));
    connect(m_action, &QAction::triggered,
            this, &KtmServiceWorkspaceHost::showWorkspace);
    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                this, &KtmServiceWorkspaceHost::shutdown);
    }
}

void KtmServiceWorkspaceHost::attachToMenu(QMenu *menu)
{
    if (!menu)
        return;
    if (m_menu && m_menu != menu)
        m_menu->removeAction(m_action);
    while (menu->actions().contains(m_action))
        menu->removeAction(m_action);
    menu->addAction(m_action);
    m_menu = menu;
}

void KtmServiceWorkspaceHost::retranslate(const QString &actionText)
{
    m_action->setText(actionText);
}

void KtmServiceWorkspaceHost::showWorkspace()
{
    if (m_shutdown)
        return;
    if (!m_workspace)
        m_workspace = new KtmServiceWorkspace(m_owner);
    m_workspace->show();
    m_workspace->raise();
    m_workspace->activateWindow();
}

void KtmServiceWorkspaceHost::shutdown()
{
    if (m_shutdown)
        return;
    m_shutdown = true;
    if (m_workspace)
        m_workspace->shutdown();
}

} // namespace ktm
