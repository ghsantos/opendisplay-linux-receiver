#include "opendisplay/wire.hpp"

#include <algorithm>

namespace od::wire {

std::string frame(const std::string_view payload) {
    const auto size = static_cast<std::uint32_t>(payload.size());
    std::string output(4, '\0');
    output[0] = static_cast<char>((size >> 24U) & 0xffU);
    output[1] = static_cast<char>((size >> 16U) & 0xffU);
    output[2] = static_cast<char>((size >> 8U) & 0xffU);
    output[3] = static_cast<char>(size & 0xffU);
    output.append(payload);
    return output;
}

std::optional<std::uint32_t> decodeLength(const std::span<const char, 4> header) {
    const auto size = (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24U)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16U)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8U)
        | static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
    if (size == 0 || size >= maxInboundFrame) {
        return std::nullopt;
    }
    return size;
}

bool isControlFrame(const std::string_view payload) {
    return payload.size() < controlSizeLimit && !payload.empty() && payload.front() == '{'
        && payload.find('\0') == std::string_view::npos;
}

}  // namespace od::wire
