#include "Xc2ProcessOutput.h"

#include <QList>

#include <algorithm>
#include <array>

namespace ktm::xc2 {
namespace {

constexpr char kRedacted[] = "[redacted]";
constexpr char kTruncated[] = "[truncated]";

const std::array<QByteArray, 9> &sensitiveNames()
{
    static const std::array<QByteArray, 9> names = {
        QByteArrayLiteral("authorization"),
        QByteArrayLiteral("proxy-authorization"),
        QByteArrayLiteral("cookie"),
        QByteArrayLiteral("set-cookie"),
        QByteArrayLiteral("password"),
        QByteArrayLiteral("token"),
        QByteArrayLiteral("sessionindex"),
        QByteArrayLiteral("samlrequest"),
        QByteArrayLiteral("samlresponse"),
    };
    return names;
}

char lowerAscii(char byte)
{
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

bool isAsciiNameByte(quint32 codePoint)
{
    return (codePoint >= 'a' && codePoint <= 'z')
        || (codePoint >= 'A' && codePoint <= 'Z')
        || (codePoint >= '0' && codePoint <= '9')
        || codePoint == '-' || codePoint == '_';
}

bool isCandidatePrefix(QByteArrayView candidate)
{
    for (const QByteArray &name : sensitiveNames()) {
        if (candidate.size() > name.size())
            continue;

        bool matches = true;
        for (qsizetype i = 0; i < candidate.size(); ++i) {
            if (lowerAscii(candidate.at(i)) != name.at(i)) {
                matches = false;
                break;
            }
        }
        if (matches)
            return true;
    }
    return false;
}

bool isSensitiveName(QByteArrayView candidate)
{
    for (const QByteArray &name : sensitiveNames()) {
        if (candidate.size() != name.size())
            continue;

        bool matches = true;
        for (qsizetype i = 0; i < candidate.size(); ++i) {
            if (lowerAscii(candidate.at(i)) != name.at(i)) {
                matches = false;
                break;
            }
        }
        if (matches)
            return true;
    }
    return false;
}

bool isValueTerminator(quint32 codePoint)
{
    switch (codePoint) {
    case ' ':
    case '\t':
    case '&':
    case ';':
    case ',':
    case ')':
    case '}':
    case ']':
        return true;
    default:
        return false;
    }
}

bool isFilteredControl(quint32 codePoint)
{
    if (codePoint == '\t')
        return false;
    return codePoint <= 0x1f || codePoint == 0x7f
        || (codePoint >= 0x80 && codePoint <= 0x9f);
}

QByteArray encodeUtf8(quint32 codePoint)
{
    QByteArray bytes;
    if (codePoint <= 0x7f) {
        bytes += static_cast<char>(codePoint);
    } else if (codePoint <= 0x7ff) {
        bytes += static_cast<char>(0xc0 | (codePoint >> 6));
        bytes += static_cast<char>(0x80 | (codePoint & 0x3f));
    } else if (codePoint <= 0xffff) {
        bytes += static_cast<char>(0xe0 | (codePoint >> 12));
        bytes += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
        bytes += static_cast<char>(0x80 | (codePoint & 0x3f));
    } else {
        bytes += static_cast<char>(0xf0 | (codePoint >> 18));
        bytes += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f));
        bytes += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
        bytes += static_cast<char>(0x80 | (codePoint & 0x3f));
    }
    return bytes;
}

qsizetype validPrefixLength(QByteArrayView bytes, qsizetype limit)
{
    qsizetype cursor = 0;
    while (cursor < bytes.size() && cursor < limit) {
        const auto lead = static_cast<quint8>(bytes.at(cursor));
        qsizetype width = 1;
        if ((lead & 0xe0) == 0xc0)
            width = 2;
        else if ((lead & 0xf0) == 0xe0)
            width = 3;
        else if ((lead & 0xf8) == 0xf0)
            width = 4;
        if (cursor + width > limit || cursor + width > bytes.size())
            break;
        cursor += width;
    }
    return cursor;
}

qsizetype previousUtf8Boundary(const QByteArray &bytes, qsizetype limit)
{
    qsizetype boundary = std::min(limit, bytes.size());
    while (boundary > 0 && boundary < bytes.size()
           && (static_cast<quint8>(bytes.at(boundary)) & 0xc0) == 0x80) {
        --boundary;
    }
    return boundary;
}

} // namespace

