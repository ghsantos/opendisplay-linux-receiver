#pragma once

#include "opendisplay/types.hpp"
#include "opendisplay/wire.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>

namespace od::msg {

// --- Outgoing (receiver -> sender), all returned as unframed JSON payloads.

struct Hello {
    int pixelsWide = 0;
    int pixelsHigh = 0;
    double scale = 1.0;
    std::string device = "Linux";
    std::string installId;
    int pv = wire::protocolVersion;
    std::optional<int> cursorPort;
    std::optional<Size> maxEncode;
};

std::string hello(const Hello& hello);
std::string ping(double receiverTimeMs);
std::string touch(const QString& phase, double x, double y,
                  std::optional<double> senderTimeMs);
std::string scroll(double dx, double dy);
std::string keyframeRequest();
std::string stats(const StatsSnapshot& snapshot);
std::string cursorAck();
std::string sleeping();
std::string closing();

// --- Incoming (sender -> receiver). Each returns nullopt when the object is
// not that message type or lacks the fields the type requires.

struct Welcome {
    int pv = wire::assumedWhenAbsent;
    int min = 1;
};

struct Pong {
    double t = 0;    // echoed receiver timestamp
    double mt = 0;   // sender clock at reply time
};

struct SenderPing {
    int encDrops = 0;
    int netDrops = 0;
    int pending = 0;
    double inputP50 = 0;
    double inputP95 = 0;
    double capFps = 0;
};

struct Cursor {
    double x = 0;
    double y = 0;
    bool visible = false;
    std::optional<std::uint64_t> seq;   // `s`, absent on pre-side-channel senders
};

struct CursorImage {
    QByteArray png;          // decoded image bytes
    double normWidth = 0;    // normalized to display width
    double normHeight = 0;
    double anchorX = 0;      // hotspot, normalized within the sprite
    double anchorY = 0;
};

struct UpdateRequired {
    QString target;
    QString store;           // sanitized; empty when the URL fails validation
    QString message;
};

std::optional<QJsonObject> parseControl(std::string_view payload);
std::optional<Welcome> parseWelcome(const QJsonObject& object);
std::optional<Pong> parsePong(const QJsonObject& object);
std::optional<SenderPing> parseSenderPing(const QJsonObject& object);
std::optional<Cursor> parseCursor(const QJsonObject& object);
std::optional<CursorImage> parseCursorImage(const QJsonObject& object);
std::optional<UpdateRequired> parseUpdateRequired(const QJsonObject& object);

/// The `store` field crosses an unauthenticated socket — accept only https
/// URLs on known hosts so no UI code has to remember to sanitize it
/// (mirrors the Android receiver's sanitizedStoreUrl).
QString sanitizedStoreUrl(const QString& raw);

}  // namespace od::msg
