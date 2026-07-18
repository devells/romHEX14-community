#pragma once

#include <QDialog>
#include <QObject>
#include <QPointer>

class QAction;
class QCloseEvent;
class QMainWindow;
class QMenu;

namespace ktm {

class KtmConnectionPage;
class KtmSessionController;

class KtmServiceWorkspace final : public QDialog {
    Q_OBJECT

public:
    explicit KtmServiceWorkspace(QWidget *owner);
    void shutdown();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    KtmSessionController *m_controller = nullptr;
    KtmConnectionPage *m_page = nullptr;
    bool m_shutdown = false;
};

class KtmServiceWorkspaceHost final : public QObject {
    Q_OBJECT

public:
    explicit KtmServiceWorkspaceHost(QMainWindow *owner);

    void attachToMenu(QMenu *menu);
    void retranslate(const QString &actionText);
    void showWorkspace();
    void shutdown();

private:
    QMainWindow *m_owner = nullptr;
    QAction *m_action = nullptr;
    QPointer<QMenu> m_menu;
    QPointer<KtmServiceWorkspace> m_workspace;
    bool m_shutdown = false;
};

} // namespace ktm
