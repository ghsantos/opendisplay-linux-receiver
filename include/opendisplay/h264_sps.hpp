#pragma once

#include "opendisplay/types.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace od {

/// Minimal H.264 SPS Exp-Golomb parser — extracts only the coded picture
/// width/height (post-cropping). Ported from the Android receiver's
/// `H264Sps.kt` (peetzweg/opendisplay family); the decoder needs the real
/// bitstream size to know how many rawvideo bytes make one output frame,
/// and the GUI needs it for aspect-ratio letterboxing.
///
/// @param sps one SPS NALU, header byte first (as produced by annexb::parse).
/// @return the coded frame size after cropping, or nullopt when malformed or
/// using syntax this parser doesn't handle (caller falls back to the
/// announced panel size).
std::optional<Size> parseSpsDimensions(std::span<const std::uint8_t> sps);

}  // namespace od
