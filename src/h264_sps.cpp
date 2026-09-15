#include "opendisplay/h264_sps.hpp"

#include <stdexcept>
#include <vector>

namespace od {
namespace {

constexpr int kProfilesWithChromaInfo[] = {100, 110, 122, 244, 44, 83, 86,
                                           118, 128, 138, 139, 134, 135};

bool hasChromaInfo(const int profileIdc) {
    for (const int profile : kProfilesWithChromaInfo) {
        if (profile == profileIdc) {
            return true;
        }
    }
    return false;
}

/// Removes `emulation_prevention_three_byte` (0x03 after any 00 00) and the
/// leading NAL header byte, leaving the raw RBSP bit sequence.
std::vector<std::uint8_t> stripEmulationPrevention(
    const std::span<const std::uint8_t> nalu) {
    std::vector<std::uint8_t> out;
    out.reserve(nalu.size());
    int zeroRun = 0;
    for (std::size_t i = 1; i < nalu.size(); ++i) {
        const auto byte = nalu[i];
        if (zeroRun >= 2 && byte == 0x03) {
            zeroRun = 0;
            continue;
        }
        out.push_back(byte);
        zeroRun = byte == 0 ? zeroRun + 1 : 0;
    }
    return out;
}

class BitReader {
public:
    explicit BitReader(std::vector<std::uint8_t> data) : data_(std::move(data)) {}

    int u(const int n) {
        int value = 0;
        for (int i = 0; i < n; ++i) {
            value = (value << 1) | bit();
        }
        return value;
    }

    int ue() {
        int leadingZeros = 0;
        while (bit() == 0) {
            ++leadingZeros;
            if (leadingZeros > 32) {
                throw std::runtime_error("malformed exp-golomb code");
            }
        }
        int value = 1;
        for (int i = 0; i < leadingZeros; ++i) {
            value = (value << 1) | bit();
        }
        return value - 1;
    }

    int se() {
        const int codeNum = ue();
        const int sign = codeNum % 2 == 0 ? -1 : 1;
        return sign * ((codeNum + 1) / 2);
    }

private:
    int bit() {
        const auto byteIndex = bitPos_ / 8;
        if (byteIndex >= data_.size()) {
            throw std::runtime_error("out of bits");
        }
        const auto bitIndex = 7 - static_cast<int>(bitPos_ % 8);
        ++bitPos_;
        return (data_[byteIndex] >> bitIndex) & 1;
    }

    std::vector<std::uint8_t> data_;
    std::size_t bitPos_ = 0;
};

void skipScalingList(BitReader& reader, const int size) {
    int lastScale = 8;
    int nextScale = 8;
    for (int i = 0; i < size; ++i) {
        if (nextScale != 0) {
            nextScale = (lastScale + reader.se() + 256) % 256;
        }
        if (nextScale != 0) {
            lastScale = nextScale;
        }
    }
}

}  // namespace

std::optional<Size> parseSpsDimensions(const std::span<const std::uint8_t> sps) {
    if (sps.size() < 4) {
        return std::nullopt;
    }
    try {
        BitReader reader(stripEmulationPrevention(sps));
        const int profileIdc = reader.u(8);
        reader.u(8);  // constraint flags + reserved
        reader.u(8);  // levelIdc
        reader.ue();  // seqParameterSetId

        int chromaFormatIdc = 1;
        if (hasChromaInfo(profileIdc)) {
            chromaFormatIdc = reader.ue();
            if (chromaFormatIdc == 3) {
                reader.u(1);  // separateColourPlaneFlag
            }
            reader.ue();    // bitDepthLumaMinus8
            reader.ue();    // bitDepthChromaMinus8
            reader.u(1);    // qpprimeYZeroTransformBypassFlag
            if (reader.u(1) == 1) {  // seqScalingMatrixPresentFlag
                const int listCount = chromaFormatIdc != 3 ? 8 : 12;
                for (int i = 0; i < listCount; ++i) {
                    if (reader.u(1) == 1) {  // scalingListPresentFlag
                        skipScalingList(reader, i < 6 ? 16 : 64);
                    }
                }
            }
        }

        reader.ue();  // log2MaxFrameNumMinus4
        if (const int picOrderCntType = reader.ue(); picOrderCntType == 0) {
            reader.ue();  // log2MaxPicOrderCntLsbMinus4
        } else if (picOrderCntType == 1) {
            reader.u(1);  // deltaPicOrderAlwaysZeroFlag
            reader.se();  // offsetForNonRefPic
            reader.se();  // offsetForTopToBottomField
            const int cycle = reader.ue();
            for (int i = 0; i < cycle; ++i) {
                reader.se();
            }
        }

        reader.ue();    // maxNumRefFrames
        reader.u(1);    // gapsInFrameNumValueAllowedFlag
        const int picWidthInMbsMinus1 = reader.ue();
        const int picHeightInMapUnitsMinus1 = reader.ue();
        const int frameMbsOnlyFlag = reader.u(1);
        if (frameMbsOnlyFlag == 0) {
            reader.u(1);  // mbAdaptiveFrameFieldFlag
        }
        reader.u(1);    // direct8x8InferenceFlag

        int cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
        if (reader.u(1) == 1) {  // frame_cropping_flag
            cropLeft = reader.ue();
            cropRight = reader.ue();
            cropTop = reader.ue();
            cropBottom = reader.ue();
        }

        const int subWidthC = chromaFormatIdc == 1 || chromaFormatIdc == 2 ? 2 : 1;
        const int subHeightC = chromaFormatIdc == 1 ? 2 : 1;
        const int cropUnitX = chromaFormatIdc == 0 ? 1 : subWidthC;
        const int cropUnitY = chromaFormatIdc == 0 ? 2 - frameMbsOnlyFlag
                                                   : subHeightC * (2 - frameMbsOnlyFlag);

        const int width = (picWidthInMbsMinus1 + 1) * 16 - cropUnitX * (cropLeft + cropRight);
        const int height = (2 - frameMbsOnlyFlag) * (picHeightInMapUnitsMinus1 + 1) * 16
            - cropUnitY * (cropTop + cropBottom);
        if (width <= 0 || height <= 0) {
            return std::nullopt;
        }
        return Size{.width = width, .height = height};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace od
