#include "opendisplay/wire.hpp"

#include <cassert>
#include <cstdio>
#include <span>
#include <string>

using namespace od::wire;

namespace {

std::span<const char, 4> headerOf(const std::string& framed) {
    return std::span<const char, 4>(framed.data(), 4);
}

void testFrameRoundTrip() {
    const std::string framed = frame("{\"type\":\"ping\"}");
    assert(framed.size() == 4 + 15);
    const auto size = decodeLength(headerOf(framed));
    assert(size.has_value() && *size == 15);
    assert(framed.substr(4) == "{\"type\":\"ping\"}");
}

void testDecodeLengthRejects() {
    const std::string zero = std::string("\0\0\0\0", 4);
    assert(!decodeLength(headerOf(zero)).has_value());
}

void testIsControlFrame() {
    assert(isControlFrame("{\"type\":\"kf\"}"));
    // >= 32768 bytes is never control.
    assert(!isControlFrame("{" + std::string(32768, 'x')));
    // NUL byte disqualifies even with a leading '{'.
    assert(!isControlFrame(std::string("{\0", 2)));
    // Video frames begin with start-code NULs, not '{'.
    assert(!isControlFrame(std::string("\0\0\0\1\x67", 5)));
    // A video frame may begin with '{' (telemetry prefix) but contains NULs.
    const std::string video = "{\"cap\":1}" + std::string("\0\0\0\1\x65", 5);
    assert(!isControlFrame(video));
    assert(!isControlFrame(""));
}

void testDeframer() {
    Deframer deframer;
    bool failed = false;
    // Partial header, then the rest.
    const std::string framed = frame("{\"type\":\"hello\"}");
    deframer.append(framed.substr(0, 3));
    assert(!deframer.next(failed).has_value() && !failed);
    deframer.append(framed.substr(3));
    const auto payload = deframer.next(failed);
    assert(payload.has_value() && *payload == "{\"type\":\"hello\"}");

    // Two frames packed in one append.
    const std::string a = frame("a");
    const std::string b = frame("bb");
    deframer.append(a + b);
    assert(*deframer.next(failed) == "a");
    assert(*deframer.next(failed) == "bb");
    assert(!deframer.next(failed).has_value());
}

void testDeframerBadLength() {
    Deframer deframer;
    deframer.append(std::string("\0\0\0\0", 4));
    bool failed = false;
    assert(!deframer.next(failed).has_value() && failed);
}

}  // namespace

int main() {
    testFrameRoundTrip();
    testDecodeLengthRejects();
    testIsControlFrame();
    testDeframer();
    testDeframerBadLength();
    std::puts("wire tests passed");
    return 0;
}
