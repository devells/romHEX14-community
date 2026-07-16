#include "Xc2JsonCodec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

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

void skipJsonWhitespace(const QByteArray &json, qsizetype &position)
{
    while (position < json.size()) {
        const char byte = json.at(position);
        if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n')
            break;
        ++position;
    }
}

bool skipJsonString(const QByteArray &json, qsizetype &position)
{
    if (position >= json.size() || json.at(position) != '"')
        return false;
    ++position;
    while (position < json.size()) {
        const char byte = json.at(position++);
        if (byte == '"')
            return true;
        if (byte != '\\')
            continue;
        if (position >= json.size())
            return false;
        if (json.at(position++) == 'u') {
            if (position + 4 > json.size())
                return false;
            position += 4;
        }
    }
    return false;
}

bool skipJsonValue(const QByteArray &json, qsizetype &position);

bool skipJsonObject(const QByteArray &json, qsizetype &position)
{
    if (position >= json.size() || json.at(position++) != '{')
        return false;
    skipJsonWhitespace(json, position);
    if (position < json.size() && json.at(position) == '}') {
        ++position;
        return true;
    }
    while (position < json.size()) {
        if (!skipJsonString(json, position))
            return false;
        skipJsonWhitespace(json, position);
        if (position >= json.size() || json.at(position++) != ':')
            return false;
        if (!skipJsonValue(json, position))
            return false;
        skipJsonWhitespace(json, position);
        if (position >= json.size())
            return false;
        const char delimiter = json.at(position++);
        if (delimiter == '}')
            return true;
        if (delimiter != ',')
            return false;
        skipJsonWhitespace(json, position);
    }
    return false;
}

bool skipJsonArray(const QByteArray &json, qsizetype &position)
{
    if (position >= json.size() || json.at(position++) != '[')
        return false;
    skipJsonWhitespace(json, position);
    if (position < json.size() && json.at(position) == ']') {
        ++position;
        return true;
    }
    while (position < json.size()) {
        if (!skipJsonValue(json, position))
            return false;
        skipJsonWhitespace(json, position);
        if (position >= json.size())
            return false;
        const char delimiter = json.at(position++);
        if (delimiter == ']')
            return true;
        if (delimiter != ',')
            return false;
        skipJsonWhitespace(json, position);
    }
    return false;
}

bool skipJsonValue(const QByteArray &json, qsizetype &position)
{
    skipJsonWhitespace(json, position);
    if (position >= json.size())
        return false;
    if (json.at(position) == '"')
        return skipJsonString(json, position);
    if (json.at(position) == '{')
        return skipJsonObject(json, position);
    if (json.at(position) == '[')
        return skipJsonArray(json, position);

    const qsizetype start = position;
    while (position < json.size()) {
        const char byte = json.at(position);
        if (byte == ',' || byte == ']' || byte == '}' || byte == ' '
            || byte == '\t' || byte == '\r' || byte == '\n') {
            break;
        }
        ++position;
    }
    return position > start;
}

bool decodeJsonStringToken(const QByteArray &token, QString &value)
{
    const QJsonDocument wrapper = QJsonDocument::fromJson(
        QByteArrayLiteral("[") + token + QByteArrayLiteral("]"));
    if (!wrapper.isArray() || wrapper.array().size() != 1
        || !wrapper.array().at(0).isString()) {
        return false;
    }
    value = wrapper.array().at(0).toString();
    return true;
}

bool jsonObjectMemberRange(const QByteArray &json,
                           qsizetype objectStart,
                           const QString &field,
                           qsizetype &valueStart,
                           qsizetype &valueEnd)
{
    qsizetype position = objectStart;
    if (position >= json.size() || json.at(position++) != '{')
        return false;
    skipJsonWhitespace(json, position);

    bool found = false;
    while (position < json.size() && json.at(position) != '}') {
        const qsizetype keyStart = position;
        if (!skipJsonString(json, position))
            return false;
        QString key;
        if (!decodeJsonStringToken(json.mid(keyStart, position - keyStart), key))
            return false;
        skipJsonWhitespace(json, position);
        if (position >= json.size() || json.at(position++) != ':')
            return false;
        skipJsonWhitespace(json, position);
        const qsizetype memberStart = position;
        if (!skipJsonValue(json, position))
            return false;
        if (key == field) {
            valueStart = memberStart;
            valueEnd = position;
            found = true;
        }
        skipJsonWhitespace(json, position);
        if (position >= json.size() || json.at(position) == '}')
            break;
        if (json.at(position++) != ',')
            return false;
        skipJsonWhitespace(json, position);
    }
    return found;
}

