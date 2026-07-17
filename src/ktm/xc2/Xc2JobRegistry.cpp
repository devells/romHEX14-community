#include "Xc2JobRegistry.h"

#include "Xc2JsonCodec.h"

#include <QPointer>

#include <utility>

namespace ktm::xc2 {
namespace {

void clearError(Xc2Error *error)
{
    if (error)
        *error = {};
}

bool fail(Xc2Error *error,
          Xc2ErrorCategory category,
          QString jobId,
          QString message)
{
    if (error) {
        Xc2Error failure;
        failure.category = category;
        failure.jobId = std::move(jobId);
        failure.message = std::move(message);
        *error = std::move(failure);
    }
    return false;
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

bool isTerminalState(Xc2JobState state)
{
    return state == Xc2JobState::Finished
        || state == Xc2JobState::Canceled || state == Xc2JobState::Error
        || state == Xc2JobState::NotAuthorized;
}

bool validateEventShape(const Xc2JobEvent &event, Xc2Error *error)
{
    const Xc2JobProgress &progress = event.progress;
    if (event.generation == 0) {
        return fail(error, Xc2ErrorCategory::Contract, progress.jobId,
                    QStringLiteral("XC2 job event generation must be non-zero"));
    }
    if (event.messageId.isEmpty()) {
        return fail(error, Xc2ErrorCategory::Contract, progress.jobId,
                    QStringLiteral("XC2 job event message ID must not be empty"));
    }
    if (progress.jobId.trimmed().isEmpty()) {
        return fail(error, Xc2ErrorCategory::Contract, progress.jobId,
                    QStringLiteral("XC2 job ID must not be blank"));
    }
    if (progress.state == Xc2JobState::Created) {
        return fail(error, Xc2ErrorCategory::Contract, progress.jobId,
                    QStringLiteral("Created is not a valid XC2 progress state"));
    }
    if (!isTerminalState(progress.state)
        && progress.state != Xc2JobState::InProgress) {
        return fail(error, Xc2ErrorCategory::Contract, progress.jobId,
                    QStringLiteral("Unknown XC2 job progress state"));
    }
    if (progress.ticks < 0 || progress.totalTicks < 0) {
        return fail(error, Xc2ErrorCategory::Contract, progress.jobId,
                    QStringLiteral("XC2 job progress counters must be non-negative"));
    }
    if (progress.totalTicks > 0 && progress.ticks > progress.totalTicks) {
        return fail(error, Xc2ErrorCategory::Contract, progress.jobId,
                    QStringLiteral("XC2 job progress ticks exceed total ticks"));
    }
    return true;
}

} // namespace

bool Xc2JobRecord::terminal() const
{
    return isTerminalState(state);
}

Xc2JobRegistry::Xc2JobRegistry(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<Xc2JobEvent>();
    qRegisterMetaType<Xc2JobRecord>();
}

bool Xc2JobRegistry::accept(const Xc2JobAccepted &job, Xc2Error *error)
{
    if (job.jobId.trimmed().isEmpty()) {
        return fail(error, Xc2ErrorCategory::Contract, job.jobId,
                    QStringLiteral("XC2 job ID must not be blank"));
    }
    if (m_tombstones.contains(job.jobId)) {
        return fail(error, Xc2ErrorCategory::Job, job.jobId,
                    QStringLiteral("XC2 job ID has been retired"));
    }
    if (m_jobs.contains(job.jobId)) {
        clearError(error);
        return true;
    }

    Xc2JobRecord record;
    record.jobId = job.jobId;
    m_firstObservedOrder.append(job.jobId);
    m_jobs.insert(job.jobId, record);
    clearError(error);
    publish({{PendingNotification::Kind::Changed, record, {}}});
    return true;
}

bool Xc2JobRegistry::apply(const Xc2JobEvent &event, Xc2Error *error)
{
    if (!validateEventShape(event, error))
        return false;

    const Xc2JobProgress &progress = event.progress;
    const auto generationIt = m_deliveries.constFind(event.generation);
    if (generationIt != m_deliveries.cend()) {
        const auto deliveryIt = generationIt->constFind(event.messageId);
        if (deliveryIt != generationIt->cend()) {
            if (!progressEqual(deliveryIt->progress, progress)) {
                return fail(
                    error, Xc2ErrorCategory::Contract, progress.jobId,
                    QStringLiteral("XC2 delivery identity was reused with a different payload"));
            }
            clearError(error);
            return true;
        }
    }

    if (m_tombstones.contains(progress.jobId)) {
        return fail(error, Xc2ErrorCategory::Job, progress.jobId,
                    QStringLiteral("XC2 job ID has been retired"));
    }

    auto recordIt = m_jobs.find(progress.jobId);
    if (recordIt != m_jobs.end()) {
        const Xc2JobRecord &record = recordIt.value();
        if (record.terminal()) {
            return fail(error, Xc2ErrorCategory::Job, progress.jobId,
                        QStringLiteral("XC2 job is already terminal"));
        }
        const auto watermarkIt =
            m_visibilityLostGeneration.constFind(progress.jobId);
        if (watermarkIt != m_visibilityLostGeneration.cend()
            && event.generation <= watermarkIt.value()) {
            return fail(error, Xc2ErrorCategory::Job, progress.jobId,
                        QStringLiteral("XC2 job progress generation is stale"));
        }
        if (record.state == Xc2JobState::InProgress
            && progress.state == Xc2JobState::InProgress) {
            const Xc2JobProgress &previous = record.events.constLast().progress;
            if (progress.ticks < previous.ticks) {
                return fail(error, Xc2ErrorCategory::Job, progress.jobId,
                            QStringLiteral("XC2 job progress ticks regressed"));
            }
            if (previous.totalTicks > 0
                && (progress.totalTicks == 0
                    || progress.totalTicks < previous.totalTicks)) {
                return fail(error, Xc2ErrorCategory::Job, progress.jobId,
                            QStringLiteral("XC2 job progress total ticks regressed"));
            }
        }
    }

    if (recordIt == m_jobs.end()) {
        Xc2JobRecord record;
        record.jobId = progress.jobId;
        record.state = progress.state;
        record.events.append(event);
        m_firstObservedOrder.append(progress.jobId);
        recordIt = m_jobs.insert(progress.jobId, std::move(record));
    } else {
        recordIt->events.append(event);
        recordIt->state = progress.state;
        if (recordIt->visibilityLost)
            recordIt->visibilityLost = false;
    }
    m_deliveries[event.generation].insert(event.messageId, event);

    const Xc2JobRecord committed = recordIt.value();
    clearError(error);
    QList<PendingNotification> notifications{
        {PendingNotification::Kind::Changed, committed, {}}
    };
    if (committed.terminal()) {
        notifications.append(
            {PendingNotification::Kind::Terminal, committed, {}});
    }
    publish(std::move(notifications));
    return true;
}

bool Xc2JobRegistry::applyMessage(const Xc2StompMessage &message,
                                  Xc2Error *error)
{
    if (message.topic != Topic::Progress) {
        return fail(error, Xc2ErrorCategory::Contract, {},
                    QStringLiteral("STOMP message is not a progress topic"));
    }
    if (message.destination
        != Xc2ContractProfile::approved().topic(Topic::Progress)) {
        return fail(error, Xc2ErrorCategory::Contract, {},
                    QStringLiteral("STOMP progress destination is not approved"));
    }
    if (message.messageId.isEmpty()) {
        return fail(error, Xc2ErrorCategory::Contract, {},
                    QStringLiteral("STOMP progress message ID must not be empty"));
    }
    if (message.generation == 0) {
        return fail(error, Xc2ErrorCategory::Contract, {},
                    QStringLiteral("STOMP progress generation must be non-zero"));
    }

    const Xc2Result<Xc2JobProgress> decoded =
        Xc2JsonCodec::jobProgress(message.body);
    if (!decoded.ok()) {
        if (error)
            *error = decoded.error;
        return false;
    }

    return apply({message.generation, message.messageId, *decoded.value}, error);
}

void Xc2JobRegistry::markVisibilityLost(Xc2StompGeneration generation)
{
    if (generation == 0)
        return;

    QList<PendingNotification> notifications;
    for (const QString &jobId : std::as_const(m_firstObservedOrder)) {
        auto recordIt = m_jobs.find(jobId);
        if (recordIt == m_jobs.end() || recordIt->terminal())
            continue;

        const Xc2StompGeneration oldWatermark =
            m_visibilityLostGeneration.value(jobId);
        if (generation <= oldWatermark)
            continue;
        m_visibilityLostGeneration.insert(jobId, generation);
        if (recordIt->visibilityLost)
            continue;

        recordIt->visibilityLost = true;
        notifications.append(
            {PendingNotification::Kind::Changed, recordIt.value(), {}});
    }

    publish(std::move(notifications));
}

std::optional<Xc2JobRecord> Xc2JobRegistry::job(const QString &jobId) const
{
    const auto it = m_jobs.constFind(jobId);
    if (it == m_jobs.cend())
        return std::nullopt;
    return it.value();
}

QList<Xc2JobRecord> Xc2JobRegistry::activeJobs() const
{
    QList<Xc2JobRecord> result;
    result.reserve(m_jobs.size());
    for (const QString &jobId : m_firstObservedOrder) {
        const auto it = m_jobs.constFind(jobId);
        if (it != m_jobs.cend() && !it->terminal())
            result.append(it.value());
    }
    return result;
}

bool Xc2JobRegistry::clearTerminal(const QString &jobId, Xc2Error *error)
{
    const QString stableJobId = jobId;
    if (stableJobId.trimmed().isEmpty()) {
        return fail(error, Xc2ErrorCategory::Contract, stableJobId,
                    QStringLiteral("XC2 job ID must not be blank"));
    }

    const auto it = m_jobs.constFind(stableJobId);
    if (it == m_jobs.cend() || !it->terminal()) {
        return fail(error, Xc2ErrorCategory::Job, stableJobId,
                    QStringLiteral("XC2 job is not a retained terminal job"));
    }

    m_jobs.remove(stableJobId);
    m_firstObservedOrder.removeAll(stableJobId);
    m_visibilityLostGeneration.remove(stableJobId);
    m_tombstones.insert(stableJobId);
    clearError(error);
    publish({{PendingNotification::Kind::Removed, {}, stableJobId}});
    return true;
}

void Xc2JobRegistry::publish(QList<PendingNotification> notifications)
{
    if (notifications.isEmpty())
        return;

    m_pendingNotifications.reserve(m_pendingNotifications.size()
                                   + notifications.size());
    for (PendingNotification &notification : notifications)
        m_pendingNotifications.append(std::move(notification));

    if (m_drainingNotifications)
        return;

    m_drainingNotifications = true;
    QPointer<Xc2JobRegistry> owner(this);
    while (owner && !owner->m_pendingNotifications.isEmpty()) {
        PendingNotification notification =
            owner->m_pendingNotifications.takeFirst();
        Xc2JobRegistry *current = owner.data();
        switch (notification.kind) {
        case PendingNotification::Kind::Changed:
            emit current->jobChanged(notification.record);
            break;
        case PendingNotification::Kind::Terminal:
            emit current->jobTerminal(notification.record);
            break;
        case PendingNotification::Kind::Removed:
            emit current->jobRemoved(notification.jobId);
            break;
        }
        if (!owner)
            return;
    }
    if (owner)
        owner->m_drainingNotifications = false;
}

} // namespace ktm::xc2