struct Xc2ProcessOutput::StreamState {
    enum class AnsiState {
        Normal,
        Escape,
        Csi,
        Osc,
        OscEscape,
    };

    enum class ScannerState {
        Normal,
        ProbeName,
        UnquotedDelimiterWait,
        JsonDelimiterWait,
        HeaderBeforeValue,
        HeaderValue,
        KeyBeforeValue,
        KeyUnquotedValue,
        KeyQuotedValue,
        JsonBeforeValue,
        JsonBeforeValueCrossLine,
        JsonUnquotedValue,
        JsonQuotedValue,
        FailClosedLine,
    };

    QByteArray line;
    QList<QByteArray> ring;
    qsizetype ringBytes = 0;
    QByteArray probe;
    qsizetype candidateOffset = 0;
    qsizetype candidateLength = 0;
    bool quotedCandidate = false;
    bool previousWasNameByte = false;
    bool lineTouched = false;
    bool lineHadRawContent = false;
    bool sawCarriageReturn = false;
    bool oversized = false;
    bool finalized = false;

    quint32 utf8CodePoint = 0;
    quint32 utf8Minimum = 0;
    int utf8Remaining = 0;
    int utf8BufferedBytes = 0;

    AnsiState ansiState = AnsiState::Normal;
    ScannerState scannerState = ScannerState::Normal;
    char quotedValueDelimiter = 0;
    bool quotedValueEscape = false;
    bool failClosedReprobe = false;

    void appendPending(QByteArrayView bytes)
    {
        if (oversized || bytes.isEmpty())
            return;

        if (line.size() <= Xc2ProcessOutput::MaximumLineBytes - bytes.size()) {
            line.append(bytes.data(), bytes.size());
            return;
        }

        constexpr qsizetype suffixBytes = sizeof(kTruncated) - 1;
        constexpr qsizetype prefixLimit =
            Xc2ProcessOutput::MaximumLineBytes - suffixBytes;
        if (line.size() > prefixLimit)
            line.resize(previousUtf8Boundary(line, prefixLimit));

        const qsizetype available = prefixLimit - line.size();
        const qsizetype copyBytes = validPrefixLength(bytes, available);
        if (copyBytes > 0)
            line.append(bytes.data(), copyBytes);
        line += kTruncated;
        oversized = true;
    }

    void appendCodePoint(quint32 codePoint)
    {
        const QByteArray bytes = encodeUtf8(codePoint);
        appendPending(QByteArrayView(bytes));
        previousWasNameByte = isAsciiNameByte(codePoint);
    }

    void appendAscii(QByteArrayView bytes)
    {
        appendPending(bytes);
        if (!bytes.isEmpty()) {
            previousWasNameByte = isAsciiNameByte(
                static_cast<quint8>(bytes.back()));
        }
    }

    void clearProbe()
    {
        probe.clear();
        candidateOffset = 0;
        candidateLength = 0;
        quotedCandidate = false;
    }

    void commitProbe()
    {
        appendAscii(QByteArrayView(probe));
        clearProbe();
    }

    bool appendProbe(quint32 codePoint)
    {
        if (codePoint > 0x7f
            || probe.size() >= Xc2ProcessOutput::MaximumLookBehindBytes) {
            return false;
        }
        probe += static_cast<char>(codePoint);
        return true;
    }

    QByteArrayView candidate() const
    {
        return QByteArrayView(probe).sliced(candidateOffset, candidateLength);
    }

    void beginProbe(quint32 codePoint, bool quoted)
    {
        clearProbe();
        failClosedReprobe = false;
        quotedCandidate = quoted;
        if (quoted) {
            probe += '"';
            candidateOffset = 1;
        }
        probe += static_cast<char>(codePoint);
        candidateLength = 1;
        scannerState = ScannerState::ProbeName;
    }

    void appendMarker()
    {
        appendAscii(QByteArrayView(kRedacted));
    }

