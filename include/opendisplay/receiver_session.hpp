#pragma once

#include "opendisplay/clock_sync.hpp"
#include "opendisplay/cursor_channel.hpp"
#include "opendisplay/ffmpeg_decoder.hpp"
#include "opendisplay/types.hpp"
#include "opendisplay/wire.hpp"

#include <QImage>
#include <QObject>
#include <QString>

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>

class QTcpServer;
class QTcpSocket;
class QTimer;

namespace od {

/// The listening half of OpenDisplay on Linux: accepts one sender at a time
/// on TCP :9000, speaks the receiver side of the wire protocol (hello first,
/// ping every 2 s, >5 s silence is death), feeds video frames to the FFmpeg
/// decoder, and surfaces decoded frames / cursor state / peer signals to the
/// GUI as Qt signals.
///
/// Ported behavior worth knowing (see PROTOCOL.md and Shared/StreamReceiver.swift):
/// a newcomer connection must prove itself with bytes before it may replace a
/// live session — a Bonjour dial can race IPv6/IPv4 and produce twins where
/// adopting both kills the winner.
class ReceiverSession : public QObject {
    Q_OBJECT
public:
    explicit ReceiverSession(ReceiverOptions options, QObject* parent = nullptr);
    ~ReceiverSession() override;

    void start();
    /// Sends `closing` then drops the connection — the session ends for good.
    void shutdown();
    /// Sends `sleeping` then drops the connection — a wake is expected.
    void suspend();

    /// Panel announced in `hello`; re-sends hello on a live connection when
    /// the dimensions change (display hotplug, fullscreen move).
    void setPanel(Size pixels, double scale);
    void setServiceName(const QString& name);

    void sendTouch(const QString& phase, double x, double y);
    void sendScroll(double dx, double dy);
    void requestKeyframe();

    [[nodiscard]] bool connected() const { return connection_ != nullptr; }
    [[nodiscard]] QString installId() const { return installId_; }

signals:
    void statusChanged(QString status);
    void connectedChanged(bool connected);
    /// Emitted on the decoder's reader thread; delivery is queued to this
    /// object's thread. `capturedAtMs` is the sender's capture timestamp, 0
    /// when the telemetry prefix was absent.
    void frameReady(QImage frame, qint64 capturedAtMs);
    void videoSizeChanged(int width, int height);
    void cursorChanged(double x, double y, bool visible);
    void cursorImageReady(QByteArray png, double normWidth, double normHeight,
                          double anchorX, double anchorY);
    void hudChanged(QString text);
    /// `updateRequired`, an outdated sender, or a newcomer replacing us —
    /// the GUI shows it; `url` is pre-sanitized and may be empty.
    void peerMessage(QString title, QString message, QString url);
    void advertisedChanged(bool advertised, QString name);
    void advertiseFailed(QString message);
    void connectionUnstable();
    /// Internal: carries a decoder error string from the decoder thread.
    void decoderError(QString error);

private:
    void onNewConnection();
    void adopt(QTcpSocket* socket, bool greeted, const QByteArray& initialData);
    void onReadyRead(QTcpSocket* socket);
    void onDisconnected(QTcpSocket* socket);
    void dispatchControl(const QJsonObject& object);
    void applyCursor(double x, double y, bool visible, std::optional<std::uint64_t> seq);
    void sendHello(QTcpSocket* socket);
    void sendControl(const std::string& payload);
    void sendControlOn(QTcpSocket* socket, const std::string& payload);
    void closeAnnouncing(const std::string& payload);
    void dropSession(const QString& status);
    void startDecoder();
    void rebuildStats();

private slots:
    /// Queued delivery of decoder output/errors onto this object's thread —
    /// the decoder's own threads must not touch QTcpSocket or the stats.
    void noteFrameStats(qint64 capturedAtMs);
    void handleDecoderError(QString error);

private:
    ReceiverOptions options_;
    QTcpServer* server_ = nullptr;
    QTcpSocket* connection_ = nullptr;                       // adopted session
    QList<QTcpSocket*> pending_;                             // proving themselves
    wire::Deframer deframer_;
    FfmpegDecoder decoder_;
    CursorChannel* cursorChannel_ = nullptr;
    class AvahiAdvertiser* advertiser_ = nullptr;
    QTimer* pingTimer_ = nullptr;
    QTimer* watchdogTimer_ = nullptr;
    QTimer* statsTimer_ = nullptr;

    QString installId_;
    Size panel_;
    double panelScale_ = 1.0;
    Size decodedSize_;           // decoder reader thread only
    int senderPv_ = wire::assumedWhenAbsent;
    std::uint64_t lastCursorSeq_ = 0;
    ClockSync clockSync_;
    std::chrono::steady_clock::time_point lastDataReceived_{};
    std::set<std::string> loggedTypes_;

    // stats window
    int framesThisWindow_ = 0;
    qint64 bytesThisWindow_ = 0;
    int stallsThisWindow_ = 0;
    int cursorThisWindow_ = 0;
    int cursorLostThisWindow_ = 0;
    double lastRttMs_ = 0.0;
    std::chrono::steady_clock::time_point lastFrameAt_{};
    std::chrono::steady_clock::time_point statsWindowStart_{};
    std::deque<double> e2eWindow_;
    StatsSnapshot lastStats_;
};

}  // namespace od
