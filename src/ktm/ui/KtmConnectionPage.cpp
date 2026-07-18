#include "ktm/ui/KtmConnectionPage.h"

#include "ktm/xc2/Xc2ContractProfile.h"
#include "ktm/xc2/Xc2Settings.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace ktm {

KtmConnectionPage::KtmConnectionPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ktmConnectionPage"));

    m_installRootEdit = new QLineEdit(this);
    m_installRootEdit->setObjectName(
        QStringLiteral("ktmInstallRootEdit"));
    m_installRootEdit->setText(xc2::Xc2Settings::installRoot());
    m_browseButton = new QPushButton(tr("Browse..."), this);
    m_browseButton->setObjectName(
        QStringLiteral("ktmBrowseInstallRootButton"));

    auto *rootRow = new QHBoxLayout;
    rootRow->addWidget(m_installRootEdit, 1);
    rootRow->addWidget(m_browseButton);

    m_startButton = new QPushButton(tr("Start"), this);
    m_startButton->setObjectName(QStringLiteral("ktmStartSessionButton"));
    m_stopButton = new QPushButton(tr("Stop"), this);
    m_stopButton->setObjectName(QStringLiteral("ktmStopSessionButton"));
    auto *sessionButtons = new QHBoxLayout;
    sessionButtons->addWidget(m_startButton);
    sessionButtons->addWidget(m_stopButton);
    sessionButtons->addStretch();

    m_lookupButton = new QPushButton(tr("Find VCI"), this);
    m_lookupButton->setObjectName(QStringLiteral("ktmLookupVciButton"));
    m_deviceCombo = new QComboBox(this);
    m_deviceCombo->setObjectName(QStringLiteral("ktmVciDeviceCombo"));
    m_connectButton = new QPushButton(tr("Connect"), this);
    m_connectButton->setObjectName(QStringLiteral("ktmConnectVciButton"));
    m_disconnectButton = new QPushButton(tr("Disconnect"), this);
    m_disconnectButton->setObjectName(
        QStringLiteral("ktmDisconnectVciButton"));
    auto *vciRow = new QHBoxLayout;
    vciRow->addWidget(m_lookupButton);
    vciRow->addWidget(m_deviceCombo, 1);
    vciRow->addWidget(m_connectButton);
    vciRow->addWidget(m_disconnectButton);

    m_stateLabel = new QLabel(this);
    m_stateLabel->setObjectName(QStringLiteral("ktmSessionStateLabel"));
    m_progressBar = new QProgressBar(this);
    m_progressBar->setObjectName(QStringLiteral("ktmVciProgressBar"));
    m_progressBar->setTextVisible(false);
    m_errorLabel = new QLabel(this);
    m_errorLabel->setObjectName(QStringLiteral("ktmSessionErrorLabel"));
    m_errorLabel->setWordWrap(true);
    m_voltageLabel = new QLabel(QStringLiteral("--"), this);
    m_voltageLabel->setObjectName(QStringLiteral("ktmVciVoltageLabel"));

    auto *details = new QFormLayout;
    details->addRow(tr("State:"), m_stateLabel);
    details->addRow(tr("Voltage:"), m_voltageLabel);
    details->addRow(tr("Progress:"), m_progressBar);
    details->addRow(tr("Error:"), m_errorLabel);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("XC2 installation:"), this));
    layout->addLayout(rootRow);
    layout->addLayout(sessionButtons);
    layout->addLayout(vciRow);
    layout->addLayout(details);
    layout->addStretch();

    m_directoryPicker = [](QWidget *owner, const QString &current) {
        return QFileDialog::getExistingDirectory(
            owner, tr("Select XC2 installation"), current);
    };

    connect(m_browseButton, &QPushButton::clicked,
            this, &KtmConnectionPage::browseInstallRoot);
    connect(m_installRootEdit, &QLineEdit::editingFinished,
            this, &KtmConnectionPage::persistInstallRoot);
    connect(m_startButton, &QPushButton::clicked,
            this, &KtmConnectionPage::requestStart);
    connect(m_stopButton, &QPushButton::clicked,
            this, &KtmConnectionPage::stopRequested);
    connect(m_lookupButton, &QPushButton::clicked,
            this, &KtmConnectionPage::lookupRequested);
    connect(m_connectButton, &QPushButton::clicked,
            this, &KtmConnectionPage::requestApply);
    connect(m_disconnectButton, &QPushButton::clicked,
            this, &KtmConnectionPage::closeRequested);
    connect(m_deviceCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this] { updateControls(); });

    projectState(KtmSessionState::Stopped);
    projectOperation(KtmSessionOperation::None);
}

