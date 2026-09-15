#include "opendisplay/receiver_messages.hpp"

#include <QJsonDocument>
#include <QJsonObject>

#include <cassert>
#include <cstdio>
#include <string>

using namespace od;

namespace {

QJsonObject json(const std::string& payload) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(payload)).object();
}

void testHello() {
    msg::Hello hello;
    hello.pixelsWide = 1920;
    hello.pixelsHigh = 1080;
    hello.scale = 1.0;
    hello.installId = "test-id";
    hello.cursorPort = 9001;
    hello.maxEncode = Size{.width = 3840, .height = 2160};
    const auto object = json(msg::hello(hello));
    assert(object["type"].toString() == "hello");
    assert(object["pixelsWide"].toInt() == 1920);
    assert(object["pixelsHigh"].toInt() == 1080);
    assert(object["pv"].toInt() == wire::protocolVersion);
    assert(object["id"].toString() == "test-id");
    assert(object["cursorPort"].toInt() == 9001);
    assert(object["maxEncodeWide"].toInt() == 3840);
}

void testHelloOmitsOptionals() {
    msg::Hello hello;
    hello.pixelsWide = 800;
    hello.pixelsHigh = 600;
    const auto object = json(msg::hello(hello));
    assert(!object.contains("cursorPort"));
    assert(!object.contains("maxEncodeWide"));
}

void testTouchWithAndWithoutTimestamp() {
    const auto withT = json(msg::touch("moved", 0.5, 0.25, 1234.0));
    assert(withT["type"].toString() == "touch");
    assert(withT["phase"].toString() == "moved");
    assert(withT["t"].toDouble() == 1234.0);
    const auto without = json(msg::touch("began", 0.0, 1.0, std::nullopt));
    assert(!without.contains("t"));
}

void testParsers() {
    const auto welcome = msg::parseWelcome(json("{\"type\":\"welcome\",\"pv\":3,\"min\":1}"));
    assert(welcome.has_value() && welcome->pv == 3 && welcome->min == 1);
    assert(!msg::parseWelcome(json("{\"type\":\"pong\"}")).has_value());

    const auto pong = msg::parsePong(json("{\"type\":\"pong\",\"t\":100,\"mt\":1050}"));
    assert(pong.has_value() && pong->t == 100 && pong->mt == 1050);
    assert(!msg::parsePong(json("{\"type\":\"pong\",\"t\":1}")).has_value());

    const auto cursor = msg::parseCursor(
        json("{\"type\":\"cursor\",\"x\":0.4,\"y\":0.7,\"v\":1,\"s\":88}"));
    assert(cursor.has_value() && cursor->visible && cursor->seq.has_value()
           && *cursor->seq == 88);
    // Older senders omit `s` — still applies.
    const auto legacy = msg::parseCursor(json("{\"type\":\"cursor\",\"x\":0.1,\"y\":0.2,\"v\":0}"));
    assert(legacy.has_value() && !legacy->visible && !legacy->seq.has_value());
}

void testCursorImage() {
    const QByteArray png = QByteArray::fromHex("89504e47");
    const QString payload = QStringLiteral(
        "{\"type\":\"cursorImg\",\"nw\":0.02,\"nh\":0.03,\"ax\":0.1,\"ay\":0.2,\"png\":\"%1\"}")
        .arg(QString::fromLatin1(png.toBase64()));
    const auto image = msg::parseCursorImage(json(payload.toStdString()));
    assert(image.has_value());
    assert(image->png == png);
    assert(image->normWidth == 0.02 && image->anchorY == 0.2);
    assert(!msg::parseCursorImage(json("{\"type\":\"cursorImg\"}")).has_value());
}

void testUpdateRequiredSanitizesStore() {
    const auto good = msg::parseUpdateRequired(json(
        "{\"type\":\"updateRequired\",\"target\":\"linux\",\"message\":\"update\","
        "\"store\":\"https://github.com/x/y\"}"));
    assert(good.has_value() && good->store == "https://github.com/x/y");
    const auto evil = msg::parseUpdateRequired(json(
        "{\"type\":\"updateRequired\",\"message\":\"x\","
        "\"store\":\"javascript:alert(1)\"}"));
    assert(evil.has_value() && evil->store.isEmpty());
    const auto unknown = msg::parseUpdateRequired(json(
        "{\"type\":\"updateRequired\",\"message\":\"x\","
        "\"store\":\"https://evil.example/pay\"}"));
    assert(unknown.has_value() && unknown->store.isEmpty());
}

void testParseControlRejects() {
    assert(!msg::parseControl("not json").has_value());
    assert(!msg::parseControl("[]").has_value());
    assert(!msg::parseControl("{\"x\":1}").has_value());  // no `type`
}

}  // namespace

int main() {
    testHello();
    testHelloOmitsOptionals();
    testTouchWithAndWithoutTimestamp();
    testParsers();
    testCursorImage();
    testUpdateRequiredSanitizesStore();
    testParseControlRejects();
    std::puts("messages tests passed");
    return 0;
}
