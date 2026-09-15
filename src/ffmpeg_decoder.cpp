#include "opendisplay/ffmpeg_decoder.hpp"

#include "opendisplay/h264_sps.hpp"
#include "opendisplay/log.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <span>
#include <stdexcept>

namespace od {
namespace {

/// Deepest input backlog the decoder may hold before oldest frames drop —
/// keeps displayed latency bounded instead of growing a queue (mirrors the
/// Android receiver's overflow=DROP_OLDEST channel).
constexpr std::size_t maxPending = 4;

/// pipe2 is Linux-only; fall back to pipe + FD_CLOEXEC elsewhere (the app is
/// built for Ubuntu, but this keeps the file portable for dev builds).
int cloexecPipe(int fds[2]) {
#ifdef __linux__
    return ::pipe2(fds, O_CLOEXEC);
#else
    if (::pipe(fds) != 0) {
        return -1;
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

bool writeAll(const int fd, const std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

}  // namespace

FfmpegDecoder::~FfmpegDecoder() { stop(); }

void FfmpegDecoder::start(DecoderConfig config, const Size fallbackSize,
                          FrameCallback onFrame, ErrorCallback onError) {
    stop();
    std::signal(SIGPIPE, SIG_IGN);
    config_ = std::move(config);
    fallbackSize_ = fallbackSize;
    onFrame_ = std::move(onFrame);
    onError_ = std::move(onError);
    {
        std::lock_guard lock(mutex_);
        running_ = true;
        restartNeeded_ = false;
        droppedFrames_ = 0;
    }
    writer_ = std::thread(&FfmpegDecoder::writerLoop, this);
}

void FfmpegDecoder::submit(VideoFrame frame) {
    {
        std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        if (!frame.sps.empty()) {
            sps_ = frame.sps;
            if (const auto dims = parseSpsDimensions(
                    std::span<const std::uint8_t>(
                        reinterpret_cast<const std::uint8_t*>(sps_.data()), sps_.size()))) {
                videoSize_ = *dims;
            }
        }
        if (!frame.pps.empty()) {
            pps_ = frame.pps;
        }
        const Size size = sizeFor(frame);
        pending_.push_back(Queued{.frame = std::move(frame), .size = size});
        while (pending_.size() > maxPending) {
            pending_.pop_front();
            ++droppedFrames_;
        }
    }
    condition_.notify_one();
}

void FfmpegDecoder::stop() {
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        pending_.clear();
        expected_.clear();
    }
    condition_.notify_all();
    if (writer_.joinable()) {
        writer_.join();
    }
    onFrame_ = {};
    onError_ = {};
}

Size FfmpegDecoder::videoSize() const {
    std::lock_guard lock(mutex_);
    return videoSize_;
}

Size FfmpegDecoder::sizeFor(const VideoFrame& frame) const {
    if (!frame.sps.empty()) {
        if (const auto dims = parseSpsDimensions(
                std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(frame.sps.data()),
                    frame.sps.size()))) {
            return *dims;
        }
    }
    return videoSize_.width > 0 ? videoSize_ : fallbackSize_;
}

std::vector<std::string> FfmpegDecoder::arguments() const {
    std::vector<std::string> args{
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
    };
    const bool vaapi = config_.kind == DecoderKind::Vaapi
        && ::access(config_.vaapiDevice.c_str(), R_OK) == 0;
    if (vaapi) {
        // Download to software frames (nv12); the rawvideo sink converts to
        // bgra. Keeps the read-size math identical to software decode.
        args.insert(args.end(), {"-hwaccel", "vaapi",
                                 "-hwaccel_device", config_.vaapiDevice,
                                 "-hwaccel_output_format", "nv12"});
    }
    // NB: no "-fflags +nobuffer" — the raw-h264 demuxer needs its probe
    // pass to estimate the stream rate and emits nothing without it.
    args.insert(args.end(), {
        "-flags", "+low_delay",
        "-probesize", "32", "-analyzeduration", "0",
        "-f", "h264", "-i", "pipe:0",
        "-an", "-sn", "-dn",
        "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1",
    });
    return args;
}

void FfmpegDecoder::startProcess() {
    int inputPipe[2]{};
    int outputPipe[2]{};
    if (cloexecPipe(inputPipe) != 0 || cloexecPipe(outputPipe) != 0) {
        if (inputPipe[0] > 0) ::close(inputPipe[0]);
        if (inputPipe[1] > 0) ::close(inputPipe[1]);
        throw std::runtime_error("cannot create FFmpeg pipes");
    }
    const auto args = arguments();
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(inputPipe[0]); ::close(inputPipe[1]);
        ::close(outputPipe[0]); ::close(outputPipe[1]);
        throw std::runtime_error("cannot fork FFmpeg");
    }
    if (pid == 0) {
        ::dup2(inputPipe[0], STDIN_FILENO);
        ::dup2(outputPipe[1], STDOUT_FILENO);
        ::close(inputPipe[0]); ::close(inputPipe[1]);
        ::close(outputPipe[0]); ::close(outputPipe[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& argument : args) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv.front(), argv.data());
        _exit(127);
    }
    ::close(inputPipe[0]);
    ::close(outputPipe[1]);
    inputFd_ = inputPipe[1];
    outputFd_ = outputPipe[0];
    childPid_ = static_cast<int>(pid);
    reader_ = std::thread(&FfmpegDecoder::readerLoop, this, outputFd_);
}

void FfmpegDecoder::stopProcess() {
    if (inputFd_ >= 0) {
        ::close(inputFd_);
        inputFd_ = -1;
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    outputFd_ = -1;
    if (childPid_ > 0) {
        int status = 0;
        while (::waitpid(childPid_, &status, 0) < 0 && errno == EINTR) {}
        childPid_ = -1;
    }
    {
        std::lock_guard lock(mutex_);
        expected_.clear();
    }
}

void FfmpegDecoder::writerLoop() {
    try {
        for (;;) {
            Queued queued;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] {
                    return !running_ || !pending_.empty() || restartNeeded_;
                });
                if (!running_) {
                    break;
                }
                if (restartNeeded_) {
                    if (inputFd_ >= 0) {
                        stopProcess();
                    }
                    startProcess();
                    restartNeeded_ = false;
                }
                if (pending_.empty()) {
                    continue;
                }
                queued = std::move(pending_.front());
                pending_.pop_front();
            }
            if (inputFd_ < 0) {
                startProcess();
            }
            const auto expected = Expected{.size = queued.size,
                                           .capturedAtMs = queued.frame.capturedAtMs};
            if (!writeAll(inputFd_, queued.frame.annexB)) {
                log("FFmpeg decoder pipe failed; restarting it");
                stopProcess();
                startProcess();
                if (onError_) {
                    onError_("decoder restarted");
                }
                if (!writeAll(inputFd_, queued.frame.annexB)) {
                    throw std::runtime_error("FFmpeg decoder pipe failed twice");
                }
            }
            {
                std::lock_guard lock(mutex_);
                expected_.push_back(expected);
            }
        }
    } catch (const std::exception& error) {
        log(std::string("Decoder error: ") + error.what());
        if (onError_) {
            onError_(error.what());
        }
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    stopProcess();
}

