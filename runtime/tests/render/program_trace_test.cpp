#include "render/program_trace.h"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace nxrt;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    uint64_t prog = 0;
    std::string path;
    CHECK(ProgramTrace::ParseSpec("2044e14ad8", "/x/census.csv", prog, path) && prog == 0x2044e14ad8ULL &&
          path == "/x/census.csv.program.csv");
    CHECK(ProgramTrace::ParseSpec("2044e14ad8", nullptr, prog, path) && path == "/tmp/trace-program.csv");
    CHECK(ProgramTrace::ParseSpec("a,/tmp/dir", nullptr, prog, path) && path == "/tmp/dir/program.csv");
    CHECK(ProgramTrace::ParseSpec("a,/tmp/f.csv", nullptr, prog, path) && path == "/tmp/f.csv");
    CHECK(!ProgramTrace::ParseSpec("zz", nullptr, prog, path) && !ProgramTrace::ParseSpec("", nullptr, prog, path));
    CHECK(ProgramTraceHash("a", 1) != ProgramTraceHash("b", 1));

    const std::string file = "/tmp/program_trace_test.csv";
    {
        ProgramTrace t(0x2044e14ad8ULL, file);
        std::vector<uint8_t> mem(0x1500, 0), out(0x1500, 0);
        ProgramTrace::Dispatch d;
        d.frame = 7; d.index = 1; d.c3_hash = 0xabc; d.c3_43z = 0x11; d.ssbo_va = 0x6604e8080; d.ssbo_cpu = 1000;
        d.ssbo_size = mem.size();
        t.Begin(d, mem.data(), mem.size());
        out[0x14C0] = 0x11;
        t.Writeback(1, 1000, out.data(), out.size());
        t.OnGuestRead(1000 + 0x14C0, 4);
        // the guest changes a byte between dispatches
        mem = out;
        mem[0x20] = 0x5a;
        d.frame = 8; d.index = 2;
        t.Begin(d, mem.data(), mem.size());
        t.Writeback(2, 1000, out.data(), out.size());
    }
    std::ifstream in(file);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    CHECK(text.find("D,7,1,0000000000000abc,00000011,00000000:00000000:00000000:00000000,6604e8080,5376,"
                    "00000000000000000000000000000000,00000000,00000011,") != std::string::npos);
    CHECK(text.find("E,7,guest_read,14c0,4,00000011") != std::string::npos);
    CHECK(text.find("E,8,guest_write,20,1,") != std::string::npos);
    std::remove(file.c_str());
    return failures ? 1 : 0;
}
