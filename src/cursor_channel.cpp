#include "opendisplay/cursor_channel.hpp"

#include "opendisplay/log.hpp"
#include "opendisplay/receiver_messages.hpp"

#include <QNetworkDatagram>
#include <QUdpSocket>

namespace od {

CursorChannel::CursorChannel(QObject* parent) : QObject(parent) {}

CursorChannel::~CursorChannel() { stop(); }

bool CursorChannel::start(const quint16 port) {
    stop();
    socket_ = new QUdpSocket(this);
    if (!socket_->bind(QHostAddress::Any, port, QUdpSocket::ShareAddress)) {
        debug("cursor channel: cannot bind udp :" + std::to_string(port));
        socket_->deleteLater();
        socket_ = nullptr;
        return false;
    }
    port_ = port;
    flowSeen_ = false;
    connect(socket_, &QUdpSocket::readyRead, this, &CursorChannel::onDatagrams);
    debug("cursor channel listening on udp :" + std::to_string(port));
    return true;
}

void CursorChannel::stop() {
    if (socket_ != nullptr) {
        socket_->close();
        socket_->deleteLater();
        socket_ = nullptr;
    }
    port_ = 0;
    flowSeen_ = false;
}

bool CursorChannel::listening() const { return socket_ != nullptr && port_ != 0; }

void CursorChannel::onDatagrams() {
    while (socket_->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = socket_->receiveDatagram();
        const auto bytes = datagram.data();
        const auto payload = std::string_view(bytes.constData(),
                                              static_cast<std::size_t>(bytes.size()));
        const auto object = msg::parseControl(payload);
        if (!object.has_value()) {
            continue;
        }
        const auto cursor = msg::parseCursor(*object);
        if (!cursor.has_value() || !cursor->seq.has_value()) {
            continue;   // the channel carries only sequenced cursor messages
        }
        // A new sender flow (fresh ephemeral port after a rebuild) becomes the
        // live one; its sequence restarts and the tracker resets with it.
        const bool newFlow = !flowSeen_ || datagram.senderAddress() != flowAddress_
            || datagram.senderPort() != flowPort_;
        if (newFlow) {
            flowAddress_ = datagram.senderAddress();
            flowPort_ = static_cast<quint16>(datagram.senderPort());
            flowSeen_ = true;
            emit flowProven();
        }
        emit cursorReceived(cursor->x, cursor->y, cursor->visible, *cursor->seq);
    }
}

}  // namespace od
