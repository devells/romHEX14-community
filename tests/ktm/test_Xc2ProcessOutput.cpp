#include <QtTest>

#include <QByteArrayView>

#include "ktm/xc2/internal/Xc2ProcessOutput.h"

using ktm::xc2::Xc2ProcessOutput;

namespace {

using Stream = Xc2ProcessOutput::Stream;

constexpr auto kStdout = Stream::StandardOutput;
constexpr auto kStderr = Stream::StandardError;

QString replacementCharacter()
{
    return QString(QChar::ReplacementCharacter);
}

QByteArray mixedCase(QByteArray name)
{
    for (qsizetype i = 0; i < name.size(); ++i) {
        if ((i % 2) == 0 && name.at(i) >= 'a' && name.at(i) <= 'z')
            name[i] = static_cast<char>(name.at(i) - 'a' + 'A');
    }
    return name;
}

QByteArray ansiInside(const QByteArray &name)
{
    const qsizetype split = name.size() / 2;
    return name.left(split) + QByteArrayLiteral("\x1b[31m")
        + name.mid(split) + QByteArrayLiteral("\x1b[0m");
}

QStringList feedOneByteAtATime(Xc2ProcessOutput &output,
                               Stream stream,
                               const QByteArray &wire,
                               const QList<QByteArray> &secrets = {})
{
    QStringList lines;
    for (const char byte : wire) {
        const QByteArray chunk(1, byte);
        lines.append(output.feed(stream, QByteArrayView(chunk)));
        QTest::qVerify(output.pendingLineByteCount(stream)
                           <= Xc2ProcessOutput::MaximumLineBytes,
                       "pending line stays bounded", "", __FILE__, __LINE__);
        QTest::qVerify(output.retainedStateByteCount(stream)
                           <= Xc2ProcessOutput::MaximumRetainedBytes,
                       "all retained state stays bounded", "", __FILE__,
                       __LINE__);
        for (const QByteArray &secret : secrets) {
            QTest::qVerify(!output.pendingLineUtf8(stream).contains(secret),
                           "pending line never stores a secret", "", __FILE__,
                           __LINE__);
            for (const QString &line : output.recentOutput(stream)) {
                QTest::qVerify(!line.toUtf8().contains(secret),
                               "recent ring never stores a secret", "",
                               __FILE__, __LINE__);
            }
        }
    }
    return lines;
}

void verifyAbsent(const QByteArray &haystack,
                  std::initializer_list<QByteArray> needles)
{
    for (const QByteArray &needle : needles)
        QVERIFY2(!haystack.contains(needle), haystack.constData());
}

} // namespace

