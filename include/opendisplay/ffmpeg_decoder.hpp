#pragma once

#include "opendisplay/types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace od {

struct DecoderConfig {
    DecoderKind kind = DecoderKind::Auto;
    std::string vaapiDevice = "/dev/dri/renderD128";
};

/// Low-latency FFmpeg subprocess decoder, the mirror image of the sender
/// port's FfmpegEncoder: Annex-B access units go to the child's stdin,
/// rawvideo RGBA frames come back on stdout. The stream stays H.264 with no
/// B-frames, so input and output frames stay aligned 1:1 in order — each
/// submitted access unit records the output size its decoded frame will
/// have (from its SPS, or the announced panel as fallback), letting the
/// reader survive mid-stream resolution changes.
///
/// A bounded pending queue drops the oldest undelivered frame under load so
/// latency cannot grow; drops and process failures report through
/// ErrorCallback so the session can request a fresh IDR (`kf`).
class FfmpegDecoder {
public:
    using FrameCallback = std::function<void(DecodedFrame)>;
    using ErrorCallback = std::function<void(const std::string&)>;

    FfmpegDecoder() = default;
    ~FfmpegDecoder();
    FfmpegDecoder(const FfmpegDecoder&) = delete;
    FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;

    /// `fallbackSize` is the announced panel size, used for output frames
    /// whose access unit carried no parseable SPS.
    void start(DecoderConfig config, Size fallbackSize,
               FrameCallback onFrame, ErrorCallback onError);
    void submit(VideoFrame frame);
    void stop();
    [[nodiscard]] Size videoSize() const;
    [[nodiscard]] int droppedFrames() const { return droppedFrames_.load(); }

private:
    struct Expected {
        Size size;
        std::int64_t capturedAtMs = 0;
    };
    struct Queued {
        VideoFrame frame;
        Size size;
    };

    void writerLoop();
    void readerLoop(int fd);
    void startProcess();
    void stopProcess();
    std::vector<std::string> arguments() const;
    Size sizeFor(const VideoFrame& frame) const;

    DecoderConfig config_;
    Size fallbackSize_;
    FrameCallback onFrame_;
    ErrorCallback onError_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Queued> pending_;         // submitted, not yet written
    std::deque<Expected> expected_;      // written, not yet read back
    std::thread writer_;
    std::thread reader_;
    std::atomic_bool running_ = false;
    std::atomic<int> droppedFrames_ = 0;
    int inputFd_ = -1;
    int outputFd_ = -1;
    int childPid_ = -1;
    Size videoSize_;
    std::string sps_;
    std::string pps_;
    bool restartNeeded_ = false;
};

}  // namespace od
