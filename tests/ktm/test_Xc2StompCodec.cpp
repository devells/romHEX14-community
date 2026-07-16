#include <QtTest>

#include <QFile>

#include "ktm/xc2/Xc2JsonCodec.h"
#include "ktm/xc2/Xc2StompCodec.h"

using namespace ktm::xc2;

namespace {

constexpr qsizetype kMaximumFrameBytes = 8 * 1024 * 1024;

QByteArray loadFixture(const QString &relativePath)
{
    QFile file(QStringLiteral(KTM_FIXTURE_DIR "/") + relativePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

void verifyContractError(const Xc2StompDecodeResult &result)
{
    QCOMPARE(result.frames.size(), 0);
    QCOMPARE(result.errors.size(), 1);
    QCOMPARE(result.errors.front().category, Xc2ErrorCategory::Contract);
}

} // namespace

class Xc2StompCodecTest final : public QObject {
    Q_OBJECT

private slots:
    void fragmentedAndCoalescedFrames()
    {
        Xc2StompCodec codec;

        const auto fragment = codec.feed(QByteArrayLiteral("MESS"));
        QVERIFY(fragment.frames.isEmpty());
        QVERIFY(fragment.errors.isEmpty());

        const auto result = codec.feed(QByteArrayLiteral(
            "AGE\ndestination:/topic/progress\n\nfirst\0"
            "ERROR\nmessage:synthetic\n\nsecond\0"));
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.frames.size(), 2);
        QCOMPARE(result.frames.at(0).command, QByteArrayLiteral("MESSAGE"));
        QCOMPARE(result.frames.at(0).body, QByteArrayLiteral("first"));
        QCOMPARE(result.frames.at(1).command, QByteArrayLiteral("ERROR"));
        QCOMPARE(result.frames.at(1).body, QByteArrayLiteral("second"));
    }

    void heartbeatIsNotAFrame()
    {
        Xc2StompCodec codec;
        const auto result = codec.feed(QByteArrayLiteral("\n\r\n"));

        QVERIFY(result.frames.isEmpty());
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.heartbeats, 2);
    }

    void contentLengthAllowsEmbeddedNull()
    {
        const QByteArray body("A\0B", 3);
        QByteArray wire = QByteArrayLiteral("MESSAGE\ncontent-length:3\n\n");
        wire += body;
        wire += '\0';

        Xc2StompCodec codec;
        const auto result = codec.feed(wire);
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.frames.size(), 1);
        QCOMPARE(result.frames.front().body.size(), 3);
        QCOMPARE(result.frames.front().body, body);