class Xc2ProcessOutputTest final : public QObject {
    Q_OBJECT

private slots:
    void stdoutAndStderrUseIndependentFragmentBuffers()
    {
        Xc2ProcessOutput output;

        QVERIFY(output.feed(kStdout, QByteArrayView("out-part")).isEmpty());
        QCOMPARE(output.feed(kStderr, QByteArrayView("err-one\nerr-part")),
                 QStringList{QStringLiteral("err-one")});
        QCOMPARE(output.feed(kStdout, QByteArrayView("-done\r")),
                 QStringList{QStringLiteral("out-part-done")});
        QVERIFY(output.feed(kStderr, QByteArrayView("-done\r")).size() == 1);

        // A CRLF split over feeds is one delimiter and does not emit an empty line.
        QVERIFY(output.feed(kStdout, QByteArrayView("\nnext")).isEmpty());
        QVERIFY(output.feed(kStderr, QByteArrayView("\nstderr-next")).isEmpty());
        const auto finished = output.finish();
        QCOMPARE(finished.standardOutput,
                 QStringList{QStringLiteral("next")});
        QCOMPARE(finished.standardError,
                 QStringList{QStringLiteral("stderr-next")});
        QVERIFY(output.finish(kStdout).isEmpty());
        QVERIFY(output.finish(kStderr).isEmpty());

        output.reset();
        QVERIFY(output.feed(kStdout, QByteArrayView("Authori")).isEmpty());
        QVERIFY(output.feed(kStderr, QByteArrayView("Coo")).isEmpty());
        QVERIFY(output.feed(kStdout,
                            QByteArrayView("zation: stdout-secret"))
                    .isEmpty());
        QVERIFY(output.feed(kStderr, QByteArrayView("kie=stderr-secret"))
                    .isEmpty());
        verifyAbsent(output.pendingLineUtf8(kStdout),
                     {QByteArrayLiteral("stdout-secret")});
        verifyAbsent(output.pendingLineUtf8(kStderr),
                     {QByteArrayLiteral("stderr-secret")});
        QCOMPARE(output.finish(kStdout),
                 QStringList{QStringLiteral("Authorization: [redacted]")});
        QCOMPARE(output.finish(kStderr),
                 QStringList{QStringLiteral("Cookie=[redacted]")});

        output.reset();
        const QByteArray euro = QByteArray::fromHex("e282ac");
        QVERIFY(output.feed(kStdout, QByteArrayView(euro.constData(), 1))
                    .isEmpty());
        QVERIFY(output.feed(kStderr,
                            QByteArrayView(QByteArray::fromHex("ff")))
                    .isEmpty());
        QVERIFY(output.feed(kStdout,
                            QByteArrayView(euro.constData() + 1, 1))
                    .isEmpty());
        QCOMPARE(output.feed(kStderr, QByteArrayView("bad\n")),
                 QStringList{replacementCharacter() + QStringLiteral("bad")});
        QCOMPARE(output.feed(kStdout,
                             QByteArrayView(euro.constData() + 2, 1)),
                 QStringList{});
        QCOMPARE(output.feed(kStdout, QByteArrayView("\n")),
                 QStringList{QString::fromUtf8(euro)});

        const QByteArray incomplete = QByteArrayLiteral("tail")
            + QByteArray::fromHex("e282");
        QVERIFY(output.feed(kStderr, QByteArrayView(incomplete)).isEmpty());
        QCOMPARE(output.finish(kStderr),
                 QStringList{QStringLiteral("tail") + replacementCharacter()});

        const QList<QByteArray> boundaryNames = {
            QByteArrayLiteral("LF"),
            QByteArrayLiteral("CR"),
            QByteArrayLiteral("split-CRLF"),
        };
        for (int boundary = 0; boundary < boundaryNames.size(); ++boundary) {
            const QByteArray jsonSecretA = QByteArrayLiteral("JSON-LINE-A-")
                + boundaryNames.at(boundary);
            const QByteArray jsonSecretB = QByteArrayLiteral("JSON-LINE-B-")
                + boundaryNames.at(boundary);
            const QByteArray keySecretA = QByteArrayLiteral("KEY-LINE-A-")
                + boundaryNames.at(boundary);
            const QByteArray keySecretB = QByteArrayLiteral("KEY-LINE-B-")
                + boundaryNames.at(boundary);
            const QList<QByteArray> secrets = {
                jsonSecretA,
                jsonSecretB,
                keySecretA,
                keySecretB,
            };

            output.reset();
            const auto verifyState = [&](const QStringList &lines) {
                for (const QString &line : lines) {
                    for (const QByteArray &secret : secrets)
                        verifyAbsent(line.toUtf8(), {secret});
                }
                for (const QByteArray &secret : secrets) {
                    verifyAbsent(output.pendingLineUtf8(kStdout), {secret});
                    for (const QString &line : output.recentOutput(kStdout))
                        verifyAbsent(line.toUtf8(), {secret});
                }
            };
            const auto feedBoundary = [&](QByteArray prefix) {
                prefix += boundary == 0 ? '\n' : '\r';
                QStringList lines = output.feed(kStdout, QByteArrayView(prefix));
                if (boundary == 2) {
                    const QStringList splitLf =
                        output.feed(kStdout, QByteArrayView("\n"));
                    QTest::qVerify(splitLf.isEmpty(),
                                   "split CRLF emits no second line", "",
                                   __FILE__, __LINE__);
                    lines.append(splitLf);
                }
                return lines;
            };

            QStringList lines = feedBoundary(QByteArrayLiteral("{\"token\":"));
            QCOMPARE(lines.size(), 1);
            verifyState(lines);
            lines = feedBoundary(QByteArrayLiteral("\"") + jsonSecretA);
            QCOMPARE(lines.size(), 1);
            verifyState(lines);
            verifyState(output.feed(kStdout,
                                    QByteArrayView(jsonSecretB + "\"}")));
            verifyState(output.finish(kStdout));
            const QString jsonOutput =
                output.recentOutput(kStdout).join(QLatin1Char('\n'));
            QVERIFY(jsonOutput.contains(QStringLiteral("\"token\":")));
            QVERIFY(jsonOutput.contains(QStringLiteral("[redacted]")));
            QVERIFY(jsonOutput.contains(QLatin1Char('}')));

            output.reset();
            lines = feedBoundary(QByteArrayLiteral("password="));
            QCOMPARE(lines.size(), 1);
            verifyState(lines);
            lines = feedBoundary(QByteArrayLiteral("\"") + keySecretA);
            QCOMPARE(lines.size(), 1);
            verifyState(lines);
            verifyState(output.feed(
                kStdout, QByteArrayView(keySecretB + "\";visible=ok")));
            verifyState(output.finish(kStdout));
            const QString keyOutput =
                output.recentOutput(kStdout).join(QLatin1Char('\n'));
            QVERIFY(keyOutput.contains(QStringLiteral("password=")));
            QVERIFY(keyOutput.contains(QStringLiteral("[redacted]")));
            QVERIFY(keyOutput.contains(QStringLiteral("visible=ok")));
        }
    }

