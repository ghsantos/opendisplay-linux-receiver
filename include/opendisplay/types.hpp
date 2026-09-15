#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace od {

struct Size {
    int width = 0;
    int height = 0;
};

enum class DecoderKind { Auto, Software, Vaapi };

/// One wire video frame after Annex-B parsing: the telemetry prefix is
/// stripped off and the NALUs classified; `annexB` stays feedable to the
/// decoder unchanged.
struct VideoFrame {
    std::string annexB;          // 4-byte-start-code Annex B access unit
    std::int64_t capturedAtMs = 0;   // sender telemetry prefix, 0 = absent
    std::int64_t sentAtMs = 0;
    std::string sps;             // empty when this frame carries none
    std::string pps;
    bool keyframe = false;
};

struct DecodedFrame {
    int width = 0;
    int height = 0;
    std::int64_t capturedAtMs = 0;   // sender clock, from the telemetry prefix
    std::string rgba;                // width*height*4 bytes
};

struct CursorState {
    double x = 0.5;
    double y = 0.5;
    bool visible = false;
};

struct ReceiverOptions {
    std::uint16_t port = 9000;
    std::string serviceName;            // empty -> host name
    std::optional<Size> panel;          // announced panel pixels override
    std::optional<double> scale;        // announced scale override
    bool fullscreen = false;
    bool input = true;
    bool cursorChannel = true;
    DecoderKind decoder = DecoderKind::Auto;
    std::string vaapiDevice = "/dev/dri/renderD128";
    std::optional<Size> maxEncode;      // hello decode ceiling
    bool verbose = false;
    bool showHud = true;
};

/// One-second window of pipeline health for the HUD and `stats` reports.
struct StatsSnapshot {
    int fps = 0;
    double mbps = 0.0;
    double rttMs = 0.0;
    double e2eP50 = 0.0;
    double e2eP95 = 0.0;
    int stalls = 0;          // frames arriving >50ms after the previous one
    int decodeFlushes = 0;   // decoder resyncs since connect
    std::string transport;   // "USB" (loopback) or "WiFi"
    int cursorPerSec = 0;
    int cursorLost = 0;
};

}  // namespace od
