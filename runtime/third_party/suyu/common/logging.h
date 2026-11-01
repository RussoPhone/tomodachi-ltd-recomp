// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt shim for suyu's logging: fmt formatting for enums, and LOG_* macros that
// take a log class first (ignored) and print to stderr.
#pragma once

#include <cstdio>
#include <type_traits>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/swap.h"

#if FMT_VERSION >= 80100
template <typename T>
struct fmt::formatter<T, std::enable_if_t<std::is_enum_v<T>, char>> : formatter<std::underlying_type_t<T>> {
    template <typename FormatContext>
    auto format(const T& value, FormatContext& ctx) const -> decltype(ctx.out()) {
        return fmt::formatter<std::underlying_type_t<T>>::format(static_cast<std::underlying_type_t<T>>(value), ctx);
    }
};
#endif

#define SUYU_LOG_IMPL(level, ...) (std::fprintf(stderr, "shader %s: %s\n", level, fmt::format(__VA_ARGS__).c_str()))
#ifndef LOG_WARNING
#define LOG_TRACE(log_class, ...) ((void)0)
#define LOG_DEBUG(log_class, ...) ((void)0)
#define LOG_INFO(log_class, ...) ((void)0)
#define LOG_WARNING(log_class, ...) SUYU_LOG_IMPL("warning", __VA_ARGS__)
#define LOG_ERROR(log_class, ...) SUYU_LOG_IMPL("error", __VA_ARGS__)
#define LOG_CRITICAL(log_class, ...) SUYU_LOG_IMPL("critical", __VA_ARGS__)
#endif
