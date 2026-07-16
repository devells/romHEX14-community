#include "Xc2JsonCodec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

#include <cmath>
#include <limits>

namespace ktm::xc2 {
namespace {

Xc2Error contractError(const QByteArray &body, QString message)
{
    Xc2Error error;
    error.category = Xc2ErrorCategory::Contract;
    error.message = std::move(message);
    error.rawPayload = body;
    return error;
}

template<typename T>
Xc2Result<T> contractFailure(const QByteArray &body, QString message)
{
    return Xc2Result<T>::failure(contractError(body, std::move(message)));
}

bool parseDocument(const QByteArray &body,
                   QJsonDocument &document,
                   QString &failureMessage)
{
    QJsonParseError parseError;
    document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error == QJsonParseError::NoError)
        return true;

    failureMessage = QStringLiteral("Invalid JSON at offset %1: %2")
                         .arg(parseError.offset)
                         .arg(parseError.errorString());
    return false;
}

bool requireString(const QJsonObject &object,
                   const QString &field,
                   QString &value,
                   QString &failureMessage)
{
    if (!object.contains(field)) {
        failureMessage = QStringLiteral("Missing required field '%1'").arg(field);
        return false;
    }
    const QJsonValue jsonValue = object.value(field);
    if (!jsonValue.isString()) {
        failureMessage = QStringLiteral("Field '%1' must be a string").arg(field);
        return false;
    }
    value = jsonValue.toString();
    return true;
}

bool optionalNullableString(const QJsonObject &object,
                            const QString &field,
                            std::optional<QString> &value,
                            QString &failureMessage)
{
    if (!object.contains(field) || object.value(field).isNull()) {
        value.reset();
        return true;
    }
    const QJsonValue jsonValue = object.value(field);
    if (!jsonValue.isString()) {
        failureMessage = QStringLiteral("Field '%1' must be a string or null")
                             .arg(field);
        return false;
    }
    value = jsonValue.toString();
    return true;
}

bool requireStringList(const QJsonObject &object,
                       const QString &field,
                       QStringList &value,
                       QString &failureMessage)
{
    if (!object.contains(field)) {
        failureMessage = QStringLiteral("Missing required field '%1'").arg(field);
        return false;
    }
    const QJsonValue jsonValue = object.value(field);
    if (!jsonValue.isArray()) {
        failureMessage = QStringLiteral("Field '%1' must be an array of strings")
                             .arg(field);
        return false;
    }

    const QJsonArray array = jsonValue.toArray();
    value.clear();
    value.reserve(array.size());
    for (qsizetype i = 0; i < array.size(); ++i) {
        if (!array.at(i).isString()) {
            failureMessage = QStringLiteral("Field '%1[%2]' must be a string")
                                 .arg(field)
                                 .arg(i);
            return false;
        }
        value.append(array.at(i).toString());
    }
    return true;
}

bool requireInteger64(const QJsonObject &object,
                      const QString &field,
                      qint64 &value,
                      QString &failureMessage)
{
    if (!object.contains(field)) {
        failureMessage = QStringLiteral("Missing required field '%1'").arg(field);
        return false;
    }
    const QJsonValue jsonValue = object.value(field);
    if (!jsonValue.isDouble()) {
        failureMessage = QStringLiteral("Field '%1' must be an integer").arg(field);
        return false;
    }

    const double number = jsonValue.toDouble();
    constexpr double qint64Minimum = -9223372036854775808.0;
    constexpr double qint64UpperBound = 9223372036854775808.0;
    if (!std::isfinite(number)
        || std::trunc(number) != number
        || number < qint64Minimum
        || number >= qint64UpperBound) {
        failureMessage = QStringLiteral("Field '%1' must be a 64-bit integer")
                             .arg(field);
        return false;
    }
    value = static_cast<qint64>(number);
    return true;
}

bool requireInteger(const QJsonObject &object,
                    const QString &field,
                    int &value,
                    QString &failureMessage)
{
    qint64 parsed = 0;
    if (!requireInteger64(object, field, parsed, failureMessage))
        return false;
    if (parsed < std::numeric_limits<int>::min()
        || parsed > std::numeric_limits<int>::max()) {
        failureMessage = QStringLiteral("Field '%1' must be a 32-bit integer")
                             .arg(field);
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

std::optional<Xc2JobState> jobState(const QString &status)
{
    if (status == QStringLiteral("IN_PROGRESS"))
        return Xc2JobState::InProgress;
    if (status == QStringLiteral("FINISHED"))
        return Xc2JobState::Finished;
    if (status == QStringLiteral("CANCELED"))
        return Xc2JobState::Canceled;
    if (status == QStringLiteral("ERROR"))
        return Xc2JobState::Error;
    if (status == QStringLiteral("NOT_AUTHORIZED"))
        return Xc2JobState::NotAuthorized;
    return std::nullopt;
}

} // namespace

Xc2Result<Xc2ServiceStatus> Xc2JsonCodec::serviceStatus(const QByteArray &body)
{
    if (body.trimmed() == QByteArray("alive"))
        return Xc2Result<Xc2ServiceStatus>::success({true});
    return contractFailure<Xc2ServiceStatus>(
        body, QStringLiteral("Service status must be 'alive'"));
}

Xc2Result<Xc2CurrentUser> Xc2JsonCodec::currentUser(const QByteArray &body)
{
    QJsonDocument document;
    QString failureMessage;
    if (!parseDocument(body, document, failureMessage))
        return contractFailure<Xc2CurrentUser>(body, std::move(failureMessage));
    if (!document.isObject()) {
        return contractFailure<Xc2CurrentUser>(
            body, QStringLiteral("Current user payload must be a JSON object"));
    }

    const QJsonObject object = document.object();
    Xc2CurrentUser user;
    if (!requireString(object, QStringLiteral("loginName"), user.loginName,
                       failureMessage)
        || !requireString(object, QStringLiteral("name"), user.name,
                          failureMessage)
        || !requireStringList(object, QStringLiteral("permissions"),
                              user.permissions, failureMessage)
        || !optionalNullableString(object, QStringLiteral("dealerId"),
                                   user.dealerId, failureMessage)
        || !optionalNullableString(object, QStringLiteral("country"),
                                   user.country, failureMessage)
        || !optionalNullableString(object, QStringLiteral("address1"),
                                   user.address1, failureMessage)
        || !optionalNullableString(object, QStringLiteral("address2"),
                                   user.address2, failureMessage)
        || !optionalNullableString(object, QStringLiteral("dealerType"),
                                   user.dealerType, failureMessage)
        || !optionalNullableString(object, QStringLiteral("audience"),
                                   user.audience, failureMessage)
        || !optionalNullableString(object, QStringLiteral("sessionIndex"),
                                   user.sessionIndex, failureMessage)) {
        return contractFailure<Xc2CurrentUser>(body, std::move(failureMessage));
    }
    return Xc2Result<Xc2CurrentUser>::success(std::move(user));
}

Xc2Result<QList<Xc2VciDevice>> Xc2JsonCodec::vciDevices(
    const QByteArray &body)
{
    QJsonDocument document;
    QString failureMessage;
    if (!parseDocument(body, document, failureMessage)) {
        return contractFailure<QList<Xc2VciDevice>>(
            body, std::move(failureMessage));
    }
    if (!document.isArray()) {
        return contractFailure<QList<Xc2VciDevice>>(
            body, QStringLiteral("VCI device payload must be a JSON array"));
    }

    const QJsonArray array = document.array();
    QList<Xc2VciDevice> devices;
    devices.reserve(array.size());
    for (qsizetype i = 0; i < array.size(); ++i) {
        if (!array.at(i).isObject()) {
            return contractFailure<QList<Xc2VciDevice>>(
                body,
                QStringLiteral("VCI device at index %1 must be a JSON object")
                    .arg(i));
        }

        const QJsonObject object = array.at(i).toObject();
        Xc2VciDevice device;
        if (!requireString(object, QStringLiteral("id"), device.id,
                           failureMessage)
            || !requireString(object, QStringLiteral("name"), device.name,
                              failureMessage)
            || !requireString(object, QStringLiteral("internalName"),
                              device.internalName, failureMessage)
            || !optionalNullableString(
                object, QStringLiteral("additionalModuleInformation"),
                device.additionalModuleInformation, failureMessage)) {
            return contractFailure<QList<Xc2VciDevice>>(
                body,
                QStringLiteral("VCI device at index %1: %2")
                    .arg(i)
                    .arg(failureMessage));
        }
        devices.append(std::move(device));
    }
    return Xc2Result<QList<Xc2VciDevice>>::success(std::move(devices));
}

QJsonObject Xc2JsonCodec::vciDeviceJson(const Xc2VciDevice &device)
{
    QJsonObject object{
        {QStringLiteral("id"), device.id},
        {QStringLiteral("name"), device.name},
        {QStringLiteral("internalName"), device.internalName}
    };
    if (device.additionalModuleInformation.has_value()) {
        object.insert(QStringLiteral("additionalModuleInformation"),
                      *device.additionalModuleInformation);
    } else {
        object.insert(QStringLiteral("additionalModuleInformation"),
                      QJsonValue(QJsonValue::Null));
    }
    return object;
}

Xc2Result<Xc2JobAccepted> Xc2JsonCodec::jobAccepted(const QByteArray &body)
{
    QJsonDocument document;
    QString failureMessage;
    if (!parseDocument(body, document, failureMessage))
        return contractFailure<Xc2JobAccepted>(body, std::move(failureMessage));
    if (!document.isObject()) {
        return contractFailure<Xc2JobAccepted>(
            body, QStringLiteral("Job acceptance payload must be a JSON object"));
    }

    Xc2JobAccepted accepted;
    if (!requireString(document.object(), QStringLiteral("jobID"),
                       accepted.jobId, failureMessage)) {
        return contractFailure<Xc2JobAccepted>(body, std::move(failureMessage));
    }
    return Xc2Result<Xc2JobAccepted>::success(std::move(accepted));
}

Xc2Result<Xc2JobProgress> Xc2JsonCodec::jobProgress(const QByteArray &body)
{
    QJsonDocument document;
    QString failureMessage;
    if (!parseDocument(body, document, failureMessage))
        return contractFailure<Xc2JobProgress>(body, std::move(failureMessage));
    if (!document.isObject()) {
        return contractFailure<Xc2JobProgress>(
            body, QStringLiteral("Job progress payload must be a JSON object"));
    }

    const QJsonObject object = document.object();
    Xc2JobProgress progress;
    QString status;
    if (!requireString(object, QStringLiteral("jobId"), progress.jobId,
                       failureMessage)
        || !requireString(object, QStringLiteral("status"), status,
                          failureMessage)
        || !requireInteger64(object, QStringLiteral("ticks"), progress.ticks,
                             failureMessage)
        || !requireInteger64(object, QStringLiteral("totalTicks"),
                             progress.totalTicks, failureMessage)
        || !requireString(object, QStringLiteral("message"), progress.message,
                          failureMessage)) {
        return contractFailure<Xc2JobProgress>(body, std::move(failureMessage));
    }

    const std::optional<Xc2JobState> parsedState = jobState(status);
    if (!parsedState.has_value()) {
        return contractFailure<Xc2JobProgress>(
            body,
            QStringLiteral("Unknown job status '%1'").arg(status));
    }
    progress.state = *parsedState;
    progress.raw = object;
    return Xc2Result<Xc2JobProgress>::success(std::move(progress));
}

Xc2Error Xc2JsonCodec::error(const QByteArray &body,
                             int httpStatus,
                             const QString &endpoint)
{
    Xc2Error result;
    result.category = Xc2ErrorCategory::Backend;
    result.httpStatus = httpStatus;
    result.endpoint = endpoint;
    result.rawPayload = body;

    QJsonDocument document;
    QString failureMessage;
    if (!parseDocument(body, document, failureMessage)) {
        result.category = Xc2ErrorCategory::Contract;
        result.message = std::move(failureMessage);
        return result;
    }
    if (!document.isObject()) {
        result.category = Xc2ErrorCategory::Contract;
        result.message = QStringLiteral("Error payload must be a JSON object");
        return result;
    }

    const QJsonObject object = document.object();
    QString wireMessage;
    if (!requireInteger(object, QStringLiteral("status"), result.xc2Status,
                        failureMessage)
        || !requireString(object, QStringLiteral("message"), wireMessage,
                          failureMessage)
        || !requireInteger(object, QStringLiteral("code"), result.xc2Code,
                           failureMessage)
        || !requireString(object, QStringLiteral("devMessage"),
                          result.developerMessage, failureMessage)
        || !requireString(object, QStringLiteral("info"), result.info,
                          failureMessage)) {
        result.category = Xc2ErrorCategory::Contract;
        result.message = std::move(failureMessage);
        return result;
    }

    result.message = std::move(wireMessage);
    return result;
}

} // namespace ktm::xc2