bool parseInteger64Literal(const QByteArray &literal, qint64 &value)
{
    // Qt 6.8 can saturate an out-of-range JSON integer at qint64 minimum.
    if (literal.isEmpty())
        return false;
    qsizetype position = 0;
    const bool negative = literal.at(0) == '-';
    if (negative && ++position == literal.size())
        return false;

    const quint64 positiveLimit =
        quint64(std::numeric_limits<qint64>::max());
    const quint64 limit = negative ? positiveLimit + 1 : positiveLimit;
    quint64 magnitude = 0;
    for (; position < literal.size(); ++position) {
        const char byte = literal.at(position);
        if (byte < '0' || byte > '9')
            return false;
        const quint64 digit = quint64(byte - '0');
        if (magnitude > (limit - digit) / 10)
            return false;
        magnitude = magnitude * 10 + digit;
    }

    if (!negative) {
        value = static_cast<qint64>(magnitude);
    } else if (magnitude == positiveLimit + 1) {
        value = std::numeric_limits<qint64>::min();
    } else {
        value = -static_cast<qint64>(magnitude);
    }
    return true;
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

    const QVariant variant = jsonValue.toVariant();
    if (variant.metaType().id() != QMetaType::LongLong) {
        failureMessage = QStringLiteral("Field '%1' must be a 64-bit integer")
                             .arg(field);
        return false;
    }
    value = variant.toLongLong();
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

bool requireInteger32(const QJsonObject &object,
                      const QString &field,
                      const QByteArray &source,
                      qsizetype objectStart,
                      qint32 &value,
                      QString &failureMessage)
{
    if (!object.contains(field)) {
        failureMessage = QStringLiteral("Missing required field '%1'").arg(field);
        return false;
    }
    if (!object.value(field).isDouble()) {
        failureMessage = QStringLiteral("Field '%1' must be an integer").arg(field);
        return false;
    }
    qsizetype valueStart = 0;
    qsizetype valueEnd = 0;
    qint64 parsed = 0;
    if (!jsonObjectMemberRange(source, objectStart, field, valueStart, valueEnd)
        || !parseInteger64Literal(
            source.mid(valueStart, valueEnd - valueStart), parsed)) {
        failureMessage = QStringLiteral("Field '%1' must be a 32-bit integer")
                             .arg(field);
        return false;
    }
    if (parsed < std::numeric_limits<qint32>::min()
        || parsed > std::numeric_limits<qint32>::max()) {
        failureMessage = QStringLiteral("Field '%1' must be a 32-bit integer")
                             .arg(field);
        return false;
    }
    value = static_cast<qint32>(parsed);
    return true;
}

bool requireNullableLocalizedText(
    const QJsonObject &object,
    const QString &field,
    const QByteArray &source,
    qsizetype objectStart,
    std::optional<Xc2LocalizedText> &value,
    QString &failureMessage)
{
    if (!object.contains(field)) {
        failureMessage = QStringLiteral("Missing required field '%1'").arg(field);
        return false;
    }

    const QJsonValue jsonValue = object.value(field);
    if (jsonValue.isNull()) {
        value.reset();
        return true;
    }
    if (!jsonValue.isObject()) {
        failureMessage = QStringLiteral(
                             "Field '%1' must be an object or null")
                             .arg(field);
        return false;
    }

    const QJsonObject localizedObject = jsonValue.toObject();
    qsizetype localizedStart = 0;
    qsizetype localizedEnd = 0;
    if (!jsonObjectMemberRange(source, objectStart, field, localizedStart,
                               localizedEnd)) {
        failureMessage = QStringLiteral("Missing required field '%1'").arg(field);
        return false;
    }
    Xc2LocalizedText localized;
    if (!localizedObject.contains(QStringLiteral("id"))) {
        failureMessage = QStringLiteral("Missing required field 'id'");
    } else if (!localizedObject.value(QStringLiteral("id")).isDouble()) {
        failureMessage = QStringLiteral("Field 'id' must be an integer");
    } else {
        qsizetype idStart = 0;
        qsizetype idEnd = 0;
        if (!jsonObjectMemberRange(source, localizedStart,
                                   QStringLiteral("id"), idStart, idEnd)
            || !parseInteger64Literal(source.mid(idStart, idEnd - idStart),
                                      localized.id)) {
            failureMessage = QStringLiteral("Field 'id' must be a 64-bit integer");
        }
    }
    if (!failureMessage.isEmpty()
        || !requireString(localizedObject, QStringLiteral("text"),
                          localized.text, failureMessage)) {
        failureMessage = QStringLiteral("Field '%1': %2")
                             .arg(field, failureMessage);
        return false;
    }
    value = std::move(localized);
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
    qsizetype objectStart = 0;
    skipJsonWhitespace(body, objectStart);
    Xc2JobProgress progress;
    QString status;
    if (!requireString(object, QStringLiteral("jobId"), progress.jobId,
                       failureMessage)
        || !requireString(object, QStringLiteral("status"), status,
                          failureMessage)
        || !requireInteger32(object, QStringLiteral("ticks"), body,
                             objectStart, progress.ticks, failureMessage)
        || !requireInteger32(object, QStringLiteral("totalTicks"), body,
                             objectStart, progress.totalTicks, failureMessage)
        || !requireNullableLocalizedText(
            object, QStringLiteral("message"), body, objectStart,
            progress.message, failureMessage)) {
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
