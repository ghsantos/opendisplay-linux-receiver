#include "opendisplay/receiver_messages.hpp"

#include <QJsonDocument>
#include <QUrl>

namespace od::msg {
namespace {

std::string serialize(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

}  // namespace

std::string hello(const Hello& hello) {
    QJsonObject object{
        {"type", "hello"},
        {"pixelsWide", hello.pixelsWide},
        {"pixelsHigh", hello.pixelsHigh},
        {"scale", hello.scale},
        {"device", QString::fromStdString(hello.device)},
        {"id", QString::fromStdString(hello.installId)},
        {"pv", hello.pv},
    };
    if (hello.cursorPort.has_value()) {
        object.insert("cursorPort", *hello.cursorPort);
    }
    if (hello.maxEncode.has_value()) {
        object.insert("maxEncodeWide", hello.maxEncode->width);
        object.insert("maxEncodeHigh", hello.maxEncode->height);
    }
    return serialize(object);
}

std::string ping(const double receiverTimeMs) {
    return serialize(QJsonObject{{"type", "ping"}, {"t", receiverTimeMs}});
}

std::string touch(const QString& phase, const double x, const double y,
                  const std::optional<double> senderTimeMs) {
    QJsonObject object{
        {"type", "touch"},
        {"phase", phase},
        {"x", x},
        {"y", y},
    };
    if (senderTimeMs.has_value()) {
        object.insert("t", *senderTimeMs);
    }
    return serialize(object);
}

std::string scroll(const double dx, const double dy) {
    return serialize(QJsonObject{{"type", "scroll"}, {"dx", dx}, {"dy", dy}});
}

std::string keyframeRequest() { return serialize(QJsonObject{{"type", "kf"}}); }

std::string stats(const StatsSnapshot& snapshot) {
    return serialize(QJsonObject{
        {"type", "stats"},
        {"transport", QString::fromStdString(snapshot.transport)},
        {"fps", snapshot.fps},
        {"mbps", snapshot.mbps},
        {"rtt", snapshot.rttMs},
        {"e2e50", snapshot.e2eP50},
        {"e2e95", snapshot.e2eP95},
        {"stalls", snapshot.stalls},
        {"decodeFlushes", snapshot.decodeFlushes},
        {"cursorPerSec", snapshot.cursorPerSec},
        {"cursorLost", snapshot.cursorLost},
    });
}

std::string cursorAck() { return serialize(QJsonObject{{"type", "cursorAck"}}); }

std::string sleeping() { return serialize(QJsonObject{{"type", "sleeping"}}); }

std::string closing() { return serialize(QJsonObject{{"type", "closing"}}); }

std::optional<QJsonObject> parseControl(const std::string_view payload) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(payload.data(), static_cast<qsizetype>(payload.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()
        || !document.object().contains("type")) {
        return std::nullopt;
    }
    return document.object();
}

std::optional<Welcome> parseWelcome(const QJsonObject& object) {
    if (object.value("type").toString() != QLatin1String("welcome")) {
        return std::nullopt;
    }
    return Welcome{
        .pv = object.value("pv").toInt(wire::assumedWhenAbsent),
        .min = object.value("min").toInt(1),
    };
}

std::optional<Pong> parsePong(const QJsonObject& object) {
    if (object.value("type").toString() != QLatin1String("pong")
        || !object.contains("t") || !object.contains("mt")) {
        return std::nullopt;
    }
    return Pong{
        .t = object.value("t").toDouble(),
        .mt = object.value("mt").toDouble(),
    };
}

std::optional<SenderPing> parseSenderPing(const QJsonObject& object) {
    if (object.value("type").toString() != QLatin1String("ping")) {
        return std::nullopt;
    }
    return SenderPing{
        .encDrops = object.value("encDrops").toInt(),
        .netDrops = object.value("netDrops").toInt(),
        .pending = object.value("pending").toInt(),
        .inputP50 = object.value("inp50").toDouble(),
        .inputP95 = object.value("inp95").toDouble(),
        .capFps = object.value("capFps").toDouble(),
    };
}

std::optional<Cursor> parseCursor(const QJsonObject& object) {
    if (object.value("type").toString() != QLatin1String("cursor")) {
        return std::nullopt;
    }
    Cursor cursor{
        .x = object.value("x").toDouble(),
        .y = object.value("y").toDouble(),
        .visible = object.value("v").toInt() == 1,
    };
    if (object.contains("s")) {
        cursor.seq = static_cast<std::uint64_t>(object.value("s").toDouble());
    }
    return cursor;
}

std::optional<CursorImage> parseCursorImage(const QJsonObject& object) {
    if (object.value("type").toString() != QLatin1String("cursorImg")
        || !object.contains("png")) {
        return std::nullopt;
    }
    const auto png = QByteArray::fromBase64(
        object.value("png").toString().toLatin1());
    if (png.isEmpty()) {
        return std::nullopt;
    }
    return CursorImage{
        .png = png,
        .normWidth = object.value("nw").toDouble(),
        .normHeight = object.value("nh").toDouble(),
        .anchorX = object.value("ax").toDouble(),
        .anchorY = object.value("ay").toDouble(),
    };
}

std::optional<UpdateRequired> parseUpdateRequired(const QJsonObject& object) {
    if (object.value("type").toString() != QLatin1String("updateRequired")) {
        return std::nullopt;
    }
    return UpdateRequired{
        .target = object.value("target").toString(),
        .store = sanitizedStoreUrl(object.value("store").toString()),
        .message = object.value("message").toString(),
    };
}

QString sanitizedStoreUrl(const QString& raw) {
    if (raw.isEmpty()) {
        return {};
    }
    const QUrl url(raw);
    static const QStringList allowedHosts{
        QStringLiteral("github.com"),
        QStringLiteral("apps.apple.com"),
        QStringLiteral("flathub.org"),
    };
    if (url.scheme() != QLatin1String("https") || !allowedHosts.contains(url.host())) {
        return {};
    }
    return raw;
}

}  // namespace od::msg
