#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <mutex>
#include <thread>

struct AvahiSimplePoll;
struct AvahiClient;
struct AvahiEntryGroup;

namespace od {

/// Publishes `_opensidecar._tcp` via Avahi so senders can discover this
/// receiver on WiFi (protocol section 2.1). Runs Avahi's simple-poll loop on
/// a dedicated thread; name changes and shutdown are applied between
/// poll iterations so every Avahi call stays on its own thread.
///
/// Absence of the daemon is not fatal: AVAHI_CLIENT_NO_FAIL keeps the client
/// retrying, and senders can still reach this receiver by address.
class AvahiAdvertiser : public QObject {
    Q_OBJECT
public:
    explicit AvahiAdvertiser(QObject* parent = nullptr);
    ~AvahiAdvertiser() override;

    void start(QString serviceName, quint16 port, QString installId);
    void setServiceName(const QString& name);
    void stop();

    [[nodiscard]] QString effectiveName() const;

signals:
    /// Emitted when the published name settles (initial publish or after a
    /// collision rename) and when the advertisement is withdrawn.
    void advertisedChanged(bool advertised, QString name);
    void errorOccurred(QString message);

private:
    void run();
    void applyPending();      // poll thread: process name changes / stop
    void publish();           // poll thread: (re)create the entry group
    void unpublish();         // poll thread

    static void clientCallback(AvahiClient* client, int state, void* userdata);
    static void entryGroupCallback(AvahiEntryGroup* group, int state, void* userdata);

    quint16 port_ = 0;
    std::atomic_bool stopRequested_ = false;
    std::atomic_bool dirty_ = false;

    mutable std::mutex mutex_;
    QString requestedName_;   // written by any thread
    QString installId_;
    QString publishedName_;   // poll thread only

    std::thread thread_;
    AvahiSimplePoll* poll_ = nullptr;    // poll thread only
    AvahiClient* client_ = nullptr;
    AvahiEntryGroup* group_ = nullptr;
};

}  // namespace od
