#include "opendisplay/receiver_session.hpp"

#include "opendisplay/advertise.hpp"
#include "opendisplay/annexb.hpp"
#include "opendisplay/log.hpp"
#include "opendisplay/receiver_messages.hpp"

#include <QHostAddress>
#include <QHostInfo>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace od {
namespace {

/// Longest gap between frames that still counts as "streaming" for the
/// stall counter (protocol-mandated ping cadence is 2 s, so a static screen
/// must not read as stalled).
constexpr double stallThresholdMs = 50.0;

double nowMs() { return static_cast<double>(wallClockMs()); }

double percentile(std::deque<double> values, const double p) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        std::clamp(p * static_cast<double>(values.size() - 1), 0.0,
                   static_cast<double>(values.size() - 1)));
    return values[index];
}

}  // namespace

ReceiverSession::ReceiverSession(ReceiverOptions options, QObject* parent)
    : QObject(parent), options_(std::move(options)) {
    installId_ = QSettings(QStringLiteral("opendisplay"),
                           QStringLiteral("opendisplay-receiver"))
                     .value(QStringLiteral("installId"))
                     .toString();
    if (installId_.isEmpty()) {
        installId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QSettings(QStringLiteral("opendisplay"),
                  QStringLiteral("opendisplay-receiver"))
            .setValue(QStringLiteral("installId"), installId_);
    }
    statsWindowStart_ = std::chrono::steady_clock::now();
}

ReceiverSession::~ReceiverSession() {
    decoder_.stop();
    if (advertiser_ != nullptr) {
        advertiser_->stop();
    }
}

void ReceiverSession::start() {
    server_ = new QTcpServer(this);
    if (!server_->listen(QHostAddress::Any, options_.port)) {
        emit statusChanged(QStringLiteral("Cannot listen on :%1 — %2")
                               .arg(options_.port)
                               .arg(server_->errorString()));
        return;
    }
    connect(server_, &QTcpServer::newConnection, this,
            &ReceiverSession::onNewConnection);
    emit statusChanged(QStringLiteral("Listening on :%1").arg(options_.port));

    // Sender-side health and our own liveness both ride 2 s timers.
    pingTimer_ = new QTimer(this);
    connect(pingTimer_, &QTimer::timeout, this, [this] {
        if (connection_ != nullptr) {
            sendControl(msg::ping(nowMs()));
        }
    });
    pingTimer_->start(2000);

    watchdogTimer_ = new QTimer(this);
    connect(watchdogTimer_, &QTimer::timeout, this, [this] {
        if (connection_ == nullptr) {
            return;
        }
        const auto silent = std::chrono::steady_clock::now() - lastDataReceived_;
        if (silent > std::chrono::seconds(5)) {
            dropSession(QStringLiteral("Connection timed out — listening"));
        }
    });
    watchdogTimer_->start(1000);

    statsTimer_ = new QTimer(this);
    connect(statsTimer_, &QTimer::timeout, this, &ReceiverSession::rebuildStats);
    statsTimer_->start(5000);

    if (options_.cursorChannel) {
        cursorChannel_ = new CursorChannel(this);
        connect(cursorChannel_, &CursorChannel::cursorReceived, this,
                [this](double x, double y, bool visible, quint64 seq) {
                    applyCursor(x, y, visible, seq);
                });
        connect(cursorChannel_, &CursorChannel::flowProven, this, [this] {
            // A new UDP flow restarts the shared sequence and proves itself
            // with the datagram that just arrived — ack it over TCP.
            lastCursorSeq_ = 0;
            sendControl(msg::cursorAck());
        });
        if (!cursorChannel_->start(options_.port + 1)) {
            debug("cursor channel unavailable; positions stay on TCP");
        }
    }

    advertiser_ = new AvahiAdvertiser(this);
    connect(advertiser_, &AvahiAdvertiser::advertisedChanged, this,
            &ReceiverSession::advertisedChanged);
    connect(advertiser_, &AvahiAdvertiser::errorOccurred, this,
            &ReceiverSession::advertiseFailed);
    QString name = QString::fromStdString(options_.serviceName);
    if (name.isEmpty()) {
        name = QHostInfo::localHostName() + QStringLiteral(" (OpenDisplay)");
    }
    advertiser_->start(name, options_.port, installId_);

    // Queued: the decoder's threads emit these, the slots run here where
    // QTcpSocket and the stats state are safe to touch.
    connect(this, &ReceiverSession::frameReady, this,
            [this](const QImage&, qint64 capturedAtMs) {
                noteFrameStats(capturedAtMs);
            },
            Qt::QueuedConnection);
    connect(this, &ReceiverSession::decoderError, this,
            &ReceiverSession::handleDecoderError, Qt::QueuedConnection);

    startDecoder();
}

