#pragma once

#include "opendisplay/types.hpp"

#include <QHostAddress>
#include <QObject>

#include <cstdint>

class QUdpSocket;

namespace od {

/// The UDP cursor side channel (protocol section 6.3): one datagram is one
/// `cursor` JSON plus a per-flow sequence `s`. Positions arrive faster than
/// TCP behind multi-hundred-KB video frames; the sender keeps mirroring to
/// TCP until we prove delivery by answering `cursorAck` on the first
/// accepted datagram of a flow.
///
/// Only the transport lives here: sequence dedup and merging with TCP
/// `cursor` frames happen in ReceiverSession, which owns the shared floor.
class CursorChannel : public QObject {
    Q_OBJECT
public:
    explicit CursorChannel(QObject* parent = nullptr);
    ~CursorChannel() override;

    /// Bind the UDP listener. Returns false when the port is unavailable —
    /// the session then simply omits `cursorPort` from hello.
    bool start(quint16 port);
    void stop();
    [[nodiscard]] bool listening() const;
    [[nodiscard]] quint16 port() const { return port_; }

signals:
    /// A cursor message arrived over UDP, sequence intact or not — the
    /// session applies the shared ordering rule.
    void cursorReceived(double x, double y, bool visible, quint64 seq);
    /// First datagram of a new flow: the session should send `cursorAck`.
    void flowProven();
    /// Datagram count that arrived with a non-increasing sequence (dropped
    /// or reordered) since the last window — fed into stats.
    void cursorLost(int count);

private:
    void onDatagrams();

    QUdpSocket* socket_ = nullptr;
    quint16 port_ = 0;
    QHostAddress flowAddress_;
    quint16 flowPort_ = 0;
    bool flowSeen_ = false;
};

}  // namespace od