    void consumeNormal(quint32 codePoint)
    {
        if (codePoint == '"') {
            clearProbe();
            probe += '"';
            candidateOffset = 1;
            candidateLength = 0;
            quotedCandidate = true;
            scannerState = ScannerState::ProbeName;
            return;
        }

        if (!previousWasNameByte && isAsciiNameByte(codePoint)) {
            const char byte = static_cast<char>(codePoint);
            if (isCandidatePrefix(QByteArrayView(&byte, 1))) {
                beginProbe(codePoint, false);
                return;
            }
        }

        appendCodePoint(codePoint);
    }

    void failProbeAndReprocess(quint32 codePoint)
    {
        if (failClosedReprobe) {
            clearProbe();
            failClosedReprobe = false;
            scannerState = ScannerState::FailClosedLine;
            return;
        }
        commitProbe();
        scannerState = ScannerState::Normal;
        consumeNormal(codePoint);
    }

    void acceptSensitiveDelimiter(quint32 delimiter, bool jsonName)
    {
        appendCodePoint(delimiter);
        failClosedReprobe = false;
        if (jsonName) {
            scannerState = ScannerState::JsonBeforeValue;
        } else if (delimiter == ':') {
            scannerState = ScannerState::HeaderBeforeValue;
        } else {
            scannerState = ScannerState::KeyBeforeValue;
        }
    }

    void consumeProbeName(quint32 codePoint)
    {
        const bool exact = isSensitiveName(candidate());
        if (quotedCandidate && exact && codePoint == '"') {
            if (!appendProbe(codePoint)) {
                failProbeAndReprocess(codePoint);
                return;
            }
            commitProbe();
            scannerState = ScannerState::JsonDelimiterWait;
            return;
        }
        if (!quotedCandidate && exact
            && (codePoint == ':' || codePoint == '=')) {
            commitProbe();
            acceptSensitiveDelimiter(codePoint, false);
            return;
        }
        if (!quotedCandidate && exact
            && (codePoint == ' ' || codePoint == '\t')) {
            commitProbe();
            appendCodePoint(codePoint);
            scannerState = ScannerState::UnquotedDelimiterWait;
            return;
        }

        if (isAsciiNameByte(codePoint)) {
            const char byte = static_cast<char>(codePoint);
            QByteArray extended(candidate().data(), candidate().size());
            extended += byte;
            if (isCandidatePrefix(QByteArrayView(extended))
                && appendProbe(codePoint)) {
                ++candidateLength;
                return;
            }
        }

        failProbeAndReprocess(codePoint);
    }

    void consumeUnquotedDelimiterWait(quint32 codePoint)
    {
        if (codePoint == ' ' || codePoint == '\t') {
            appendCodePoint(codePoint);
            return;
        }

        if (codePoint == ':' || codePoint == '=') {
            acceptSensitiveDelimiter(codePoint, false);
            return;
        }
        if (failClosedReprobe) {
            failClosedReprobe = false;
            scannerState = ScannerState::FailClosedLine;
            return;
        }
        scannerState = ScannerState::Normal;
        consumeNormal(codePoint);
    }

    void consumeJsonDelimiterWait(quint32 codePoint)
    {
        if (codePoint == ' ' || codePoint == '\t') {
            appendCodePoint(codePoint);
            return;
        }

        if (codePoint == ':') {
            acceptSensitiveDelimiter(codePoint, true);
            return;
        }
        scannerState = ScannerState::Normal;
        consumeNormal(codePoint);
    }

    void consumeKeyUnquotedValue(quint32 codePoint)
    {
        if (!isValueTerminator(codePoint))
            return;
        scannerState = ScannerState::Normal;
        consumeNormal(codePoint);
    }

    void consumeJsonUnquotedValue(quint32 codePoint)
    {
        if (codePoint != ' ' && codePoint != '\t' && codePoint != ','
            && codePoint != '}' && codePoint != ']') {
            return;
        }
        scannerState = ScannerState::Normal;
        consumeNormal(codePoint);
    }

    void consumeQuotedValue(quint32 codePoint)
    {
        if (quotedValueEscape) {
            quotedValueEscape = false;
            return;
        }
        if (codePoint == '\\') {
            quotedValueEscape = true;
            return;
        }
        if (codePoint != static_cast<quint8>(quotedValueDelimiter))
            return;

        appendCodePoint(codePoint);
        scannerState = ScannerState::Normal;
        quotedValueDelimiter = 0;
        quotedValueEscape = false;
    }

