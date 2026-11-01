// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt shim: a minimal Horizon Result for the vendored suyu code.
#pragma once

#include "common/common_types.h"

enum class ErrorModule : u32 {
    Mii = 126,
};

struct Result {
    u32 raw = 0;
    constexpr Result() = default;
    constexpr Result(ErrorModule module, u32 description)
        : raw(static_cast<u32>(module) | (description << 9)) {}
    constexpr bool IsSuccess() const { return raw == 0; }
    constexpr bool IsError() const { return raw != 0; }
    constexpr bool operator==(const Result&) const = default;
};

constexpr Result ResultSuccess{};

#define R_SUCCEED() return ResultSuccess
#define R_RETURN(expr) return (expr)
#define R_THROW(expr) return (expr)
#define R_UNLESS(cond, res) do { if (!(cond)) return (res); } while (0)
#define R_SUCCEED_IF(cond) do { if (cond) return ResultSuccess; } while (0)
#define R_TRY(expr) do { const Result r_try_ = (expr); if (r_try_.IsError()) return r_try_; } while (0)
