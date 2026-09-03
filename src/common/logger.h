#pragma once

// 轻量分级日志：stdio 实现，无第三方依赖。
// 格式: [HH:MM:SS.mmm][LEVEL][tag] msg

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <string>

#include "timer.h"

namespace rtstream {

enum class LogLevel : int {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
};

inline LogLevel& log_level() {
    static LogLevel g_level = LogLevel::Info;
    return g_level;
}

inline const char* level_name(LogLevel lv) {
    switch (lv) {
        case LogLevel::Debug: return "D";
        case LogLevel::Info:  return "I";
        case LogLevel::Warn:  return "W";
        default:              return "E";
    }
}

inline void log_printf(LogLevel lv, const char* tag, const char* fmt, ...) {
    if (lv < log_level()) return;
    int64_t us = now_us();
    time_t sec = static_cast<time_t>(us / 1000000);
    struct tm tmv;
    localtime_r(&sec, &tmv);
    std::fprintf(stderr, "[%02d:%02d:%02d.%03d][%s][%s] ",
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                 static_cast<int>((us / 1000) % 1000),
                 level_name(lv), tag);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

} // namespace rtstream

#define RTS_LOGD(tag, ...) ::rtstream::log_printf(::rtstream::LogLevel::Debug, tag, __VA_ARGS__)
#define RTS_LOGI(tag, ...) ::rtstream::log_printf(::rtstream::LogLevel::Info,  tag, __VA_ARGS__)
#define RTS_LOGW(tag, ...) ::rtstream::log_printf(::rtstream::LogLevel::Warn,  tag, __VA_ARGS__)
#define RTS_LOGE(tag, ...) ::rtstream::log_printf(::rtstream::LogLevel::Error, tag, __VA_ARGS__)
