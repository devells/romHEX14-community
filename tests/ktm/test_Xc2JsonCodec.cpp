#include <QtTest>

#include "ktm/xc2/Xc2JsonCodec.h"

using namespace ktm::xc2;

namespace {

QByteArray loadFixture(const QString &relativePath)
{
    QFile file(QStringLiteral(KTM_FIXTURE_DIR "/") + relativePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

} // namespace

class Xc2JsonCodecTest final : public QObject {
    Q_OBJECT

private slots:
    void serviceStatusRequiresAlive()
    {
        QVERIFY(Xc2JsonCodec::serviceStatus("alive").ok());
        const auto bad = Xc2JsonCodec::serviceStatus("starting");
        QVERIFY(!bad.ok());
        QCOMPARE(bad.error.category, Xc2ErrorCategory::Contract);
    }

    void currentUserRequiresIdentityAndPermissions()
    {
        const auto ok = Xc2JsonCodec::currentUser(
            loadFixture(QStringLiteral("rest/current-user.json")));
        QVERIFY(ok.ok());
        QCOMPARE(ok.value->loginName, QStringLiteral("xcd"));
        QVERIFY(ok.value->permissions.contains(QStringLiteral("EcuDiagnosticRead")));

        const auto bad = Xc2JsonCodec::currentUser(
            loadFixture(QStringLiteral("rest/contract-missing-field.json")));
        QVERIFY(!bad.ok());
        QCOMPARE(bad.error.category, Xc2ErrorCategory::Contract);
        QVERIFY(bad.error.message.contains(QStringLiteral("permissions")));
    }

    void malformedJsonAndTopLevelKindsFail()
    {
        const QByteArray malformed("{");
        const auto badJson = Xc2JsonCodec::currentUser(malformed);
        QVERIFY(!badJson.ok());
        QCOMPARE(badJson.error.category, Xc2ErrorCategory::Contract);
        QCOMPARE(badJson.error.rawPayload, malformed);

        QVERIFY(!Xc2JsonCodec::vciDevices("{}").ok());
        QVERIFY(!Xc2JsonCodec::jobAccepted("null").ok());
        QVERIFY(!Xc2JsonCodec::jobProgress("[]").ok());
    }

    void errorRetainsContext()
    {
        const auto result = Xc2JsonCodec::error(
            loadFixture(QStringLiteral("rest/error.json")),
            403,
            QStringLiteral("ecu/scan"));
        QCOMPARE(result.httpStatus, 403);
        QCOMPARE(result.xc2Status, 403);
        QCOMPARE(result.xc2Code, 1007);
        QCOMPARE(result.developerMessage, QStringLiteral("synthetic detail"));
        QCOMPARE(result.info, QStringLiteral("synthetic info"));
        QVERIFY(!result.rawPayload.isEmpty());
    }

    void vciDeviceRoundTripsCompleteApplyPayload()
    {
        const auto devices = Xc2JsonCodec::vciDevices(
            loadFixture(QStringLiteral("rest/device-get.json")));
        QVERIFY(devices.ok());
        QCOMPARE(devices.value->size(), 1);
        const Xc2VciDevice &device = devices.value->front();
        QCOMPARE(device.internalName,
                 QStringLiteral("AVL Ditest VCI2K_DPDU_API"));
        const QJsonObject body = Xc2JsonCodec::vciDeviceJson(device);
        QCOMPARE(body.value(QStringLiteral("id")).toString(), device.id);
        QCOMPARE(body.value(QStringLiteral("name")).toString(), device.name);
        QCOMPARE(body.value(QStringLiteral("internalName")).toString(),
                 device.internalName);
        QVERIFY(!device.additionalModuleInformation.has_value());
        QVERIFY(body.value(QStringLiteral("additionalModuleInformation")).isNull());
    }

    void vciOptionalInformationIsStrict()
    {
        QVERIFY(Xc2JsonCodec::vciDevices(
            R"([{"id":"a","name":"A","internalName":"AVL Ditest VCI2K_DPDU_API"}])").ok());
        QVERIFY(Xc2JsonCodec::vciDevices(
            R"([{"id":"a","name":"A","internalName":"AVL Ditest VCI2K_DPDU_API","additionalModuleInformation":"synthetic"}])").ok());
        QVERIFY(!Xc2JsonCodec::vciDevices(
            R"([{"id":"a","name":"A","internalName":"AVL Ditest VCI2K_DPDU_API","additionalModuleInformation":7}])").ok());
    }

    void jobSchemasUseExactWireFieldNamesAndStates()
    {
        const auto accepted = Xc2JsonCodec::jobAccepted(
            loadFixture(QStringLiteral("rest/job-accepted.json")));
        QVERIFY(accepted.ok());
        QVERIFY(!Xc2JsonCodec::jobAccepted(
            R"({"jobId":"synthetic"})").ok());

        const auto progress = Xc2JsonCodec::jobProgress(
            loadFixture(QStringLiteral("rest/job-progress.json")));
        QVERIFY(progress.ok());
        QCOMPARE(progress.value->state, Xc2JobState::InProgress);

        QVERIFY(!Xc2JsonCodec::jobProgress(
            R"({"jobId":"synthetic","state":"IN_PROGRESS","ticks":1,"totalTicks":2,"message":"x"})").ok());
        QVERIFY(!Xc2JsonCodec::jobProgress(
            R"({"jobId":"synthetic","status":"CREATED","ticks":1,"totalTicks":2,"message":"x"})").ok());
    }
};

QTEST_APPLESS_MAIN(Xc2JsonCodecTest)
#include "test_Xc2JsonCodec.moc"
