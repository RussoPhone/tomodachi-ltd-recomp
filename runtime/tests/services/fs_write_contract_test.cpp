// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstdio>
#include <vector>

#include "services/fs_write_options.h"

using namespace nxrt::hle::detail;

namespace {
int failures = 0;

void Check(bool condition, const char* label) {
    if (condition) return;
    std::printf("FAIL: %s\n", label);
    ++failures;
}
} // namespace

int main() {
    const std::uint8_t input[] = {1, 2, 3, 4, 5};
    std::vector<std::uint8_t> output;
    std::vector<std::int64_t> offsets;
    int flushes = 0;

    const auto partial_write = [&](const std::uint8_t* data, std::uint64_t size,
                                   std::int64_t offset) -> std::int64_t {
        const std::uint64_t count = size < 2 ? size : 2;
        offsets.push_back(offset);
        output.insert(output.end(), data, data + count);
        return static_cast<std::int64_t>(count);
    };
    const auto flush = [&] {
        ++flushes;
        return true;
    };

    Check(WriteFileWithOptions(0, input, sizeof(input), 10, partial_write, flush),
          "partial writes complete successfully");
    Check(output == std::vector<std::uint8_t>(input, input + sizeof(input)),
          "all requested bytes are written");
    Check(offsets == std::vector<std::int64_t>({10, 12, 14}),
          "partial writes advance the offset");
    Check(flushes == 0, "option zero does not flush");

    output.clear();
    offsets.clear();
    Check(WriteFileWithOptions(kWriteOptionFlush, input, sizeof(input), 20,
                               partial_write, flush),
          "flush option succeeds when flush succeeds");
    Check(flushes == 1, "flush option flushes after the entire write");

    int unexpected_flushes = 0;
    const auto unexpected_flush = [&] {
        ++unexpected_flushes;
        return true;
    };
    Check(WriteFileWithOptions(2, input, sizeof(input), 0, partial_write,
                               unexpected_flush),
          "unrecognized option bits do not request a flush");
    Check(unexpected_flushes == 0, "only the documented flush bit triggers flush");

    const auto short_failure = [](const std::uint8_t*, std::uint64_t,
                                  std::int64_t) -> std::int64_t { return 0; };
    Check(!WriteFileWithOptions(kWriteOptionFlush, input, sizeof(input), 0,
                                short_failure, unexpected_flush),
          "zero-byte write is an error");
    Check(unexpected_flushes == 0, "failed writes are not flushed");

    const auto flush_failure = [] { return false; };
    const auto full_write = [](const std::uint8_t*, std::uint64_t size,
                               std::int64_t) -> std::int64_t {
        return static_cast<std::int64_t>(size);
    };
    Check(!WriteFileWithOptions(kWriteOptionFlush, input, sizeof(input), 0,
                                full_write, flush_failure),
          "flush failure is returned to the IPC handler");

    std::printf(failures == 0 ? "ALL OK\n" : "FAILED: %d\n", failures);
    return failures != 0;
}
