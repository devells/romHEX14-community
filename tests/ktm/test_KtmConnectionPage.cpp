#include "ktm/ui/KtmConnectionPage.h"

#include "ktm/xc2/Xc2ContractProfile.h"
#include "ktm/xc2/Xc2Settings.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>

using namespace ktm;
using namespace ktm::xc2;

namespace ktm {

class KtmConnectionPageTestAccess final {
public:
    static void setDirectoryPicker(
        KtmConnectionPage &page,
        std::function<QString(QWidget *, const QString &)> picker)
    {
        page.m_directoryPicker = std::move(picker);
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

Xc2VciDevice approvedDevice()
{
    return {QStringLiteral("vci-approved"),
            QStringLiteral("Workshop VCI"),
            Xc2ContractProfile::approved().supportedPduApiShortName(),
            QStringLiteral("module-details")};
}

Xc2VciDevice wrongProviderDevice()
{
    return {QStringLiteral("vci-other"),
            QStringLiteral("Unsupported VCI"),
            QStringLiteral("OTHER_PROVIDER"), std::nullopt};
}

} // namespace

class KtmConnectionPageTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        m_previousFormat = QSettings::defaultFormat();
        QVERIFY(m_settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_settingsDir.path());
        qRegisterMetaType<Xc2VciDevice>();
        qRegisterMetaType<Xc2Error>();
    }

    void cleanupTestCase()
    {
        Xc2Settings::clearInstallRoot();
        QSettings::setDefaultFormat(m_previousFormat);
    }

    void init()
    {
        Xc2Settings::clearInstallRoot();
    }

    void frozenObjectNamesAndStoredRoot()
    {
        Xc2Settings::setInstallRoot(QStringLiteral("C:/XC2/stored"));
        KtmConnectionPage page;
        QCOMPARE(page.objectName(), QStringLiteral("ktmConnectionPage"));
        QCOMPARE(requiredChild<QLineEdit>(
                     page, "ktmInstallRootEdit")->text(),
                 QStringLiteral("C:/XC2/stored"));
        QVERIFY(requiredChild<QPushButton>(
            page, "ktmBrowseInstallRootButton"));
        QVERIFY(requiredChild<QPushButton>(page, "ktmStartSessionButton"));
        QVERIFY(requiredChild<QPushButton>(page, "ktmStopSessionButton"));
        QVERIFY(requiredChild<QPushButton>(page, "ktmLookupVciButton"));
        QVERIFY(requiredChild<QComboBox>(page, "ktmVciDeviceCombo"));
        QVERIFY(requiredChild<QPushButton>(page, "ktmConnectVciButton"));
        QVERIFY(requiredChild<QPushButton>(
            page, "ktmDisconnectVciButton"));
        QVERIFY(requiredChild<QLabel>(page, "ktmSessionStateLabel"));
        QVERIFY(requiredChild<QProgressBar>(page, "ktmVciProgressBar"));
        QVERIFY(requiredChild<QLabel>(page, "ktmSessionErrorLabel"));
        QVERIFY(requiredChild<QLabel>(page, "ktmVciVoltageLabel"));
    }

    void browseEditAndStartPersistInstallRoot()
    {
        KtmConnectionPage page;
        QLineEdit *const edit = requiredChild<QLineEdit>(
            page, "ktmInstallRootEdit");
        QPushButton *const browse = requiredChild<QPushButton>(
            page, "ktmBrowseInstallRootButton");
        bool pickerOwnerWasPage = false;
        QString pickerCurrent;
        KtmConnectionPageTestAccess::setDirectoryPicker(
            page, [&](QWidget *owner, const QString &current) {
            pickerOwnerWasPage = owner == &page;
            pickerCurrent = current;
            return QStringLiteral("C:\\XC2\\selected");
        });
        browse->click();
        QVERIFY(pickerOwnerWasPage);
        QCOMPARE(pickerCurrent, QString());
        QCOMPARE(edit->text(), QStringLiteral("C:/XC2/selected"));
        QCOMPARE(Xc2Settings::installRoot(),
                 QStringLiteral("C:/XC2/selected"));

        edit->setText(QStringLiteral("  "));
        QVERIFY(QMetaObject::invokeMethod(edit, "editingFinished"));
        QVERIFY(Xc2Settings::installRoot().isEmpty());

        edit->setText(QStringLiteral("C:\\XC2\\typed\\..\\typed"));
        QVERIFY(QMetaObject::invokeMethod(edit, "editingFinished"));
        QCOMPARE(edit->text(), QStringLiteral("C:/XC2/typed"));
        QCOMPARE(Xc2Settings::installRoot(), QStringLiteral("C:/XC2/typed"));

        QSignalSpy started(&page, &KtmConnectionPage::startRequested);
        requiredChild<QPushButton>(page, "ktmStartSessionButton")->click();
        QCOMPARE(started.count(), 1);
        QCOMPARE(started.constFirst().constFirst().toString(),
                 QStringLiteral("C:/XC2/typed"));
    }

