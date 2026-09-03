#pragma once

// 高精度单调时钟工具：所有"端到端延时"统计都基于 CLOCK_MONOTONIC，
// 避免系统时间跳变影响。时间单位统一为毫秒(double)与微秒(int64_t)。

#include <cstdint>
#include <ctime>

namespace rtstream {

inline int64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

// 毫秒，double 便于统计小数延时
inline double now_ms() {
    return static_cast<double>(now_us()) / 1000.0;
}

// 系统墙钟（UTC us），仅在需要落日志/跨机对照时使用，不参与延时计算
inline int64_t wall_us() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

} // namespace rtstream
