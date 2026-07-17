#include "ktm/xc2/Xc2ContractProfile.h"
#include "ktm/xc2/Xc2JobRegistry.h"

#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest>

#include <new>
#include <optional>
#include <utility>

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

Xc2JobRecord expectedRecord(QString jobId,
                            Xc2JobState state = Xc2JobState::Created,
                            QList<Xc2JobEvent> events = {},
                            bool visibilityLost = false)
{
    Xc2JobRecord record;
    record.jobId = std::move(jobId);
    record.state = state;
    record.events = std::move(events);
    record.visibilityLost = visibilityLost;
    return record;
}

enum class ObservedKind { Changed, Terminal, Removed };

struct ObservedNotification {
    ObservedKind kind = ObservedKind::Changed;
    Xc2JobRecord record;
    QString jobId;
};

void observeNotifications(Xc2JobRegistry &registry,
                          QObject &context,
                          QList<ObservedNotification> &notifications)
{
    QObject::connect(
        &registry, &Xc2JobRegistry::jobChanged, &context,
        [&notifications](const Xc2JobRecord &record) {
            notifications.append(
                {ObservedKind::Changed, record, record.jobId});
        },
        Qt::DirectConnection);
    QObject::connect(
        &registry, &Xc2JobRegistry::jobTerminal, &context,
        [&notifications](const Xc2JobRecord &record) {
            notifications.append(
                {ObservedKind::Terminal, record, record.jobId});
        },
        Qt::DirectConnection);
    QObject::connect(
        &registry, &Xc2JobRegistry::jobRemoved, &context,
        [&notifications](const QString &jobId) {
            notifications.append({ObservedKind::Removed, {}, jobId});
        },
        Qt::DirectConnection);
}

bool notificationEquals(const ObservedNotification &notification,
                        ObservedKind kind,
                        const Xc2JobRecord &record)
{
    return notification.kind == kind && notification.jobId == record.jobId
        && recordEqual(notification.record, record);
}