    void consumeVisible(quint32 codePoint)
    {
        lineTouched = true;
        switch (scannerState) {
        case ScannerState::Normal:
            consumeNormal(codePoint);
            break;
        case ScannerState::ProbeName:
            consumeProbeName(codePoint);
            break;
        case ScannerState::UnquotedDelimiterWait:
            consumeUnquotedDelimiterWait(codePoint);
            break;
        case ScannerState::JsonDelimiterWait:
            consumeJsonDelimiterWait(codePoint);
            break;
        case ScannerState::HeaderBeforeValue:
            if (codePoint == ' ' || codePoint == '\t') {
                appendCodePoint(codePoint);
            } else {
                appendMarker();
                scannerState = ScannerState::HeaderValue;
            }
            break;
        case ScannerState::HeaderValue:
            break;
        case ScannerState::KeyBeforeValue:
            if (codePoint == ' ' || codePoint == '\t') {
                appendCodePoint(codePoint);
            } else if (codePoint == '"' || codePoint == '\'') {
                appendCodePoint(codePoint);
                appendMarker();
                quotedValueDelimiter = static_cast<char>(codePoint);
                quotedValueEscape = false;
                scannerState = ScannerState::KeyQuotedValue;
            } else {
                appendMarker();
                scannerState = ScannerState::KeyUnquotedValue;
                consumeKeyUnquotedValue(codePoint);
            }
            break;
        case ScannerState::KeyUnquotedValue:
            consumeKeyUnquotedValue(codePoint);
            break;
        case ScannerState::KeyQuotedValue:
            consumeQuotedValue(codePoint);
            break;
        case ScannerState::JsonBeforeValue:
            if (codePoint == ' ' || codePoint == '\t') {
                appendCodePoint(codePoint);
            } else if (codePoint == '"') {
                appendCodePoint(codePoint);
                appendMarker();
                quotedValueDelimiter = '"';
                quotedValueEscape = false;
                scannerState = ScannerState::JsonQuotedValue;
            } else {
                appendMarker();
                scannerState = ScannerState::JsonUnquotedValue;
                consumeJsonUnquotedValue(codePoint);
            }
            break;
        case ScannerState::JsonBeforeValueCrossLine:
            if (codePoint == ' ' || codePoint == '\t') {
                appendCodePoint(codePoint);
            } else if (codePoint == '"') {
                appendCodePoint(codePoint);
                appendMarker();
                quotedValueDelimiter = '"';
                quotedValueEscape = false;
                scannerState = ScannerState::JsonQuotedValue;
            } else {
                appendMarker();
                resetScanner();
                if (isAsciiNameByte(codePoint)) {
                    const char byte = static_cast<char>(codePoint);
                    if (isCandidatePrefix(QByteArrayView(&byte, 1))) {
                        beginProbe(codePoint, false);
                        failClosedReprobe = true;
                        break;
                    }
                }
                scannerState = ScannerState::FailClosedLine;
            }
            break;
        case ScannerState::JsonUnquotedValue:
            consumeJsonUnquotedValue(codePoint);
            break;
        case ScannerState::JsonQuotedValue:
            consumeQuotedValue(codePoint);
            break;
        case ScannerState::FailClosedLine:
            break;
        }
    }

    void consumeFiltered(quint32 codePoint)
    {
        switch (ansiState) {
        case AnsiState::Normal:
            if (codePoint == 0x1b) {
                ansiState = AnsiState::Escape;
            } else if (codePoint == 0x9b) {
                ansiState = AnsiState::Csi;
            } else if (codePoint == 0x9d) {
                ansiState = AnsiState::Osc;
            } else if (!isFilteredControl(codePoint)) {
                consumeVisible(codePoint);
            }
            break;
        case AnsiState::Escape:
            if (codePoint == '[') {
                ansiState = AnsiState::Csi;
            } else if (codePoint == ']') {
                ansiState = AnsiState::Osc;
            } else if (codePoint != 0x1b) {
                ansiState = AnsiState::Normal;
            }
            break;
        case AnsiState::Csi:
            if (codePoint >= 0x40 && codePoint <= 0x7e)
                ansiState = AnsiState::Normal;
            else if (codePoint == 0x1b)
                ansiState = AnsiState::Escape;
            break;
        case AnsiState::Osc:
            if (codePoint == 0x07 || codePoint == 0x9c) {
                ansiState = AnsiState::Normal;
            } else if (codePoint == 0x1b) {
                ansiState = AnsiState::OscEscape;
            }
            break;
        case AnsiState::OscEscape:
            if (codePoint == '\\' || codePoint == 0x07
                || codePoint == 0x9c) {
                ansiState = AnsiState::Normal;
            } else if (codePoint != 0x1b) {
                ansiState = AnsiState::Osc;
            }
            break;
        }
    }