void ReceiverSession::startDecoder() {
    decoder_.start(DecoderConfig{.kind = options_.decoder,
                                 .vaapiDevice = options_.vaapiDevice},
                   panel_,
                   [this](DecodedFrame frame) {
                       const auto image = QImage(
                           reinterpret_cast<const uchar*>(frame.rgba.data()),
                           frame.width, frame.height, frame.width * 4,
                           QImage::Format_RGBA8888)
                                              .copy();
                       if (frame.width != decodedSize_.width
                           || frame.height != decodedSize_.height) {
                           decodedSize_ = Size{frame.width, frame.height};
                           debug("decoded video size "
                                 + std::to_string(frame.width) + "x"
                                 + std::to_string(frame.height));
                           emit videoSizeChanged(frame.width, frame.height);
                       }
                       emit frameReady(image, frame.capturedAtMs);
                   },
                   [this](const std::string& error) {
                       emit decoderError(QString::fromStdString(error));
                   });
}

void ReceiverSession::noteFrameStats(const qint64 capturedAtMs) {
    if (capturedAtMs <= 0) {
        return;
    }
    const auto offset = clockSync_.offset();
    if (!offset.has_value()) {
        return;
    }
    const double e2e = nowMs() + *offset - static_cast<double>(capturedAtMs);
    if (e2e > -50 && e2e < 5000) {
        e2eWindow_.push_back(std::max(e2e, 0.0));
        if (e2eWindow_.size() > 120) {
            e2eWindow_.pop_front();
        }
    }
}

void ReceiverSession::handleDecoderError(const QString error) {
    emit connectionUnstable();
    debug("decoder resync: " + error.toStdString());
    if (connection_ != nullptr) {
        sendControl(msg::keyframeRequest());
    }
}

void ReceiverSession::shutdown() { closeAnnouncing(msg::closing()); }

void ReceiverSession::suspend() { closeAnnouncing(msg::sleeping()); }

void ReceiverSession::setPanel(const Size pixels, const double scale) {
    if (pixels.width <= 0 || pixels.height <= 0) {
        return;
    }
    if (pixels.width == panel_.width && pixels.height == panel_.height
        && scale == panelScale_) {
        return;
    }
    panel_ = pixels;
    panelScale_ = scale;
    if (connection_ != nullptr) {
        sendHello(connection_);
    }
}

void ReceiverSession::setServiceName(const QString& name) {
    if (advertiser_ != nullptr) {
        advertiser_->setServiceName(name);
    }
}

void ReceiverSession::sendTouch(const QString& phase, const double x, const double y) {
    if (connection_ == nullptr) {
        return;
    }
    std::optional<double> senderTime;
    if (const auto offset = clockSync_.offset()) {
        senderTime = nowMs() + *offset;
    }
    sendControl(msg::touch(phase, x, y, senderTime));
}

void ReceiverSession::sendScroll(const double dx, const double dy) {
    if (connection_ != nullptr) {
        return;
    }
    sendControl(msg::scroll(dx, dy));
}

void ReceiverSession::requestKeyframe() {
    if (connection_ != nullptr) {
        sendControl(msg::keyframeRequest());
    }
}

void ReceiverSession::onNewConnection() {
    while (server_->hasPendingConnections()) {
        QTcpSocket* socket = server_->nextPendingConnection();
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        log("new connection from " + socket->peerAddress().toString().toStdString());
        if (connection_ != nullptr) {
            // A live session exists: the newcomer greets first, then must
            // stream bytes before it may replace the winner (a Bonjour
            // dial's IPv6/IPv4 twin would otherwise evict it).
            pending_.append(socket);
            sendHello(socket);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                const QByteArray data = socket->readAll();
                if (!pending_.contains(socket)) {
                    socket->deleteLater();
                    return;
                }
                pending_.removeAll(socket);
                if (data.isEmpty()) {
                    socket->deleteLater();
                    return;
                }
                adopt(socket, /*greeted=*/true, data);
            });
            connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                pending_.removeAll(socket);
                socket->deleteLater();
            });
        } else {
            adopt(socket, /*greeted=*/false, {});
        }
    }
}

