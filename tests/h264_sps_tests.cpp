#include "opendisplay/h264_sps.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using od::parseSpsDimensions;
using od::Size;

namespace {

std::span<const std::uint8_t> span(const std::vector<std::uint8_t>& v) {
    return {v.data(), v.size()};
}

// Baseline profile, 1280x720, no cropping (ported from Android H264SpsTest).
const std::vector<std::uint8_t> kBaseline1280x720{
    0x67, 0x42, 0x00, 0x1E, 0xF4, 0x02, 0x80, 0x2D, 0xC8,
};

// High profile (chroma/scaling fields), 4:2:0, 1366x1024 — the Mac Mirror-mode
// resolution needing horizontal cropping (coded width is 1376).
const std::vector<std::uint8_t> kHigh1366x1024{
    0x67, 0x64, 0x00, 0x1F, 0xAC, 0xE8, 0x05, 0x60, 0x20, 0x79, 0xB4,
};

/// Exp-Golomb/fixed-width writer, MSB-first — the inverse of the parser's
/// BitReader, so synthetic streams reach every branch through the same public
/// entry point (ported from the Android test's SpsBitWriter).
class SpsBitWriter {
public:
    void u(const int value, const int n) {
        for (int i = n - 1; i >= 0; --i) {
            bits_.push_back((value >> i) & 1);
        }
    }
    void u8(const int value) { u(value, 8); }
    void ue(const int value) {
        const int codeNum = value + 1;
        int bitLength = 0;
        for (int c = codeNum; c != 0; c >>= 1) ++bitLength;
        for (int i = 0; i < bitLength - 1; ++i) bits_.push_back(0);
        u(codeNum, bitLength);
    }
    void se(const int value) {
        ue(value <= 0 ? -2 * value : 2 * value - 1);
    }
    std::vector<std::uint8_t> toBytes() const {
        std::vector<int> padded = bits_;
        while (padded.size() % 8 != 0) padded.push_back(0);
        std::vector<std::uint8_t> out(padded.size() / 8);
        for (std::size_t i = 0; i < out.size(); ++i) {
            int byte = 0;
            for (int b = 0; b < 8; ++b) {
                byte = (byte << 1) | padded[i * 8 + static_cast<std::size_t>(b)];
            }
            out[i] = static_cast<std::uint8_t>(byte);
        }
        return out;
    }
    std::vector<std::uint8_t> toSpsNalu(const int headerByte = 0x67) const {
        std::vector<std::uint8_t> out{static_cast<std::uint8_t>(headerByte)};
        const auto body = toBytes();
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }

private:
    std::vector<int> bits_;
};

void testBaseline() {
    const auto dims = parseSpsDimensions(span(kBaseline1280x720));
    assert(dims.has_value());
    assert(dims->width == 1280 && dims->height == 720);
}

void testHighCropped() {
    const auto dims = parseSpsDimensions(span(kHigh1366x1024));
    assert(dims.has_value());
    assert(dims->width == 1366 && dims->height == 1024);
}

void testTruncatedAndEmpty() {
    assert(!parseSpsDimensions(span(std::vector<std::uint8_t>{0x67, 0x42})).has_value());
    assert(!parseSpsDimensions({}).has_value());
}

void test444Chroma() {
    SpsBitWriter w;
    w.u8(244);      // profileIdc: High 4:4:4
    w.u8(0); w.u8(0); w.ue(0);
    w.ue(3);        // chromaFormatIdc = 3
    w.u(0, 1);      // separateColourPlaneFlag
    w.ue(0); w.ue(0); w.u(0, 1); w.u(0, 1);
    w.ue(0);        // log2MaxFrameNumMinus4
    w.ue(0); w.ue(0);  // picOrderCntType = 0
    w.ue(0); w.u(0, 1);
    w.ue(4);        // width 80
    w.ue(2);        // height
    w.u(1, 1); w.u(0, 1); w.u(0, 1);
    const auto dims = parseSpsDimensions(span(w.toSpsNalu()));
    assert(dims.has_value());
    assert(dims->width == 80 && dims->height == 48);
}

void testPocType1FieldPictures() {
    SpsBitWriter w;
    w.u8(66);       // Baseline — chroma-info block skipped entirely
    w.u8(0); w.u8(0); w.ue(0);
    w.ue(0);        // log2MaxFrameNumMinus4
    w.ue(1);        // picOrderCntType = 1
    w.u(0, 1); w.se(0); w.se(0); w.ue(0);
    w.ue(0); w.u(0, 1);
    w.ue(4); w.ue(2);
    w.u(0, 1);      // frameMbsOnlyFlag = 0 (field pictures)
    w.u(0, 1); w.u(0, 1); w.u(0, 1);
    const auto dims = parseSpsDimensions(span(w.toSpsNalu()));
    assert(dims.has_value());
    assert(dims->width == 80 && dims->height == 96);
}

void testMonochromeWithScalingMatrix() {
    SpsBitWriter w;
    w.u8(100); w.u8(0); w.u8(0); w.ue(0);
    w.ue(0);        // chromaFormatIdc = 0
    w.ue(0); w.ue(0); w.u(0, 1);
    w.u(1, 1);      // seqScalingMatrixPresentFlag
    w.u(1, 1); w.se(-8);   // list 0 (size 16), zeroes nextScale
    w.u(0, 1); w.u(0, 1); w.u(0, 1); w.u(0, 1); w.u(0, 1);
    w.u(1, 1); w.se(-8);   // list 6 (size 64), same trick
    w.u(0, 1);
    w.ue(0); w.ue(0); w.ue(0);
    w.ue(0); w.u(0, 1);
    w.ue(4); w.ue(2);
    w.u(1, 1); w.u(0, 1); w.u(0, 1);
    const auto dims = parseSpsDimensions(span(w.toSpsNalu()));
    assert(dims.has_value());
    assert(dims->width == 80 && dims->height == 48);
}

void test422Chroma() {
    SpsBitWriter w;
    w.u8(100); w.u8(0); w.u8(0); w.ue(0);
    w.ue(2);        // 4:2:2
    w.ue(0); w.ue(0); w.u(0, 1); w.u(0, 1);
    w.ue(0); w.ue(0); w.ue(0);
    w.ue(0); w.u(0, 1);
    w.ue(4); w.ue(2);
    w.u(1, 1); w.u(0, 1); w.u(0, 1);
    const auto dims = parseSpsDimensions(span(w.toSpsNalu()));
    assert(dims.has_value());
    assert(dims->width == 80 && dims->height == 48);
}

void testCroppingConsumesWidth() {
    SpsBitWriter w;
    w.u8(66); w.u8(0); w.u8(0); w.ue(0);
    w.ue(0); w.ue(0); w.ue(0);
    w.ue(0); w.u(0, 1);
    w.ue(0);        // raw width 16
    w.ue(2);
    w.u(1, 1); w.u(0, 1);
    w.u(1, 1);      // cropping on
    w.ue(4); w.ue(4); w.ue(0); w.ue(0);  // cropUnitX(2)*(4+4)=16 → width <= 0
    assert(!parseSpsDimensions(span(w.toSpsNalu())).has_value());
}

void testEmulationPrevention() {
    SpsBitWriter w;
    w.u8(0);        // profileIdc = 0
    w.u8(0);        // two zero bytes line up right after the NAL header
    w.u8(5);
    w.ue(0); w.ue(0); w.ue(0); w.ue(0);
    w.ue(0); w.u(0, 1);
    w.ue(4); w.ue(2);
    w.u(1, 1); w.u(0, 1); w.u(0, 1);
    const auto unescaped = w.toBytes();
    std::vector<std::uint8_t> escaped{0x67, unescaped[0], unescaped[1], 0x03};
    escaped.insert(escaped.end(), unescaped.begin() + 2, unescaped.end());
    const auto dims = parseSpsDimensions(span(escaped));
    assert(dims.has_value());
    assert(dims->width == 80 && dims->height == 48);
}

void testMalformedExpGolomb() {
    const std::vector<std::uint8_t> sps{0x67, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00};
    assert(!parseSpsDimensions(span(sps)).has_value());
}

void testOutOfBits() {
    const std::vector<std::uint8_t> sps{0x67, 0x00, 0x00, 0x00};
    assert(!parseSpsDimensions(span(sps)).has_value());
}

}  // namespace

int main() {
    testBaseline();
    testHighCropped();
    testTruncatedAndEmpty();
    test444Chroma();
    testPocType1FieldPictures();
    testMonochromeWithScalingMatrix();
    test422Chroma();
    testCroppingConsumesWidth();
    testEmulationPrevention();
    testMalformedExpGolomb();
    testOutOfBits();
    std::puts("h264_sps tests passed");
    return 0;
}