void FfmpegDecoder::readerLoop(const int fd) {
    std::array<char, 256 * 1024> buffer{};
    std::string pending;
    for (;;) {
        const auto count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        pending.append(buffer.data(), static_cast<std::size_t>(count));
        for (;;) {
            Expected expected;
            {
                std::lock_guard lock(mutex_);
                if (expected_.empty()) {
                    // Output without a matching input record means the queues
                    // desynced (shouldn't happen): drop everything and resync
                    // on the next IDR.
                    pending.clear();
                    break;
                }
                expected = expected_.front();
            }
            const std::size_t frameBytes =
                static_cast<std::size_t>(expected.size.width)
                * static_cast<std::size_t>(expected.size.height) * 4U;
            if (frameBytes == 0) {
                // No SPS and no usable fallback: this output frame's size is
                // unknowable and the rawvideo stream gives no resync point —
                // restart the decoder and ask upstream for a fresh IDR.
                {
                    std::lock_guard lock(mutex_);
                    expected_.pop_front();
                    restartNeeded_ = true;
                }
                pending.clear();
                condition_.notify_one();
                if (onError_) {
                    onError_("unknown frame size");
                }
                continue;
            }
            if (pending.size() < frameBytes) {
                break;
            }
            {
                std::lock_guard lock(mutex_);
                if (!expected_.empty()) {
                    expected_.pop_front();
                }
            }
            if (onFrame_) {
                onFrame_(DecodedFrame{
                    .width = expected.size.width,
                    .height = expected.size.height,
                    .capturedAtMs = expected.capturedAtMs,
                    .rgba = pending.substr(0, frameBytes),
                });
            }
            pending.erase(0, frameBytes);
        }
    }
    ::close(fd);
}

}  // namespace od