    void resetScanner()
    {
        clearProbe();
        scannerState = ScannerState::Normal;
        quotedValueDelimiter = 0;
        quotedValueEscape = false;
        failClosedReprobe = false;
    }

    void prepareScannerForBoundary(bool endOfStream, bool hasLineContent)
    {
        switch (scannerState) {
        case ScannerState::ProbeName:
            commitProbe();
            resetScanner();
            break;
        case ScannerState::UnquotedDelimiterWait:
            resetScanner();
            break;
        case ScannerState::JsonDelimiterWait:
            if (endOfStream)
                resetScanner();
            break;
        case ScannerState::HeaderBeforeValue:
            if (!endOfStream || hasLineContent)
                appendMarker();
            resetScanner();
            break;
        case ScannerState::HeaderValue:
        case ScannerState::KeyUnquotedValue:
        case ScannerState::JsonUnquotedValue:
            resetScanner();
            break;
        case ScannerState::KeyBeforeValue:
            if (!endOfStream || hasLineContent)
                appendMarker();
            resetScanner();
            break;
        case ScannerState::JsonBeforeValue:
            if (endOfStream) {
                if (hasLineContent)
                    appendMarker();
                resetScanner();
            } else {
                scannerState = ScannerState::JsonBeforeValueCrossLine;
            }
            break;
        case ScannerState::JsonBeforeValueCrossLine:
            if (endOfStream) {
                if (hasLineContent)
                    appendMarker();
                resetScanner();
            }
            break;
        case ScannerState::KeyQuotedValue:
        case ScannerState::JsonQuotedValue:
            if (endOfStream)
                resetScanner();
            break;
        case ScannerState::FailClosedLine:
            resetScanner();
            break;
        case ScannerState::Normal:
            break;
        }
    }

    void completeLine(QStringList &completed, bool delimited)
    {
        const bool hasLineContent =
            lineHadRawContent || lineTouched || !line.isEmpty();
        prepareScannerForBoundary(!delimited, hasLineContent);
        ansiState = AnsiState::Normal;

        if (delimited || lineTouched || !line.isEmpty()) {
            ring += line;
            ringBytes += line.size();
            while (ring.size() > Xc2ProcessOutput::MaximumRecentLines)
                ringBytes -= ring.takeFirst().size();
            completed += QString::fromUtf8(line);
        }

        line.clear();
        previousWasNameByte = false;
        lineTouched = false;
        lineHadRawContent = false;
        oversized = false;
    }

    void consumeCodePoint(quint32 codePoint, QStringList &completed)
    {
        if (codePoint == '\n') {
            if (sawCarriageReturn) {
                sawCarriageReturn = false;
                return;
            }
            completeLine(completed, true);
            return;
        }
        if (codePoint == '\r') {
            completeLine(completed, true);
            sawCarriageReturn = true;
            return;
        }

        sawCarriageReturn = false;
        lineHadRawContent = true;
        consumeFiltered(codePoint);
    }

    void resetUtf8Decoder()
    {
        utf8CodePoint = 0;
        utf8Minimum = 0;
        utf8Remaining = 0;
        utf8BufferedBytes = 0;
    }

    void emitReplacement(QStringList &completed)
    {
        consumeCodePoint(0xfffd, completed);
    }

