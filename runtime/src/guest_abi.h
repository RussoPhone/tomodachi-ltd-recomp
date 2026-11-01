// SPDX-License-Identifier: GPL-3.0-or-later
// The contract between this runtime and the code emitted by the static recompiler
// (suyu's AOT exporter, as used by mk8-recomp). Every generated module is plain C
// that operates on a GuestContext; it reaches memory through RecompHostMem and
// asks for kernel services by parking an SVC number in pending_svc and returning.
//
// The layout must match recomp_runtime.h in the export byte for byte. The
// generated header pins the same offsets with its own assertions.
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

struct RecompHostMem {
    void* user;
    uint64_t (*load)(void* user, uint64_t va, uint32_t size);
    void (*store)(void* user, uint64_t va, uint32_t size, uint64_t value);
    uint64_t (*excl_load)(void* user, uint64_t va, uint32_t size);
    uint32_t (*excl_store)(void* user, uint64_t va, uint32_t size, uint64_t value);
    void (*clear_excl)(void* user);
    uint64_t (*read_cntpct)(void* user);
    void (*excl_load_pair)(void* user, uint64_t va, uint32_t size, uint64_t* lo, uint64_t* hi);
    uint32_t (*excl_store_pair)(void* user, uint64_t va, uint32_t size, uint64_t lo, uint64_t hi);
    const void* page_entries;
    uint64_t page_entry_stride;
    uint64_t page_bits;
    uint64_t pointer_mask;
    uint64_t address_space_max;
    const uint64_t* guard_generation; // reserved, must be null
};

struct GuestContext {
    uint64_t x[32]; // x[31] is SP
    uint64_t pc;
    uint8_t n, z, c, v;
    uint8_t* mem;
    uint64_t mem_size;
    uint64_t mem_base_vaddr;
    int halted;
    uint64_t pending_svc;
    uint64_t vreg[32][2];
    uint64_t tpidr_el0;
    const RecompHostMem* host_mem;
    uint64_t tpidrro_el0;
    uint64_t fpcr;
    uint64_t fpsr;
    int chain_budget;
    uint32_t fm_reserved;
    const unsigned char* fm_table;
    uint64_t fm_limit;
    // Fields used only by the generator's own standalone runtime.
    char save_dir[512];
    uint64_t heap_base, heap_end, heap_cur;
    uint32_t ipc_cmd[64];
    void* save_handles[16];
    int save_handle_count;
};

static_assert(offsetof(RecompHostMem, page_entries) == 72);
static_assert(offsetof(GuestContext, pending_svc) == 304);
static_assert(offsetof(GuestContext, vreg) == 312);
static_assert(offsetof(GuestContext, tpidr_el0) == 824);
static_assert(offsetof(GuestContext, host_mem) == 832);
static_assert(offsetof(GuestContext, tpidrro_el0) == 840);
static_assert(offsetof(GuestContext, chain_budget) == 864);
static_assert(offsetof(GuestContext, fm_table) == 872);
static_assert(offsetof(GuestContext, fm_limit) == 880);

using RecompBlockFn = void (*)(void*);

// Generated per game by the exporter (recomp_registration.c), listing the
// statically linked modules in NSO load order.
struct RecompStaticModule {
    const char* name;
    RecompBlockFn (*lookup)(uint64_t);
    void (*set_base)(uint64_t);
    RecompBlockFn run_slice;
    unsigned (*image_abi)(void);
};
const RecompStaticModule* suyu_recomp_static_modules_v4(unsigned* count);
int suyu_recomp_static_guard_v2(unsigned version);
int suyu_recomp_static_fastmem_v1(uint32_t page_bits, uint32_t stride_log2, uint64_t pointer_mask,
                                  uint32_t off_table, uint32_t off_limit);
unsigned suyu_recomp_static_features_v1(void);
struct RecompGuardGenModule {
    uint32_t* word;
    const uint64_t* base;
    uint64_t code_lo, code_end;
};
unsigned suyu_recomp_static_guard_gen_v1(uint32_t host_version, RecompGuardGenModule* out,
                                         unsigned max);
unsigned suyu_recomp_static_fpx_v1(uint32_t off_fpcr, uint32_t off_fpsr, uint64_t inhibit_bit);

} // extern "C"

namespace nxrt {
constexpr uint64_t kNoPendingSvc = ~uint64_t{0};
constexpr int kHaltStop = 1;
constexpr int kHaltUnhandled = 2;
constexpr int kHaltBreakpoint = 3;
constexpr int kHaltIcIvau = 4;
// Generated FP fast paths stay off while this bit is set in fpcr.
constexpr uint64_t kFpxInhibit = uint64_t{1} << 32;
// Fast-path page table contract (RECOMP_FM_*): 4 KiB pages, 32-byte entries,
// pointer in the first word with the low two bits reserved, watch word at +24.
constexpr uint32_t kPageBits = 12;
constexpr uint64_t kPageSize = uint64_t{1} << kPageBits;
constexpr uint32_t kPageEntryStrideLog2 = 5;
constexpr uint64_t kPagePointerMask = ~uint64_t{3};
} // namespace nxrt
