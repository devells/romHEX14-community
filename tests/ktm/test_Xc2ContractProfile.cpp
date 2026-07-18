#include <QtTest>

#include "ktm/xc2/Xc2ContractProfile.h"

using namespace ktm::xc2;

class Xc2ContractProfileTest final : public QObject {
    Q_OBJECT
private slots:
    void approvedEndpointMatrixIsExact()
    {
        struct ExpectedEndpoint {
            Endpoint endpoint;
            const char *path;
            HttpMethod method;
            OperationSemantics semantics;
        };

        const QList<ExpectedEndpoint> expected = {
            {Endpoint::ServiceStatus, "serviceStatus/status", HttpMethod::Get,
             OperationSemantics::ReadOnly},
            {Endpoint::Shutdown, "serviceStatus/shutdown", HttpMethod::PostForm,
             OperationSemantics::StateChanging},
            {Endpoint::CurrentUser, "auth/currentUser", HttpMethod::Get,
             OperationSemantics::ReadOnly},
            {Endpoint::Login, "auth/login", HttpMethod::PostForm,
             OperationSemantics::StateChanging},
            {Endpoint::Logout, "auth/logout", HttpMethod::PostForm,
             OperationSemantics::StateChanging},
            {Endpoint::DeviceLookup, "device/lookup", HttpMethod::Get,
             OperationSemantics::StateChanging},
            {Endpoint::DeviceGet, "device/get", HttpMethod::Get,
             OperationSemantics::ReadOnly},
            {Endpoint::DeviceGetSelected, "device/getSelected", HttpMethod::Get,
             OperationSemantics::ReadOnly},
            {Endpoint::DeviceApply, "device/apply", HttpMethod::PostJson,
             OperationSemantics::StateChanging},
            {Endpoint::DeviceClose, "device/close", HttpMethod::PostJson,
             OperationSemantics::StateChanging},
            {Endpoint::SettingsGet, "settings/get", HttpMethod::Get,
             OperationSemantics::ReadOnly},
            {Endpoint::SettingsSet, "settings/set", HttpMethod::PostJson,
             OperationSemantics::StateChanging},
            {Endpoint::VehicleDetect, "vehicle/detect", HttpMethod::Get,
             OperationSemantics::StateChanging},
            {Endpoint::VehicleManufacturers, "vehicle/manufacturer",
             HttpMethod::PostJson, OperationSemantics::ReadOnly},
            {Endpoint::VehicleSeries, "vehicle/series", HttpMethod::PostJson,
             OperationSemantics::ReadOnly},
            {Endpoint::VehicleModels, "vehicle/vehicle", HttpMethod::PostJson,
             OperationSemantics::ReadOnly},
            {Endpoint::VehicleSelect, "vehicle/select", HttpMethod::PostJson,
             OperationSemantics::StateChanging},
            {Endpoint::VehicleInfo, "vehicleinfo", HttpMethod::Get,
             OperationSemantics::ReadOnly},
            {Endpoint::AutoScan, "autoscan/start", HttpMethod::Get,
             OperationSemantics::StateChanging},
            {Endpoint::EcuDomains, "ecu/ecuDomains", HttpMethod::Get,
             OperationSemantics::ReadOnly},
            {Endpoint::EcuOpen, "ecu/open/{ecuId}", HttpMethod::PostJson,
             OperationSemantics::StateChanging},
            {Endpoint::EcuClose, "ecu/close/{ecuId}", HttpMethod::PostJson,
             OperationSemantics::StateChanging},
            {Endpoint::EcuScan, "ecu/scan", HttpMethod::PostForm,
             OperationSemantics::StateChanging},
            {Endpoint::EcuClearDtc, "ecu/clearDtc", HttpMethod::PostForm,
             OperationSemantics::StateChanging},
            {Endpoint::EcuMeasurementsGet, "ecu/measurements/{ecuId}",
             HttpMethod::Get, OperationSemantics::ReadOnly},
            {Endpoint::EcuMeasurementsStart, "ecu/measurements/{ecuId}",
             HttpMethod::PostJson, OperationSemantics::StateChanging},
            {Endpoint::EcuMeasurementsStop, "ecu/measurements/",
             HttpMethod::Delete, OperationSemantics::StateChanging},
            {Endpoint::EcuExecuteFlow, "ecu/executeFlow", HttpMethod::PostForm,
             OperationSemantics::StateChanging},
            {Endpoint::VehicleExecuteFlow, "vehicle/executeFlow",
             HttpMethod::PostForm, OperationSemantics::StateChanging},
            {Endpoint::FlowUpdateGui, "flow/updateGui", HttpMethod::PostJson,
             OperationSemantics::StateChanging},
            {Endpoint::DownloadMapping, "ecu/downloadMapping",
             HttpMethod::PostForm, OperationSemantics::StateChanging},
            {Endpoint::FlashAutomatic, "ecu/flashAutomatic",
             HttpMethod::PostForm, OperationSemantics::StateChanging},
            {Endpoint::FlashFile, "ecu/flashFile", HttpMethod::PostForm,
             OperationSemantics::StateChanging},
        };

        const QList<Endpoint> actual = Xc2ContractProfile::allEndpoints();
        QCOMPARE(actual.size(), expected.size());
        for (qsizetype i = 0; i < expected.size(); ++i) {
            const ExpectedEndpoint &row = expected.at(i);
            QCOMPARE(actual.at(i), row.endpoint);
            const EndpointSpec spec =
                Xc2ContractProfile::approved().endpoint(row.endpoint);
            QCOMPARE(spec.path, QString::fromLatin1(row.path));
            QCOMPARE(spec.method, row.method);
            QCOMPARE(spec.semantics, row.semantics);
            QCOMPARE(spec.maxAutomaticRetries, 0);
        }
    }