    void consumeByte(quint8 byte, QStringList &completed)
    {
        if (utf8Remaining == 0) {
            if (byte <= 0x7f) {
                consumeCodePoint(byte, completed);
            } else if (byte >= 0xc2 && byte <= 0xdf) {
                utf8CodePoint = byte & 0x1f;
                utf8Minimum = 0x80;
                utf8Remaining = 1;
                utf8BufferedBytes = 1;
            } else if (byte >= 0xe0 && byte <= 0xef) {
                utf8CodePoint = byte & 0x0f;
                utf8Minimum = 0x800;
                utf8Remaining = 2;
                utf8BufferedBytes = 1;
            } else if (byte >= 0xf0 && byte <= 0xf4) {
                utf8CodePoint = byte & 0x07;
                utf8Minimum = 0x10000;
                utf8Remaining = 3;
                utf8BufferedBytes = 1;
            } else {
                emitReplacement(completed);
            }
            return;
        }

        if ((byte & 0xc0) != 0x80) {
            resetUtf8Decoder();
            emitReplacement(completed);
            consumeByte(byte, completed);
            return;
        }

        utf8CodePoint = (utf8CodePoint << 6) | (byte & 0x3f);
        --utf8Remaining;
        ++utf8BufferedBytes;
        if (utf8Remaining != 0)
            return;

        const quint32 decoded = utf8CodePoint;
        const quint32 minimum = utf8Minimum;
        resetUtf8Decoder();
        if (decoded < minimum || decoded > 0x10ffff
            || (decoded >= 0xd800 && decoded <= 0xdfff)) {
            emitReplacement(completed);
        } else {
            consumeCodePoint(decoded, completed);
        }
    }

    QStringList feed(QByteArrayView bytes)
    {
        QStringList completed;
        if (finalized)
            return completed;

        for (const char byte : bytes)
            consumeByte(static_cast<quint8>(byte), completed);
        return completed;
    }

    QStringList finish()
    {
        QStringList completed;
        if (finalized)
            return completed;

        if (utf8Remaining != 0) {
            resetUtf8Decoder();
            emitReplacement(completed);
        }
        completeLine(completed, false);
        sawCarriageReturn = false;
        finalized = true;
        return completed;
    }

    QStringList recentOutput() const
    {
        QStringList output;
        output.reserve(ring.size());
        for (const QByteArray &entry : ring)
            output += QString::fromUtf8(entry);
        return output;
    }

    qsizetype retainedStateByteCount() const
    {
        return ringBytes + line.size() + probe.size() + utf8BufferedBytes;
    }
};

Xc2ProcessOutput::Xc2ProcessOutput()
    : m_streams{std::make_unique<StreamState>(),
                std::make_unique<StreamState>()}
{
}

Xc2ProcessOutput::~Xc2ProcessOutput() = default;

Xc2ProcessOutput::StreamState &Xc2ProcessOutput::state(Stream stream)
{
    return *m_streams.at(stream == Stream::StandardError ? 1 : 0);
}

const Xc2ProcessOutput::StreamState &Xc2ProcessOutput::state(Stream stream) const
{
    return *m_streams.at(stream == Stream::StandardError ? 1 : 0);
}

QStringList Xc2ProcessOutput::feed(Stream stream, QByteArrayView bytes)
{
    return state(stream).feed(bytes);
}

QStringList Xc2ProcessOutput::finish(Stream stream)
{
    return state(stream).finish();
}

Xc2ProcessOutput::FinishedLines Xc2ProcessOutput::finish()
{
    FinishedLines completed;
    completed.standardOutput = state(Stream::StandardOutput).finish();
    completed.standardError = state(Stream::StandardError).finish();
    return completed;
}

void Xc2ProcessOutput::reset()
{
    m_streams[0] = std::make_unique<StreamState>();
    m_streams[1] = std::make_unique<StreamState>();
}

QStringList Xc2ProcessOutput::recentOutput(Stream stream) const
{
    return state(stream).recentOutput();
}

QByteArray Xc2ProcessOutput::pendingLineUtf8(Stream stream) const
{
    return state(stream).line;
}

qsizetype Xc2ProcessOutput::pendingLineByteCount(Stream stream) const
{
    return state(stream).line.size();
}

qsizetype Xc2ProcessOutput::retainedStateByteCount(Stream stream) const
{
    return state(stream).retainedStateByteCount();
}

bool Xc2ProcessOutput::discardingOversizedLine(Stream stream) const
{
    return state(stream).oversized;
}

} // namespace ktm::xc2
