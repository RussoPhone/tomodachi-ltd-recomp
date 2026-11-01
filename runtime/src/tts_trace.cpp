// SPDX-License-Identifier: GPL-3.0-or-later
#include "tts_trace.h"

#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <thread>

#include "common.h"
#include "cpu.h"
#include "kernel/kernel.h"

namespace nxrt::tts_trace {

namespace {

constexpr auto kPeriod = std::chrono::seconds(5);
constexpr std::size_t kMaxOpenLines = 3000;

std::mutex g_mutex;
FsStats* g_fs = nullptr;        // leaked on purpose: the reporter outlives static destruction
Counter* g_ipc = nullptr;
std::size_t g_open_lines = 0;
std::condition_variable g_cv;
bool g_report_now = false;
std::uint64_t g_mark_count = 0;
std::uint64_t g_window_start_ns = 0;
std::map<std::uint64_t, std::uint64_t> g_last_svc_count; // thread id -> count at the previous report

std::string WallClock() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

const char* SvcName(std::uint32_t imm) {
    switch (imm) {
    case 0x0B: return "SleepThread";
    case 0x18: return "WaitSynchronization";
    case 0x1A: return "ArbitrateLock";
    case 0x1B: return "ArbitrateUnlock";
    case 0x1C: return "WaitProcessWideKeyAtomic";
    case 0x1D: return "SignalProcessWideKey";
    case 0x21: return "SendSyncRequest";
    case 0x22: return "SendSyncRequestWithUserBuffer";
    case 0x34: return "WaitForAddress";
    case 0x35: return "SignalToAddress";
    default: return nullptr;
    }
}

std::string Ms(std::uint64_t ns) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.2f", static_cast<double>(ns) / 1e6);
    return b;
}

void ReportFs() {
    std::vector<std::pair<std::string, FileWindow>> files;
    {
        std::lock_guard lock(g_mutex);
        if (g_fs) files = g_fs->TakeWindow();
    }
    static const char* const names[kHistBuckets] = {"<=512", "<=4K", "<=32K", "<=256K", "<=1M", "<=4M", "<=16M", ">16M"};
    for (const auto& [key, w] : files) {
        std::string hist;
        for (int i = 0; i < kHistBuckets; ++i) {
            if (!w.hist[i]) continue;
            hist += std::string(hist.empty() ? "" : " ") + names[i] + ":" + std::to_string(w.hist[i]);
        }
        LOG_INFO("tts-fs %s reads=%llu bytes=%llu t_total=%sms t_max=%sms off=[%lld,%lld) cum_reads=%llu cum_bytes=%llu "
                 "cum_t=%sms sizes{%s}",
                 key.c_str(), static_cast<unsigned long long>(w.reads), static_cast<unsigned long long>(w.bytes),
                 Ms(w.ns_total).c_str(), Ms(w.ns_max).c_str(), static_cast<long long>(w.off_min),
                 static_cast<long long>(w.off_max), static_cast<unsigned long long>(w.cum_reads),
                 static_cast<unsigned long long>(w.cum_bytes), Ms(w.cum_ns).c_str(), hist.c_str());
    }
    if (files.empty()) LOG_INFO("tts-fs (sem leituras na janela)");
}

void ReportIpc(double seconds) {
    std::vector<std::pair<std::string, std::uint64_t>> top;
    {
        std::lock_guard lock(g_mutex);
        if (g_ipc) top = g_ipc->TakeTop(10);
    }
    for (const auto& [key, n] : top)
        LOG_INFO("tts-ipc %s n=%llu rate=%.1f/s", key.c_str(), static_cast<unsigned long long>(n),
                 seconds > 0 ? static_cast<double>(n) / seconds : 0.0);
    if (top.empty()) LOG_INFO("tts-ipc (sem comandos na janela)");
}

void ReportThreads() {
    struct Row {
        std::uint64_t id, entry, svc_start, svc_end, count, addr;
        std::uint32_t imm;
        std::int64_t timeout;
        std::string wait;
        bool running;
    };
    std::vector<Row> rows;
    {
        // wait_what is only written under the kernel lock.
        std::lock_guard lock(g_kernel.lock);
        for (const auto& t : g_kernel.threads) {
            if (!t->started || t->exited) continue;
            rows.push_back({t->id, t->entry, t->trace_svc_start_ns.load(), t->trace_svc_end_ns.load(),
                            t->trace_svc_count.load(), t->trace_svc_addr.load(), t->trace_svc_imm.load(),
                            t->trace_svc_timeout.load(), t->wait_what, true});
        }
    }
    const std::uint64_t now = NowNs();
    for (const Row& r : rows) {
        const bool in_svc = r.svc_start > r.svc_end;
        std::string state;
        std::uint64_t since;
        if (!in_svc) {
            state = "executando";
            since = r.svc_end ? r.svc_end : r.svc_start;
        } else {
            since = r.svc_start;
            if (!r.wait.empty()) {
                if (r.wait.rfind("address ", 0) == 0) state = "esperando endereco " + r.wait.substr(8);
                else if (r.wait.rfind("sync:", 0) == 0) state = "esperando evento/objeto:" + r.wait.substr(5);
                else if (r.wait.rfind("mutex ", 0) == 0) state = "mutex " + r.wait.substr(6);
                else state = r.wait; // condvar ...
            } else if (r.imm == 0x0B) {
                state = "dormindo";
            } else {
                char b[32];
                std::snprintf(b, sizeof(b), "dentro do svc 0x%02x", r.imm);
                state = b;
            }
        }
        std::uint64_t& last = g_last_svc_count[r.id];
        const std::uint64_t delta = r.count - last;
        last = r.count;
        const char* svc = SvcName(r.imm);
        char svcbuf[48];
        if (!svc) {
            std::snprintf(svcbuf, sizeof(svcbuf), "svc0x%02x", r.imm);
            svc = svcbuf;
        }
        char timeout[32];
        if (r.timeout < 0) std::snprintf(timeout, sizeof(timeout), "infinito");
        else std::snprintf(timeout, sizeof(timeout), "%lldns", static_cast<long long>(r.timeout));
        LOG_INFO("tts-thr id=%llu entry=%s estado=[%s] ha=%sms ultimo_svc=%s timeout=%s addr=0x%llx svcs_na_janela=%llu",
                 static_cast<unsigned long long>(r.id), Symbolize(r.entry).c_str(), state.c_str(),
                 since ? Ms(now - since).c_str() : "?", svc, timeout, static_cast<unsigned long long>(r.addr),
                 static_cast<unsigned long long>(delta));
    }
}

void Report(const char* why) {
    const std::uint64_t now = NowNs();
    const double seconds = g_window_start_ns ? static_cast<double>(now - g_window_start_ns) / 1e9 : 5.0;
    g_window_start_ns = now;
    LOG_INFO("tts-report %s hora=%s janela=%.1fs", why, WallClock().c_str(), seconds);
    ReportFs();
    ReportIpc(seconds);
    ReportThreads();
}

void ReporterLoop() {
    std::unique_lock lock(g_mutex);
    for (;;) {
        g_cv.wait_for(lock, kPeriod, [] { return g_report_now; });
        const bool forced = g_report_now;
        g_report_now = false;
        lock.unlock();
        Report(forced ? "marca" : "periodico");
        lock.lock();
    }
}

} // namespace

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Start() {
    if (!Enabled()) return;
    static std::once_flag once;
    std::call_once(once, [] {
        {
            std::lock_guard lock(g_mutex);
            g_fs = new FsStats;
            g_ipc = new Counter;
        }
        g_window_start_ns = NowNs();
        LOG_INFO("tts-trace ligado (fs/ipc/threads a cada 5 s; F10 marca) hora=%s", WallClock().c_str());
        std::thread(ReporterLoop).detach();
    });
}

