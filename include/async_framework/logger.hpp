// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <mutex>
#include <print>
#include <string_view>

namespace morph {

/// @brief Severity levels for the logging system.
enum class LogLevel : std::uint8_t {
    /// @brief Fine-grained diagnostic output.
    debug = 0,
    /// @brief General informational messages.
    info = 1,
    /// @brief Recoverable conditions worth noting.
    warn = 2,
    /// @brief Errors that should be investigated.
    error = 3,
    /// @brief Suppresses all output when used as the minimum level.
    off = 4,
};

/// @brief Returns the human-readable name for a log level.
/// @param level The log level to convert.
/// @return A fixed-width string_view, e.g. `"DEBUG"`, `"INFO "`.
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

/// @brief Sink function type for custom log output.
///
/// Receives the level and the pre-formatted message text (no prefix).
/// The default sink prepends `"[LEVEL] "` and writes to stderr.
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

/// @brief Replaces the global log sink.
///
/// Thread-safe. The new sink is called for every message whose level meets the
/// current minimum. Pass a no-op lambda to silence all output.
/// @param logger New sink function.
inline void setLogger(Logger logger) {
    std::scoped_lock lock{detail::logState().mtx};
    detail::logState().sink = std::move(logger);
}

/// @brief Sets the minimum log level.
///
/// Messages below this level are silently dropped. Thread-safe.
/// @param level Minimum level to emit.
inline void setLogLevel(LogLevel level) {
    std::scoped_lock lock{detail::logState().mtx};
    detail::logState().minLevel = level;
}

/// @brief Returns the current minimum log level. Thread-safe.
/// @return The active minimum level.
inline LogLevel getLogLevel() {
    std::scoped_lock lock{detail::logState().mtx};
    return detail::logState().minLevel;
}

// ── Core emit ─────────────────────────────────────────────────────────────────

/// @brief Emits a log message at the given level.
///
/// No-op if @p level is below the current minimum or no sink is installed.
/// Thread-safe.
/// @param level Severity of the message.
/// @param msg   The message text.
inline void log(LogLevel level, std::string_view msg) {
    std::scoped_lock lock{detail::logState().mtx};
    auto& state = detail::logState();
    if (state.sink && level >= state.minLevel) {
        state.sink(level, msg);
    }
}

// ── Level helpers ─────────────────────────────────────────────────────────────

/// @brief Logs @p msg at `LogLevel::debug`. @see log()
inline void logDebug(std::string_view msg) { log(LogLevel::debug, msg); }
/// @brief Logs @p msg at `LogLevel::info`. @see log()
inline void logInfo(std::string_view msg) { log(LogLevel::info, msg); }
/// @brief Logs @p msg at `LogLevel::warn`. @see log()
inline void logWarn(std::string_view msg) { log(LogLevel::warn, msg); }
/// @brief Logs @p msg at `LogLevel::error`. @see log()
inline void logError(std::string_view msg) { log(LogLevel::error, msg); }

}  // namespace morph
