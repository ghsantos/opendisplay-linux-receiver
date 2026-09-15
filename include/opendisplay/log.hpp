#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string_view>

namespace od {

inline bool verboseLogging = false;
inline std::mutex logMutex;

inline void log(std::string_view message) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);
    std::lock_guard lock(logMutex);
    std::cerr << std::put_time(&tm, "%H:%M:%S") << "  " << message << '\n';
}

inline void debug(std::string_view message) {
    if (verboseLogging) {
        log(message);
    }
}

inline std::int64_t wallClockMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace od
