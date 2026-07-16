#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QStringList>

#include <array>
#include <memory>

namespace ktm::xc2 {

class Xc2ProcessOutput final {
public:
    enum class Stream {
        StandardOutput,
        StandardError,
    };

    struct FinishedLines {
        QStringList standardOutput;
        QStringList standardError;
    };

    static constexpr qsizetype MaximumLineBytes = 16 * 1024;
    static constexpr qsizetype MaximumRecentLines = 256;
    static constexpr qsizetype MaximumLookBehindBytes = 96;
    static constexpr qsizetype MaximumUtf8DecoderBytes = 3;
    // Per stream: UTF-8 ring, pending line, recognizer, and decoder state.
    static constexpr qsizetype MaximumRetainedBytes =
        (MaximumRecentLines + 1) * MaximumLineBytes
        + MaximumLookBehindBytes + MaximumUtf8DecoderBytes;

    Xc2ProcessOutput();
    ~Xc2ProcessOutput();

    Xc2ProcessOutput(const Xc2ProcessOutput &) = delete;
    Xc2ProcessOutput &operator=(const Xc2ProcessOutput &) = delete;

    QStringList feed(Stream stream, QByteArrayView bytes);
    QStringList finish(Stream stream);
    FinishedLines finish();
    void reset();

    QStringList recentOutput(Stream stream) const;
    QByteArray pendingLineUtf8(Stream stream) const;
    qsizetype pendingLineByteCount(Stream stream) const;
    qsizetype retainedStateByteCount(Stream stream) const;
    bool discardingOversizedLine(Stream stream) const;

private:
    struct StreamState;

    StreamState &state(Stream stream);
    const StreamState &state(Stream stream) const;

    std::array<std::unique_ptr<StreamState>, 2> m_streams;
};

} // namespace ktm::xc2