void KtmConnectionPage::projectState(KtmSessionState state)
{
    m_state = state;
    m_stateLabel->setText(stateText(state));
    updateControls();
    updateProgress();
}

void KtmConnectionPage::projectOperation(KtmSessionOperation operation)
{
    m_operation = operation;
    updateControls();
    updateProgress();
}

void KtmConnectionPage::projectDevices(
    const QList<xc2::Xc2VciDevice> &devices)
{
    m_deviceCombo->clear();
    for (const xc2::Xc2VciDevice &device : devices) {
        const QString label = device.name.trimmed().isEmpty()
            ? device.id : device.name;
        m_deviceCombo->addItem(label, QVariant::fromValue(device));
    }
    m_deviceCombo->setCurrentIndex(-1);
    updateControls();
}

void KtmConnectionPage::projectStatus(const xc2::Xc2VciStatus &status)
{
    m_voltageLabel->setText(
        status.connected
            ? QLocale().toString(status.voltage, 'f', 1) + tr(" V")
            : QStringLiteral("--"));
}

void KtmConnectionPage::projectError(const xc2::Xc2Error &error)
{
    m_errorLabel->setText(error.message);
}

void KtmConnectionPage::browseInstallRoot()
{
    const QString selected = m_directoryPicker(
        this, m_installRootEdit->text());
    if (selected.isEmpty())
        return;
    m_installRootEdit->setText(selected);
    persistInstallRoot();
}

void KtmConnectionPage::persistInstallRoot()
{
    if (m_installRootEdit->text().trimmed().isEmpty())
        xc2::Xc2Settings::clearInstallRoot();
    else
        xc2::Xc2Settings::setInstallRoot(m_installRootEdit->text());
    m_installRootEdit->setText(xc2::Xc2Settings::installRoot());
}

void KtmConnectionPage::requestStart()
{
    persistInstallRoot();
    emit startRequested(m_installRootEdit->text());
}

void KtmConnectionPage::requestApply()
{
    if (!currentDeviceApproved())
        return;
    emit applyRequested(
        m_deviceCombo->currentData().value<xc2::Xc2VciDevice>());
}

void KtmConnectionPage::updateControls()
{
    const bool busy = m_operation != KtmSessionOperation::None;
    const bool stopped = m_state == KtmSessionState::Stopped;
    const bool failed = m_state == KtmSessionState::Failed;
    const bool sessionReady = m_state == KtmSessionState::SessionReady;
    const bool vciReady = m_state == KtmSessionState::VciReady;
    m_startButton->setEnabled(!busy && (stopped || failed));
    m_stopButton->setEnabled(busy || !stopped);
    m_lookupButton->setEnabled(!busy && sessionReady);
    m_deviceCombo->setEnabled(!busy && sessionReady);
    m_connectButton->setEnabled(
        !busy && sessionReady && currentDeviceApproved());
    m_disconnectButton->setEnabled(!busy && vciReady);
}

void KtmConnectionPage::updateProgress()
{
    const bool busy = m_state == KtmSessionState::VciLookup
        || m_operation != KtmSessionOperation::None;
    m_progressBar->setRange(0, busy ? 0 : 1);
    if (!busy)
        m_progressBar->setValue(0);
}

bool KtmConnectionPage::currentDeviceApproved() const
{
    if (m_deviceCombo->currentIndex() < 0)
        return false;
    const xc2::Xc2VciDevice device =
        m_deviceCombo->currentData().value<xc2::Xc2VciDevice>();
    return !device.id.trimmed().isEmpty()
        && device.internalName
            == xc2::Xc2ContractProfile::approved()
                   .supportedPduApiShortName();
}

QString KtmConnectionPage::stateText(KtmSessionState state)
{
    switch (state) {
    case KtmSessionState::Stopped:
        return tr("Stopped");
    case KtmSessionState::BackendStarting:
        return tr("Starting backend");
    case KtmSessionState::BackendReady:
        return tr("Backend ready");
    case KtmSessionState::SessionStarting:
        return tr("Starting session");
    case KtmSessionState::SessionReady:
        return tr("Session ready");
    case KtmSessionState::VciLookup:
        return tr("Finding VCI");
    case KtmSessionState::VciApplying:
        return tr("Connecting VCI");
    case KtmSessionState::VciReady:
        return tr("VCI connected");
    case KtmSessionState::VciClosing:
        return tr("Disconnecting VCI");
    case KtmSessionState::Failed:
        return tr("Failed");
    }
    return {};
}

} // namespace ktm