void FsOpen(const std::string& description) {
    if (!Enabled()) return;
    std::lock_guard lock(g_mutex);
    if (g_open_lines >= kMaxOpenLines) return;
    if (++g_open_lines == kMaxOpenLines) LOG_INFO("fs-open: limite de %zu linhas atingido", kMaxOpenLines);
    LOG_INFO("fs-open %s", description.c_str());
}

void FsRead(const std::string& key, std::int64_t offset, std::uint64_t bytes, std::uint64_t ns) {
    if (!Enabled()) return;
    bool first = false;
    {
        std::lock_guard lock(g_mutex);
        if (!g_fs) return;
        first = g_fs->Record(key, offset, bytes, ns);
    }
    if (first) FsOpen(key + " (primeira leitura, offset=" + std::to_string(offset) + ")");
}

void IpcCount(const std::string& service, const char* command) {
    if (!Enabled()) return;
    std::lock_guard lock(g_mutex);
    if (g_ipc) g_ipc->Add(service + "::" + (command ? command : "?"));
}

void SvcEnter(KThread* t, std::uint32_t imm, const std::uint64_t* x) {
    std::uint64_t addr = 0;
    std::int64_t timeout = -1;
    switch (imm) {
    case 0x0B: timeout = static_cast<std::int64_t>(x[0]); break;
    case 0x18: addr = x[1]; timeout = static_cast<std::int64_t>(x[3]); break;
    case 0x1A: addr = x[1]; break;
    case 0x1C: addr = x[0]; timeout = static_cast<std::int64_t>(x[3]); break;
    case 0x21: addr = x[0]; break;
    case 0x22: addr = x[2]; break;
    case 0x34: addr = x[0]; timeout = static_cast<std::int64_t>(x[3]); break;
    default: break;
    }
    t->trace_svc_imm.store(imm, std::memory_order_relaxed);
    t->trace_svc_addr.store(addr, std::memory_order_relaxed);
    t->trace_svc_timeout.store(timeout, std::memory_order_relaxed);
    t->trace_svc_count.fetch_add(1, std::memory_order_relaxed);
    t->trace_svc_start_ns.store(NowNs(), std::memory_order_release);
}

void SvcLeave(KThread* t) {
    t->trace_svc_end_ns.store(NowNs(), std::memory_order_release);
}

void Mark(std::uint64_t frame) {
    if (!Enabled()) return;
    std::uint64_t n;
    {
        std::lock_guard lock(g_mutex);
        n = ++g_mark_count;
        g_report_now = true;
    }
    LOG_INFO("tts-mark #%llu hora=%s quadro=%llu", static_cast<unsigned long long>(n), WallClock().c_str(),
             static_cast<unsigned long long>(frame));
    g_cv.notify_all();
}

} // namespace nxrt::tts_trace
