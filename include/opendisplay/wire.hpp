#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace od::wire {

constexpr int protocolVersion = 3;
constexpr int minSupportedPeer = 1;
constexpr int assumedWhenAbsent = 1;

/// Protocol.md section 3: sender-to-receiver frames carry multi-megabyte
/// video; a generous guard bounds deframing buffers without constraining
/// legitimate streams.
constexpr std::uint32_t maxInboundFrame = 64U << 20U;
/// Section 4: the JSON-demux heuristic only applies below this size.
constexpr std::uint32_t controlSizeLimit = 32768U;
/// Receiver-to-sender payloads must stay inside 1..2^20-1 bytes.
constexpr std::uint32_t maxControlSize = 1U << 20U;

/// `[4-byte big-endian length][payload]`; caller sends the result verbatim.
std::string frame(std::string_view payload);

/// Big-endian 4-byte length. Returns nullopt for a size of 0 or beyond the
/// inbound guard — the official peers treat those as protocol errors.
std::optional<std::uint32_t> decodeLength(std::span<const char, 4> header);

/// The section 4 demux heuristic, isolated so a future typed frame header
/// (pv 4) is a one-function swap: a frame is a JSON control message iff it
/// is < 32768 bytes, starts with '{', and contains no NUL byte.
bool isControlFrame(std::string_view payload);

/// Incremental length-prefixed deframer: feed socket bytes, pull complete
/// payloads. Keeps partial frames buffered across reads.
class Deframer {
public:
    void append(std::string_view bytes) { buffer_.append(bytes); }

    /// Returns the next complete payload, or nullopt when more bytes are
    /// needed. A malformed length prefix returns nullopt with `failed` set —
    /// the connection is unrecoverable and should be dropped.
    std::optional<std::string> next(bool& failed) {
        failed = false;
        if (buffer_.size() < 4) {
            return std::nullopt;
        }
        const std::span<const char, 4> header(buffer_.data(), 4);
        const auto size = decodeLength(header);
        if (!size.has_value()) {
            failed = true;
            return std::nullopt;
        }
        if (buffer_.size() < 4 + *size) {
            return std::nullopt;
        }
        std::string payload = buffer_.substr(4, *size);
        buffer_.erase(0, 4 + *size);
        return payload;
    }

    void reset() { buffer_.clear(); }
    [[nodiscard]] std::size_t buffered() const { return buffer_.size(); }

private:
    std::string buffer_;
};

}  // namespace od::wire
