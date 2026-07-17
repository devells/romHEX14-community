#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace ktm::xc2 {

enum class OperationSemantics { ReadOnly, StateChanging };
enum class HttpMethod { Get, PostJson, PostForm, Delete };

enum class Endpoint {
    ServiceStatus, Shutdown, CurrentUser, Login, Logout,
    DeviceLookup, DeviceGet, DeviceGetSelected, DeviceApply, DeviceClose,
    SettingsGet, SettingsSet,
    VehicleDetect, VehicleManufacturers, VehicleSeries, VehicleModels,
    VehicleSelect, VehicleInfo, AutoScan,
    EcuDomains, EcuOpen, EcuClose, EcuScan, EcuClearDtc,
    EcuMeasurementsGet, EcuMeasurementsStart,
    EcuMeasurementsStop, EcuExecuteFlow, VehicleExecuteFlow, FlowUpdateGui,
    DownloadMapping, FlashAutomatic, FlashFile
};

enum class Topic {
    VciStatus, VehicleInfo, Ecu, Progress, MeasurementValues,
    FlowGui, FlowProgress, Login
};

struct EndpointSpec {
    QString path;
    HttpMethod method = HttpMethod::Get;
    OperationSemantics semantics = OperationSemantics::ReadOnly;
    int maxAutomaticRetries = 0;
};

class Xc2ContractProfile final {
public:
    static const Xc2ContractProfile &approved();
    static QList<Endpoint> allEndpoints();
    static QList<Topic> allTopics();

    QByteArray backendSha256Hex() const;
    QByteArray providerSha256Hex() const;
    QString profileId() const;
    QString restContext() const;
    QString webSocketPath() const;
    QString supportedPduApiShortName() const;
    EndpointSpec endpoint(Endpoint endpoint) const;
    QString topic(Topic topic) const;
    QList<Topic> foundationProbeTopics() const;
    QStringList foundationProbePermissions() const;

private:
    Xc2ContractProfile() = default;
};

} // namespace ktm::xc2
