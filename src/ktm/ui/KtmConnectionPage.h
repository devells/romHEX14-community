#pragma once

#include "ktm/KtmSessionController.h"

#include <QWidget>

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

namespace ktm {

class KtmConnectionPageTestAccess;

class KtmConnectionPage final : public QWidget {
    Q_OBJECT

public:
    explicit KtmConnectionPage(QWidget *parent = nullptr);

    void projectState(KtmSessionState state);
    void projectOperation(KtmSessionOperation operation);
    void projectDevices(const QList<xc2::Xc2VciDevice> &devices);
    void projectStatus(const xc2::Xc2VciStatus &status);
    void projectError(const xc2::Xc2Error &error);

signals:
    void startRequested(const QString &installRoot);
    void stopRequested();
    void lookupRequested();
    void applyRequested(const ktm::xc2::Xc2VciDevice &device);
    void closeRequested();

private:
    friend class KtmConnectionPageTestAccess;

    using DirectoryPicker =
        std::function<QString(QWidget *, const QString &)>;

    void browseInstallRoot();
    void persistInstallRoot();
    void requestStart();
    void requestApply();
    void updateControls();
    void updateProgress();
    bool currentDeviceApproved() const;
    static QString stateText(KtmSessionState state);

    QLineEdit *m_installRootEdit = nullptr;
    QPushButton *m_browseButton = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPushButton *m_lookupButton = nullptr;
    QComboBox *m_deviceCombo = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_disconnectButton = nullptr;
    QLabel *m_stateLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QLabel *m_errorLabel = nullptr;
    QLabel *m_voltageLabel = nullptr;
    DirectoryPicker m_directoryPicker;
    KtmSessionState m_state = KtmSessionState::Stopped;
    KtmSessionOperation m_operation = KtmSessionOperation::None;
};

} // namespace ktm
