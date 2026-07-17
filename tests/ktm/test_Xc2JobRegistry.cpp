#include "ktm/xc2/Xc2ContractProfile.h"
#include "ktm/xc2/Xc2JobRegistry.h"

#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest>

#include <optional>

using namespace ktm::xc2;

namespace {

QString wireState(Xc2JobState state)
{
    switch (state) {
    case Xc2JobState::Created:
        return QStringLiteral("CREATED");
    case Xc2JobState::InProgress:
        return QStringLiteral("IN_PROGRESS");
    case Xc2JobState::Finished:
        return QStringLiteral("FINISHED");
    case Xc2JobState::Canceled:
        return QStringLiteral("CANCELED");
    case Xc2JobState::Error:
        return QStringLiteral("ERROR");
    case Xc2JobState::NotAuthorized:
        return QStringLiteral("NOT_AUTHORIZED");
    }
    return {};
}

Xc2JobEvent makeEvent(Xc2StompGeneration generation,
                      QString messageId,
                      QString jobId,
                      Xc2JobState state,
                      qint32 ticks = 0,
                      qint32 totalTicks = 0)
{
    Xc2JobProgress progress;
    progress.jobId = std::move(jobId);
    progress.state = state;
    progress.ticks = ticks;
    progress.totalTicks = totalTicks;
    progress.raw = {
        {QStringLiteral("jobId"), progress.jobId},
        {QStringLiteral("status"), wireState(progress.state)},
        {QStringLiteral("ticks"), progress.ticks},
        {QStringLiteral("totalTicks"), progress.totalTicks},
        {QStringLiteral("message"), QJsonValue(QJsonValue::Null)}
    };
    return {generation, std::move(messageId), std::move(progress)};
}

void setMessage(Xc2JobEvent &event, qint64 id, const QString &text)
{
    event.progress.message = Xc2LocalizedText{id, text};
    event.progress.raw.insert(
        QStringLiteral("message"),
        QJsonObject{{QStringLiteral("id"), id},
                    {QStringLiteral("text"), text}});
}

QByteArray progressBody(const QString &jobId,
                        Xc2JobState state,
                        qint32 ticks,
                        qint32 totalTicks,
                        std::optional<Xc2LocalizedText> message = std::nullopt)
{
    QJsonObject object{
        {QStringLiteral("jobId"), jobId},
        {QStringLiteral("status"), wireState(state)},
        {QStringLiteral("ticks"), ticks},
        {QStringLiteral("totalTicks"), totalTicks},
        {QStringLiteral("message"), QJsonValue(QJsonValue::Null)}
    };
    if (message) {
        object.insert(
            QStringLiteral("message"),
            QJsonObject{{QStringLiteral("id"), message->id},
                        {QStringLiteral("text"), message->text}});
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool localizedEqual(const std::optional<Xc2LocalizedText> &left,
                    const std::optional<Xc2LocalizedText> &right)
{
    if (left.has_value() != right.has_value())
        return false;
    return !left || (left->id == right->id && left->text == right->text);
}

bool progressEqual(const Xc2JobProgress &left, const Xc2JobProgress &right)
{
    return left.jobId == right.jobId && left.state == right.state
        && left.ticks == right.ticks && left.totalTicks == right.totalTicks
        && localizedEqual(left.message, right.message) && left.raw == right.raw;
}

bool eventEqual(const Xc2JobEvent &left, const Xc2JobEvent &right)
{
    return left.generation == right.generation
        && left.messageId == right.messageId
        && progressEqual(left.progress, right.progress);
}

bool recordEqual(const Xc2JobRecord &left, const Xc2JobRecord &right)
{
    if (left.jobId != right.jobId || left.state != right.state
        || left.visibilityLost != right.visibilityLost
        || left.events.size() != right.events.size()) {
        return false;
    }
    for (qsizetype i = 0; i < left.events.size(); ++i) {
        if (!eventEqual(left.events.at(i), right.events.at(i)))
            return false;
    }
    return true;
}

bool recordsEqual(const QList<Xc2JobRecord> &left,
                  const QList<Xc2JobRecord> &right)
{
    if (left.size() != right.size())
        return false;
    for (qsizetype i = 0; i < left.size(); ++i) {
        if (!recordEqual(left.at(i), right.at(i)))
            return false;
    }
    return true;
}

struct RegistrySnapshot {
    QStringList keys;
    QList<std::optional<Xc2JobRecord>> jobs;
    QList<Xc2JobRecord> active;
};

RegistrySnapshot takeSnapshot(const Xc2JobRegistry &registry,
                              QStringList keys)
{
    keys.removeDuplicates();
    RegistrySnapshot snapshot;
    snapshot.keys = std::move(keys);
    for (const QString &key : snapshot.keys)
        snapshot.jobs.append(registry.job(key));
    snapshot.active = registry.activeJobs();
    return snapshot;
}

bool snapshotEqual(const RegistrySnapshot &left,
                   const RegistrySnapshot &right)
{
    if (left.keys != right.keys || left.jobs.size() != right.jobs.size()
        || !recordsEqual(left.active, right.active)) {
        return false;
    }
    for (qsizetype i = 0; i < left.jobs.size(); ++i) {
        const auto &leftJob = left.jobs.at(i);
        const auto &rightJob = right.jobs.at(i);
        if (leftJob.has_value() != rightJob.has_value())
            return false;
        if (leftJob && !recordEqual(*leftJob, *rightJob))
            return false;
    }
    return true;
}

Xc2Error dirtyError()
{
    Xc2Error error;
    error.category = Xc2ErrorCategory::Backend;
    error.transportReason = Xc2TransportReason::Timeout;
    error.message = QStringLiteral("dirty");
    error.httpStatus = 500;
    error.xc2Status = 1;
    error.xc2Code = 2;
    error.developerMessage = QStringLiteral("developer");
    error.info = QStringLiteral("info");
    error.endpoint = QStringLiteral("endpoint");
    error.jobId = QStringLiteral("dirty-job");
    error.ecuId = QStringLiteral("ecu");
    error.dpdu.insert(QStringLiteral("dirty"), true);
    error.rawPayload = QByteArrayLiteral("dirty");
    return error;
}

bool errorCleared(const Xc2Error &error)
{
    return error.category == Xc2ErrorCategory::None
        && error.transportReason == Xc2TransportReason::None
        && error.message.isEmpty() && error.httpStatus == 0
        && error.xc2Status == 0 && error.xc2Code == 0
        && error.developerMessage.isEmpty() && error.info.isEmpty()
        && error.endpoint.isEmpty() && error.jobId.isEmpty()
        && error.ecuId.isEmpty() && error.dpdu.isEmpty()
        && error.rawPayload.isEmpty();
}

Xc2JobRecord signalRecord(const QSignalSpy &spy, qsizetype index)
{
    return qvariant_cast<Xc2JobRecord>(spy.at(index).at(0));
}

QStringList recordIds(const QList<Xc2JobRecord> &records)
{
    QStringList result;
    for (const Xc2JobRecord &record : records)
        result.append(record.jobId);
    return result;
}

} // namespace

class TestXc2JobRegistry : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<Xc2JobEvent>("ktm::xc2::Xc2JobEvent");
        qRegisterMetaType<Xc2JobRecord>("ktm::xc2::Xc2JobRecord");
    }

