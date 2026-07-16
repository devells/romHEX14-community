#include <QtTest>

#include "ktm/xc2/Xc2ContractProfile.h"

using namespace ktm::xc2;

class Xc2ContractProfileTest final : public QObject {
    Q_OBJECT
private slots:
    void approvedHashesAreExact()
    {
        const auto &p = Xc2ContractProfile::approved();
        QCOMPARE(p.backendSha256Hex(),
                 QByteArray("B64A38C47F74D02145F462CDDA16EEA421A2170146155602575A1BD8B7E62840"));
        QCOMPARE(p.providerSha256Hex(),
                 QByteArray("3F790B47D3F968FE2E757F309BFFDC0448C2FCE8A488DFA7C00F1FEDCB2A3075"));
        QCOMPARE(p.restContext(), QStringLiteral("/xc2/1.0"));
        QCOMPARE(p.webSocketPath(), QStringLiteral("/xc2-websocket"));
    }

    void routesAndTopicsAreUniqueAndRelative()
    {
        QSet<QString> routes;
        for (Endpoint endpoint : Xc2ContractProfile::allEndpoints()) {
            const EndpointSpec spec = Xc2ContractProfile::approved().endpoint(endpoint);
            QVERIFY2(!spec.path.startsWith(QStringLiteral("http")), qPrintable(spec.path));
            const QString routeKey = QStringLiteral("%1:%2")
                .arg(static_cast<int>(spec.method))
                .arg(spec.path);
            QVERIFY2(!routes.contains(routeKey), qPrintable(routeKey));
            routes.insert(routeKey);
        }
        QSet<QString> topics;
        for (Topic topic : Xc2ContractProfile::allTopics()) {
            const QString destination = Xc2ContractProfile::approved().topic(topic);
            QVERIFY(destination.startsWith(QStringLiteral("/topic/")));
            QVERIFY(!topics.contains(destination));
            topics.insert(destination);
        }
    }

    void stateChangingEndpointsNeverRetry()
    {
        for (Endpoint endpoint : Xc2ContractProfile::allEndpoints()) {
            const EndpointSpec spec = Xc2ContractProfile::approved().endpoint(endpoint);
            if (spec.semantics == OperationSemantics::StateChanging)
                QCOMPARE(spec.maxAutomaticRetries, 0);
        }
    }

    void deviceSelectionIsRestrictedToAvlVci2k()
    {
        const auto &p = Xc2ContractProfile::approved();
        QCOMPARE(p.supportedPduApiShortName(),
                 QStringLiteral("AVL Ditest VCI2K_DPDU_API"));
        const EndpointSpec apply = p.endpoint(Endpoint::DeviceApply);
        QCOMPARE(apply.path, QStringLiteral("device/apply"));
        QCOMPARE(apply.method, HttpMethod::PostJson);
        QCOMPARE(apply.semantics, OperationSemantics::StateChanging);
        QCOMPARE(apply.maxAutomaticRetries, 0);
    }

    void manualVehicleSelectionAndVehicleFlowRoutesAreFrozen()
    {
        const auto &p = Xc2ContractProfile::approved();
        QCOMPARE(p.endpoint(Endpoint::VehicleManufacturers).path,
                 QStringLiteral("vehicle/manufacturer"));
        QCOMPARE(p.endpoint(Endpoint::VehicleSeries).path,
                 QStringLiteral("vehicle/series"));
        QCOMPARE(p.endpoint(Endpoint::VehicleModels).path,
                 QStringLiteral("vehicle/vehicle"));
        QCOMPARE(p.endpoint(Endpoint::VehicleSelect).path,
                 QStringLiteral("vehicle/select"));
        QCOMPARE(p.endpoint(Endpoint::VehicleExecuteFlow).path,
                 QStringLiteral("vehicle/executeFlow"));
        QCOMPARE(p.endpoint(Endpoint::VehicleExecuteFlow).method,
                 HttpMethod::PostForm);
    }
};

QTEST_APPLESS_MAIN(Xc2ContractProfileTest)
#include "test_Xc2ContractProfile.moc"
