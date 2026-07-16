#include <QtTest>

#include <limits>
#include <type_traits>

#include "ktm/xc2/Xc2JsonCodec.h"

using namespace ktm::xc2;

static_assert(std::is_same_v<decltype(Xc2LocalizedText::id), qint64>);
static_assert(std::is_same_v<decltype(Xc2LocalizedText::text), QString>);
static_assert(std::is_same_v<decltype(Xc2JobProgress::ticks), qint32>);
static_assert(std::is_same_v<decltype(Xc2JobProgress::totalTicks), qint32>);
static_assert(std::is_same_v<decltype(Xc2JobProgress::message),
                             std::optional<Xc2LocalizedText>>);

namespace {

QByteArray loadFixture(const QString &relativePath)
{
    QFile file(QStringLiteral(KTM_FIXTURE_DIR "/") + relativePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

QByteArray jobProgressPayload(
    const QByteArray &status = QByteArrayLiteral("\"IN_PROGRESS\""),
    const QByteArray &ticks = QByteArrayLiteral("1"),
    const QByteArray &totalTicks = QByteArrayLiteral("2"),
    const QByteArray &message = QByteArrayLiteral(
        R"({"id":42,"text":"synthetic progress"})"))
{
    return QByteArrayLiteral(
               R"({"jobId":"synthetic","status":)")
        + status + QByteArrayLiteral(R"(,"ticks":)") + ticks
        + QByteArrayLiteral(R"(,"totalTicks":)") + totalTicks
        + QByteArrayLiteral(R"(,"message":)") + message
        + QByteArrayLiteral("}");
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

    void jobAcceptanceUsesExactWireFieldName()
    {
        const auto accepted = Xc2JsonCodec::jobAccepted(
            loadFixture(QStringLiteral("rest/job-accepted.json")));
        QVERIFY(accepted.ok());
        QVERIFY(!Xc2JsonCodec::jobAccepted(
            R"({"jobId":"synthetic"})").ok());
    }

    void jobProgressFixtureUsesLocalizedTextObject()
    {
        const auto progress = Xc2JsonCodec::jobProgress(
            loadFixture(QStringLiteral("rest/job-progress.json")));
        QVERIFY(progress.ok());
        QCOMPARE(progress.value->state, Xc2JobState::InProgress);
        QVERIFY(progress.value->message.has_value());
        QCOMPARE(progress.value->message->id, qint64{42});
        QCOMPARE(progress.value->message->text,
                 QStringLiteral("synthetic progress"));
    }

    void jobProgressAcceptsUtf8BomAtDocumentStart()
    {
        const QByteArray payload = QByteArray::fromHex("efbbbf")
            + QByteArrayLiteral(" \t\r\n")
            + jobProgressPayload(
                QByteArrayLiteral("\"IN_PROGRESS\""),
                QByteArrayLiteral("-2147483648"),
                QByteArrayLiteral("2147483647"),
                QByteArrayLiteral(
                    R"({"id":9223372036854775807,"text":"bom progress"})"));

        const auto progress = Xc2JsonCodec::jobProgress(payload);

        QVERIFY2(progress.ok(), qPrintable(progress.error.message));
        QCOMPARE(progress.value->ticks, std::numeric_limits<qint32>::min());
        QCOMPARE(progress.value->totalTicks,
                 std::numeric_limits<qint32>::max());
        QVERIFY(progress.value->message.has_value());
        QCOMPARE(progress.value->message->id,
                 std::numeric_limits<qint64>::max());
        QCOMPARE(progress.value->message->text, QStringLiteral("bom progress"));
    }

    void jobProgressAcceptsLocalizedTextObjectAndNull_data()
    {
        QTest::addColumn<QByteArray>("message");
        QTest::addColumn<bool>("hasMessage");

        QTest::newRow("localized text object")
            << QByteArrayLiteral(R"({"id":42,"text":"synthetic progress"})")
            << true;
        QTest::newRow("explicit null") << QByteArrayLiteral("null") << false;
    }

    void jobProgressAcceptsLocalizedTextObjectAndNull()
    {
        QFETCH(QByteArray, message);
        QFETCH(bool, hasMessage);

        const QByteArray payload = jobProgressPayload(
            QByteArrayLiteral("\"IN_PROGRESS\""), QByteArrayLiteral("1"),
            QByteArrayLiteral("2"), message);
        const auto progress = Xc2JsonCodec::jobProgress(payload);

        QVERIFY2(progress.ok(), qPrintable(progress.error.message));
        QCOMPARE(progress.value->message.has_value(), hasMessage);
        if (hasMessage) {
            QCOMPARE(progress.value->message->id, qint64{42});
            QCOMPARE(progress.value->message->text,
                     QStringLiteral("synthetic progress"));
        }
        QCOMPARE(progress.value->raw.value(QStringLiteral("message")).isNull(),
                 !hasMessage);
    }

    void jobProgressRejectsInvalidLocalizedText_data()
    {
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<QString>("errorToken");

        QTest::newRow("legacy string")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral("\"legacy\""))
            << QStringLiteral("message");
        QTest::newRow("missing message")
            << QByteArrayLiteral(
                   R"({"jobId":"synthetic","status":"IN_PROGRESS","ticks":1,"totalTicks":2})")
            << QStringLiteral("message");
        QTest::newRow("message array")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral("[]"))
            << QStringLiteral("message");
        QTest::newRow("message number")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral("7"))
            << QStringLiteral("message");
        QTest::newRow("message boolean")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral("true"))
            << QStringLiteral("message");
        QTest::newRow("localized text missing id")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral(R"({"text":"x"})"))
            << QStringLiteral("id");
        QTest::newRow("localized text missing text")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral(R"({"id":1})"))
            << QStringLiteral("text");
        QTest::newRow("localized text id string")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral(
                                      R"({"id":"1","text":"x"})"))
            << QStringLiteral("id");
        QTest::newRow("localized text id fractional")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral(
                                      R"({"id":1.5,"text":"x"})"))
            << QStringLiteral("id");
        QTest::newRow("localized text id null")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral(
                                      R"({"id":null,"text":"x"})"))
            << QStringLiteral("id");
        QTest::newRow("localized text id boolean")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral(
                                      R"({"id":true,"text":"x"})"))
            << QStringLiteral("id");
        QTest::newRow("localized text id above int64")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral(
                                      R"({"id":9223372036854775808,"text":"x"})"))
            << QStringLiteral("id");
        QTest::newRow("localized text id below int64")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral(
                                      R"({"id":-9223372036854775809,"text":"x"})"))
            << QStringLiteral("id");
        QTest::newRow("localized text text number")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral(R"({"id":1,"text":7})"))
            << QStringLiteral("text");
        QTest::newRow("localized text text null")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral(
                                      R"({"id":1,"text":null})"))
            << QStringLiteral("text");
        QTest::newRow("localized text text boolean")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral(
                                      R"({"id":1,"text":false})"))
            << QStringLiteral("text");
        QTest::newRow("localized text text array")
            << jobProgressPayload(QByteArrayLiteral("\"IN_PROGRESS\""),
                                  QByteArrayLiteral("1"),
                                  QByteArrayLiteral("2"),
                                  QByteArrayLiteral(R"({"id":1,"text":[]})"))
            << QStringLiteral("text");
    }

    void jobProgressRejectsInvalidLocalizedText()
    {
        QFETCH(QByteArray, payload);
        QFETCH(QString, errorToken);
        const auto progress = Xc2JsonCodec::jobProgress(payload);

        QVERIFY(progress.value == std::nullopt);
        QCOMPARE(progress.error.category, Xc2ErrorCategory::Contract);
        QVERIFY2(progress.error.message.contains(errorToken),
                 qPrintable(progress.error.message));
        QCOMPARE(progress.error.rawPayload, payload);
    }

    void jobProgressPreservesInt64LocalizedTextId_data()
    {
        QTest::addColumn<QByteArray>("id");
        QTest::addColumn<qint64>("expectedId");

        QTest::newRow("int64 minimum")
            << QByteArrayLiteral("-9223372036854775808")
            << std::numeric_limits<qint64>::min();
        QTest::newRow("int64 maximum")
            << QByteArrayLiteral("9223372036854775807")
            << std::numeric_limits<qint64>::max();
    }

    void jobProgressPreservesInt64LocalizedTextId()
    {
        QFETCH(QByteArray, id);
        QFETCH(qint64, expectedId);

        const QByteArray message = QByteArrayLiteral("{\"id\":") + id
            + QByteArrayLiteral(",\"text\":\"boundary\"}");
        const auto progress = Xc2JsonCodec::jobProgress(jobProgressPayload(
            QByteArrayLiteral("\"IN_PROGRESS\""), QByteArrayLiteral("1"),
            QByteArrayLiteral("2"), message));

        QVERIFY2(progress.ok(), qPrintable(progress.error.message));
        QVERIFY(progress.value->message.has_value());
        QCOMPARE(progress.value->message->id, expectedId);
        QCOMPARE(progress.value->message->text, QStringLiteral("boundary"));
    }

    void jobProgressAcceptsInt32CounterBoundaries_data()
    {
        QTest::addColumn<QByteArray>("ticks");
        QTest::addColumn<QByteArray>("totalTicks");
        QTest::addColumn<qint32>("expectedTicks");
        QTest::addColumn<qint32>("expectedTotalTicks");

        QTest::newRow("both minimum") << QByteArrayLiteral("-2147483648")
                                       << QByteArrayLiteral("-2147483648")
                                       << std::numeric_limits<qint32>::min()
                                       << std::numeric_limits<qint32>::min();
        QTest::newRow("both maximum") << QByteArrayLiteral("2147483647")
                                       << QByteArrayLiteral("2147483647")
                                       << std::numeric_limits<qint32>::max()
                                       << std::numeric_limits<qint32>::max();
    }

    void jobProgressAcceptsInt32CounterBoundaries()
    {
        QFETCH(QByteArray, ticks);
        QFETCH(QByteArray, totalTicks);
        QFETCH(qint32, expectedTicks);
        QFETCH(qint32, expectedTotalTicks);

        const auto progress = Xc2JsonCodec::jobProgress(jobProgressPayload(
            QByteArrayLiteral("\"IN_PROGRESS\""), ticks, totalTicks));
        QVERIFY2(progress.ok(), qPrintable(progress.error.message));
        QCOMPARE(progress.value->ticks, expectedTicks);
        QCOMPARE(progress.value->totalTicks, expectedTotalTicks);
    }

    void jobProgressRejectsInvalidCounters_data()
    {
        QTest::addColumn<QByteArray>("ticks");
        QTest::addColumn<QByteArray>("totalTicks");
        QTest::addColumn<QString>("field");

        QTest::newRow("ticks below int32") << QByteArrayLiteral("-2147483649")
                                            << QByteArrayLiteral("2")
                                            << QStringLiteral("ticks");
        QTest::newRow("ticks above int32") << QByteArrayLiteral("2147483648")
                                            << QByteArrayLiteral("2")
                                            << QStringLiteral("ticks");
        QTest::newRow("ticks fractional") << QByteArrayLiteral("1.5")
                                           << QByteArrayLiteral("2")
                                           << QStringLiteral("ticks");
        QTest::newRow("ticks string") << QByteArrayLiteral("\"1\"")
                                       << QByteArrayLiteral("2")
                                       << QStringLiteral("ticks");
        QTest::newRow("ticks null") << QByteArrayLiteral("null")
                                     << QByteArrayLiteral("2")
                                     << QStringLiteral("ticks");
        QTest::newRow("ticks boolean") << QByteArrayLiteral("true")
                                        << QByteArrayLiteral("2")
                                        << QStringLiteral("ticks");
        QTest::newRow("total below int32") << QByteArrayLiteral("1")
                                            << QByteArrayLiteral("-2147483649")
                                            << QStringLiteral("totalTicks");
        QTest::newRow("total above int32") << QByteArrayLiteral("1")
                                            << QByteArrayLiteral("2147483648")
                                            << QStringLiteral("totalTicks");
        QTest::newRow("total fractional") << QByteArrayLiteral("1")
                                           << QByteArrayLiteral("2.5")
                                           << QStringLiteral("totalTicks");
        QTest::newRow("total string") << QByteArrayLiteral("1")
                                       << QByteArrayLiteral("\"2\"")
                                       << QStringLiteral("totalTicks");
        QTest::newRow("total null") << QByteArrayLiteral("1")
                                     << QByteArrayLiteral("null")
                                     << QStringLiteral("totalTicks");
        QTest::newRow("total boolean") << QByteArrayLiteral("1")
                                        << QByteArrayLiteral("false")
                                        << QStringLiteral("totalTicks");
    }

    void jobProgressRejectsInvalidCounters()
    {
        QFETCH(QByteArray, ticks);
        QFETCH(QByteArray, totalTicks);
        QFETCH(QString, field);

        const QByteArray payload = jobProgressPayload(
            QByteArrayLiteral("\"IN_PROGRESS\""), ticks, totalTicks);
        const auto progress = Xc2JsonCodec::jobProgress(payload);
        QVERIFY(!progress.ok());
        QVERIFY2(progress.error.message.contains(field),
                 qPrintable(progress.error.message));
        QCOMPARE(progress.error.rawPayload, payload);
    }

    void jobProgressAcceptsOnlyFiveWireStatuses_data()
    {
        QTest::addColumn<QByteArray>("status");
        QTest::addColumn<Xc2JobState>("expected");

        QTest::newRow("in progress") << QByteArrayLiteral("IN_PROGRESS")
                                      << Xc2JobState::InProgress;
        QTest::newRow("finished") << QByteArrayLiteral("FINISHED")
                                   << Xc2JobState::Finished;
        QTest::newRow("canceled") << QByteArrayLiteral("CANCELED")
                                   << Xc2JobState::Canceled;
        QTest::newRow("error") << QByteArrayLiteral("ERROR")
                                << Xc2JobState::Error;
        QTest::newRow("not authorized")
            << QByteArrayLiteral("NOT_AUTHORIZED")
            << Xc2JobState::NotAuthorized;
    }

    void jobProgressAcceptsOnlyFiveWireStatuses()
    {
        QFETCH(QByteArray, status);
        QFETCH(Xc2JobState, expected);

        const auto progress = Xc2JsonCodec::jobProgress(jobProgressPayload(
            QByteArrayLiteral("\"") + status + QByteArrayLiteral("\"")));
        QVERIFY2(progress.ok(), qPrintable(progress.error.message));
        QCOMPARE(progress.value->state, expected);
    }

    void jobProgressRejectsStatusAliasesAndFieldCasing_data()
    {
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<QString>("errorToken");

        const QList<QByteArray> statuses = {
            QByteArrayLiteral("CREATED"), QByteArrayLiteral("Created"),
            QByteArrayLiteral("INPROGRESS"), QByteArrayLiteral("in_progress"),
            QByteArrayLiteral("CANCELLED"), QByteArrayLiteral("finished"),
        };
        for (const QByteArray &status : statuses) {
            QTest::newRow(status.constData())
                << jobProgressPayload(QByteArrayLiteral("\"") + status
                                      + QByteArrayLiteral("\""))
                << QStringLiteral("Unknown job status");
        }

        QTest::newRow("status field casing")
            << QByteArrayLiteral(
                   R"({"jobId":"synthetic","Status":"IN_PROGRESS","ticks":1,"totalTicks":2,"message":{"id":1,"text":"x"}})")
            << QStringLiteral("status");
        QTest::newRow("jobId field casing")
            << QByteArrayLiteral(
                   R"({"jobID":"synthetic","status":"IN_PROGRESS","ticks":1,"totalTicks":2,"message":{"id":1,"text":"x"}})")
            << QStringLiteral("jobId");
        QTest::newRow("ticks field casing")
            << QByteArrayLiteral(
                   R"({"jobId":"synthetic","status":"IN_PROGRESS","Ticks":1,"totalTicks":2,"message":{"id":1,"text":"x"}})")
            << QStringLiteral("ticks");
        QTest::newRow("totalTicks field casing")
            << QByteArrayLiteral(
                   R"({"jobId":"synthetic","status":"IN_PROGRESS","ticks":1,"totalticks":2,"message":{"id":1,"text":"x"}})")
            << QStringLiteral("totalTicks");
        QTest::newRow("message field casing")
            << QByteArrayLiteral(
                   R"({"jobId":"synthetic","status":"IN_PROGRESS","ticks":1,"totalTicks":2,"Message":{"id":1,"text":"x"}})")
            << QStringLiteral("message");
        QTest::newRow("status wrong type")
            << QByteArrayLiteral(
                   R"({"jobId":"synthetic","status":7,"ticks":1,"totalTicks":2,"message":{"id":1,"text":"x"}})")
            << QStringLiteral("status");
        QTest::newRow("jobId wrong type")
            << QByteArrayLiteral(
                   R"({"jobId":7,"status":"IN_PROGRESS","ticks":1,"totalTicks":2,"message":{"id":1,"text":"x"}})")
            << QStringLiteral("jobId");
    }

    void jobProgressRejectsStatusAliasesAndFieldCasing()
    {
        QFETCH(QByteArray, payload);
        QFETCH(QString, errorToken);
        const auto progress = Xc2JsonCodec::jobProgress(payload);
        QVERIFY(!progress.ok());
        QVERIFY2(progress.error.message.contains(errorToken),
                 qPrintable(progress.error.message));
        QCOMPARE(progress.error.rawPayload, payload);
    }

    void jobProgressRetainsUnknownTopLevelFieldsInRaw()
    {
        const QByteArray payload = QByteArrayLiteral(
            R"({"jobId":"synthetic","status":"FINISHED","ticks":1,"totalTicks":2,"message":null,"future":{"nested":true}})");
        const auto progress = Xc2JsonCodec::jobProgress(payload);

        QVERIFY2(progress.ok(), qPrintable(progress.error.message));
        QVERIFY(progress.value->raw.value(QStringLiteral("future")).isObject());
    }
};

QTEST_APPLESS_MAIN(Xc2JsonCodecTest)
#include "test_Xc2JsonCodec.moc"
