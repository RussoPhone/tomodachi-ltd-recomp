// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <cstdarg>
#include <mutex>

#include "common.h"

namespace nxrt {

namespace {
LogLevel MinLevel() {
    static const LogLevel level = [] {
        const char* v = std::getenv("NXRT_LOG");
        if (!v) return LogLevel::Info;
        const std::string s = v;
        if (s == "trace") return LogLevel::Trace;
        if (s == "debug") return LogLevel::Debug;
        if (s == "warning") return LogLevel::Warning;
        if (s == "error") return LogLevel::Error;
        return LogLevel::Info;
    }();
    return level;
}
std::mutex g_log_mutex;
} // namespace

bool LogEnabled(LogLevel level) {
    return level >= MinLevel();
}

static void VLog(LogLevel level, const char* fmt, va_list ap) {
    static const auto start = std::chrono::steady_clock::now();
    static const char* const names[] = {"trace", "debug", "info", "warn", "error"};
    const double t =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::lock_guard lock(g_log_mutex);
    std::fprintf(stderr, "[%9.3f] %-5s ", t, names[static_cast<int>(level)]);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}

void Log(LogLevel level, const char* fmt, ...) {
    if (!LogEnabled(level)) return;
    va_list ap;
    va_start(ap, fmt);
    VLog(level, fmt, ap);
    va_end(ap);
}

void Fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    VLog(LogLevel::Error, fmt, ap);
    va_end(ap);
    std::fflush(stderr);
    std::_Exit(1);
}

} // namespace nxrt
