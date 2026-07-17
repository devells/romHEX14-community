#pragma once

#include "Xc2StompClient.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <optional>

namespace ktm::xc2 {

struct Xc2JobEvent {
    Xc2StompGeneration generation = 0;
    QString messageId;
    Xc2JobProgress progress;
};

struct Xc2JobRecord {
    QString jobId;
    Xc2JobState state = Xc2JobState::Created;
    QList<Xc2JobEvent> events;
    bool visibilityLost = false;

    bool terminal() const;
};

class Xc2JobRegistry final : public QObject {
    Q_OBJECT

public:
    explicit Xc2JobRegistry(QObject *parent = nullptr);

    bool accept(const Xc2JobAccepted &job, Xc2Error *error = nullptr);
    bool apply(const Xc2JobEvent &event, Xc2Error *error = nullptr);
    bool applyMessage(const Xc2StompMessage &message,
                      Xc2Error *error = nullptr);
    void markVisibilityLost(Xc2StompGeneration generation);
    std::optional<Xc2JobRecord> job(const QString &jobId) const;
    QList<Xc2JobRecord> activeJobs() const;
    bool clearTerminal(const QString &jobId, Xc2Error *error = nullptr);

signals:
    void jobChanged(const Xc2JobRecord &record);
    void jobTerminal(const Xc2JobRecord &record);
    void jobRemoved(const QString &jobId);

private:
    QHash<QString, Xc2JobRecord> m_jobs;
    QStringList m_firstObservedOrder;
    QSet<QString> m_tombstones;
    QHash<Xc2StompGeneration, QHash<QString, Xc2JobEvent>> m_deliveries;
    QHash<QString, Xc2StompGeneration> m_visibilityLostGeneration;
};

} // namespace ktm::xc2

Q_DECLARE_METATYPE(ktm::xc2::Xc2JobEvent)
Q_DECLARE_METATYPE(ktm::xc2::Xc2JobRecord)