    void outputLinesAndRingsAreBounded()
    {
        Xc2ProcessOutput output;
        const QByteArray oversized(
            Xc2ProcessOutput::MaximumLineBytes + 4096, 'x');

        QVERIFY(output.feed(kStdout, QByteArrayView(oversized)).isEmpty());
        QVERIFY(output.discardingOversizedLine(kStdout));
        QCOMPARE(output.pendingLineByteCount(kStdout),
                 Xc2ProcessOutput::MaximumLineBytes);
        QVERIFY(output.pendingLineUtf8(kStdout).endsWith("[truncated]"));
        QCOMPARE(output.pendingLineUtf8(kStdout).count("[truncated]"), 1);

        const QByteArray hidden = QByteArrayLiteral(
            " token=oversized-secret-that-must-not-be-stored");
        QVERIFY(output.feed(kStdout, QByteArrayView(hidden)).isEmpty());
        verifyAbsent(output.pendingLineUtf8(kStdout),
                     {QByteArrayLiteral("oversized-secret"),
                      QByteArrayLiteral("must-not-be-stored")});
        const QStringList oversizedLine = output.finish(kStdout);
        QCOMPARE(oversizedLine.size(), 1);
        QCOMPARE(oversizedLine.front().toUtf8().size(),
                 Xc2ProcessOutput::MaximumLineBytes);
        QVERIFY(oversizedLine.front().endsWith(QStringLiteral("[truncated]")));
        verifyAbsent(output.recentOutput(kStdout).front().toUtf8(),
                     {QByteArrayLiteral("oversized-secret"),
                      QByteArrayLiteral("must-not-be-stored")});

        output.reset();
        const QByteArray multibyteLine(
            Xc2ProcessOutput::MaximumLineBytes - 1, 'u');
        const QByteArray euro = QByteArray::fromHex("e282ac");
        QVERIFY(output.feed(kStdout, QByteArrayView(multibyteLine)).isEmpty());
        QVERIFY(output.feed(kStdout, QByteArrayView(euro)).isEmpty());
        const QString utf8Bounded = output.finish(kStdout).front();
        QVERIFY(utf8Bounded.toUtf8().size()
                <= Xc2ProcessOutput::MaximumLineBytes);
        QVERIFY(utf8Bounded.endsWith(QStringLiteral("[truncated]")));
        QVERIFY(!utf8Bounded.contains(QChar::ReplacementCharacter));

        output.reset();
        for (int i = 0; i < 300; ++i) {
            const QByteArray line = QByteArrayLiteral("out-")
                + QByteArray::number(i).rightJustified(3, '0') + '\n';
            QCOMPARE(output.feed(kStdout, QByteArrayView(line)).size(), 1);
        }
        for (int i = 0; i < 260; ++i) {
            const QByteArray line = QByteArrayLiteral("err-")
                + QByteArray::number(i).rightJustified(3, '0') + '\n';
            QCOMPARE(output.feed(kStderr, QByteArrayView(line)).size(), 1);
        }

        const QStringList stdoutRing = output.recentOutput(kStdout);
        const QStringList stderrRing = output.recentOutput(kStderr);
        QCOMPARE(stdoutRing.size(), Xc2ProcessOutput::MaximumRecentLines);
        QCOMPARE(stderrRing.size(), Xc2ProcessOutput::MaximumRecentLines);
        QCOMPARE(stdoutRing.front(), QStringLiteral("out-044"));
        QCOMPARE(stdoutRing.back(), QStringLiteral("out-299"));
        QCOMPARE(stderrRing.front(), QStringLiteral("err-004"));
        QCOMPARE(stderrRing.back(), QStringLiteral("err-259"));
        QVERIFY(output.retainedStateByteCount(kStdout)
                <= Xc2ProcessOutput::MaximumRetainedBytes);
        QVERIFY(output.retainedStateByteCount(kStderr)
                <= Xc2ProcessOutput::MaximumRetainedBytes);

        // An unterminated OSC is discarded without retaining its payload.
        output.reset();
        QVERIFY(output.feed(kStdout, QByteArrayView("\x1b]0;"))
                    .isEmpty());
        for (int i = 0; i < 32; ++i) {
            const QByteArray oscPayload(4096, 'z');
            QVERIFY(output.feed(kStdout, QByteArrayView(oscPayload)).isEmpty());
            QCOMPARE(output.pendingLineByteCount(kStdout), qsizetype{0});
            QVERIFY(output.retainedStateByteCount(kStdout)
                    <= Xc2ProcessOutput::MaximumRetainedBytes);
        }
        QVERIFY(output.finish(kStdout).isEmpty());
    }