    void approvedTopicMatrixIsExact()
    {
        struct ExpectedTopic {
            Topic topic;
            const char *destination;
        };

        const QList<ExpectedTopic> expected = {
            {Topic::VciStatus, "/topic/vci/status"},
            {Topic::VehicleInfo, "/topic/vehicleinfo"},
            {Topic::Ecu, "/topic/ecu"},
            {Topic::Progress, "/topic/progress"},
            {Topic::MeasurementValues, "/topic/measurementValues"},
            {Topic::FlowGui, "/topic/flowGUI"},
            {Topic::FlowProgress, "/topic/flowProgress"},
            {Topic::Login, "/topic/login"},
        };

        const QList<Topic> actual = Xc2ContractProfile::allTopics();
        QCOMPARE(actual.size(), expected.size());
        for (qsizetype i = 0; i < expected.size(); ++i) {
            const ExpectedTopic &row = expected.at(i);
            QCOMPARE(actual.at(i), row.topic);
            QCOMPARE(Xc2ContractProfile::approved().topic(row.topic),
                     QString::fromLatin1(row.destination));
        }
    }

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

    void callableProfileExcludesStaleFrontendGetFunctions()
    {
        const QString deadRoute = QStringLiteral("ecu/getFunctions/{ecuId}");
        for (Endpoint endpoint : Xc2ContractProfile::allEndpoints()) {
            const EndpointSpec spec =
                Xc2ContractProfile::approved().endpoint(endpoint);
            QVERIFY2(spec.path != deadRoute, qPrintable(spec.path));
        }
    }

    void foundationProbePolicyIsExactOrderedAndCaseSensitive()
    {
        const Xc2ContractProfile &profile = Xc2ContractProfile::approved();
        QCOMPARE(profile.foundationProbeTopics(),
                 QList<Topic>({Topic::VciStatus, Topic::Login}));
        QCOMPARE(profile.foundationProbePermissions(),
                 QStringList({QStringLiteral("EcuDiagnosticRead")}));
        QVERIFY(!profile.foundationProbePermissions().contains(
            QStringLiteral("ecudiagnosticread")));
        QVERIFY(!profile.foundationProbeTopics().contains(Topic::Progress));
    }
};

QTEST_APPLESS_MAIN(Xc2ContractProfileTest)
#include "test_Xc2ContractProfile.moc"
