#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>

namespace ktm::xc2 {

enum class Xc2ErrorCategory {
    None,
    Prerequisite,
    Backend,
    Contract,
    Session,
    Vci,
    Vehicle,
    Job,
    FlashCritical,
    Transport
};

enum class Xc2TransportReason { None, Canceled, Timeout, Network };

struct Xc2Error {
    Xc2ErrorCategory category = Xc2ErrorCategory::None;
    Xc2TransportReason transportReason = Xc2TransportReason::None;
    QString message;
    int httpStatus = 0;
    int xc2Status = 0;
    int xc2Code = 0;
    QString developerMessage;
    QString info;
    QString endpoint;
    QString jobId;
    QString ecuId;
    QJsonObject dpdu;
    QByteArray rawPayload;
};

template<typename T>
struct Xc2Result {
    std::optional<T> value;
    Xc2Error error;

    bool ok() const { return value.has_value(); }
    static Xc2Result success(T v) { return {std::move(v), {}}; }
    static Xc2Result failure(Xc2Error e)
    {
        return {std::nullopt, std::move(e)};
    }
};

struct Xc2ServiceStatus {
    bool alive = false;
};

struct Xc2CurrentUser {
    QString loginName;
    QString name;
    std::optional<QString> dealerId;
    std::optional<QString> country;
    std::optional<QString> address1;
    std::optional<QString> address2;
    std::optional<QString> dealerType;
    std::optional<QString> audience;
    QStringList permissions;
    std::optional<QString> sessionIndex;
};

struct Xc2JobAccepted {
    QString jobId;
};

struct Xc2VciDevice {
    QString id;
    QString name;
    QString internalName;
    std::optional<QString> additionalModuleInformation;
};

enum class Xc2JobState {
    Created,
    InProgress,
    Finished,
    Canceled,
    Error,
    NotAuthorized
};

struct Xc2LocalizedText {
    qint64 id = 0;
    QString text;
};

struct Xc2JobProgress {
    QString jobId;
    Xc2JobState state = Xc2JobState::Created;
    qint32 ticks = 0;
    qint32 totalTicks = 0;
    std::optional<Xc2LocalizedText> message;
    QJsonObject raw;
};

} // namespace ktm::xc2

Q_DECLARE_METATYPE(ktm::xc2::Xc2Error)
Q_DECLARE_METATYPE(ktm::xc2::Xc2TransportReason)
Q_DECLARE_METATYPE(ktm::xc2::Xc2ServiceStatus)
Q_DECLARE_METATYPE(ktm::xc2::Xc2CurrentUser)
Q_DECLARE_METATYPE(ktm::xc2::Xc2JobAccepted)
Q_DECLARE_METATYPE(ktm::xc2::Xc2VciDevice)
Q_DECLARE_METATYPE(ktm::xc2::Xc2LocalizedText)
Q_DECLARE_METATYPE(ktm::xc2::Xc2JobProgress)
Q_DECLARE_METATYPE(ktm::xc2::Xc2Result<ktm::xc2::Xc2ServiceStatus>)
Q_DECLARE_METATYPE(ktm::xc2::Xc2Result<ktm::xc2::Xc2CurrentUser>)
Q_DECLARE_METATYPE(ktm::xc2::Xc2Result<QList<ktm::xc2::Xc2VciDevice>>)
Q_DECLARE_METATYPE(ktm::xc2::Xc2Result<ktm::xc2::Xc2JobAccepted>)
Q_DECLARE_METATYPE(ktm::xc2::Xc2Result<ktm::xc2::Xc2JobProgress>)