    void outputControlsAndSensitiveFieldsAreRemoved_data()
    {
        QTest::addColumn<QByteArray>("fieldName");
        QTest::addColumn<int>("form");
        QTest::addColumn<bool>("standardError");

        const QList<QByteArray> fields = {
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
        const char *formNames[] = {"header", "key-value", "json-string"};

        for (const QByteArray &field : fields) {
            for (int form = 0; form < 3; ++form) {
                for (const bool standardError : {false, true}) {
                    const QByteArray row = field + '-' + formNames[form]
                        + (standardError ? "-stderr" : "-stdout");
                    QTest::newRow(row.constData())
                        << field << form << standardError;
                }
            }
        }
    }

    void outputControlsAndSensitiveFieldsAreRemoved()
    {
        QFETCH(QByteArray, fieldName);
        QFETCH(int, form);
        QFETCH(bool, standardError);

        const Stream stream = standardError ? kStderr : kStdout;
        const QByteArray visibleName = mixedCase(fieldName);
        const QByteArray wireName = ansiInside(visibleName);
        const QByteArray secretA = QByteArrayLiteral("s3cr3t-A-7419");
        const QByteArray secretB = QByteArrayLiteral("s3cr3t-B-8520");
        const QByteArray secretC = QByteArrayLiteral("s3cr3t-C-9631");
        QByteArray wire;
        QByteArray expected;

        if (form == 0) {
            wire = wireName + ": " + secretA + ';' + secretB + ',' + secretC
                + '\n';
            expected = visibleName + QByteArrayLiteral(": [redacted]");
        } else if (form == 1) {
            wire = QByteArrayLiteral("before ") + wireName + "=\"" + secretA
                + "\\\"" + secretB + "\\\\" + secretC + "\";"
                + wireName + '=' + secretA + '&' + "visible=ok\n";
            expected = QByteArrayLiteral("before ") + visibleName
                + "=\"[redacted]\";" + visibleName
                + "=[redacted]&visible=ok";
        } else {
            wire = QByteArrayLiteral("{\"") + wireName + "\":\"" + secretA
                + "\\\"" + secretB + "\\\\" + secretC
                + "\",\"safe\":\"ok\"}\n";
            expected = QByteArrayLiteral("{\"") + visibleName
                + "\":\"[redacted]\",\"safe\":\"ok\"}";
        }

        Xc2ProcessOutput output;
        const QStringList lines = feedOneByteAtATime(
            output, stream, wire, {secretA, secretB, secretC});
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines.front().toUtf8(), expected);
        const QByteArray completed = lines.front().toUtf8();
        verifyAbsent(completed, {secretA, secretB, secretC});
        verifyAbsent(output.pendingLineUtf8(stream),
                     {secretA, secretB, secretC});
        for (const QString &line : output.recentOutput(stream))
            verifyAbsent(line.toUtf8(), {secretA, secretB, secretC});
        QCOMPARE(completed.count("[redacted]"), form == 1 ? 2 : 1);
        QCOMPARE(output.recentOutput(standardError ? kStdout : kStderr).size(),
                 0);

        if (fieldName != QByteArrayLiteral("authorization") || form != 0
            || standardError) {
            return;
        }

        output.reset();
        QByteArray controls;
        controls += 'A';
        controls += QByteArray::fromHex("00017f");
        controls += '\t';
        controls += QByteArrayLiteral("\x1b[31mRED\x1b[0m");
        controls += QByteArrayLiteral("\x1b]0;hidden\x07");
        controls += 'B';
        controls += QByteArray::fromHex("c29b"); // C1 CSI
        controls += QByteArrayLiteral("32mGREEN");
        controls += QByteArray::fromHex("c29d"); // C1 OSC
        controls += QByteArrayLiteral("hidden-too");
        controls += QByteArray::fromHex("c29c"); // C1 ST
        controls += QByteArray::fromHex("c285"); // standalone C1 NEL
        controls += 'C';
        controls += '\n';
        QCOMPARE(feedOneByteAtATime(output, kStdout, controls),
                 QStringList{QStringLiteral("A\tREDBGREENC")});

        output.reset();
        QVERIFY(output.feed(kStdout, QByteArrayView("left\x1b]title"))
                    .isEmpty());
        QVERIFY(output.feed(kStdout, QByteArrayView(" payload\x1b"))
                    .isEmpty());
        QCOMPARE(output.feed(kStdout, QByteArrayView("\\right\n")),
                 QStringList{QStringLiteral("leftright")});

        const QByteArray longOws(
            Xc2ProcessOutput::MaximumLookBehindBytes + 32, ' ');
        struct OwsCase {
            QByteArray wire;
            QByteArray secret;
            QByteArray expected;
        };
        const QList<OwsCase> owsCases = {
            {QByteArrayLiteral("\"token\"") + longOws
                 + QByteArrayLiteral(":\"JSON-OWS-SECRET\""),
             QByteArrayLiteral("JSON-OWS-SECRET"),
             QByteArrayLiteral("\"token\"") + longOws
                 + QByteArrayLiteral(":\"[redacted]\"")},
            {QByteArrayLiteral("Authorization") + longOws
                 + QByteArrayLiteral(": HEADER-OWS-SECRET"),
             QByteArrayLiteral("HEADER-OWS-SECRET"),
             QByteArrayLiteral("Authorization") + longOws
                 + QByteArrayLiteral(": [redacted]")},
            {QByteArrayLiteral("password") + longOws
                 + QByteArrayLiteral("=KEY-OWS-SECRET&visible=ok"),
             QByteArrayLiteral("KEY-OWS-SECRET"),
             QByteArrayLiteral("password") + longOws
                 + QByteArrayLiteral("=[redacted]&visible=ok")},
        };
        for (const OwsCase &testCase : owsCases) {
            output.reset();
            QVERIFY(feedOneByteAtATime(output, kStdout, testCase.wire,
                                      {testCase.secret})
                        .isEmpty());
            const QStringList eofLine = output.finish(kStdout);
            QCOMPARE(eofLine.size(), 1);
            QCOMPARE(eofLine.front().toUtf8(), testCase.expected);
            verifyAbsent(eofLine.front().toUtf8(), {testCase.secret});
            verifyAbsent(output.recentOutput(kStdout).front().toUtf8(),
                         {testCase.secret});
        }

        output.reset();
        const QByteArray truncatedSecret =
            QByteArrayLiteral("TRUNCATED-OWS-SECRET");
        const QByteArray hugeOws(Xc2ProcessOutput::MaximumLineBytes + 128, ' ');
        const QByteArray truncatedKey = QByteArrayLiteral("\"token\"")
            + hugeOws + QByteArrayLiteral(":\n");
        const QStringList truncatedKeyLine =
            output.feed(kStdout, QByteArrayView(truncatedKey));
        QCOMPARE(truncatedKeyLine.size(), 1);
        QVERIFY(truncatedKeyLine.front().endsWith(
            QStringLiteral("[truncated]")));
        verifyAbsent(truncatedKeyLine.front().toUtf8(), {truncatedSecret});
        QVERIFY(output.feed(kStdout,
                            QByteArrayView('"' + truncatedSecret + '"'))
                    .isEmpty());
        verifyAbsent(output.pendingLineUtf8(kStdout), {truncatedSecret});
        const QStringList truncatedEof = output.finish(kStdout);
        for (const QString &line : truncatedEof)
            verifyAbsent(line.toUtf8(), {truncatedSecret});
        for (const QString &line : output.recentOutput(kStdout))
            verifyAbsent(line.toUtf8(), {truncatedSecret});
        QVERIFY(output.recentOutput(kStdout).join(QLatin1Char('\n')).contains(
            QStringLiteral("[redacted]")));
    }
};

QTEST_APPLESS_MAIN(Xc2ProcessOutputTest)
#include "test_Xc2ProcessOutput.moc"