void ReceiverSession::adopt(QTcpSocket* socket, const bool greeted,
                            const QByteArray& initialData) {
    if (connection_ != nullptr) {
        connection_->disconnect(this);
        connection_->abort();
        connection_->deleteLater();
    }
    for (QTcpSocket* rival : pending_) {
        if (rival != socket) {
            rival->disconnect(this);
            rival->abort();
            rival->deleteLater();
        }
    }
    pending_.clear();
    connection_ = socket;
    deframer_.reset();
    clockSync_.reset();
    lastCursorSeq_ = 0;
    senderPv_ = wire::assumedWhenAbsent;
    lastDataReceived_ = std::chrono::steady_clock::now();
    statsWindowStart_ = std::chrono::steady_clock::now();
    framesThisWindow_ = 0;
    bytesThisWindow_ = 0;
    stallsThisWindow_ = 0;
    cursorThisWindow_ = 0;
    cursorLostThisWindow_ = 0;
    e2eWindow_.clear();
    loggedTypes_.clear();
    lastFrameAt_ = {};
    lastStats_ = {};

    // Fresh session, fresh decoder state: restart so stale expected-frame
    // records cannot misalign the rawvideo stream.
    decoder_.stop();
    startDecoder();

    emit cursorChanged(0.5, 0.5, false);   // hide a previous sender's cursor
    emit connectedChanged(true);
    emit statusChanged(QStringLiteral("Connected to %1")
                           .arg(socket->peerAddress().toString()));
    lastStats_.transport =
        socket->peerAddress().isLoopback() ? "USB" : "WiFi";

    connect(socket, &QTcpSocket::readyRead, this,
            [this, socket] { onReadyRead(socket); });
    connect(socket, &QTcpSocket::disconnected, this,
            [this, socket] { onDisconnected(socket); });
    if (!greeted) {
        sendHello(socket);
    }
    if (!initialData.isEmpty()) {
        bytesThisWindow_ += initialData.size();
        deframer_.append(std::string_view(initialData.constData(),
                                          static_cast<std::size_t>(initialData.size())));
        onReadyRead(socket);
    }
}

void ReceiverSession::onReadyRead(QTcpSocket* socket) {
    if (socket != connection_) {
        return;
    }
    lastDataReceived_ = std::chrono::steady_clock::now();
    const QByteArray data = socket->readAll();
    bytesThisWindow_ += data.size();
    deframer_.append(std::string_view(data.constData(),
                                      static_cast<std::size_t>(data.size())));
    for (;;) {
        bool failed = false;
        const auto payload = deframer_.next(failed);
        if (failed) {
            dropSession(QStringLiteral("Protocol error — listening"));
            return;
        }
        if (!payload.has_value()) {
            return;
        }
        if (wire::isControlFrame(*payload)) {
            if (const auto object = msg::parseControl(*payload)) {
                dispatchControl(*object);
            }
        } else if (auto frame = annexb::parse(*payload)) {
            ++framesThisWindow_;
            const auto now = std::chrono::steady_clock::now();
            if (lastFrameAt_.time_since_epoch().count() != 0) {
                const double gap =
                    std::chrono::duration<double, std::milli>(now - lastFrameAt_).count();
                if (gap > stallThresholdMs) {
                    ++stallsThisWindow_;
                }
            }
            lastFrameAt_ = now;
            decoder_.submit(std::move(*frame));
        }
    }
}

void ReceiverSession::onDisconnected(QTcpSocket* socket) {
    if (socket != connection_) {
        socket->deleteLater();
        return;
    }
    connection_ = nullptr;
    decoder_.stop();
    emit connectedChanged(false);
    emit cursorChanged(0.5, 0.5, false);
    emit statusChanged(QStringLiteral("Disconnected — listening on :%1")
                           .arg(options_.port));
    socket->deleteLater();
}

void ReceiverSession::dispatchControl(const QJsonObject& object) {
    const QString type = object.value(QStringLiteral("type")).toString();
    if (const auto welcome = msg::parseWelcome(object)) {
        senderPv_ = welcome->pv;
        if (welcome->pv < wire::minSupportedPeer) {
            emit peerMessage(QStringLiteral("Sender is outdated"),
                             QStringLiteral("The connected sender speaks protocol %1, "
                                            "which this receiver no longer supports.")
                                 .arg(welcome->pv),
                             {});
        }
        return;
    }
    if (const auto pong = msg::parsePong(object)) {
        const double t2 = nowMs();
        const double rtt = t2 - pong->t;
        lastRttMs_ = rtt;
        clockSync_.addSample(rtt, pong->mt - (pong->t + t2) / 2.0);
        return;
    }
    if (const auto ping = msg::parseSenderPing(object)) {
        // Sender health counters — surfaced through the stats report only.
        return;
    }
    if (const auto cursor = msg::parseCursor(object)) {
        applyCursor(cursor->x, cursor->y, cursor->visible, cursor->seq);
        return;
    }
    if (const auto image = msg::parseCursorImage(object)) {
        emit cursorImageReady(image->png, image->normWidth, image->normHeight,
                              image->anchorX, image->anchorY);
        return;
    }
    if (const auto update = msg::parseUpdateRequired(object)) {
        emit peerMessage(QStringLiteral("Update required"),
                         update->message, update->store);
        return;
    }
    if (loggedTypes_.insert(type.toStdString()).second) {
        debug("ignoring unknown control type: " + type.toStdString());
    }
}