bool removedNotificationEquals(const ObservedNotification &notification,
                               const QString &jobId)
{
    return notification.kind == ObservedKind::Removed
        && notification.jobId == jobId;
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
        const Xc2JobRecord expected = expectedRecord(QStringLiteral("job-a"));
        QVERIFY(recordEqual(*record, expected));
        QVERIFY(recordsEqual(registry.activeJobs(), {expected}));
        QVERIFY(recordEqual(signalRecord(changed, 0), expected));
    }

    void duplicateAcceptIsIdempotentAndNeverResets()
    {
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
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
        QCOMPARE(removed.size(), 0);

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
        QCOMPARE(removed.size(), 0);
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
            makeEvent(1, QStringLiteral("negative-total"), invalidJob,
                      initial, 0, -1),
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

        {
            const QString unknownJobId = QStringLiteral("unknown-terminal");
            const Xc2JobEvent unknownTerminal = makeEvent(
                1, QStringLiteral("unknown-first-terminal"), unknownJobId,
                initial, 0, 0);
            const Xc2JobRecord expected = expectedRecord(
                unknownJobId, initial, {unknownTerminal});
            Xc2JobRegistry unknownRegistry;
            QSignalSpy unknownChanged(
                &unknownRegistry, &Xc2JobRegistry::jobChanged);
            QSignalSpy unknownTerminalSignal(
                &unknownRegistry, &Xc2JobRegistry::jobTerminal);
            QSignalSpy unknownRemoved(
                &unknownRegistry, &Xc2JobRegistry::jobRemoved);
            QStringList unknownOrder;
            connect(&unknownRegistry, &Xc2JobRegistry::jobChanged,
                    &unknownRegistry,
                    [&unknownOrder](const Xc2JobRecord &) {
                unknownOrder.append(QStringLiteral("changed"));
            });
            connect(&unknownRegistry, &Xc2JobRegistry::jobTerminal,
                    &unknownRegistry,
                    [&unknownOrder](const Xc2JobRecord &) {
                unknownOrder.append(QStringLiteral("terminal"));
            });

            QVERIFY(unknownRegistry.apply(unknownTerminal));
            QCOMPARE(unknownOrder,
                     (QStringList{QStringLiteral("changed"),
                                  QStringLiteral("terminal")}));
            QCOMPARE(unknownChanged.size(), 1);
            QCOMPARE(unknownTerminalSignal.size(), 1);
            QCOMPARE(unknownRemoved.size(), 0);
            QVERIFY(recordEqual(signalRecord(unknownChanged, 0), expected));
            QVERIFY(recordEqual(signalRecord(unknownTerminalSignal, 0),
                                expected));
            const auto retained = unknownRegistry.job(unknownJobId);
            QVERIFY(retained.has_value());
            QVERIFY(recordEqual(*retained, expected));
            QVERIFY(unknownRegistry.activeJobs().isEmpty());
        }

        const Xc2JobEvent baseline = makeEvent(
            1, QStringLiteral("baseline"), jobId,
            Xc2JobState::InProgress, 8, 10);
        QVERIFY(registry.accept({jobId}));
        QVERIFY(registry.apply(baseline));
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
        const Xc2JobRecord expected = expectedRecord(
            jobId, initial, {baseline, first});
        QVERIFY(recordEqual(*committed, expected));
        QVERIFY(recordEqual(signalRecord(changed, 0), expected));
        QVERIFY(recordEqual(signalRecord(terminal, 0), expected));
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
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        bool creationWasCommitted = false;
        bool deliveryWasCommitted = false;
        connect(&registry, &Xc2JobRegistry::jobChanged, this,
                [&registry, &event, &creationWasCommitted,
                 &deliveryWasCommitted](const Xc2JobRecord &record) {
            const Xc2JobRecord expected = expectedRecord(
                event.progress.jobId, Xc2JobState::InProgress, {event});
            const auto retained = registry.job(event.progress.jobId);
            creationWasCommitted = retained
                && recordEqual(*retained, expected)
                && recordsEqual(registry.activeJobs(), {expected});
            Xc2Error replayError = dirtyError();
            deliveryWasCommitted = registry.apply(event, &replayError)
                && errorCleared(replayError);
        }, Qt::DirectConnection);
        QVERIFY(registry.apply(event));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVERIFY(creationWasCommitted);
        QVERIFY(deliveryWasCommitted);

        const auto record = registry.job(exactJobId);
        QVERIFY(record.has_value());
        QCOMPARE(record->jobId, exactJobId);
        QCOMPARE(record->state, Xc2JobState::InProgress);
        QCOMPARE(record->events.size(), 1);
        QVERIFY(eventEqual(record->events.constFirst(), event));
        QVERIFY(!registry.job(exactJobId.trimmed()).has_value());
        const Xc2JobRecord expected = expectedRecord(
            exactJobId, Xc2JobState::InProgress, {event});
        QVERIFY(recordEqual(*record, expected));
        QVERIFY(recordsEqual(registry.activeJobs(), {expected}));
        QVERIFY(recordEqual(signalRecord(changed, 0), expected));
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
        const Xc2JobRecord expectedFirst = expectedRecord(
            QStringLiteral("only-active"));
        const Xc2JobRecord expectedSecond = expectedRecord(
            QStringLiteral("other-job"), Xc2JobState::InProgress, {event});
        QVERIFY(recordEqual(*first, expectedFirst));
        QVERIFY(recordEqual(*second, expectedSecond));
        QVERIFY(recordsEqual(registry.activeJobs(),
                             {expectedFirst, expectedSecond}));
    }

    void duplicateDeliveryIsIdempotent()
    {
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
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
        QCOMPARE(removed.size(), 0);

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
        QCOMPARE(removed.size(), 0);
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
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
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
        const Xc2JobRecord afterFirst = expectedRecord(
            jobId, Xc2JobState::InProgress, {first});
        const Xc2JobRecord afterSecond = expectedRecord(
            jobId, Xc2JobState::InProgress, {first, second});
        const Xc2JobRecord afterThird = expectedRecord(
            jobId, Xc2JobState::InProgress, {first, second, third});
        QVERIFY(recordEqual(*record, afterThird));
        QVERIFY(recordsEqual(registry.activeJobs(), {afterThird}));
        QCOMPARE(changed.size(), 3);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVERIFY(recordEqual(signalRecord(changed, 0), afterFirst));
        QVERIFY(recordEqual(signalRecord(changed, 1), afterSecond));
        QVERIFY(recordEqual(signalRecord(changed, 2), afterThird));
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

    void positiveInProgressCounterEvolutionIsAccepted()
    {
        const QString jobId = QStringLiteral("counter-growth");
        const Xc2JobEvent unknownTotal = makeEvent(
            1, QStringLiteral("unknown-total"), jobId,
            Xc2JobState::InProgress, 1, 0);
        const Xc2JobEvent knownTotal = makeEvent(
            2, QStringLiteral("known-total"), jobId,
            Xc2JobState::InProgress, 2, 10);
        const Xc2JobEvent grownTotal = makeEvent(
            3, QStringLiteral("grown-total"), jobId,
            Xc2JobState::InProgress, 3, 12);
        const QList<Xc2JobEvent> events{
            unknownTotal, knownTotal, grownTotal
        };
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        QList<Xc2JobEvent> committedEvents;

        for (qsizetype i = 0; i < events.size(); ++i) {
            committedEvents.append(events.at(i));
            Xc2Error error = dirtyError();
            QVERIFY(registry.apply(events.at(i), &error));
            QVERIFY(errorCleared(error));
            const Xc2JobRecord expected = expectedRecord(
                jobId, Xc2JobState::InProgress, committedEvents);
            const auto retained = registry.job(jobId);
            QVERIFY(retained.has_value());
            QVERIFY(recordEqual(*retained, expected));
            QVERIFY(recordsEqual(registry.activeJobs(), {expected}));
            QCOMPARE(changed.size(), i + 1);
            QVERIFY(recordEqual(signalRecord(changed, i), expected));
            QCOMPARE(terminal.size(), 0);
            QCOMPARE(removed.size(), 0);
        }
    }

    void terminalTransitionEmitsChangedThenTerminalExactlyOnce()
    {
        const QString jobId = QStringLiteral("job-a");
        Xc2JobRegistry registry;
        QVERIFY(registry.accept({jobId}));
        const Xc2JobEvent progress = makeEvent(
            1, QStringLiteral("progress"), jobId,
            Xc2JobState::InProgress, 2, 10);
        QVERIFY(registry.apply(progress));
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
        const Xc2JobRecord expected = expectedRecord(
            jobId, Xc2JobState::Error, {progress, event});
        QVERIFY(registry.apply(event));
        QCOMPARE(order, (QStringList{QStringLiteral("changed"),
                                     QStringLiteral("terminal")}));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 1);
        QCOMPARE(removed.size(), 0);
        QVERIFY(changedSawCommitted);
        QVERIFY(changedSawCommittedDelivery);
        QVERIFY(terminalSawCommitted);
        QVERIFY(recordEqual(signalRecord(changed, 0), expected));
        QVERIFY(recordEqual(signalRecord(terminal, 0), expected));
        QVERIFY(recordEqual(*registry.job(jobId), expected));

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
        for (int attempt = 0; attempt < 2; ++attempt) {
            error = dirtyError();
            QVERIFY(!registry.apply(newIdentity, &error));
            QCOMPARE(error.category, Xc2ErrorCategory::Job);
            QCOMPARE(error.jobId, jobId);
            QVERIFY(snapshotEqual(before, takeSnapshot(registry, {jobId})));
            QCOMPARE(changed.size(), 0);
            QCOMPARE(terminal.size(), 0);
            QCOMPARE(removed.size(), 0);
            QCOMPARE(order.size(), 0);
        }
    }

    void activeJobsHaveStableFirstObservedOrder()
    {
        Xc2JobRegistry registry;
        QVERIFY(registry.accept({QStringLiteral("job-b")}));
        QVERIFY(registry.accept({QStringLiteral("job-a")}));
        QVERIFY(registry.accept({QStringLiteral("job-c")}));
        const Xc2JobEvent finishA = makeEvent(
            1, QStringLiteral("finish-a"), QStringLiteral("job-a"),
            Xc2JobState::Finished);
        const Xc2JobEvent discoverD = makeEvent(
            1, QStringLiteral("discover-d"), QStringLiteral("job-d"),
            Xc2JobState::InProgress, 1, 1);
        QVERIFY(registry.apply(finishA));
        QVERIFY(registry.apply(discoverD));

        const Xc2JobRecord expectedB = expectedRecord(QStringLiteral("job-b"));
        const Xc2JobRecord expectedC = expectedRecord(QStringLiteral("job-c"));
        const Xc2JobRecord expectedD = expectedRecord(
            QStringLiteral("job-d"), Xc2JobState::InProgress, {discoverD});
        QVERIFY(recordsEqual(registry.activeJobs(),
                             {expectedB, expectedC, expectedD}));
        QVERIFY(registry.clearTerminal(QStringLiteral("job-a")));
        QVERIFY(recordsEqual(registry.activeJobs(),
                             {expectedB, expectedC, expectedD}));

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
        QVERIFY(recordsEqual(
            registry.activeJobs(),
            {expectedB, expectedC, expectedD, expectedRecord(leadingSpace),
             expectedRecord(differentCase), expectedRecord(composed),
             expectedRecord(decomposed)}));
    }

    void repeatedVisibilityLossIsIdempotentAndOrdered()
    {
        {
            const QString onlyTerminalId = QStringLiteral("only-terminal");
            const Xc2JobEvent onlyTerminalEvent = makeEvent(
                3, QStringLiteral("only-terminal"), onlyTerminalId,
                Xc2JobState::Finished);
            const Xc2JobRecord onlyTerminalRecord = expectedRecord(
                onlyTerminalId, Xc2JobState::Finished, {onlyTerminalEvent});
            Xc2JobRegistry terminalOnly;
            QSignalSpy changedOnly(&terminalOnly,
                                   &Xc2JobRegistry::jobChanged);
            QSignalSpy terminalOnlySignal(&terminalOnly,
                                          &Xc2JobRegistry::jobTerminal);
            QSignalSpy removedOnly(&terminalOnly,
                                   &Xc2JobRegistry::jobRemoved);
            QVERIFY(terminalOnly.apply(onlyTerminalEvent));
            changedOnly.clear();
            terminalOnlySignal.clear();
            const RegistrySnapshot before = takeSnapshot(
                terminalOnly, {onlyTerminalId});
            terminalOnly.markVisibilityLost(3);
            QVERIFY(snapshotEqual(before, takeSnapshot(
                terminalOnly, {onlyTerminalId})));
            const auto retained = terminalOnly.job(onlyTerminalId);
            QVERIFY(retained.has_value());
            QVERIFY(recordEqual(*retained, onlyTerminalRecord));
            QVERIFY(terminalOnly.activeJobs().isEmpty());
            QCOMPARE(changedOnly.size(), 0);
            QCOMPARE(terminalOnlySignal.size(), 0);
            QCOMPARE(removedOnly.size(), 0);
        }

        const QString thirdId = QStringLiteral("third");
        const QString firstId = QStringLiteral("first");
        const QString secondId = QStringLiteral("second");
        const Xc2JobEvent firstProgress = makeEvent(
            2, QStringLiteral("first-progress"), firstId,
            Xc2JobState::InProgress, 2, 10);
        const Xc2JobEvent secondTerminal = makeEvent(
            2, QStringLiteral("second-terminal"), secondId,
            Xc2JobState::Canceled);
        const Xc2JobRecord visibleThird = expectedRecord(thirdId);
        const Xc2JobRecord visibleFirst = expectedRecord(
            firstId, Xc2JobState::InProgress, {firstProgress});
        const Xc2JobRecord terminalSecond = expectedRecord(
            secondId, Xc2JobState::Canceled, {secondTerminal});
        const Xc2JobRecord lostThird = expectedRecord(
            thirdId, Xc2JobState::Created, {}, true);
        const Xc2JobRecord lostFirst = expectedRecord(
            firstId, Xc2JobState::InProgress, {firstProgress}, true);
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        registry.markVisibilityLost(1);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVERIFY(registry.accept({thirdId}));
        QVERIFY(registry.accept({firstId}));
        QVERIFY(registry.accept({secondId}));
        QVERIFY(registry.apply(firstProgress));
        QVERIFY(registry.apply(secondTerminal));
        QVERIFY(recordEqual(*registry.job(thirdId), visibleThird));
        QVERIFY(recordEqual(*registry.job(firstId), visibleFirst));
        QVERIFY(recordEqual(*registry.job(secondId), terminalSecond));
        QVERIFY(recordsEqual(registry.activeJobs(),
                             {visibleThird, visibleFirst}));
        changed.clear();
        terminal.clear();
        removed.clear();

        int visibilitySignalsObserved = 0;
        bool everySignalSawAtomicSnapshot = true;
        connect(&registry, &Xc2JobRegistry::jobChanged, &registry,
                [&registry, &visibilitySignalsObserved,
                 &everySignalSawAtomicSnapshot, &thirdId, &firstId,
                 &lostThird, &lostFirst](const Xc2JobRecord &record) {
            if (!record.visibilityLost)
                return;
            ++visibilitySignalsObserved;
            const auto third = registry.job(thirdId);
            const auto first = registry.job(firstId);
            everySignalSawAtomicSnapshot = everySignalSawAtomicSnapshot
                && third && recordEqual(*third, lostThird)
                && first && recordEqual(*first, lostFirst);
        });

        registry.markVisibilityLost(7);
        QCOMPARE(changed.size(), 2);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QCOMPARE(visibilitySignalsObserved, 2);
        QVERIFY(everySignalSawAtomicSnapshot);
        QVERIFY(recordEqual(signalRecord(changed, 0), lostThird));
        QVERIFY(recordEqual(signalRecord(changed, 1), lostFirst));
        QVERIFY(recordEqual(*registry.job(thirdId), lostThird));
        QVERIFY(recordEqual(*registry.job(firstId), lostFirst));
        QVERIFY(recordEqual(*registry.job(secondId), terminalSecond));
        QVERIFY(recordsEqual(registry.activeJobs(), {lostThird, lostFirst}));

        const RegistrySnapshot before = takeSnapshot(
            registry, {thirdId, firstId, secondId});
        changed.clear();
        registry.markVisibilityLost(10);
        registry.markVisibilityLost(10);
        registry.markVisibilityLost(6);
        QVERIFY(snapshotEqual(before, takeSnapshot(
            registry, {thirdId, firstId, secondId})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);

        const Xc2JobEvent stale = makeEvent(
            9, QStringLiteral("between-loss-watermarks"), firstId,
            Xc2JobState::InProgress, 3, 10);
        Xc2Error error = dirtyError();
        QVERIFY(!registry.apply(stale, &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Job);
        QCOMPARE(error.jobId, firstId);
        QVERIFY(snapshotEqual(before, takeSnapshot(
            registry, {thirdId, firstId, secondId})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);

        const Xc2JobEvent fresh = makeEvent(
            11, QStringLiteral("after-higher-watermark"), firstId,
            Xc2JobState::InProgress, 3, 12);
        QVERIFY(registry.apply(fresh));
        const Xc2JobRecord recoveredFirst = expectedRecord(
            firstId, Xc2JobState::InProgress, {firstProgress, fresh});
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVERIFY(recordEqual(signalRecord(changed, 0), recoveredFirst));
        QVERIFY(recordEqual(*registry.job(thirdId), lostThird));
        QVERIFY(recordEqual(*registry.job(firstId), recoveredFirst));
        QVERIFY(recordEqual(*registry.job(secondId), terminalSecond));
        QVERIFY(recordsEqual(registry.activeJobs(),
                             {lostThird, recoveredFirst}));
    }

    void laterGenerationProgressRecoversOnlyItsExactJob()
    {
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        const Xc2JobEvent a0 = makeEvent(
            4, QStringLiteral("a-0"), QStringLiteral("job-a"),
            Xc2JobState::InProgress, 1, 10);
        const Xc2JobEvent b0 = makeEvent(
            4, QStringLiteral("b-0"), QStringLiteral("job-b"),
            Xc2JobState::InProgress, 1, 10);
        const Xc2JobEvent c0 = makeEvent(
            9, QStringLiteral("c-0"), QStringLiteral("job-c"),
            Xc2JobState::InProgress, 1, 10);
        const Xc2JobEvent a1 = makeEvent(
            5, QStringLiteral("a-1"), QStringLiteral("job-a"),
            Xc2JobState::InProgress, 2, 10);
        const Xc2JobEvent staleTerminal = makeEvent(
            4, QStringLiteral("b-stale-terminal"), QStringLiteral("job-b"),
            Xc2JobState::Error, 0, 0);
        const Xc2JobEvent freshTerminal = makeEvent(
            5, QStringLiteral("b-fresh-terminal"), QStringLiteral("job-b"),
            Xc2JobState::Error, 0, 0);
        QVERIFY(registry.apply(a0));
        QVERIFY(registry.apply(b0));
        registry.markVisibilityLost(4);
        changed.clear();

        QVERIFY(registry.apply(c0));
        const Xc2JobRecord lostA = expectedRecord(
            QStringLiteral("job-a"), Xc2JobState::InProgress, {a0}, true);
        const Xc2JobRecord lostB = expectedRecord(
            QStringLiteral("job-b"), Xc2JobState::InProgress, {b0}, true);
        const Xc2JobRecord visibleC = expectedRecord(
            QStringLiteral("job-c"), Xc2JobState::InProgress, {c0});
        QVERIFY(recordEqual(*registry.job(QStringLiteral("job-a")), lostA));
        QVERIFY(recordEqual(*registry.job(QStringLiteral("job-b")), lostB));
        QVERIFY(recordEqual(*registry.job(QStringLiteral("job-c")), visibleC));
        QVERIFY(recordsEqual(registry.activeJobs(), {lostA, lostB, visibleC}));

        changed.clear();
        QVERIFY(registry.apply(a1));
        const Xc2JobRecord recoveredA = expectedRecord(
            QStringLiteral("job-a"), Xc2JobState::InProgress, {a0, a1});
        QCOMPARE(changed.size(), 1);
        QVERIFY(recordEqual(signalRecord(changed, 0), recoveredA));
        QVERIFY(recordEqual(*registry.job(QStringLiteral("job-a")), recoveredA));
        QVERIFY(recordEqual(*registry.job(QStringLiteral("job-b")), lostB));
        QVERIFY(recordEqual(*registry.job(QStringLiteral("job-c")), visibleC));
        QVERIFY(recordsEqual(registry.activeJobs(),
                             {recoveredA, lostB, visibleC}));

        changed.clear();
        const QStringList keys{QStringLiteral("job-a"),
                               QStringLiteral("job-b"),
                               QStringLiteral("job-c")};
        const RegistrySnapshot beforeStaleTerminal = takeSnapshot(registry, keys);
        Xc2Error error = dirtyError();
        QVERIFY(!registry.apply(staleTerminal, &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Job);
        QCOMPARE(error.jobId, QStringLiteral("job-b"));
        QVERIFY(snapshotEqual(beforeStaleTerminal, takeSnapshot(registry, keys)));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);

        QVERIFY(registry.apply(freshTerminal));
        const Xc2JobRecord terminalB = expectedRecord(
            QStringLiteral("job-b"), Xc2JobState::Error,
            {b0, freshTerminal});
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 1);
        QCOMPARE(removed.size(), 0);
        QVERIFY(recordEqual(signalRecord(changed, 0), terminalB));
        QVERIFY(recordEqual(signalRecord(terminal, 0), terminalB));
        QVERIFY(recordEqual(*registry.job(QStringLiteral("job-b")), terminalB));
        QVERIFY(recordEqual(*registry.job(QStringLiteral("job-a")), recoveredA));
        QVERIFY(recordEqual(*registry.job(QStringLiteral("job-c")), visibleC));
        QVERIFY(recordsEqual(registry.activeJobs(), {recoveredA, visibleC}));
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

        const Xc2JobEvent freshRecovery = makeEvent(
            6, QStringLiteral("fresh"), jobId,
            Xc2JobState::InProgress, 2, 10);
        QVERIFY(registry.apply(freshRecovery));
        const Xc2JobRecord recoveredRecord = expectedRecord(
            jobId, Xc2JobState::InProgress, {original, freshRecovery});
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVERIFY(recordEqual(signalRecord(changed, 0), recoveredRecord));
        QVERIFY(recordEqual(*registry.job(jobId), recoveredRecord));
        QVERIFY(recordsEqual(registry.activeJobs(), {recoveredRecord}));

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

        const Xc2JobEvent freshAfterRejectedStale = makeEvent(
            7, QStringLiteral("fresh-after-rejected-stale"), jobId,
            Xc2JobState::InProgress, 3, 12);
        QVERIFY(registry.apply(freshAfterRejectedStale));
        const Xc2JobRecord finalRecord = expectedRecord(
            jobId, Xc2JobState::InProgress,
            {original, freshRecovery, freshAfterRejectedStale});
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVERIFY(recordEqual(signalRecord(changed, 0), finalRecord));
        QVERIFY(recordEqual(*registry.job(jobId), finalRecord));
        QVERIFY(recordsEqual(registry.activeJobs(), {finalRecord}));
    }

    void clearRemovesOnlyRequestedTerminalAndEmitsRemoved()
    {
        const QString activeId = QStringLiteral("active");
        const QString terminalBId = QStringLiteral("terminal-b");
        const QString terminalCId = QStringLiteral("terminal-c");
        const Xc2JobEvent terminalBEvent = makeEvent(
            1, QStringLiteral("terminal-b"), terminalBId,
            Xc2JobState::Finished);
        const Xc2JobEvent terminalCEvent = makeEvent(
            1, QStringLiteral("terminal-c"), terminalCId,
            Xc2JobState::Error);
        const Xc2JobRecord activeCreated = expectedRecord(activeId);
        const Xc2JobRecord terminalBRecord = expectedRecord(
            terminalBId, Xc2JobState::Finished, {terminalBEvent});
        const Xc2JobRecord terminalCRecord = expectedRecord(
            terminalCId, Xc2JobState::Error, {terminalCEvent});
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        QVERIFY(registry.accept({activeId}));
        QVERIFY(registry.apply(terminalBEvent));
        QVERIFY(registry.apply(terminalCEvent));
        QVERIFY(recordEqual(*registry.job(activeId), activeCreated));
        QVERIFY(recordEqual(*registry.job(terminalBId), terminalBRecord));
        QVERIFY(recordEqual(*registry.job(terminalCId), terminalCRecord));
        QVERIFY(recordsEqual(registry.activeJobs(), {activeCreated}));
        changed.clear();
        terminal.clear();
        removed.clear();

        const QString blankId = QStringLiteral(" \t");
        const RegistrySnapshot beforeBlankClear = takeSnapshot(
            registry, {activeId, terminalBId, terminalCId, blankId});
        Xc2Error error = dirtyError();
        QVERIFY(!registry.clearTerminal(blankId, &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Contract);
        QCOMPARE(error.jobId, blankId);
        QVERIFY(snapshotEqual(beforeBlankClear, takeSnapshot(
            registry, {activeId, terminalBId, terminalCId, blankId})));
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
        QVERIFY(registry.clearTerminal(terminalBId, &error));
        QVERIFY(errorCleared(error));
        QCOMPARE(removed.size(), 1);
        QCOMPARE(removed.at(0).at(0).toString(), terminalBId);
        QVERIFY(removedSawDeletedRecord);
        QVERIFY(removedSawCommittedTombstone);
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QVERIFY(!registry.job(terminalBId).has_value());
        QVERIFY(recordEqual(*registry.job(terminalCId), terminalCRecord));
        QVERIFY(recordEqual(*registry.job(activeId), activeCreated));
        QVERIFY(recordsEqual(registry.activeJobs(), {activeCreated}));

        const QStringList keys{activeId,
                               terminalBId,
                               terminalCId,
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
        QVERIFY(registry.accept({activeId}, &error));
        QVERIFY(errorCleared(error));
        QCOMPARE(changed.size(), 1);
        const Xc2JobEvent activeProgressEvent = makeEvent(
            2, QStringLiteral("active-progress"), activeId,
            Xc2JobState::InProgress, 1, 1);
        QVERIFY(registry.apply(activeProgressEvent));
        const Xc2JobRecord unknownCreated = expectedRecord(
            QStringLiteral("unknown"));
        const Xc2JobRecord activeProgress = expectedRecord(
            activeId, Xc2JobState::InProgress, {activeProgressEvent});
        QCOMPARE(changed.size(), 2);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVERIFY(recordEqual(signalRecord(changed, 0), unknownCreated));
        QVERIFY(recordEqual(signalRecord(changed, 1), activeProgress));
        QVERIFY(recordEqual(*registry.job(activeId), activeProgress));
        QVERIFY(recordEqual(*registry.job(terminalCId), terminalCRecord));
        QVERIFY(recordEqual(*registry.job(QStringLiteral("unknown")),
                            unknownCreated));
        QVERIFY(recordsEqual(registry.activeJobs(),
                             {activeProgress, unknownCreated}));
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
        for (int attempt = 0; attempt < 2; ++attempt) {
            error = dirtyError();
            QVERIFY(!registry.apply(late, &error));
            QCOMPARE(error.category, Xc2ErrorCategory::Job);
            QCOMPARE(error.jobId, jobId);
            QVERIFY(snapshotEqual(retired, takeSnapshot(registry, {jobId})));
            QCOMPARE(changed.size(), 0);
            QCOMPARE(terminal.size(), 0);
            QCOMPARE(removed.size(), 0);
        }

        error = dirtyError();
        QVERIFY(!registry.accept({jobId}, &error));
        QCOMPARE(error.category, Xc2ErrorCategory::Job);
        QCOMPARE(error.jobId, jobId);
        QVERIFY(snapshotEqual(retired, takeSnapshot(registry, {jobId})));
        QCOMPARE(changed.size(), 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
    }

    void terminalSignalSelfDeleteStopsSafely()
    {
        auto *registry = new Xc2JobRegistry;
        QSignalSpy changed(registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(registry, &Xc2JobRegistry::jobRemoved);
        const Xc2JobEvent event = makeEvent(
            2, QStringLiteral("delete-terminal"),
            QStringLiteral("delete-terminal"), Xc2JobState::Finished);
        const Xc2JobRecord expected = expectedRecord(
            QStringLiteral("delete-terminal"), Xc2JobState::Finished,
            {event});
        connect(registry, &Xc2JobRegistry::jobChanged, this,
                [&registry](const Xc2JobRecord &) {
            delete registry;
            registry = nullptr;
        }, Qt::DirectConnection);

        QVERIFY(registry->apply(event));
        QVERIFY(!registry);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVERIFY(recordEqual(signalRecord(changed, 0), expected));
    }

    void visibilitySignalSelfDeleteStopsSafely()
    {
        auto *registry = new Xc2JobRegistry;
        QVERIFY(registry->accept({QStringLiteral("delete-a")}));
        QVERIFY(registry->accept({QStringLiteral("delete-b")}));
        QSignalSpy changed(registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(registry, &Xc2JobRegistry::jobRemoved);
        const Xc2JobRecord expected = expectedRecord(
            QStringLiteral("delete-a"), Xc2JobState::Created, {}, true);
        connect(registry, &Xc2JobRegistry::jobChanged, this,
                [&registry](const Xc2JobRecord &) {
            delete registry;
            registry = nullptr;
        }, Qt::DirectConnection);

        registry->markVisibilityLost(4);
        QVERIFY(!registry);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVERIFY(recordEqual(signalRecord(changed, 0), expected));
    }

    void terminalSignalLifetimeReplacementStopsSafely()
    {
        alignas(Xc2JobRegistry)
            unsigned char storage[sizeof(Xc2JobRegistry)];
        auto *registry = ::new (static_cast<void *>(storage)) Xc2JobRegistry;
        Xc2JobRegistry *replacement = nullptr;
        int replacementTerminalSignals = 0;
        connect(registry, &Xc2JobRegistry::jobChanged, this,
                [this, &registry, &replacement, &replacementTerminalSignals,
                 &storage](const Xc2JobRecord &) {
            registry->~Xc2JobRegistry();
            registry = nullptr;
            replacement = ::new (static_cast<void *>(storage))
                Xc2JobRegistry;
            connect(replacement, &Xc2JobRegistry::jobTerminal, this,
                    [&replacementTerminalSignals](const Xc2JobRecord &) {
                ++replacementTerminalSignals;
            }, Qt::DirectConnection);
        }, Qt::DirectConnection);
        const Xc2JobEvent terminalEvent = makeEvent(
            2, QStringLiteral("replace-terminal"),
            QStringLiteral("replace-terminal"), Xc2JobState::Finished);

        const bool applied = registry->apply(terminalEvent);
        const bool originalLifetimeEnded = registry == nullptr;
        const bool replacementCreated = replacement != nullptr;
        Xc2JobRegistry *liveObject = replacement ? replacement : registry;
        if (liveObject)
            liveObject->~Xc2JobRegistry();

        QVERIFY(applied);
        QVERIFY(originalLifetimeEnded);
        QVERIFY(replacementCreated);
        QCOMPARE(replacementTerminalSignals, 0);
    }

    void visibilitySignalLifetimeReplacementStopsSafely()
    {
        alignas(Xc2JobRegistry)
            unsigned char storage[sizeof(Xc2JobRegistry)];
        auto *registry = ::new (static_cast<void *>(storage)) Xc2JobRegistry;
        QVERIFY(registry->accept({QStringLiteral("replace-a")}));
        QVERIFY(registry->accept({QStringLiteral("replace-b")}));
        Xc2JobRegistry *replacement = nullptr;
        int replacementChangedSignals = 0;
        connect(registry, &Xc2JobRegistry::jobChanged, this,
                [this, &registry, &replacement, &replacementChangedSignals,
                 &storage](const Xc2JobRecord &record) {
            if (!record.visibilityLost || replacement)
                return;
            registry->~Xc2JobRegistry();
            registry = nullptr;
            replacement = ::new (static_cast<void *>(storage))
                Xc2JobRegistry;
            connect(replacement, &Xc2JobRegistry::jobChanged, this,
                    [&replacementChangedSignals](const Xc2JobRecord &) {
                ++replacementChangedSignals;
            }, Qt::DirectConnection);
        }, Qt::DirectConnection);

        registry->markVisibilityLost(4);
        const bool originalLifetimeEnded = registry == nullptr;
        const bool replacementCreated = replacement != nullptr;
        Xc2JobRegistry *liveObject = replacement ? replacement : registry;
        if (liveObject)
            liveObject->~Xc2JobRegistry();

        QVERIFY(originalLifetimeEnded);
        QVERIFY(replacementCreated);
        QCOMPARE(replacementChangedSignals, 0);
    }

    void acceptReentrantNotificationsRemainFifo()
    {
        const QString jobId = QStringLiteral("fifo-accept");
        const Xc2JobEvent progress = makeEvent(
            2, QStringLiteral("fifo-progress"), jobId,
            Xc2JobState::InProgress, 1, 10);
        const Xc2JobRecord created = expectedRecord(jobId);
        const Xc2JobRecord inProgress = expectedRecord(
            jobId, Xc2JobState::InProgress, {progress});
        Xc2JobRegistry registry;
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        bool nestedApplied = false;
        bool creationWasCommitted = false;
        bool deliveryWasCommitted = false;
        connect(&registry, &Xc2JobRegistry::jobChanged, this,
                [&registry, &progress, &created, &nestedApplied,
                 &creationWasCommitted,
                 &deliveryWasCommitted](const Xc2JobRecord &record) {
            if (record.state == Xc2JobState::Created && !nestedApplied) {
                const auto retained = registry.job(record.jobId);
                creationWasCommitted = retained
                    && recordEqual(*retained, created)
                    && recordsEqual(registry.activeJobs(), {created});
                Xc2Error error = dirtyError();
                nestedApplied = registry.apply(progress, &error)
                    && errorCleared(error);
            } else if (record.state == Xc2JobState::InProgress
                       && !deliveryWasCommitted) {
                Xc2Error error = dirtyError();
                deliveryWasCommitted = registry.apply(progress, &error)
                    && errorCleared(error);
            }
        }, Qt::DirectConnection);
        QList<ObservedNotification> notifications;
        observeNotifications(registry, *this, notifications);

        Xc2Error error = dirtyError();
        QVERIFY(registry.accept({jobId}, &error));
        QVERIFY(errorCleared(error));
        QVERIFY(nestedApplied);
        QVERIFY(creationWasCommitted);
        QVERIFY(deliveryWasCommitted);
        QCOMPARE(notifications.size(), 2);
        QVERIFY(notificationEquals(notifications.at(0),
                                   ObservedKind::Changed, created));
        QVERIFY(notificationEquals(notifications.at(1),
                                   ObservedKind::Changed, inProgress));
        QCOMPARE(changed.size(), 2);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVERIFY(recordEqual(*registry.job(jobId), inProgress));
        QVERIFY(recordsEqual(registry.activeJobs(), {inProgress}));
    }

    void terminalClearReentrantNotificationsRemainFifo()
    {
        const QString jobId = QStringLiteral("fifo-terminal");
        const Xc2JobEvent progress = makeEvent(
            2, QStringLiteral("fifo-progress"), jobId,
            Xc2JobState::InProgress, 4, 10);
        const Xc2JobEvent finished = makeEvent(
            3, QStringLiteral("fifo-finished"), jobId,
            Xc2JobState::Finished, 1, 1);
        const Xc2JobRecord terminalRecord = expectedRecord(
            jobId, Xc2JobState::Finished, {progress, finished});
        Xc2JobRegistry registry;
        QVERIFY(registry.accept({jobId}));
        QVERIFY(registry.apply(progress));
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        bool cleared = false;
        connect(&registry, &Xc2JobRegistry::jobChanged, this,
                [&registry, &cleared](const Xc2JobRecord &record) {
            if (!record.terminal() || cleared)
                return;
            Xc2Error error = dirtyError();
            cleared = registry.clearTerminal(record.jobId, &error)
                && errorCleared(error);
        }, Qt::DirectConnection);
        QList<ObservedNotification> notifications;
        observeNotifications(registry, *this, notifications);

        QVERIFY(registry.apply(finished));
        QVERIFY(cleared);
        QCOMPARE(notifications.size(), 3);
        QVERIFY(notificationEquals(notifications.at(0),
                                   ObservedKind::Changed, terminalRecord));
        QVERIFY(notificationEquals(notifications.at(1),
                                   ObservedKind::Terminal, terminalRecord));
        QVERIFY(removedNotificationEquals(notifications.at(2), jobId));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(terminal.size(), 1);
        QCOMPARE(removed.size(), 1);
        QVERIFY(!registry.job(jobId).has_value());
        QVERIFY(registry.activeJobs().isEmpty());
    }

    void visibilityReentrantNotificationsRemainFifo()
    {
        const QString jobA = QStringLiteral("fifo-a");
        const QString jobB = QStringLiteral("fifo-b");
        const Xc2JobEvent a0 = makeEvent(
            4, QStringLiteral("a-0"), jobA,
            Xc2JobState::InProgress, 1, 10);
        const Xc2JobEvent b0 = makeEvent(
            4, QStringLiteral("b-0"), jobB,
            Xc2JobState::InProgress, 1, 10);
        const Xc2JobEvent recoverA = makeEvent(
            6, QStringLiteral("a-1"), jobA,
            Xc2JobState::InProgress, 2, 10);
        const Xc2JobEvent finishB = makeEvent(
            6, QStringLiteral("b-terminal"), jobB,
            Xc2JobState::Finished, 0, 0);
        const Xc2JobRecord lostA = expectedRecord(
            jobA, Xc2JobState::InProgress, {a0}, true);
        const Xc2JobRecord lostB = expectedRecord(
            jobB, Xc2JobState::InProgress, {b0}, true);
        const Xc2JobRecord recoveredA = expectedRecord(
            jobA, Xc2JobState::InProgress, {a0, recoverA});
        const Xc2JobRecord terminalB = expectedRecord(
            jobB, Xc2JobState::Finished, {b0, finishB});
        Xc2JobRegistry registry;
        QVERIFY(registry.apply(a0));
        QVERIFY(registry.apply(b0));
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
        bool nestedMutationRan = false;
        bool watermarkWasCommitted = false;
        connect(&registry, &Xc2JobRegistry::jobChanged, this,
                [&registry, &nestedMutationRan, &watermarkWasCommitted,
                 &recoverA, &finishB, &jobB](const Xc2JobRecord &record) {
            if (!record.visibilityLost || nestedMutationRan)
                return;
            const Xc2JobEvent stale = makeEvent(
                5, QStringLiteral("slot-stale"), record.jobId,
                Xc2JobState::InProgress, 2, 10);
            Xc2Error staleError = dirtyError();
            watermarkWasCommitted = !registry.apply(stale, &staleError)
                && staleError.category == Xc2ErrorCategory::Job
                && staleError.jobId == record.jobId;
            nestedMutationRan = registry.apply(recoverA)
                && registry.apply(finishB)
                && registry.clearTerminal(jobB);
        }, Qt::DirectConnection);
        QList<ObservedNotification> notifications;
        observeNotifications(registry, *this, notifications);

        registry.markVisibilityLost(5);
        QVERIFY(nestedMutationRan);
        QVERIFY(watermarkWasCommitted);
        QCOMPARE(notifications.size(), 6);
        QVERIFY(notificationEquals(notifications.at(0),
                                   ObservedKind::Changed, lostA));
        QVERIFY(notificationEquals(notifications.at(1),
                                   ObservedKind::Changed, lostB));
        QVERIFY(notificationEquals(notifications.at(2),
                                   ObservedKind::Changed, recoveredA));
        QVERIFY(notificationEquals(notifications.at(3),
                                   ObservedKind::Changed, terminalB));
        QVERIFY(notificationEquals(notifications.at(4),
                                   ObservedKind::Terminal, terminalB));
        QVERIFY(removedNotificationEquals(notifications.at(5), jobB));
        QCOMPARE(changed.size(), 4);
        QCOMPARE(terminal.size(), 1);
        QCOMPARE(removed.size(), 1);
        QVERIFY(recordEqual(*registry.job(jobA), recoveredA));
        QVERIFY(!registry.job(jobB).has_value());
        QVERIFY(recordsEqual(registry.activeJobs(), {recoveredA}));
    }

    void staleVisibilityLossReplayIsFiltered_data()
    {
        QTest::addColumn<qulonglong>("lossGeneration");
        QTest::addColumn<bool>("expectLoss");
        QTest::newRow("lower") << qulonglong(4) << false;
        QTest::newRow("same") << qulonglong(5) << false;
        QTest::newRow("higher") << qulonglong(6) << true;
    }

    void staleVisibilityLossReplayIsFiltered()
    {
        QFETCH(qulonglong, lossGeneration);
        QFETCH(bool, expectLoss);
        const QString jobId = QStringLiteral("loss-watermark");
        const Xc2JobEvent initial = makeEvent(
            4, QStringLiteral("initial"), jobId,
            Xc2JobState::InProgress, 1, 10);
        const Xc2JobEvent recovered = makeEvent(
            6, QStringLiteral("recovered"), jobId,
            Xc2JobState::InProgress, 2, 10);
        const Xc2JobEvent fresh = makeEvent(
            7, QStringLiteral("fresh"), jobId,
            Xc2JobState::InProgress, 3, 12);
        Xc2JobRegistry registry;
        QVERIFY(registry.apply(initial));
        registry.markVisibilityLost(5);
        QVERIFY(registry.apply(recovered));
        const Xc2JobRecord visible = expectedRecord(
            jobId, Xc2JobState::InProgress, {initial, recovered});
        QVERIFY(recordEqual(*registry.job(jobId), visible));
        QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
        QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
        QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);

        registry.markVisibilityLost(Xc2StompGeneration(lossGeneration));
        const Xc2JobRecord afterLoss = expectedRecord(
            jobId, Xc2JobState::InProgress, {initial, recovered}, expectLoss);
        QVERIFY(recordEqual(*registry.job(jobId), afterLoss));
        QCOMPARE(changed.size(), expectLoss ? 1 : 0);
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        if (expectLoss)
            QVERIFY(recordEqual(signalRecord(changed, 0), afterLoss));

        changed.clear();
        QVERIFY(registry.apply(fresh));
        const Xc2JobRecord afterFresh = expectedRecord(
            jobId, Xc2JobState::InProgress, {initial, recovered, fresh});
        QVERIFY(recordEqual(*registry.job(jobId), afterFresh));
        QCOMPARE(changed.size(), 1);
        QVERIFY(recordEqual(signalRecord(changed, 0), afterFresh));
        QCOMPARE(terminal.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVERIFY(recordsEqual(registry.activeJobs(), {afterFresh}));
    }

    void clearTerminalIsAliasSafe()
    {
        {
            const QString jobId = QStringLiteral("alias-success");
            const Xc2JobEvent finished = makeEvent(
                2, QStringLiteral("alias-finished"), jobId,
                Xc2JobState::Finished);
            Xc2JobRegistry registry;
            QVERIFY(registry.apply(finished));
            QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
            QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
            QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
            Xc2Error error = dirtyError();
            error.jobId = jobId;
            QVERIFY(registry.clearTerminal(error.jobId, &error));
            QVERIFY(errorCleared(error));
            QCOMPARE(changed.size(), 0);
            QCOMPARE(terminal.size(), 0);
            QCOMPARE(removed.size(), 1);
            QCOMPARE(removed.at(0).at(0).toString(), jobId);
        }
        {
            const QString jobId = QStringLiteral("alias-payload");
            Xc2JobRegistry registry;
            QVERIFY(registry.apply(makeEvent(
                2, QStringLiteral("alias-payload-finished"), jobId,
                Xc2JobState::Finished)));
            Xc2Error error = dirtyError();
            error.jobId = jobId;
            QString laterReceiverId;
            connect(&registry, &Xc2JobRegistry::jobRemoved, this,
                    [&error](const QString &) {
                error.jobId = QStringLiteral("mutated-by-first-receiver");
            }, Qt::DirectConnection);
            connect(&registry, &Xc2JobRegistry::jobRemoved, this,
                    [&laterReceiverId](const QString &signalJobId) {
                laterReceiverId = signalJobId;
            }, Qt::DirectConnection);
            QVERIFY(registry.clearTerminal(error.jobId, &error));
            QCOMPARE(laterReceiverId, jobId);
        }
        {
            Xc2JobRegistry registry;
            QSignalSpy changed(&registry, &Xc2JobRegistry::jobChanged);
            QSignalSpy terminal(&registry, &Xc2JobRegistry::jobTerminal);
            QSignalSpy removed(&registry, &Xc2JobRegistry::jobRemoved);
            Xc2Error error = dirtyError();
            error.jobId = QStringLiteral("alias-missing");
            QVERIFY(!registry.clearTerminal(error.jobId, &error));
            QCOMPARE(error.category, Xc2ErrorCategory::Job);
            QCOMPARE(error.jobId, QStringLiteral("alias-missing"));
            QCOMPARE(changed.size(), 0);
            QCOMPARE(terminal.size(), 0);
            QCOMPARE(removed.size(), 0);
        }
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
