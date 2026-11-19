/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file logger.h
 * @brief Lightweight fmt-based logger with runtime and compile-time level filtering.
 */

#pragma once

#include <fmt/format.h>
#include <string>
#include <utility>

// Compile-time log filtering. Larger values keep more verbose logs.
// Example: -DAPP_LOG_LEVEL=APP_LOG_LEVEL_DEBUG keeps DEBUG and above.
#define APP_LOG_LEVEL_OFF   0
#define APP_LOG_LEVEL_FATAL 1
#define APP_LOG_LEVEL_ERROR 2
#define APP_LOG_LEVEL_WARN  3
#define APP_LOG_LEVEL_INFO  4
#define APP_LOG_LEVEL_DEBUG 5
#define APP_LOG_LEVEL_TRACE 6

#ifndef APP_LOG_LEVEL
#define APP_LOG_LEVEL APP_LOG_LEVEL_TRACE
#endif

#if APP_LOG_LEVEL < APP_LOG_LEVEL_OFF || APP_LOG_LEVEL > APP_LOG_LEVEL_TRACE
#error "APP_LOG_LEVEL must be one of APP_LOG_LEVEL_OFF/FATAL/ERROR/WARN/INFO/DEBUG/TRACE"
#endif

namespace logger {

/** @brief Runtime severity levels, ordered from most to least verbose. */
enum class LogLevel {
    TRACE = 0,
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL,
    OFF
};

/** @brief Whether ANSI colors are used; AUTO enables them only on a terminal. */
enum class ColorMode {
    AUTO,
    ENABLE,
    DISABLE
};

/**
 * @brief Static logging facade.
 *
 * Prefer the LOG_* macros, which record the call site and are removed entirely
 * when filtered out by APP_LOG_LEVEL.
 */
class Logger {
public:
    /** @brief Applies defaults (e.g. from the environment); call once at startup. */
    static void init();

    /** @brief Sets the minimum runtime level; lower-severity messages are dropped. */
    static void set_level(LogLevel level);
    static LogLevel level();

    /** @brief Sets a tag prefixed to every message. */
    static void set_tag(const char* tag);
    static const char* tag();

    static void set_color_mode(ColorMode mode);

    /** @brief Enables or disables a timestamp prefix on each message. */
    static void set_timestamp_enabled(bool enabled);
    static bool timestamp_enabled();

    /// Compile-time level (APP_LOG_LEVEL) and per-level compile checks.
    static constexpr int compile_level() { return APP_LOG_LEVEL; }
    static constexpr bool should_compile_trace() { return APP_LOG_LEVEL >= APP_LOG_LEVEL_TRACE; }
    static constexpr bool should_compile_debug() { return APP_LOG_LEVEL >= APP_LOG_LEVEL_DEBUG; }
    static constexpr bool should_compile_info() { return APP_LOG_LEVEL >= APP_LOG_LEVEL_INFO; }
    static constexpr bool should_compile_warn() { return APP_LOG_LEVEL >= APP_LOG_LEVEL_WARN; }
    static constexpr bool should_compile_error() { return APP_LOG_LEVEL >= APP_LOG_LEVEL_ERROR; }
    static constexpr bool should_compile_fatal() { return APP_LOG_LEVEL >= APP_LOG_LEVEL_FATAL; }

    /// @name Formatted logging without source location
    template<typename... Args>
    static void trace(fmt::format_string<Args...> fmt_str,
                      Args&&... args)
    {
        log(LogLevel::TRACE,
            fmt::format(fmt_str,
                        std::forward<Args>(args)...));
    }

    template<typename... Args>
    static void debug(fmt::format_string<Args...> fmt_str,
                      Args&&... args)
    {
        log(LogLevel::DEBUG,
            fmt::format(fmt_str,
                        std::forward<Args>(args)...));
    }

    template<typename... Args>
    static void info(fmt::format_string<Args...> fmt_str,
                     Args&&... args)
    {
        log(LogLevel::INFO,
            fmt::format(fmt_str,
                        std::forward<Args>(args)...));
    }

    template<typename... Args>
    static void warn(fmt::format_string<Args...> fmt_str,
                     Args&&... args)
    {
        log(LogLevel::WARN,
            fmt::format(fmt_str,
                        std::forward<Args>(args)...));
    }

