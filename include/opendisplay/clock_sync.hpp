#pragma once

#include <cstdint>
#include <deque>
#include <optional>

namespace od {

/// NTP-style receiver→sender clock offset (protocol section 8.1): keeps the
/// last 15 ping/pong samples and reports the offset of the minimum-RTT one —
/// the sample least distorted by queueing. Feeds e2e latency math and the
/// sender-clock `t` stamped on touch/pencil events.
class ClockSync {
public:
    /// Record one pong: `rtt` is receiver-clock round trip in ms, `offset` is
    /// senderClock − receiverClock in ms. Samples with rtt < 0 or >= 2000 ms
    /// are discarded (mirrors the official receiver).
    void addSample(const double rtt, const double offset) {
        if (rtt < 0 || rtt >= 2000.0) {
            return;
        }
        samples_.push_back({rtt, offset});
        while (samples_.size() > 15) {
            samples_.pop_front();
        }
    }

    /// senderClock − receiverClock, or nullopt until the first usable pong.
    [[nodiscard]] std::optional<double> offset() const {
        if (samples_.empty()) {
            return std::nullopt;
        }
        const Sample* best = &samples_.front();
        for (const auto& sample : samples_) {
            if (sample.rtt < best->rtt) {
                best = &sample;
            }
        }
        return best->offset;
    }

    void reset() { samples_.clear(); }

private:
    struct Sample {
        double rtt;
        double offset;
    };
    std::deque<Sample> samples_;
};

}  // namespace od
