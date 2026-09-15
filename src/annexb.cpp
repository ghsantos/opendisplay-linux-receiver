#include "opendisplay/annexb.hpp"

#include <cstring>

namespace od::annexb {
namespace {

std::size_t startCodeLength(const std::string_view bytes, const std::size_t position) {
    if (position + 3 < bytes.size() && bytes[position] == 0 && bytes[position + 1] == 0
        && bytes[position + 2] == 0 && bytes[position + 3] == 1) {
        return 4;
    }
    return 3;
}

/// Read one numeric field out of the telemetry prefix JSON (`{"cap":1,"snd":2}`).
/// Deliberately not a JSON parser — the prefix shape is fixed and tiny, and the
/// decoder never sees it.
std::int64_t telemetryField(const std::string_view prefix, const std::string_view key) {
    const auto at = prefix.find(key);
    if (at == std::string_view::npos) {
        return 0;
    }
    const auto colon = prefix.find(':', at + key.size());
    if (colon == std::string_view::npos) {
        return 0;
    }
    std::size_t index = colon + 1;
    while (index < prefix.size() && prefix[index] == ' ') {
        ++index;
    }
    const auto end = prefix.find_first_of(",}", index);
    const auto text = prefix.substr(index, end == std::string_view::npos
                                             ? std::string_view::npos : end - index);
    try {
        return std::stoll(std::string(text));
    } catch (...) {
        return 0;
    }
}

}  // namespace

std::size_t findStartCode(const std::string_view bytes, const std::size_t from) {
    for (std::size_t index = from; index + 3 < bytes.size(); ++index) {
        if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 0
            && bytes[index + 3] == 1) {
            return index;
        }
    }
    return std::string_view::npos;
}

std::vector<Nalu> splitNalus(const std::string_view accessUnit) {
    std::vector<Nalu> nalus;
    const auto first = findStartCode(accessUnit);
    if (first == std::string_view::npos) {
        return nalus;
    }
    std::size_t position = first;
    while (position != std::string_view::npos) {
        const auto prefix = startCodeLength(accessUnit, position);
        const auto next = findStartCode(accessUnit, position + prefix);
        const auto nalu = next == std::string_view::npos
            ? accessUnit.substr(position)
            : accessUnit.substr(position, next - position);
        if (nalu.size() > prefix) {
            nalus.push_back(Nalu{
                .data = nalu,
                .type = static_cast<unsigned char>(nalu[prefix]) & 0x1f,
            });
        }
        position = next;
    }
    return nalus;
}

std::optional<VideoFrame> parse(const std::string_view payload) {
    const auto first = findStartCode(payload);
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    VideoFrame frame;
    if (first > 0) {
        const auto prefix = payload.substr(0, first);
        frame.capturedAtMs = telemetryField(prefix, "\"cap\"");
        frame.sentAtMs = telemetryField(prefix, "\"snd\"");
    }
    frame.annexB = payload.substr(first);
    for (const auto& nalu : splitNalus(frame.annexB)) {
        const auto body = nalu.data.substr(startCodeLength(nalu.data, 0));
        if (nalu.type == 7 && frame.sps.empty()) {
            frame.sps = std::string(body);
        } else if (nalu.type == 8 && frame.pps.empty()) {
            frame.pps = std::string(body);
        } else if (nalu.type == 5) {
            frame.keyframe = true;
        }
    }
    return frame;
}

}  // namespace od::annexb
