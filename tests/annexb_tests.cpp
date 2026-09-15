#include "opendisplay/annexb.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using namespace od::annexb;

namespace {

const std::string kStart = std::string("\0\0\0\1", 4);

void testFindStartCode() {
    assert(findStartCode("abc") == std::string::npos);
    assert(findStartCode("ab" + kStart + "x") == 2);
    // A 3-byte start code alone is NOT a match (senders must emit 4-byte).
    assert(findStartCode("\0\0\1x") == std::string::npos);
}

void testSplitNalus() {
    const std::string au = kStart + "\x67sps" + kStart + "\x68pps"
        + kStart + "\x65idr-slice";
    const auto nalus = splitNalus(au);
    assert(nalus.size() == 3);
    assert(nalus[0].type == 7);
    assert(nalus[1].type == 8);
    assert(nalus[2].type == 5);
    assert(nalus[0].data.substr(4) == "\x67sps");
}

void testParseWithTelemetry() {
    const std::string payload = "{\"cap\":1000,\"snd\":1010}" + kStart + "\x67\x42sps"
        + kStart + "\x68pps" + kStart + "\x65slice";
    const auto frame = parse(payload);
    assert(frame.has_value());
    assert(frame->capturedAtMs == 1000);
    assert(frame->sentAtMs == 1010);
    assert(!frame->sps.empty());
    assert(!frame->pps.empty());
    assert(frame->keyframe);
    // annexB starts at the first start code — telemetry stripped.
    assert(frame->annexB.substr(0, 4) == kStart);
}

void testParseWithoutTelemetry() {
    const std::string payload = kStart + "\x41slice";
    const auto frame = parse(payload);
    assert(frame.has_value());
    assert(frame->capturedAtMs == 0);
    assert(!frame->keyframe);
    assert(frame->sps.empty());
}

void testParseNoStartCode() {
    assert(!parse("{\"cap\":1}").has_value());
    assert(!parse("").has_value());
}

void testNonKeyframeKeepsOnlySlice() {
    // Section 5.1: non-keyframes carry slice data only — sps/pps stay empty.
    const std::string payload = kStart + "\x41predicted";
    const auto frame = parse(payload);
    assert(frame.has_value());
    assert(frame->sps.empty() && frame->pps.empty());
    assert(!frame->keyframe);
}

}  // namespace

int main() {
    testFindStartCode();
    testSplitNalus();
    testParseWithTelemetry();
    testParseWithoutTelemetry();
    testParseNoStartCode();
    testNonKeyframeKeepsOnlySlice();
    std::puts("annexb tests passed");
    return 0;
}
