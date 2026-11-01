#include <cstdlib>

#include "render/process_memory_status.h"

int main() {
    nxrt::render::ProcessMemoryStatus status{};

    if (!nxrt::render::ParseProcessMemoryStatusLine("VmRSS:\t2048 kB", status) || !status.has_rss ||
        status.rss_kib != 2048) {
        return EXIT_FAILURE;
    }

    if (!nxrt::render::ParseProcessMemoryStatusLine("VmSwap: 512 kB", status) || !status.has_swap ||
        status.swap_kib != 512) {
        return EXIT_FAILURE;
    }

    if (nxrt::render::ParseProcessMemoryStatusLine("Threads: 9", status) || status.rss_kib != 2048 ||
        status.swap_kib != 512) {
        return EXIT_FAILURE;
    }
    if (nxrt::render::ParseProcessMemoryStatusLine("VmRSS: 12 MB", status) || status.rss_kib != 2048) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