        Xc2StompFrame frame;
        frame.command = QByteArrayLiteral("MESSAGE");
        frame.body = body;
        const QByteArray encoded = Xc2StompCodec::encode(frame);
        QVERIFY(encoded.contains(QByteArrayLiteral("content-length:3\n")));
        QCOMPARE(codec.feed(encoded).frames.front().body, body);
    }

    void contentLengthWaitsForFragmentedTerminator()
    {
        const QByteArray prefix =
            QByteArrayLiteral("MESSAGE\ncontent-length:3\n\nabc");
        Xc2StompCodec codec;

        const auto bodyOnly = codec.feed(prefix);
        QVERIFY(bodyOnly.frames.isEmpty());
        QVERIFY(bodyOnly.errors.isEmpty());

        const auto terminated = codec.feed(QByteArray(1, '\0'));
        QVERIFY(terminated.errors.isEmpty());
        QCOMPARE(terminated.frames.size(), 1);
        QCOMPARE(terminated.frames.front().body, QByteArrayLiteral("abc"));

        codec.reset();
        verifyContractError(codec.feed(prefix + 'X'));
    }

    void connectHeadersRemainUnescaped()
    {
        const QList<QByteArray> commands = {
            QByteArrayLiteral("CONNECT"),
            QByteArrayLiteral("CONNECTED"),
        };

        for (const QByteArray &command : commands) {
            Xc2StompFrame frame;
            frame.command = command;
            frame.headers.insert(QByteArrayLiteral("user\\name"),
                                 QByteArrayLiteral("line\\nvalue\\craw"));

            const QByteArray encoded = Xc2StompCodec::encode(frame);
            QVERIFY(encoded.contains(
                QByteArrayLiteral("user\\name:line\\nvalue\\craw\n")));

            Xc2StompCodec codec;
            const auto result = codec.feed(encoded);
            QVERIFY(result.errors.isEmpty());
            QCOMPARE(result.frames.size(), 1);
            QCOMPARE(result.frames.front().headers, frame.headers);
        }
    }

    void messageHeaderKeysAndValuesEscapeRoundTrip()
    {
        QByteArray key = QByteArrayLiteral("key");
        key += ':';
        key += '\\';
        key += '\n';
        key += '\r';
        QByteArray value = QByteArrayLiteral("value");
        value += ':';
        value += '\\';
        value += '\n';
        value += '\r';

        Xc2StompFrame frame;
        frame.command = QByteArrayLiteral("MESSAGE");
        frame.headers.insert(key, value);
        frame.headers.insert(QByteArrayLiteral("content-length"),
                             QByteArrayLiteral("999"));
        frame.body = QByteArrayLiteral("payload");

        const QByteArray encoded = Xc2StompCodec::encode(frame);
        const QByteArray escapedKey = QByteArrayLiteral("key\\c\\\\\\n\\r");
        const QByteArray escapedValue =
            QByteArrayLiteral("value\\c\\\\\\n\\r");
        QVERIFY(encoded.contains(escapedKey + ':' + escapedValue + '\n'));
        QVERIFY(encoded.contains(QByteArrayLiteral("content-length:7\n")));
        QVERIFY(!encoded.contains(QByteArrayLiteral("content-length:999\n")));

        Xc2StompCodec codec;
        const auto result = codec.feed(encoded);
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.frames.size(), 1);
        QCOMPARE(result.frames.front().headers.value(key), value);
        QCOMPARE(result.frames.front().body, frame.body);
    }

    void duplicateHeadersKeepFirstValue()
    {
        Xc2StompCodec codec;
        const auto result = codec.feed(QByteArrayLiteral(
            "MESSAGE\nreceipt:first\nreceipt:second\n key : value \n\n\0"));

        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.frames.size(), 1);
        QCOMPARE(result.frames.front().headers.value(QByteArrayLiteral("receipt")),
                 QByteArrayLiteral("first"));
        QCOMPARE(result.frames.front().headers.value(QByteArrayLiteral(" key ")),
                 QByteArrayLiteral(" value "));
    }

    void crlfHeartbeatAndTrailingEolAreIncremental()
    {
        Xc2StompCodec codec;

        auto result = codec.feed(QByteArrayLiteral("\r"));
        QVERIFY(result.frames.isEmpty());
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.heartbeats, 0);

        result = codec.feed(QByteArrayLiteral("\nMESSAGE\r"));
        QVERIFY(result.frames.isEmpty());
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.heartbeats, 1);

        result = codec.feed(
            QByteArrayLiteral("\ndestination:/topic/progress\r"));
        QVERIFY(result.frames.isEmpty());
        QVERIFY(result.errors.isEmpty());

        result = codec.feed(QByteArrayLiteral("\n\r"));
        QVERIFY(result.frames.isEmpty());
        QVERIFY(result.errors.isEmpty());

        result = codec.feed(QByteArrayLiteral("\nbody\0\r"));
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.frames.size(), 1);
        QCOMPARE(result.frames.front().body, QByteArrayLiteral("body"));

        result = codec.feed(QByteArrayLiteral(
            "\nERROR\nmessage:synthetic\n\nfail\0"));
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.heartbeats, 1);
        QCOMPARE(result.frames.size(), 1);
        QCOMPARE(result.frames.front().command, QByteArrayLiteral("ERROR"));
    }

    void malformedFrameReturnsProtocolError()
    {
        Xc2StompCodec codec;

        verifyContractError(codec.feed(QByteArray(1, '\0')));

        verifyContractError(codec.feed(QByteArrayLiteral(
            "MESSAGE\nbad\\x:value\n\n\0")));

        verifyContractError(codec.feed(QByteArrayLiteral(
            "MESSAGE\ncontent-length:-1\n\n")));

        verifyContractError(codec.feed(QByteArrayLiteral(
            "MESSAGE\ncontent-length:not-a-number\n\n")));

        verifyContractError(codec.feed(QByteArrayLiteral(
            "MESSAGE\ncontent-length:999999999999999999999999\n\n")));

        verifyContractError(codec.feed(QByteArrayLiteral(
            "MESSAGE\ncontent-length:8388609\n\n")));

        QByteArray oversized = QByteArrayLiteral("MESSAGE\n\n");
        oversized += QByteArray(kMaximumFrameBytes, 'x');
        verifyContractError(codec.feed(oversized));

        const auto afterReset = codec.feed(QByteArrayLiteral("CONNECTED\n\n\0"));
        QVERIFY(afterReset.errors.isEmpty());
        QCOMPARE(afterReset.frames.size(), 1);
    }

    void goldenConnectedProgressAndErrorFramesParse()
    {
        const QByteArray connected =
            loadFixture(QStringLiteral("stomp/connected.frame"));
        const QByteArray progress =
            loadFixture(QStringLiteral("stomp/progress-message.frame"));
        const QByteArray error = loadFixture(QStringLiteral("stomp/error.frame"));

        QCOMPARE(connected.size(), 39);
        QCOMPARE(progress.size(), 298);
        QCOMPARE(error.size(), 107);
        QCOMPARE(connected.back(), '\0');
        QCOMPARE(progress.back(), '\0');
        QCOMPARE(error.back(), '\0');

        Xc2StompCodec codec;
        const auto result = codec.feed(connected + progress + error);
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.frames.size(), 3);
        QCOMPARE(result.frames.at(0).command, QByteArrayLiteral("CONNECTED"));
        QCOMPARE(result.frames.at(0).headers.value(QByteArrayLiteral("version")),
                 QByteArrayLiteral("1.2"));
        QCOMPARE(result.frames.at(0).headers.value(
                     QByteArrayLiteral("heart-beat")),
                 QByteArrayLiteral("0,0"));
        QCOMPARE(result.frames.at(1).command, QByteArrayLiteral("MESSAGE"));
        QCOMPARE(result.frames.at(1).headers.value(
                     QByteArrayLiteral("destination")),
                 QByteArrayLiteral("/topic/progress"));
        QCOMPARE(result.frames.at(1).headers.value(
                     QByteArrayLiteral("message-id")),
                 QByteArrayLiteral("synthetic-progress-1"));
        QCOMPARE(result.frames.at(1).headers.value(
                     QByteArrayLiteral("subscription")),
                 QByteArrayLiteral("progress-subscription"));
        QCOMPARE(result.frames.at(1).headers.value(
                     QByteArrayLiteral("content-length")),
                 QByteArrayLiteral("144"));
        QCOMPARE(result.frames.at(1).body.size(), 144);
        const auto jobProgress =
            Xc2JsonCodec::jobProgress(result.frames.at(1).body);
        QVERIFY2(jobProgress.ok(), qPrintable(jobProgress.error.message));
        QCOMPARE(jobProgress.value->jobId,
                 QStringLiteral("00000000-0000-0000-0000-000000000001"));
        QVERIFY(jobProgress.value->message.has_value());
        QCOMPARE(jobProgress.value->message->id, qint64{42});
        QCOMPARE(jobProgress.value->message->text,
                 QStringLiteral("synthetic progress"));
        QCOMPARE(result.frames.at(2).command, QByteArrayLiteral("ERROR"));
        QCOMPARE(result.frames.at(2).body,
                 QByteArrayLiteral("synthetic protocol error"));
    }
};

QTEST_APPLESS_MAIN(Xc2StompCodecTest)
#include "test_Xc2StompCodec.moc"
