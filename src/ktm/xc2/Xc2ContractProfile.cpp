#include "ktm/xc2/Xc2ContractProfile.h"

namespace ktm::xc2 {

const Xc2ContractProfile &Xc2ContractProfile::approved()
{
    static const Xc2ContractProfile profile;
    return profile;
}

QList<Endpoint> Xc2ContractProfile::allEndpoints()
{
    return {
        Endpoint::ServiceStatus,
        Endpoint::Shutdown,
        Endpoint::CurrentUser,
        Endpoint::Login,
        Endpoint::Logout,
        Endpoint::DeviceLookup,
        Endpoint::DeviceGet,
        Endpoint::DeviceGetSelected,
        Endpoint::DeviceApply,
        Endpoint::DeviceClose,
        Endpoint::SettingsGet,
        Endpoint::SettingsSet,
        Endpoint::VehicleDetect,
        Endpoint::VehicleManufacturers,
        Endpoint::VehicleSeries,
        Endpoint::VehicleModels,
        Endpoint::VehicleSelect,
        Endpoint::VehicleInfo,
        Endpoint::AutoScan,
        Endpoint::EcuDomains,
        Endpoint::EcuOpen,
        Endpoint::EcuClose,
        Endpoint::EcuScan,
        Endpoint::EcuClearDtc,
        Endpoint::EcuMeasurementsGet,
        Endpoint::EcuMeasurementsStart,
        Endpoint::EcuMeasurementsStop,
        Endpoint::EcuExecuteFlow,
        Endpoint::VehicleExecuteFlow,
        Endpoint::FlowUpdateGui,
        Endpoint::DownloadMapping,
        Endpoint::FlashAutomatic,
        Endpoint::FlashFile,
    };
}

QList<Topic> Xc2ContractProfile::allTopics()
{
    return {
        Topic::VciStatus,
        Topic::VehicleInfo,
        Topic::Ecu,
        Topic::Progress,
        Topic::MeasurementValues,
        Topic::FlowGui,
        Topic::FlowProgress,
        Topic::Login,
    };
}

QByteArray Xc2ContractProfile::backendSha256Hex() const
{
    return QByteArray("B64A38C47F74D02145F462CDDA16EEA421A2170146155602575A1BD8B7E62840");
}

QByteArray Xc2ContractProfile::providerSha256Hex() const
{
    return QByteArray("3F790B47D3F968FE2E757F309BFFDC0448C2FCE8A488DFA7C00F1FEDCB2A3075");
}

QString Xc2ContractProfile::profileId() const
{
    return QStringLiteral("xc2-approved-2026-07-16");
}

QString Xc2ContractProfile::restContext() const
{
    return QStringLiteral("/xc2/1.0");
}

QString Xc2ContractProfile::webSocketPath() const
{
    return QStringLiteral("/xc2-websocket");
}

QString Xc2ContractProfile::supportedPduApiShortName() const
{
    return QStringLiteral("AVL Ditest VCI2K_DPDU_API");
}

EndpointSpec Xc2ContractProfile::endpoint(Endpoint endpoint) const
{
    using Semantics = OperationSemantics;

    switch (endpoint) {
    case Endpoint::ServiceStatus:
        return {QStringLiteral("serviceStatus/status"), HttpMethod::Get,
                Semantics::ReadOnly, 0};
    case Endpoint::Shutdown:
        return {QStringLiteral("serviceStatus/shutdown"), HttpMethod::PostForm,
                Semantics::StateChanging, 0};
    case Endpoint::CurrentUser:
        return {QStringLiteral("auth/currentUser"), HttpMethod::Get,
                Semantics::ReadOnly, 0};
    case Endpoint::Login:
        return {QStringLiteral("auth/login"), HttpMethod::PostForm,
                Semantics::StateChanging, 0};
    case Endpoint::Logout:
        return {QStringLiteral("auth/logout"), HttpMethod::PostForm,
                Semantics::StateChanging, 0};
    case Endpoint::DeviceLookup:
        return {QStringLiteral("device/lookup"), HttpMethod::Get,
                Semantics::StateChanging, 0};
    case Endpoint::DeviceGet:
        return {QStringLiteral("device/get"), HttpMethod::Get,
                Semantics::ReadOnly, 0};
    case Endpoint::DeviceGetSelected:
        return {QStringLiteral("device/getSelected"), HttpMethod::Get,
                Semantics::ReadOnly, 0};
    case Endpoint::DeviceApply:
        return {QStringLiteral("device/apply"), HttpMethod::PostJson,
                Semantics::StateChanging, 0};
    case Endpoint::DeviceClose:
        return {QStringLiteral("device/close"), HttpMethod::PostJson,
                Semantics::StateChanging, 0};
    case Endpoint::SettingsGet:
        return {QStringLiteral("settings/get"), HttpMethod::Get,
                Semantics::ReadOnly, 0};
    case Endpoint::SettingsSet:
        return {QStringLiteral("settings/set"), HttpMethod::PostJson,
                Semantics::StateChanging, 0};
    case Endpoint::VehicleDetect:
        return {QStringLiteral("vehicle/detect"), HttpMethod::Get,
                Semantics::StateChanging, 0};
    case Endpoint::VehicleManufacturers:
        return {QStringLiteral("vehicle/manufacturer"), HttpMethod::PostJson,
                Semantics::ReadOnly, 0};
    case Endpoint::VehicleSeries:
        return {QStringLiteral("vehicle/series"), HttpMethod::PostJson,
                Semantics::ReadOnly, 0};
    case Endpoint::VehicleModels:
        return {QStringLiteral("vehicle/vehicle"), HttpMethod::PostJson,
                Semantics::ReadOnly, 0};
    case Endpoint::VehicleSelect:
        return {QStringLiteral("vehicle/select"), HttpMethod::PostJson,
                Semantics::StateChanging, 0};
    case Endpoint::VehicleInfo:
        return {QStringLiteral("vehicleinfo"), HttpMethod::Get,
                Semantics::ReadOnly, 0};
    case Endpoint::AutoScan:
        return {QStringLiteral("autoscan/start"), HttpMethod::Get,
                Semantics::StateChanging, 0};
    case Endpoint::EcuDomains:
        return {QStringLiteral("ecu/ecuDomains"), HttpMethod::Get,
                Semantics::ReadOnly, 0};
    case Endpoint::EcuOpen:
        return {QStringLiteral("ecu/open/{ecuId}"), HttpMethod::PostJson,
                Semantics::StateChanging, 0};
    case Endpoint::EcuClose:
        return {QStringLiteral("ecu/close/{ecuId}"), HttpMethod::PostJson,
                Semantics::StateChanging, 0};
    case Endpoint::EcuScan:
        return {QStringLiteral("ecu/scan"), HttpMethod::PostForm,
                Semantics::StateChanging, 0};
    case Endpoint::EcuClearDtc:
        return {QStringLiteral("ecu/clearDtc"), HttpMethod::PostForm,
                Semantics::StateChanging, 0};
    case Endpoint::EcuMeasurementsGet:
        return {QStringLiteral("ecu/measurements/{ecuId}"), HttpMethod::Get,
                Semantics::ReadOnly, 0};
    case Endpoint::EcuMeasurementsStart:
        return {QStringLiteral("ecu/measurements/{ecuId}"), HttpMethod::PostJson,
                Semantics::StateChanging, 0};
    case Endpoint::EcuMeasurementsStop:
        return {QStringLiteral("ecu/measurements/"), HttpMethod::Delete,
                Semantics::StateChanging, 0};
    case Endpoint::EcuExecuteFlow:
        return {QStringLiteral("ecu/executeFlow"), HttpMethod::PostForm,
                Semantics::StateChanging, 0};
    case Endpoint::VehicleExecuteFlow:
        return {QStringLiteral("vehicle/executeFlow"), HttpMethod::PostForm,
                Semantics::StateChanging, 0};
    case Endpoint::FlowUpdateGui:
        return {QStringLiteral("flow/updateGui"), HttpMethod::PostJson,
                Semantics::StateChanging, 0};
    case Endpoint::DownloadMapping:
        return {QStringLiteral("ecu/downloadMapping"), HttpMethod::PostForm,
                Semantics::StateChanging, 0};
    case Endpoint::FlashAutomatic:
        return {QStringLiteral("ecu/flashAutomatic"), HttpMethod::PostForm,
                Semantics::StateChanging, 0};
    case Endpoint::FlashFile:
        return {QStringLiteral("ecu/flashFile"), HttpMethod::PostForm,
                Semantics::StateChanging, 0};
    }

    return {};
}

QString Xc2ContractProfile::topic(Topic topic) const
{
    switch (topic) {
    case Topic::VciStatus:
        return QStringLiteral("/topic/vci/status");
    case Topic::VehicleInfo:
        return QStringLiteral("/topic/vehicleinfo");
    case Topic::Ecu:
        return QStringLiteral("/topic/ecu");
    case Topic::Progress:
        return QStringLiteral("/topic/progress");
    case Topic::MeasurementValues:
        return QStringLiteral("/topic/measurementValues");
    case Topic::FlowGui:
        return QStringLiteral("/topic/flowGUI");
    case Topic::FlowProgress:
        return QStringLiteral("/topic/flowProgress");
    case Topic::Login:
        return QStringLiteral("/topic/login");
    }

    return {};
}

} // namespace ktm::xc2
