// SPDX-License-Identifier: GPL-3.0-or-later
// The runtime's LOG_* macros. No include guard on purpose: a file that also
// includes vendored headers with their own LOG_* includes this again last.
// Ours win over any LOG_* a vendored header defined first.
#undef LOG_TRACE
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARN
#undef LOG_ERROR
#define LOG_TRACE(...) \
    do { if (::nxrt::LogEnabled(::nxrt::LogLevel::Trace)) ::nxrt::Log(::nxrt::LogLevel::Trace, __VA_ARGS__); } while (0)
#define LOG_DEBUG(...) \
    do { if (::nxrt::LogEnabled(::nxrt::LogLevel::Debug)) ::nxrt::Log(::nxrt::LogLevel::Debug, __VA_ARGS__); } while (0)
#define LOG_INFO(...) ::nxrt::Log(::nxrt::LogLevel::Info, __VA_ARGS__)
#define LOG_WARN(...) ::nxrt::Log(::nxrt::LogLevel::Warning, __VA_ARGS__)
#define LOG_ERROR(...) ::nxrt::Log(::nxrt::LogLevel::Error, __VA_ARGS__)
