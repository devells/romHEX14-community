#pragma once

#include "Xc2Models.h"

namespace ktm::xc2 {

class Xc2JsonCodec final {
public:
    static Xc2Result<Xc2ServiceStatus> serviceStatus(const QByteArray &body);
    static Xc2Result<Xc2CurrentUser> currentUser(const QByteArray &body);
    static Xc2Result<QList<Xc2VciDevice>> vciDevices(const QByteArray &body);
    static QJsonObject vciDeviceJson(const Xc2VciDevice &device);
    static Xc2Result<Xc2JobAccepted> jobAccepted(const QByteArray &body);
    static Xc2Result<Xc2JobProgress> jobProgress(const QByteArray &body);
    static Xc2Error error(const QByteArray &body,
                          int httpStatus,
                          const QString &endpoint);
};

} // namespace ktm::xc2