void ReceiverSession::applyCursor(const double x, const double y,
                                  const bool visible,
                                  const std::optional<std::uint64_t> seq) {
    ++cursorThisWindow_;
    if (seq.has_value()) {
        if (*seq <= lastCursorSeq_) {
            ++cursorLostThisWindow_;
            return;
        }
        cursorLostThisWindow_ += static_cast<int>(*seq - lastCursorSeq_ - 1);
        lastCursorSeq_ = *seq;
    }
    emit cursorChanged(x, y, visible);
}

void ReceiverSession::sendHello(QTcpSocket* socket) {
    msg::Hello hello;
    hello.pixelsWide = panel_.width;
    hello.pixelsHigh = panel_.height;
    hello.scale = panelScale_;
    hello.installId = installId_.toStdString();
    if (cursorChannel_ != nullptr && cursorChannel_->listening()) {
        hello.cursorPort = cursorChannel_->port();
    }
    hello.maxEncode = options_.maxEncode;
    sendControlOn(socket, msg::hello(hello));
}

void ReceiverSession::sendControl(const std::string& payload) {
    sendControlOn(connection_, payload);
}

void ReceiverSession::sendControlOn(QTcpSocket* socket, const std::string& payload) {
    if (socket == nullptr) {
        return;
    }
    const auto framed = wire::frame(payload);
    socket->write(framed.data(), static_cast<qint64>(framed.size()));
}

void ReceiverSession::closeAnnouncing(const std::string& payload) {
    if (connection_ != nullptr) {
        sendControl(payload);
        connection_->flush();
        connection_->waitForBytesWritten(300);
        dropSession(QStringLiteral("Stopped — listening on :%1").arg(options_.port));
    }
}

void ReceiverSession::dropSession(const QString& status) {
    if (connection_ != nullptr) {
        connection_->disconnect(this);
        connection_->abort();
        connection_->deleteLater();
        connection_ = nullptr;
    }
    decoder_.stop();
    emit connectedChanged(false);
    emit cursorChanged(0.5, 0.5, false);
    emit statusChanged(status);
}

void ReceiverSession::rebuildStats() {
    const auto now = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration<double>(now - statsWindowStart_).count();
    lastStats_.fps = seconds > 0
        ? static_cast<int>(std::lround(framesThisWindow_ / seconds)) : 0;
    lastStats_.mbps = seconds > 0
        ? static_cast<double>(bytesThisWindow_) * 8.0 / seconds / 1'000'000.0
        : 0.0;
    lastStats_.rttMs = lastRttMs_;
    lastStats_.e2eP50 = percentile(e2eWindow_, 0.50);
    lastStats_.e2eP95 = percentile(e2eWindow_, 0.95);
    lastStats_.stalls = stallsThisWindow_;
    lastStats_.decodeFlushes = decoder_.droppedFrames();
    lastStats_.cursorPerSec = seconds > 0
        ? static_cast<int>(std::lround(cursorThisWindow_ / seconds)) : 0;
    lastStats_.cursorLost = cursorLostThisWindow_;

    emit hudChanged(QStringLiteral("%1 fps  %2 Mb/s  rtt %3 ms  e2e %4/%5 ms")
                        .arg(lastStats_.fps)
                        .arg(lastStats_.mbps, 0, 'f', 1)
                        .arg(lastStats_.rttMs, 0, 'f', 0)
                        .arg(lastStats_.e2eP50, 0, 'f', 0)
                        .arg(lastStats_.e2eP95, 0, 'f', 0));
    if (connection_ != nullptr) {
        sendControl(msg::stats(lastStats_));
    }
    framesThisWindow_ = 0;
    bytesThisWindow_ = 0;
    stallsThisWindow_ = 0;
    cursorThisWindow_ = 0;
    cursorLostThisWindow_ = 0;
    statsWindowStart_ = now;
}

}  // namespace od
