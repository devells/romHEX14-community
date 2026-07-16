#include "Xc2StompCodec.h"

#include <optional>
#include <utility>

namespace ktm::xc2 {
namespace {

constexpr qsizetype kMaximumBufferedBytes = 8 * 1024 * 1024;

enum class LineReadResult {
    Complete,
    Incomplete,
    EmbeddedNull,
};

LineReadResult readLine(const QByteArray &buffer,
                        qsizetype offset,
                        QByteArray &line,
                        qsizetype &nextOffset)
{
    const qsizetype lf = buffer.indexOf('\n', offset);
    const qsizetype nul = buffer.indexOf('\0', offset);
    if (nul >= 0 && (lf < 0 || nul < lf))
        return LineReadResult::EmbeddedNull;
    if (lf < 0)
        return LineReadResult::Incomplete;

    qsizetype lineEnd = lf;
    if (lineEnd > offset && buffer.at(lineEnd - 1) == '\r')
        --lineEnd;
    line = buffer.mid(offset, lineEnd - offset);
    nextOffset = lf + 1;
    return LineReadResult::Complete;
}

bool isHeaderEscaped(const QByteArray &command)
{
    return command != QByteArrayLiteral("CONNECT")
        && command != QByteArrayLiteral("CONNECTED");
}

bool unescapeHeader(const QByteArray &input, QByteArray &output)
{
    output.clear();
    output.reserve(input.size());
    for (qsizetype i = 0; i < input.size(); ++i) {
        const char byte = input.at(i);
        if (byte != '\\') {
            output += byte;
            continue;
        }

        if (++i >= input.size())
            return false;
        switch (input.at(i)) {
        case '\\':
            output += '\\';
            break;
        case 'n':
            output += '\n';
            break;
        case 'r':
            output += '\r';
            break;
        case 'c':
            output += ':';
            break;
        default:
            return false;
        }
    }
    return true;
}

QByteArray escapeHeader(const QByteArray &input)
{
    QByteArray output;
    output.reserve(input.size());
    for (const char byte : input) {
        switch (byte) {
        case '\\':
            output += QByteArrayLiteral("\\\\");
            break;
        case '\n':
            output += QByteArrayLiteral("\\n");
            break;
        case '\r':
            output += QByteArrayLiteral("\\r");
            break;
        case ':':
            output += QByteArrayLiteral("\\c");
            break;
        default:
            output += byte;
            break;
        }
    }
    return output;
}

bool parseContentLength(const QByteArray &value,
                        qint64 &length,
                        QString &failureMessage)
{
    if (value.startsWith('-')) {
        failureMessage = QStringLiteral("Negative STOMP content-length");
        return false;
    }
    if (value.isEmpty()) {
        failureMessage = QStringLiteral("Non-numeric STOMP content-length");
        return false;
    }
    for (const char byte : value) {
        if (byte < '0' || byte > '9') {
            failureMessage = QStringLiteral("Non-numeric STOMP content-length");
            return false;
        }
    }

    bool ok = false;
    length = value.toLongLong(&ok, 10);
    if (!ok) {
        failureMessage = QStringLiteral("STOMP content-length is out of range");
        return false;
    }
    if (length > kMaximumBufferedBytes) {
        failureMessage = QStringLiteral("STOMP content-length exceeds 8 MiB");
        return false;
    }
    return true;
}

Xc2Error contractError(const QByteArray &payload, QString message)
{
    Xc2Error error;
    error.category = Xc2ErrorCategory::Contract;
    error.message = std::move(message);
    error.rawPayload = payload;
    return error;
}

} // namespace

Xc2StompDecodeResult Xc2StompCodec::feed(const QByteArray &bytes)
{
    Xc2StompDecodeResult result;
    const auto fail = [this, &result](const QByteArray &payload,
                                     QString message) {
        result.errors.append(contractError(payload, std::move(message)));
        m_buffer.clear();
    };

    if (bytes.size() > kMaximumBufferedBytes
        || m_buffer.size() > kMaximumBufferedBytes - bytes.size()) {
        QByteArray payload = m_buffer;
        const qsizetype remaining = kMaximumBufferedBytes + 1 - payload.size();
        payload += bytes.left(remaining);
        fail(payload, QStringLiteral("Buffered STOMP data exceeds 8 MiB"));
        return result;
    }
    m_buffer += bytes;

    qsizetype cursor = 0;
    while (cursor < m_buffer.size()) {
        if (m_buffer.at(cursor) == '\n') {
            ++cursor;
            ++result.heartbeats;
            continue;
        }
        if (m_buffer.at(cursor) == '\r') {
            if (cursor + 1 == m_buffer.size())
                break;
            if (m_buffer.at(cursor + 1) == '\n') {
                cursor += 2;
                ++result.heartbeats;
                continue;
            }
        }
        if (m_buffer.at(cursor) == '\0') {
            fail(m_buffer, QStringLiteral("STOMP frame command is missing"));
            return result;
        }

        const qsizetype frameStart = cursor;
        qsizetype position = cursor;
        qsizetype nextPosition = 0;
        QByteArray command;
        const LineReadResult commandStatus =
            readLine(m_buffer, position, command, nextPosition);
        if (commandStatus == LineReadResult::Incomplete)
            break;
        if (commandStatus == LineReadResult::EmbeddedNull) {
            fail(m_buffer,
                 QStringLiteral("NUL encountered before STOMP command EOL"));
            return result;
        }
        if (command.isEmpty()) {
            fail(m_buffer, QStringLiteral("STOMP frame command is missing"));
            return result;
        }
        position = nextPosition;

        QMap<QByteArray, QByteArray> headers;
        std::optional<qint64> contentLength;
        bool headersComplete = false;
        while (!headersComplete) {
            QByteArray line;
            const LineReadResult lineStatus =
                readLine(m_buffer, position, line, nextPosition);
            if (lineStatus == LineReadResult::Incomplete)
                break;
            if (lineStatus == LineReadResult::EmbeddedNull) {
                fail(m_buffer,
                     QStringLiteral("NUL encountered before STOMP header EOL"));
                return result;
            }
            position = nextPosition;
            if (line.isEmpty()) {
                headersComplete = true;
                break;
            }

            const qsizetype separator = line.indexOf(':');
            if (separator < 0) {
                fail(m_buffer, QStringLiteral("Malformed STOMP header"));
                return result;
            }

            const QByteArray wireKey = line.left(separator);
            const QByteArray wireValue = line.mid(separator + 1);
            QByteArray key = wireKey;
            QByteArray value = wireValue;
            if (isHeaderEscaped(command)
                && (!unescapeHeader(wireKey, key)
                    || !unescapeHeader(wireValue, value))) {
                fail(m_buffer, QStringLiteral("Invalid STOMP header escape"));
                return result;
            }

            if (headers.contains(key))
                continue;
            headers.insert(key, value);
            if (key == QByteArrayLiteral("content-length")) {
                qint64 parsedLength = 0;
                QString failureMessage;
                if (!parseContentLength(value, parsedLength, failureMessage)) {
                    fail(m_buffer, std::move(failureMessage));
                    return result;
                }
                contentLength = parsedLength;
            }
        }
        if (!headersComplete) {
            cursor = frameStart;
            break;
        }

        QByteArray body;
        qsizetype terminator = -1;
        if (contentLength.has_value()) {
            const qsizetype bodyLength =
                static_cast<qsizetype>(*contentLength);
            const qsizetype available = m_buffer.size() - position;
            if (available <= bodyLength) {
                cursor = frameStart;
                break;
            }
            terminator = position + bodyLength;
            if (m_buffer.at(terminator) != '\0') {
                fail(m_buffer,
                     QStringLiteral("STOMP content-length body lacks NUL terminator"));
                return result;
            }
            body = m_buffer.mid(position, bodyLength);
        } else {
            terminator = m_buffer.indexOf('\0', position);
            if (terminator < 0) {
                cursor = frameStart;
                break;
            }
            body = m_buffer.mid(position, terminator - position);
        }

        Xc2StompFrame frame;
        frame.command = std::move(command);
        frame.headers = std::move(headers);
        frame.body = std::move(body);
        result.frames.append(std::move(frame));
        cursor = terminator + 1;
    }

    if (cursor > 0)
        m_buffer.remove(0, cursor);
    return result;
}

void Xc2StompCodec::reset()
{
    m_buffer.clear();
}

QByteArray Xc2StompCodec::encode(const Xc2StompFrame &frame)
{
    QMap<QByteArray, QByteArray> headers = frame.headers;
    const auto contentLength = headers.find(QByteArrayLiteral("content-length"));
    if (contentLength != headers.end()) {
        contentLength.value() = QByteArray::number(frame.body.size());
    } else if (frame.body.contains('\0')) {
        headers.insert(QByteArrayLiteral("content-length"),
                       QByteArray::number(frame.body.size()));
    }

    QByteArray wire = frame.command;
    wire += '\n';
    const bool escape = isHeaderEscaped(frame.command);
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        wire += escape ? escapeHeader(it.key()) : it.key();
        wire += ':';
        wire += escape ? escapeHeader(it.value()) : it.value();
        wire += '\n';
    }
    wire += '\n';
    wire += frame.body;
    wire += '\0';
    return wire;
}

} // namespace ktm::xc2
