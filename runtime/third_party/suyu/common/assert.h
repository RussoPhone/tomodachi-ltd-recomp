// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt shim for suyu's assertion macros.
#pragma once

#include <cstdio>
#include <cstdlib>

#define ASSERT(cond) do { if (!(cond)) { std::fprintf(stderr, "assertion failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); std::abort(); } } while (0)
#define ASSERT_MSG(cond, ...) ASSERT(cond)
#define UNREACHABLE() std::abort()
#define UNREACHABLE_MSG(...) std::abort()
#define UNIMPLEMENTED() std::abort()
#define UNIMPLEMENTED_MSG(...) std::abort()
#define DEBUG_ASSERT(cond) ((void)0)