    void acceptedJobStartsCreated()
    {
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        QVERIFY(changed.isValid());
        QVERIFY(terminal.isValid());
        QVERIFY(removed.isValid());

        Xc2Error error = dirtyError();
        QVERIFY(registry.accept({QStringLiteral("job-a")}, &error));
        QVERIFY(errorCleared(error));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);

        const auto record = registry.job(QStringLiteral("job-a"));
        QVERIFY(record.has_value());
        QCOMPARE(record->jobId, QStringLiteral("job-a"));
        QCOMPARE(record->state, Xc2JobState::Created);
        QVERIFY(record->events.isEmpty());
        QVERIFY(!record->visibilityLost);
        QVERIFY(!record->terminal());
        QCOMPARE(registry.activeJobs().size(), 1);
        QVERIFY(recordEqual(signalRecord(changed, 0), *record));
    }

    void duplicateAcceptIsIdempotentAndNeverResets()
    {
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QVERIFY(registry.accept({QStringLiteral("job-a")}));
        QVERIFY(registry.apply(makeEvent(1, QStringLiteral("m-1"),
                                         QStringLiteral("job-a"),
                                         Xc2JobState::InProgress, 2, 10)));
        registry.markVisibilityLost(1);
        const RegistrySnapshot lost = takeSnapshot(registry, {QStringLiteral("job-a")});
        changed.clear();
        terminal.clear();

        Xc2Error error = dirtyError();
        QVERIFY(registry.accept({QStringLiteral("job-a")}, &error));
        QVERIFY(errorCleared(error));
        QVERIFY(snapshotEqual(lost,
                              takeSnapshot(registry, {QStringLiteral("job-a")})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);

        QVERIFY(registry.apply(makeEvent(2, QStringLiteral("m-2"),
                                         QStringLiteral("job-a"),
                                         Xc2JobState::Finished, 0, 0)));
        const RegistrySnapshot finished = takeSnapshot(
            registry, {QStringLiteral("job-a")});
        changed.clear();
        terminal.clear();
        error = dirtyError();
        QVERIFY(registry.accept({QStringLiteral("job-a")}, &error));
        QVERIFY(errorCleared(error));
        QVERIFY(snapshotEqual(finished,
                              takeSnapshot(registry, {QStringLiteral("job-a")})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
    }

    void rejectsBlankAndRetiredJobIdsAtomically()
    {
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        QVERIFY(registry.accept({QStringLiteral("stable")}));
        changed.clear();

        const QStringList blankIds{QString(), QStringLiteral(" \t\r\n")};
        for (const QString &jobId : blankIds) {
            const QStringList keys{QStringLiteral("stable"), jobId};
            const RegistrySnapshot before = takeSnapshot(registry, keys);
            Xc2Error error = dirtyError();
            QVERIFY(!registry.accept({jobId}, &error));
            QCOMPARE(error.category, Xc2ErrorCategory::Contract);
            QCOMPARE(error.jobId, jobId);
            QVERIFY(snapshotEqual(before, takeSnapshot(registry, keys)));
            QCOMPARE(changed.size(), 0);
            QCOMPARE(terminal.size(), 0);
            QCOMPARE(removed.size(), 0);
        }

        const QString retired = QStringLiteral("retired");
        QVERIFY(registry.apply(makeEvent(1, QStringLiteral("terminal"), retired,
                                         Xc2JobState::Finished)));
        QVERIFY(registry.clearTerminal(retired));
        changed.clear();
        terminal.clear();
        removed.clear();
        const QStringList keys{QStringLiteral("stable"), retired};
        const RegistrySnapshot before = takeSnapshot(registry, keys);
        Xc2Error error = dirtyError();
        QVERIFY(!registry.accept({retired}, &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Job);
        QCOMPARE(error.jobId, retired);
        QVERIFY(snapshotEqual(before, takeSnapshot(registry, keys)));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
    }

    void progressTransitionsCreatedToInProgressToFinished()
    {
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QVERIFY(registry.accept({QStringLiteral("job-a")}));
        changed.clear();

        const Xc2JobEvent progress = makeEvent(
            1, QStringLiteral("progress"), QStringLiteral("job-a"),
            Xc2JobState::InProgress, 8, 10);
        Xc2Error error = dirtyError();
        QVERIFY(registry.apply(progress, &error));
        QVERIFY(errorCleared(error));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 0);
        auto record = registry.job(QStringLiteral("job-a"));
        QVERIFY(record.has_value());
        QCOMPARE(record->state, Xc2JobState::InProgress);
        QCOMPARE(record->events.size(), 1);
        QVERIFY(eventEqual(record->events.constFirst(), progress));

        changed.clear();
        const Xc2JobEvent finished = makeEvent(
            1, QStringLiteral("finished"), QStringLiteral("job-a"),
            Xc2JobState::Finished, 1, 1);
        QVERIFY(registry.apply(finished));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 1);
        record = registry.job(QStringLiteral("job-a"));
        QVERIFY(record.has_value());
        QCOMPARE(record->state, Xc2JobState::Finished);
        QCOMPARE(record->events.size(), 2);
        QVERIFY(eventEqual(record->events.constLast(), finished));
        QVERIFY(record->terminal());
        QVERIFY(registry.activeJobs().isEmpty());
        QVERIFY(recordEqual(signalRecord(changed, 0), *record));
        QVERIFY(recordEqual(signalRecord(terminal, 0), *record));
    }

    void everyTerminalStateRejectsEveryLaterProgress_data()
    {
        QTest::addColumn<int>("terminalStateValue");
        QTest::newRow("finished") << int(Xc2JobState::Finished);
        QTest::newRow("canceled") << int(Xc2JobState::Canceled);
        QTest::newRow("error") << int(Xc2JobState::Error);
        QTest::newRow("not-authorized") << int(Xc2JobState::NotAuthorized);
    }

    void everyTerminalStateRejectsEveryLaterProgress()
    {
        QFETCH(int, terminalStateValue);
        const Xc2JobState initial = Xc2JobState(terminalStateValue);
        const QString jobId = QStringLiteral("terminal-job");
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        QStringList signalOrder;
        connect(&registry, &Xc2JobRegistry::jobChanged, &registry,
                [&signalOrder](const Xc2JobRecord &) {
            signalOrder.append(QStringLiteral("changed"));
        });
        connect(&registry, &Xc2JobRegistry::jobTerminal, &registry,
                [&signalOrder](const Xc2JobRecord &) {
            signalOrder.append(QStringLiteral("terminal"));
        });

        const QString invalidJob = QStringLiteral("invalid-terminal");
        const QList<Xc2JobEvent> invalidTerminalEvents{
            makeEvent(1, QStringLiteral("negative"), invalidJob, initial, -1, 0),
            makeEvent(1, QStringLiteral("over-total"), invalidJob, initial, 2, 1)
        };
        for (const Xc2JobEvent &invalid : invalidTerminalEvents) {
            const RegistrySnapshot before = takeSnapshot(
                registry, {jobId, invalidJob});
            Xc2Error error = dirtyError();
            QVERIFY(!registry.apply(invalid, &error));
            QCOMPARE(error.category, Xc2ErrorCategory::Contract);
            QCOMPARE(error.jobId, invalidJob);
            QVERIFY(snapshotEqual(before, takeSnapshot(
                registry, {jobId, invalidJob})));
            QCOMPARE(changed.size(), 0);
            QCOMPARE(terminal.size(), 0);
            QCOMPARE(removed.size(), 0);
            QCOMPARE(signalOrder.size(), 0);
        }

        QVERIFY(registry.accept({jobId}));
        QVERIFY(registry.apply(makeEvent(
            1, QStringLiteral("baseline"), jobId,
            Xc2JobState::InProgress, 8, 10)));
        changed.clear();
        terminal.clear();
        removed.clear();
        signalOrder.clear();
        const Xc2JobEvent first = makeEvent(
            2, QStringLiteral("first"), jobId, initial, 1, 1);
        QVERIFY(registry.apply(first));
        QCOMPARE(signalOrder,
                 (QStringList{QStringLiteral("changed"),
                              QStringLiteral("terminal")}));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 1);
        QCOMPARE(removed.size(), 0);
        const auto committed = registry.job(jobId);
        QVERIFY(committed.has_value());
        QCOMPARE(committed->state, initial);
        QCOMPARE(committed->events.size(), 2);
        QVERIFY(eventEqual(committed->events.constLast(), first));
        QVERIFY(recordEqual(signalRecord(changed, 0), *committed));
        QVERIFY(recordEqual(signalRecord(terminal, 0), *committed));
        QVERIFY(registry.activeJobs().isEmpty());
        changed.clear();
        terminal.clear();
        signalOrder.clear();

        Xc2Error replayError = dirtyError();
        QVERIFY(registry.apply(first, &replayError));
        QVERIFY(errorCleared(replayError));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QCOMPARE(signalOrder.size(), 0);

        const QList<Xc2JobState> laterStates{
            Xc2JobState::InProgress,
            Xc2JobState::Finished,
            Xc2JobState::Canceled,
            Xc2JobState::Error,
            Xc2JobState::NotAuthorized
        };
        const RegistrySnapshot before = takeSnapshot(registry, {jobId});
        for (qsizetype i = 0; i < laterStates.size(); ++i) {
            const Xc2JobEvent later = makeEvent(
                3, QStringLiteral("later-%1").arg(i), jobId,
                laterStates.at(i), 0, 0);
            Xc2Error error = dirtyError();
            QVERIFY(!registry.apply(later, &error));
            QCOMPARE(error.category, Xc2ErrorCategory::Job);
            QCOMPARE(error.jobId, jobId);
            QVERIFY(snapshotEqual(before, takeSnapshot(registry, {jobId})));
            QCOMPARE(changed.size(), 0);
            QCOMPARE(terminal.size(), 0);
            QCOMPARE(removed.size(), 0);
            QCOMPARE(signalOrder.size(), 0);
        }
    }

    void unknownJobIsCreatedFromProgressWithoutLosingEvent()
    {
        const QString exactJobId = QStringLiteral("  backend/job-01  ");
        const Xc2JobEvent event = makeEvent(
            3, QStringLiteral("delivery-1"), exactJobId,
            Xc2JobState::InProgress, 2, 12);
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QVERIFY(registry.apply(event));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 0);

        const auto record = registry.job(exactJobId);
        QVERIFY(record.has_value());
        QCOMPARE(record->jobId, exactJobId);
        QCOMPARE(record->state, Xc2JobState::InProgress);
        QCOMPARE(record->events.size(), 1);
        QVERIFY(eventEqual(record->events.constFirst(), event));
        QVERIFY(!registry.job(exactJobId.trimmed()).has_value());
        QCOMPARE(recordIds(registry.activeJobs()), QStringList{exactJobId});
    }

    void progressNeverAttachesToAnotherActiveJob()
    {
        Xc2JobRegistry registry;
        QVERIFY(registry.accept({QStringLiteral("only-active")}));
        const Xc2JobEvent event = makeEvent(
            1, QStringLiteral("other-delivery"), QStringLiteral("other-job"),
            Xc2JobState::InProgress, 1, 4);
        QVERIFY(registry.apply(event));

        const auto first = registry.job(QStringLiteral("only-active"));
        const auto second = registry.job(QStringLiteral("other-job"));
        QVERIFY(first.has_value());
        QVERIFY(second.has_value());
        QCOMPARE(first->state, Xc2JobState::Created);
        QVERIFY(first->events.isEmpty());
        QCOMPARE(second->events.size(), 1);
        QVERIFY(eventEqual(second->events.constFirst(), event));
        QCOMPARE(recordIds(registry.activeJobs()),
                 (QStringList{QStringLiteral("only-active"),
                              QStringLiteral("other-job")}));
    }

    void duplicateDeliveryIsIdempotent()
    {
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        const QString jobId = QStringLiteral("job-a");
        const Xc2JobEvent progress = makeEvent(
            1, QStringLiteral("delivery"), jobId,
            Xc2JobState::InProgress, 1, 10);
        QVERIFY(registry.apply(progress));
        changed.clear();
        const RegistrySnapshot before = takeSnapshot(registry, {jobId});
        Xc2Error error = dirtyError();
        QVERIFY(registry.apply(progress, &error));
        QVERIFY(errorCleared(error));
        QVERIFY(snapshotEqual(before, takeSnapshot(registry, {jobId})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);

        const Xc2JobEvent finished = makeEvent(
            1, QStringLiteral("terminal"), jobId,
            Xc2JobState::Finished, 0, 0);
        QVERIFY(registry.apply(finished));
        changed.clear();
        terminal.clear();
        const RegistrySnapshot terminalSnapshot = takeSnapshot(registry, {jobId});
        error = dirtyError();
        QVERIFY(registry.apply(finished, &error));
        QVERIFY(errorCleared(error));
        QVERIFY(snapshotEqual(terminalSnapshot,
                              takeSnapshot(registry, {jobId})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
    }

    void reusedDeliveryIdentityWithDifferentPayloadIsContractError()
    {
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        Xc2JobEvent original = makeEvent(
            4, QStringLiteral("global-id"), QStringLiteral("job-a"),
            Xc2JobState::InProgress, 2, 10);
        setMessage(original, 7, QStringLiteral("base"));
        original.progress.raw.insert(QStringLiteral("opaque"),
                                     QStringLiteral("base"));
        QVERIFY(registry.apply(original));
        changed.clear();

        QList<Xc2JobEvent> variants;
        Xc2JobEvent variant = original;
        variant.progress.jobId = QStringLiteral("job-b");
        variants.append(variant);
        variant = original;
        variant.progress.state = Xc2JobState::Finished;
        variants.append(variant);
        variant = original;
        variant.progress.ticks = 3;
        variants.append(variant);
        variant = original;
        variant.progress.totalTicks = 11;
        variants.append(variant);
        variant = original;
        variant.progress.message = Xc2LocalizedText{8, QStringLiteral("base")};
        variants.append(variant);
        variant = original;
        variant.progress.message =
            Xc2LocalizedText{7, QStringLiteral("different")};
        variants.append(variant);
        variant = original;
        variant.progress.message.reset();
        variants.append(variant);
        variant = original;
        variant.progress.raw.insert(QStringLiteral("opaque"),
                                    QStringLiteral("different"));
        variants.append(variant);

        const QStringList keys{QStringLiteral("job-a"), QStringLiteral("job-b")};
        const RegistrySnapshot before = takeSnapshot(registry, keys);
        for (const Xc2JobEvent &conflict : variants) {
            Xc2Error error = dirtyError();
            QVERIFY(!registry.apply(conflict, &error));
            QCOMPARE(error.category, Xc2ErrorCategory::Contract);
            QCOMPARE(error.jobId, conflict.progress.jobId);
            QVERIFY(snapshotEqual(before, takeSnapshot(registry, keys)));
            QCOMPARE(changed.size(), 0);
            QCOMPARE(terminal.size(), 0);
            QCOMPARE(removed.size(), 0);
        }

        Xc2Error replayError = dirtyError();
        QVERIFY(registry.apply(original, &replayError));
        QVERIFY(errorCleared(replayError));
        QVERIFY(snapshotEqual(before, takeSnapshot(registry, keys)));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
    }

    void identicalPayloadWithDifferentIdentityRemainsOrdered()
    {
        Xc2JobRegistry registry;
        const QString jobId = QStringLiteral("job-a");
        Xc2JobEvent first = makeEvent(
            5, QStringLiteral("first"), jobId,
            Xc2JobState::InProgress, 3, 10);
        Xc2JobEvent second = first;
        second.messageId = QStringLiteral("second");
        Xc2JobEvent third = first;
        third.generation = 6;
        QVERIFY(registry.apply(first));
        QVERIFY(registry.apply(second));
        QVERIFY(registry.apply(third));

        const auto record = registry.job(jobId);
        QVERIFY(record.has_value());
        QCOMPARE(record->events.size(), 3);
        QVERIFY(eventEqual(record->events.at(0), first));
        QVERIFY(eventEqual(record->events.at(1), second));
        QVERIFY(eventEqual(record->events.at(2), third));
        QVERIFY(progressEqual(record->events.at(0).progress,
                              record->events.at(1).progress));
        QVERIFY(progressEqual(record->events.at(0).progress,
                              record->events.at(2).progress));
    }

    void semanticValidationRejectsInvalidAndRegressiveProgress()
    {
        const QString jobId = QStringLiteral("job-a");
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        QVERIFY(registry.accept({jobId}));
        QVERIFY(registry.apply(makeEvent(1, QStringLiteral("baseline"), jobId,
                                         Xc2JobState::InProgress, 5, 10)));
        changed.clear();

        struct Rejection {
            Xc2JobEvent event;
            Xc2ErrorCategory category;
        };
        QList<Rejection> rejections;
        rejections.append({makeEvent(0, QStringLiteral("zero-generation"), jobId,
                                     Xc2JobState::InProgress, 6, 10),
                           Xc2ErrorCategory::Contract});
        rejections.append({makeEvent(2, QString(), jobId,
                                     Xc2JobState::InProgress, 6, 10),
                           Xc2ErrorCategory::Contract});
        rejections.append({makeEvent(2, QStringLiteral("blank-job"),
                                     QStringLiteral(" \t"),
                                     Xc2JobState::InProgress, 1, 1),
                           Xc2ErrorCategory::Contract});
        rejections.append({makeEvent(2, QStringLiteral("created"), jobId,
                                     Xc2JobState::Created, 0, 0),
                           Xc2ErrorCategory::Contract});
        rejections.append({makeEvent(2, QStringLiteral("negative-ticks"), jobId,
                                     Xc2JobState::InProgress, -1, 10),
                           Xc2ErrorCategory::Contract});
        rejections.append({makeEvent(2, QStringLiteral("negative-total"), jobId,
                                     Xc2JobState::InProgress, 5, -1),
                           Xc2ErrorCategory::Contract});
        rejections.append({makeEvent(2, QStringLiteral("over-total"), jobId,
                                     Xc2JobState::InProgress, 11, 10),
                           Xc2ErrorCategory::Contract});
        rejections.append({makeEvent(2, QStringLiteral("ticks-regress"), jobId,
                                     Xc2JobState::InProgress, 4, 10),
                           Xc2ErrorCategory::Job});
        rejections.append({makeEvent(2, QStringLiteral("total-unknown"), jobId,
                                     Xc2JobState::InProgress, 6, 0),
                           Xc2ErrorCategory::Job});
        rejections.append({makeEvent(2, QStringLiteral("total-regress"), jobId,
                                     Xc2JobState::InProgress, 6, 9),
                           Xc2ErrorCategory::Job});

        const QStringList keys{jobId, QStringLiteral(" \t")};
        const RegistrySnapshot before = takeSnapshot(registry, keys);
        for (const Rejection &rejection : rejections) {
            Xc2Error error = dirtyError();
            QVERIFY(!registry.apply(rejection.event, &error));
            QCOMPARE(error.category, rejection.category);
            QCOMPARE(error.jobId, rejection.event.progress.jobId);
            QVERIFY(snapshotEqual(before, takeSnapshot(registry, keys)));
            QCOMPARE(changed.size(), 0);
            QCOMPARE(terminal.size(), 0);
            QCOMPARE(removed.size(), 0);
        }

        Xc2JobEvent correctedRegression = makeEvent(
            2, QStringLiteral("ticks-regress"), jobId,
            Xc2JobState::InProgress, 6, 10);
        Xc2Error error = dirtyError();
        QVERIFY(registry.apply(correctedRegression, &error));
        QVERIFY(errorCleared(error));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QCOMPARE(registry.job(jobId)->events.size(), 2);
        changed.clear();
        const RegistrySnapshot afterRegression = takeSnapshot(registry, keys);

        const Xc2JobEvent invalidRetry = makeEvent(
            3, QStringLiteral("retryable-identity"), jobId,
            Xc2JobState::InProgress, -1, 10);
        error = dirtyError();
        QVERIFY(!registry.apply(invalidRetry, &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Contract);
        QCOMPARE(error.jobId, jobId);
        QVERIFY(snapshotEqual(afterRegression, takeSnapshot(registry, keys)));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        Xc2JobEvent corrected = invalidRetry;
        corrected.progress.ticks = 7;
        corrected.progress.raw.insert(QStringLiteral("ticks"), 7);
        error = dirtyError();
        QVERIFY(registry.apply(corrected, &error));
        QVERIFY(errorCleared(error));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
    }

    void terminalTransitionEmitsChangedThenTerminalExactlyOnce()
    {
        const QString jobId = QStringLiteral("job-a");
        Xc2JobRegistry registry;
        QVERIFY(registry.accept({jobId}));
        QVERIFY(registry.apply(makeEvent(1, QStringLiteral("progress"), jobId,
                                         Xc2JobState::InProgress, 2, 10)));
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        QStringList order;
        bool changedSawCommitted = false;
        bool terminalSawCommitted = false;
        bool changedSawCommittedDelivery = false;
        connect(&registry, &Xc2JobRegistry::jobChanged, &registry,
                [&registry, &order, &changedSawCommitted,
                 &changedSawCommittedDelivery](
                    const Xc2JobRecord &record) {
            order.append(QStringLiteral("changed"));
            const auto visible = registry.job(record.jobId);
            changedSawCommitted = visible && recordEqual(*visible, record)
                && visible->terminal() && registry.activeJobs().isEmpty();
            Xc2Error replayError = dirtyError();
            changedSawCommittedDelivery = !record.events.isEmpty()
                && registry.apply(record.events.constLast(), &replayError)
                && errorCleared(replayError);
        });
        connect(&registry, &Xc2JobRegistry::jobTerminal, &registry,
                [&registry, &order, &terminalSawCommitted](
                    const Xc2JobRecord &record) {
            order.append(QStringLiteral("terminal"));
            const auto visible = registry.job(record.jobId);
            terminalSawCommitted = visible && recordEqual(*visible, record)
                && visible->terminal() && registry.activeJobs().isEmpty();
        });

        const Xc2JobEvent event = makeEvent(
            2, QStringLiteral("terminal"), jobId,
            Xc2JobState::Error, 0, 0);
        QVERIFY(registry.apply(event));
        QCOMPARE(order, (QStringList{QStringLiteral("changed"),
                                     QStringLiteral("terminal")}));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 1);
        QCOMPARE(removed.size(), 0);
        QVERIFY(changedSawCommitted);
        QVERIFY(changedSawCommittedDelivery);
        QVERIFY(terminalSawCommitted);
        QVERIFY(recordEqual(signalRecord(changed, 0), signalRecord(terminal, 0)));

        Xc2Error error = dirtyError();
        QVERIFY(registry.apply(event, &error));
        QVERIFY(errorCleared(error));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 1);
        QCOMPARE(removed.size(), 0);
        QCOMPARE(order.size(), 2);

        const RegistrySnapshot before = takeSnapshot(registry, {jobId});
        changed.clear();
        terminal.clear();
        order.clear();
        Xc2JobEvent newIdentity = event;
        newIdentity.messageId = QStringLiteral("terminal-again");
        QVERIFY(!registry.apply(newIdentity, &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Job);
        QCOMPARE(error.jobId, jobId);
        QVERIFY(snapshotEqual(before, takeSnapshot(registry, {jobId})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QCOMPARE(order.size(), 0);
    }

    void activeJobsHaveStableFirstObservedOrder()
    {
        Xc2JobRegistry registry;
        QVERIFY(registry.accept({QStringLiteral("job-b")}));
        QVERIFY(registry.accept({QStringLiteral("job-a")}));
        QVERIFY(registry.accept({QStringLiteral("job-c")}));
        QVERIFY(registry.apply(makeEvent(1, QStringLiteral("finish-a"),
                                         QStringLiteral("job-a"),
                                         Xc2JobState::Finished)));
        QVERIFY(registry.apply(makeEvent(1, QStringLiteral("discover-d"),
                                         QStringLiteral("job-d"),
                                         Xc2JobState::InProgress, 1, 1)));

        QCOMPARE(recordIds(registry.activeJobs()),
                 (QStringList{QStringLiteral("job-b"),
                              QStringLiteral("job-c"),
                              QStringLiteral("job-d")}));
        QVERIFY(registry.clearTerminal(QStringLiteral("job-a")));
        QCOMPARE(recordIds(registry.activeJobs()),
                 (QStringList{QStringLiteral("job-b"),
                              QStringLiteral("job-c"),
                              QStringLiteral("job-d")}));

        const QString leadingSpace = QStringLiteral(" job-b");
        const QString differentCase = QStringLiteral("JOB-B");
        const QString composed = QStringLiteral("\u00e9");
        const QString decomposed = QStringLiteral("e\u0301");
        QVERIFY(composed != decomposed);
        QVERIFY(registry.accept({leadingSpace}));
        QVERIFY(registry.accept({differentCase}));
        QVERIFY(registry.accept({composed}));
        QVERIFY(registry.accept({decomposed}));
        QVERIFY(registry.job(leadingSpace).has_value());
        QVERIFY(registry.job(differentCase).has_value());
        QVERIFY(registry.job(composed).has_value());
        QVERIFY(registry.job(decomposed).has_value());
        QCOMPARE(recordIds(registry.activeJobs()),
                 (QStringList{QStringLiteral("job-b"),
                              QStringLiteral("job-c"),
                              QStringLiteral("job-d"), leadingSpace,
                              differentCase, composed, decomposed}));
    }

    void repeatedVisibilityLossIsIdempotentAndOrdered()
    {
        {
            Xc2JobRegistry terminalOnly;
            QSignalSpy changedOnly(&terminalOnly,
                                   &Xc2JobRegistry::jobChanged);
            QSignalSpy terminalOnlySignal(&terminalOnly,
                                          &Xc2JobRegistry::jobTerminal);
            QSignalSpy removedOnly(&terminalOnly,
                                   &Xc2JobRegistry::jobRemoved);
            QVERIFY(terminalOnly.apply(makeEvent(
                3, QStringLiteral("only-terminal"),
                QStringLiteral("only-terminal"), Xc2JobState::Finished)));
            changedOnly.clear();
            terminalOnlySignal.clear();
            const RegistrySnapshot before = takeSnapshot(
                terminalOnly, {QStringLiteral("only-terminal")});
            terminalOnly.markVisibilityLost(3);
            QVERIFY(snapshotEqual(before, takeSnapshot(
                terminalOnly, {QStringLiteral("only-terminal")})));
            QCOMPARE(changedOnly.size(), 0);
            QCOMPARE(terminalOnlySignal.size(), 0);
            QCOMPARE(removedOnly.size(), 0);
        }

        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        registry.markVisibilityLost(1);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVERIFY(registry.accept({QStringLiteral("third")}));
        QVERIFY(registry.accept({QStringLiteral("first")}));
        QVERIFY(registry.accept({QStringLiteral("second")}));
        QVERIFY(registry.apply(makeEvent(2, QStringLiteral("first-progress"),
                                         QStringLiteral("first"),
                                         Xc2JobState::InProgress, 2, 10)));
        QVERIFY(registry.apply(makeEvent(2, QStringLiteral("second-terminal"),
                                         QStringLiteral("second"),
                                         Xc2JobState::Canceled)));
        changed.clear();
        terminal.clear();
        removed.clear();

        int visibilitySignalsObserved = 0;
        bool everySignalSawAtomicSnapshot = true;
        connect(&registry, &Xc2JobRegistry::jobChanged, &registry,
                [&registry, &visibilitySignalsObserved,
                 &everySignalSawAtomicSnapshot](const Xc2JobRecord &record) {
            if (!record.visibilityLost)
                return;
            ++visibilitySignalsObserved;
            const auto third = registry.job(QStringLiteral("third"));
            const auto first = registry.job(QStringLiteral("first"));
            everySignalSawAtomicSnapshot = everySignalSawAtomicSnapshot
                && third && third->visibilityLost && first
                && first->visibilityLost;
        });

        registry.markVisibilityLost(7);
        QCOMPARE(changed.size(), 2);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QCOMPARE(visibilitySignalsObserved, 2);
        QVERIFY(everySignalSawAtomicSnapshot);
        QCOMPARE(signalRecord(changed, 0).jobId, QStringLiteral("third"));
        QCOMPARE(signalRecord(changed, 1).jobId, QStringLiteral("first"));
        const auto first = registry.job(QStringLiteral("first"));
        const auto third = registry.job(QStringLiteral("third"));
        const auto second = registry.job(QStringLiteral("second"));
        QVERIFY(first && first->visibilityLost);
        QVERIFY(third && third->visibilityLost);
        QVERIFY(second && !second->visibilityLost);
        QCOMPARE(first->state, Xc2JobState::InProgress);
        QCOMPARE(first->events.size(), 1);

        const RegistrySnapshot before = takeSnapshot(
            registry, {QStringLiteral("third"), QStringLiteral("first"),
                       QStringLiteral("second")});
        changed.clear();
        registry.markVisibilityLost(10);
        registry.markVisibilityLost(10);
        registry.markVisibilityLost(6);
        QVERIFY(snapshotEqual(before, takeSnapshot(
            registry, {QStringLiteral("third"), QStringLiteral("first"),
                       QStringLiteral("second")})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);

        Xc2Error error = dirtyError();
        QVERIFY(!registry.apply(makeEvent(
            9, QStringLiteral("between-loss-watermarks"),
            QStringLiteral("first"), Xc2JobState::InProgress, 3, 10),
            &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Job);
        QCOMPARE(error.jobId, QStringLiteral("first"));
        QVERIFY(snapshotEqual(before, takeSnapshot(
            registry, {QStringLiteral("third"), QStringLiteral("first"),
                       QStringLiteral("second")})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
    }

    void laterGenerationProgressRecoversOnlyItsExactJob()
    {
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        QVERIFY(registry.apply(makeEvent(4, QStringLiteral("a-0"),
                                         QStringLiteral("job-a"),
                                         Xc2JobState::InProgress, 1, 10)));
        QVERIFY(registry.apply(makeEvent(4, QStringLiteral("b-0"),
                                         QStringLiteral("job-b"),
                                         Xc2JobState::InProgress, 1, 10)));
        registry.markVisibilityLost(4);
        changed.clear();

        QVERIFY(registry.apply(makeEvent(9, QStringLiteral("c-0"),
                                         QStringLiteral("job-c"),
                                         Xc2JobState::InProgress, 1, 10)));
        QVERIFY(registry.job(QStringLiteral("job-a"))->visibilityLost);
        QVERIFY(registry.job(QStringLiteral("job-b"))->visibilityLost);
        QVERIFY(!registry.job(QStringLiteral("job-c"))->visibilityLost);

        changed.clear();
        QVERIFY(registry.apply(makeEvent(5, QStringLiteral("a-1"),
                                         QStringLiteral("job-a"),
                                         Xc2JobState::InProgress, 2, 10)));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(signalRecord(changed, 0).jobId, QStringLiteral("job-a"));
        QVERIFY(!registry.job(QStringLiteral("job-a"))->visibilityLost);
        QVERIFY(registry.job(QStringLiteral("job-b"))->visibilityLost);
        QCOMPARE(recordIds(registry.activeJobs()),
                 (QStringList{QStringLiteral("job-a"),
                              QStringLiteral("job-b"),
                              QStringLiteral("job-c")}));

        changed.clear();
        const QStringList keys{QStringLiteral("job-a"),
                               QStringLiteral("job-b"),
                               QStringLiteral("job-c")};
        const RegistrySnapshot beforeStaleTerminal = takeSnapshot(registry, keys);
        Xc2Error error = dirtyError();
        QVERIFY(!registry.apply(makeEvent(
            4, QStringLiteral("b-stale-terminal"), QStringLiteral("job-b"),
            Xc2JobState::Error, 0, 0), &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Job);
        QCOMPARE(error.jobId, QStringLiteral("job-b"));
        QVERIFY(snapshotEqual(beforeStaleTerminal, takeSnapshot(registry, keys)));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);

        QVERIFY(registry.apply(makeEvent(
            5, QStringLiteral("b-fresh-terminal"), QStringLiteral("job-b"),
            Xc2JobState::Error, 0, 0)));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 1);
        QCOMPARE(removed.size(), 0);
        QVERIFY(registry.job(QStringLiteral("job-b"))->terminal());
        QVERIFY(!registry.job(QStringLiteral("job-b"))->visibilityLost);
        QCOMPARE(recordIds(registry.activeJobs()),
                 (QStringList{QStringLiteral("job-a"),
                              QStringLiteral("job-c")}));
    }

    void staleGenerationCannotRecoverVisibility()
    {
        const QString jobId = QStringLiteral("job-a");
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        const Xc2JobEvent original = makeEvent(
            4, QStringLiteral("original"), jobId,
            Xc2JobState::InProgress, 1, 10);
        QVERIFY(registry.apply(original));
        registry.markVisibilityLost(5);
        changed.clear();
        terminal.clear();
        removed.clear();
        const RegistrySnapshot before = takeSnapshot(registry, {jobId});

        const Xc2JobEvent staleRetry = makeEvent(
            5, QStringLiteral("stale-retry"), jobId,
            Xc2JobState::InProgress, 2, 10);
        const QList<Xc2JobEvent> staleEvents{
            makeEvent(4, QStringLiteral("stale-4"), jobId,
                      Xc2JobState::InProgress, 2, 10),
            staleRetry
        };
        for (const Xc2JobEvent &stale : staleEvents) {
            Xc2Error error = dirtyError();
            QVERIFY(!registry.apply(stale, &error));
            QCOMPARE(error.category, Xc2ErrorCategory::Job);
            QCOMPARE(error.jobId, jobId);
            QVERIFY(snapshotEqual(before, takeSnapshot(registry, {jobId})));
            QCOMPARE(changed.size(), 0);
            QCOMPARE(terminal.size(), 0);
            QCOMPARE(removed.size(), 0);
        }

        Xc2Error replayError = dirtyError();
        QVERIFY(registry.apply(original, &replayError));
        QVERIFY(errorCleared(replayError));
        QVERIFY(snapshotEqual(before, takeSnapshot(registry, {jobId})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);

        Xc2Error repeatedStaleError = dirtyError();
        QVERIFY(!registry.apply(staleRetry, &repeatedStaleError));
        QCOMPARE(repeatedStaleError.category, Xc2ErrorCategory::Job);
        QCOMPARE(repeatedStaleError.jobId, jobId);
        QVERIFY(snapshotEqual(before, takeSnapshot(registry, {jobId})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);

        QVERIFY(registry.apply(makeEvent(6, QStringLiteral("fresh"), jobId,
                                         Xc2JobState::InProgress, 2, 10)));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVERIFY(!registry.job(jobId)->visibilityLost);
        QCOMPARE(registry.job(jobId)->events.size(), 2);

        const RegistrySnapshot recovered = takeSnapshot(registry, {jobId});
        changed.clear();
        Xc2Error staleAfterRecoveryError = dirtyError();
        QVERIFY(!registry.apply(staleRetry, &staleAfterRecoveryError));
        QCOMPARE(staleAfterRecoveryError.category, Xc2ErrorCategory::Job);
        QCOMPARE(staleAfterRecoveryError.jobId, jobId);
        QVERIFY(snapshotEqual(recovered, takeSnapshot(registry, {jobId})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
    }

    void clearRemovesOnlyRequestedTerminalAndEmitsRemoved()
    {
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        QVERIFY(registry.accept({QStringLiteral("active")}));
        QVERIFY(registry.apply(makeEvent(1, QStringLiteral("terminal-b"),
                                         QStringLiteral("terminal-b"),
                                         Xc2JobState::Finished)));
        QVERIFY(registry.apply(makeEvent(1, QStringLiteral("terminal-c"),
                                         QStringLiteral("terminal-c"),
                                         Xc2JobState::Error)));
        changed.clear();
        terminal.clear();
        removed.clear();

        const QString blankId = QStringLiteral(" \t");
        const RegistrySnapshot beforeBlankClear = takeSnapshot(
            registry, {QStringLiteral("active"), QStringLiteral("terminal-b"),
                       QStringLiteral("terminal-c"), blankId});
        Xc2Error error = dirtyError();
        QVERIFY(!registry.clearTerminal(blankId, &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Contract);
        QCOMPARE(error.jobId, blankId);
        QVERIFY(snapshotEqual(beforeBlankClear, takeSnapshot(
            registry, {QStringLiteral("active"), QStringLiteral("terminal-b"),
                       QStringLiteral("terminal-c"), blankId})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);

        bool removedSawDeletedRecord = false;
        bool removedSawCommittedTombstone = false;
        connect(&registry, &Xc2JobRegistry::jobRemoved, &registry,
                [&registry, &removedSawDeletedRecord,
                 &removedSawCommittedTombstone](const QString &jobId) {
            if (jobId != QStringLiteral("terminal-b"))
                return;
            removedSawDeletedRecord = !registry.job(jobId).has_value();
            Xc2Error reentrantError;
            removedSawCommittedTombstone =
                !registry.accept({jobId}, &reentrantError)
                && reentrantError.category == Xc2ErrorCategory::Job
                && reentrantError.jobId == jobId;
        });

        error = dirtyError();
        QVERIFY(registry.clearTerminal(QStringLiteral("terminal-b"), &error));
        QVERIFY(errorCleared(error));
        QCOMPARE(removed.size(), 1);
        QCOMPARE(removed.at(0).at(0).toString(), QStringLiteral("terminal-b"));
        QVERIFY(removedSawDeletedRecord);
        QVERIFY(removedSawCommittedTombstone);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QVERIFY(!registry.job(QStringLiteral("terminal-b")).has_value());
        QVERIFY(registry.job(QStringLiteral("terminal-c")).has_value());
        QVERIFY(registry.job(QStringLiteral("active")).has_value());
        QCOMPARE(recordIds(registry.activeJobs()),
                 QStringList{QStringLiteral("active")});

        const QStringList keys{QStringLiteral("active"),
                               QStringLiteral("terminal-b"),
                               QStringLiteral("terminal-c"),
                               QStringLiteral("unknown")};
        const RegistrySnapshot before = takeSnapshot(registry, keys);
        removed.clear();
        for (const QString &jobId : {QStringLiteral("unknown"),
                                    QStringLiteral("active")}) {
            error = dirtyError();
            QVERIFY(!registry.clearTerminal(jobId, &error));
            QCOMPARE(error.category, Xc2ErrorCategory::Job);
            QCOMPARE(error.jobId, jobId);
            QVERIFY(snapshotEqual(before, takeSnapshot(registry, keys)));
            QCOMPARE(changed.size(), 0);
            QCOMPARE(terminal.size(), 0);
            QCOMPARE(removed.size(), 0);
        }

        error = dirtyError();
        QVERIFY(registry.accept({QStringLiteral("unknown")}, &error));
        QVERIFY(errorCleared(error));
        QCOMPARE(changed.size(), 1);
        error = dirtyError();
        QVERIFY(registry.accept({QStringLiteral("active")}, &error));
        QVERIFY(errorCleared(error));
        QCOMPARE(changed.size(), 1);
        QVERIFY(registry.apply(makeEvent(
            2, QStringLiteral("active-progress"), QStringLiteral("active"),
            Xc2JobState::InProgress, 1, 1)));
        QCOMPARE(changed.size(), 2);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
    }

    void clearedTerminalCannotBeRecreatedByLateMessages()
    {
        const QString jobId = QStringLiteral("retired-job");
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        const Xc2JobEvent original = makeEvent(
            7, QStringLiteral("original"), jobId,
            Xc2JobState::Finished, 0, 0);
        QVERIFY(registry.apply(original));
        QVERIFY(registry.clearTerminal(jobId));
        changed.clear();
        terminal.clear();
        removed.clear();
        const RegistrySnapshot retired = takeSnapshot(registry, {jobId});

        Xc2Error error = dirtyError();
        QVERIFY(registry.apply(original, &error));
        QVERIFY(errorCleared(error));
        QVERIFY(snapshotEqual(retired, takeSnapshot(registry, {jobId})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);

        Xc2JobEvent identityConflict = original;
        identityConflict.progress.ticks = 1;
        identityConflict.progress.totalTicks = 1;
        error = dirtyError();
        QVERIFY(!registry.apply(identityConflict, &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Contract);
        QCOMPARE(error.jobId, jobId);
        QVERIFY(snapshotEqual(retired, takeSnapshot(registry, {jobId})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);

        Xc2JobEvent late = original;
        late.generation = 8;
        late.messageId = QStringLiteral("late");
        error = dirtyError();
        QVERIFY(!registry.apply(late, &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Job);
        QCOMPARE(error.jobId, jobId);
        QVERIFY(snapshotEqual(retired, takeSnapshot(registry, {jobId})));

        error = dirtyError();
        QVERIFY(!registry.accept({jobId}, &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Job);
        QCOMPARE(error.jobId, jobId);
        QVERIFY(snapshotEqual(retired, takeSnapshot(registry, {jobId})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
    }

    void malformedOrNonProgressMessageDoesNotMutate()
    {
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        QVERIFY(registry.accept({QStringLiteral("stable")}));
        changed.clear();
        const QString destination =
            Xc2ContractProfile::approved().topic(Topic::Progress);
        const QByteArray body = progressBody(
            QStringLiteral("message-job"), Xc2JobState::InProgress, 2, 10,
            Xc2LocalizedText{42, QStringLiteral("Working")});

        QList<Xc2StompMessage> rejected;
        rejected.append({1, Topic::VehicleInfo, destination, QStringLiteral("sub"),
                         QStringLiteral("wrong-topic"), body});
        rejected.append({1, Topic::Progress, QStringLiteral("/topic/not-progress"),
                         QStringLiteral("sub"), QStringLiteral("wrong-destination"),
                         body});
        rejected.append({1, Topic::Progress, destination, QStringLiteral("sub"),
                         QString(), body});
        rejected.append({0, Topic::Progress, destination, QStringLiteral("sub"),
                         QStringLiteral("zero-generation"), body});
        rejected.append({1, Topic::Progress, destination, QStringLiteral("sub"),
                         QStringLiteral("malformed"), QByteArrayLiteral("{")});
        rejected.append({1, Topic::Progress, destination, QStringLiteral("sub"),
                         QStringLiteral("not-progress"), QByteArrayLiteral("{}")});

        const QStringList keys{QStringLiteral("stable"),
                               QStringLiteral("message-job")};
        const RegistrySnapshot before = takeSnapshot(registry, keys);
        for (const Xc2StompMessage &message : rejected) {
            Xc2Error error = dirtyError();
            QVERIFY(!registry.applyMessage(message, &error));
            QCOMPARE(error.category, Xc2ErrorCategory::Contract);
            QVERIFY(snapshotEqual(before, takeSnapshot(registry, keys)));
            QCOMPARE(changed.size(), 0);
            QCOMPARE(terminal.size(), 0);
            QCOMPARE(removed.size(), 0);
        }

        Xc2StompMessage valid{2, Topic::Progress, destination,
                              QStringLiteral("sub"), QStringLiteral("valid"), body};
        Xc2Error error = dirtyError();
        QVERIFY(registry.applyMessage(valid, &error));
        QVERIFY(errorCleared(error));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        const auto record = registry.job(QStringLiteral("message-job"));
        QVERIFY(record.has_value());
        QCOMPARE(record->events.size(), 1);
        Xc2JobEvent expected = makeEvent(
            2, QStringLiteral("valid"), QStringLiteral("message-job"),
            Xc2JobState::InProgress, 2, 10);
        setMessage(expected, 42, QStringLiteral("Working"));
        QVERIFY(eventEqual(record->events.constFirst(), expected));
        QCOMPARE(record->events.constFirst().progress.raw,
                 QJsonDocument::fromJson(body).object());

        changed.clear();
        const RegistrySnapshot delivered = takeSnapshot(registry, keys);
        valid.body = QByteArrayLiteral(
            "{ \"message\" : {\"text\":\"Working\",\"id\":42}, "
            "\"totalTicks\" : 10, \"ticks\" : 2, "
            "\"status\" : \"IN_PROGRESS\", \"jobId\" : \"message-job\" }");
        error = dirtyError();
        QVERIFY(registry.applyMessage(valid, &error));
        QVERIFY(errorCleared(error));
        QVERIFY(snapshotEqual(delivered, takeSnapshot(registry, keys)));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);

        valid.body = QByteArrayLiteral(
            "{\"jobId\":\"message-job\",\"status\":\"IN_PROGRESS\","
            "\"ticks\":2,\"totalTicks\":10,"
            "\"message\":{\"id\":42,\"text\":\"Working\"},"
            "\"extra\":true}");
        error = dirtyError();
        QVERIFY(!registry.applyMessage(valid, &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Contract);
        QCOMPARE(error.jobId, QStringLiteral("message-job"));
        QVERIFY(snapshotEqual(delivered, takeSnapshot(registry, keys)));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestXc2JobRegistry)
#include "test_Xc2JobRegistry.moc"
