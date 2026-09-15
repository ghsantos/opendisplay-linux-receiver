#pragma once

#include "opendisplay/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace od::annexb {

/// One NALU inside an access unit: `data` includes its 4-byte start code,
/// `type` is `header & 0x1f` (5=IDR, 6=SEI, 7=SPS, 8=PPS, 9=AUD).
struct Nalu {
    std::string_view data;
    int type = 0;
};

/// Parse one wire video frame (protocol section 5.1): strip the optional
/// telemetry JSON prefix (everything before the first 4-byte start code,
/// `{"cap":…,"snd":…}`), then split the remainder on 4-byte start codes.
/// Returns nullopt when the payload contains no start code at all.
std::optional<VideoFrame> parse(std::string_view payload);

/// Split an Annex B access unit into its NALUs. Input must begin at a
/// 4-byte start code; only 4-byte start codes are split on — the protocol
/// forbids 3-byte codes and the official receiver does the same.
std::vector<Nalu> splitNalus(std::string_view accessUnit);

/// The position of the first `00 00 00 01` sequence, or npos.
std::size_t findStartCode(std::string_view bytes, std::size_t from = 0);

}  // namespace od::annexb