    template<typename... Args>
    static void error(fmt::format_string<Args...> fmt_str,
                      Args&&... args)
    {
        log(LogLevel::ERROR,
            fmt::format(fmt_str,
                        std::forward<Args>(args)...));
    }

    template<typename... Args>
    static void fatal(fmt::format_string<Args...> fmt_str,
                      Args&&... args)
    {
        log(LogLevel::FATAL,
            fmt::format(fmt_str,
                        std::forward<Args>(args)...));
    }

    /// @name Formatted logging with source file/line (used by LOG_* macros)
    template<typename... Args>
    static void trace_at(const char* file,
                         int line,
                         fmt::format_string<Args...> fmt_str,
                         Args&&... args)
    {
        log(LogLevel::TRACE,
            fmt::format(fmt_str,
                        std::forward<Args>(args)...),
            file,
            line);
    }

    template<typename... Args>
    static void debug_at(const char* file,
                         int line,
                         fmt::format_string<Args...> fmt_str,
                         Args&&... args)
    {
        log(LogLevel::DEBUG,
            fmt::format(fmt_str,
                        std::forward<Args>(args)...),
            file,
            line);
    }

    template<typename... Args>
    static void info_at(const char* file,
                        int line,
                        fmt::format_string<Args...> fmt_str,
                        Args&&... args)
    {
        log(LogLevel::INFO,
            fmt::format(fmt_str,
                        std::forward<Args>(args)...),
            file,
            line);
    }

    template<typename... Args>
    static void warn_at(const char* file,
                        int line,
                        fmt::format_string<Args...> fmt_str,
                        Args&&... args)
    {
        log(LogLevel::WARN,
            fmt::format(fmt_str,
                        std::forward<Args>(args)...),
            file,
            line);
    }

    template<typename... Args>
    static void error_at(const char* file,
                         int line,
                         fmt::format_string<Args...> fmt_str,
                         Args&&... args)
    {
        log(LogLevel::ERROR,
            fmt::format(fmt_str,
                        std::forward<Args>(args)...),
            file,
            line);
    }

    template<typename... Args>
    static void fatal_at(const char* file,
                         int line,
                         fmt::format_string<Args...> fmt_str,
                         Args&&... args)
    {
        log(LogLevel::FATAL,
            fmt::format(fmt_str,
                        std::forward<Args>(args)...),
            file,
            line);
    }

private:
    static void log(LogLevel level,
                    const std::string& msg,
                    const char* file = nullptr,
                    int line = 0);

    static bool should_log(LogLevel level);

    static bool should_use_color();

private:
    inline static LogLevel level_ = LogLevel::INFO;
    inline static std::string tag_ = "";
    inline static ColorMode color_mode_ = ColorMode::AUTO;
    inline static bool timestamp_enabled_ = false;
};

} // namespace logger

#if APP_LOG_LEVEL >= APP_LOG_LEVEL_TRACE
#define LOG_TRACE(...) ::logger::Logger::trace_at(__FILE__, __LINE__, __VA_ARGS__)
#else
#define LOG_TRACE(...) do {} while (0)
#endif

#if APP_LOG_LEVEL >= APP_LOG_LEVEL_DEBUG
#define LOG_DEBUG(...) ::logger::Logger::debug_at(__FILE__, __LINE__, __VA_ARGS__)
#else
#define LOG_DEBUG(...) do {} while (0)
#endif

#if APP_LOG_LEVEL >= APP_LOG_LEVEL_INFO
#define LOG_INFO(...) ::logger::Logger::info_at(__FILE__, __LINE__, __VA_ARGS__)
#else
#define LOG_INFO(...) do {} while (0)
#endif

#if APP_LOG_LEVEL >= APP_LOG_LEVEL_WARN
#define LOG_WARN(...) ::logger::Logger::warn_at(__FILE__, __LINE__, __VA_ARGS__)
#else
#define LOG_WARN(...) do {} while (0)
#endif

#if APP_LOG_LEVEL >= APP_LOG_LEVEL_ERROR
#define LOG_ERROR(...) ::logger::Logger::error_at(__FILE__, __LINE__, __VA_ARGS__)
#else
#define LOG_ERROR(...) do {} while (0)
#endif

#if APP_LOG_LEVEL >= APP_LOG_LEVEL_FATAL
#define LOG_FATAL(...) ::logger::Logger::fatal_at(__FILE__, __LINE__, __VA_ARGS__)
#else
#define LOG_FATAL(...) do {} while (0)
#endif
