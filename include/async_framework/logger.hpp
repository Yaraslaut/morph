#pragma once
#include <functional>
#include <mutex>
#include <print>
#include <string_view>

namespace morph {

enum class LogLevel : std::uint8_t {
    debug = 0,
    info = 1,
    warn = 2,
    error = 3,
    off = 4,
};

constexpr std::string_view levelName(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::debug:
            return "DEBUG";
        case LogLevel::info:
            return "INFO ";
        case LogLevel::warn:
            return "WARN ";
        case LogLevel::error:
            return "ERROR";
        case LogLevel::off:
            return "OFF  ";
    }
    return "?    ";
}

// Callback receives the level and the pre-formatted message text (no prefix).
// The default sink prepends "[LEVEL] " and writes to stderr.
using Logger = std::function<void(LogLevel, std::string_view)>;

namespace detail {

struct LogState {
    Logger sink = [](LogLevel lvl, std::string_view msg) { std::println(stderr, "[{}] {}", levelName(lvl), msg); };
    LogLevel minLevel = LogLevel::debug;
    std::mutex mtx;
};

inline LogState& logState() {
    static LogState state;
    return state;
}

}  // namespace detail

// ── Configuration ─────────────────────────────────────────────────────────────

inline void setLogger(Logger logger) {
    std::lock_guard lock{detail::logState().mtx};
    detail::logState().sink = std::move(logger);
}

inline void setLogLevel(LogLevel level) {
    std::lock_guard lock{detail::logState().mtx};
    detail::logState().minLevel = level;
}

inline LogLevel getLogLevel() {
    std::lock_guard lock{detail::logState().mtx};
    return detail::logState().minLevel;
}

// ── Core emit ─────────────────────────────────────────────────────────────────

inline void log(LogLevel level, std::string_view msg) {
    std::lock_guard lock{detail::logState().mtx};
    auto& state = detail::logState();
    if (state.sink && level >= state.minLevel) {
        state.sink(level, msg);
    }
}

// ── Level helpers ─────────────────────────────────────────────────────────────

inline void logDebug(std::string_view msg) { log(LogLevel::debug, msg); }
inline void logInfo(std::string_view msg) { log(LogLevel::info, msg); }
inline void logWarn(std::string_view msg) { log(LogLevel::warn, msg); }
inline void logError(std::string_view msg) { log(LogLevel::error, msg); }

}  // namespace morph