    void typedIntentAndCompleteDeviceData()
    {
        KtmConnectionPage page;
        QSignalSpy stopped(&page, &KtmConnectionPage::stopRequested);
        QSignalSpy lookedUp(&page, &KtmConnectionPage::lookupRequested);
        QSignalSpy applied(&page, &KtmConnectionPage::applyRequested);
        QSignalSpy closed(&page, &KtmConnectionPage::closeRequested);

        page.projectState(KtmSessionState::SessionReady);
        page.projectOperation(KtmSessionOperation::None);
        QPushButton *const stop = requiredChild<QPushButton>(
            page, "ktmStopSessionButton");
        QPushButton *const lookup = requiredChild<QPushButton>(
            page, "ktmLookupVciButton");
        QVERIFY(stop->isEnabled());
        QVERIFY(lookup->isEnabled());
        stop->click();
        lookup->click();
        QCOMPARE(stopped.count(), 1);
        QCOMPARE(lookedUp.count(), 1);

        const Xc2VciDevice approved = approvedDevice();
        const Xc2VciDevice wrong = wrongProviderDevice();
        page.projectDevices({approved, wrong});
        QComboBox *const combo = requiredChild<QComboBox>(
            page, "ktmVciDeviceCombo");
        QCOMPARE(combo->count(), 2);
        QCOMPARE(combo->currentIndex(), -1);
        const Xc2VciDevice stored = combo->itemData(0).value<Xc2VciDevice>();
        QCOMPARE(stored.id, approved.id);
        QCOMPARE(stored.name, approved.name);
        QCOMPARE(stored.internalName, approved.internalName);
        QCOMPARE(stored.additionalModuleInformation,
                 approved.additionalModuleInformation);

        QPushButton *const connectButton = requiredChild<QPushButton>(
            page, "ktmConnectVciButton");
        QVERIFY(!connectButton->isEnabled());
        combo->setCurrentIndex(0);
        QVERIFY(connectButton->isEnabled());
        connectButton->click();
        QCOMPARE(applied.count(), 1);
        const Xc2VciDevice emitted =
            applied.constFirst().constFirst().value<Xc2VciDevice>();
        QCOMPARE(emitted.id, approved.id);
        QCOMPARE(emitted.additionalModuleInformation,
                 approved.additionalModuleInformation);

        combo->setCurrentIndex(1);
        QVERIFY(!connectButton->isEnabled());
        connectButton->click();
        QCOMPARE(applied.count(), 1);

        page.projectState(KtmSessionState::VciReady);
        QPushButton *const disconnectButton = requiredChild<QPushButton>(
            page, "ktmDisconnectVciButton");
        QVERIFY(disconnectButton->isEnabled());
        disconnectButton->click();
        QCOMPARE(closed.count(), 1);
    }

    void stateStatusErrorAndIndeterminateProgress()
    {
        KtmConnectionPage page;
        QLabel *const state = requiredChild<QLabel>(
            page, "ktmSessionStateLabel");
        QLabel *const error = requiredChild<QLabel>(
            page, "ktmSessionErrorLabel");
        QLabel *const voltage = requiredChild<QLabel>(
            page, "ktmVciVoltageLabel");
        QProgressBar *const progress = requiredChild<QProgressBar>(
            page, "ktmVciProgressBar");

        page.projectState(KtmSessionState::SessionReady);
        QCOMPARE(state->text(), QStringLiteral("Session ready"));
        page.projectOperation(KtmSessionOperation::None);
        QCOMPARE(progress->minimum(), 0);
        QCOMPARE(progress->maximum(), 1);

        page.projectState(KtmSessionState::VciLookup);
        QCOMPARE(progress->minimum(), 0);
        QCOMPARE(progress->maximum(), 0);
        page.projectState(KtmSessionState::SessionReady);
        page.projectOperation(KtmSessionOperation::Apply);
        QCOMPARE(progress->minimum(), 0);
        QCOMPARE(progress->maximum(), 0);

        Xc2VciStatus status;
        status.connected = true;
        status.voltage = 0.0;
        page.projectStatus(status);
        QCOMPARE(voltage->text(),
                 QLocale().toString(0.0, 'f', 1) + QStringLiteral(" V"));

        Xc2Error projected;
        projected.message = QStringLiteral("Operator-safe message");
        projected.developerMessage = QStringLiteral("developer sentinel");
        projected.rawPayload = QByteArrayLiteral("raw sentinel");
        page.projectError(projected);
        QCOMPARE(error->text(), QStringLiteral("Operator-safe message"));
        QVERIFY(!error->text().contains(QStringLiteral("sentinel")));
    }

    void busyOperationGatesUnsafeControls_data()
    {
        QTest::addColumn<KtmSessionState>("state");
        QTest::addColumn<KtmSessionOperation>("operation");

        QTest::newRow("lookup")
            << KtmSessionState::SessionReady
            << KtmSessionOperation::Lookup;
        QTest::newRow("apply")
            << KtmSessionState::SessionReady
            << KtmSessionOperation::Apply;
        QTest::newRow("close")
            << KtmSessionState::VciReady
            << KtmSessionOperation::Close;
    }

    void busyOperationGatesUnsafeControls()
    {
        QFETCH(KtmSessionState, state);
        QFETCH(KtmSessionOperation, operation);

        KtmConnectionPage page;
        page.projectDevices({approvedDevice()});
        page.projectState(state);
        page.projectOperation(operation);

        QVERIFY(!requiredChild<QPushButton>(
            page, "ktmStartSessionButton")->isEnabled());
        QVERIFY(requiredChild<QPushButton>(
            page, "ktmStopSessionButton")->isEnabled());
        QVERIFY(!requiredChild<QPushButton>(
            page, "ktmLookupVciButton")->isEnabled());
        QVERIFY(!requiredChild<QPushButton>(
            page, "ktmConnectVciButton")->isEnabled());
        QVERIFY(!requiredChild<QPushButton>(
            page, "ktmDisconnectVciButton")->isEnabled());
    }

private:
    QTemporaryDir m_settingsDir;
    QSettings::Format m_previousFormat = QSettings::NativeFormat;
};

QTEST_MAIN(KtmConnectionPageTest)
#include "test_KtmConnectionPage.moc"
