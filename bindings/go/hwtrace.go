// Hardware-tier native runtime tracing for Go (single-step / Intel PT / AMD).
//
// This is the Go counterpart of the Python wrapper (bindings/python/asmtest/
// hwtrace.py) and the C API in include/asmtest_hwtrace.h. Like the DynamoRIO tier
// (drtrace.go) it traces host-native code as it runs **inside this Go process** —
// initialize once, materialize host-native machine code, mark a region, call into
// it, and read back basic-block coverage / the instruction stream — but it
// observes the **real CPU** and, unlike the DynamoRIO wrapper, needs no DynamoRIO
// install.
//
// Four backends share one API, selected by enum. SINGLESTEP (EFLAGS.TF #DB ->
// SIGTRAP) is the portable default: exact and complete on ANY x86-64 Linux
// (Intel, any-Zen AMD, VM, CI, container) with no PMU, no perf_event, no
// privilege, and no decoder library — so it is what this binding's self-test
// exercises live. INTEL_PT / CORESIGHT / AMD_LBR are hardware branch-trace
// backends that self-skip off the specific bare-metal hardware they need.
//
// The hardware-trace tier lives in its own shared library, libasmtest_hwtrace,
// which is built separately (`make shared-hwtrace`) and so may be absent. Exactly
// like the DynamoRIO wrapper, this file therefore does NOT link the lib: it
// dlopen()s it at RUN TIME — from $ASMTEST_HWTRACE_LIB, else
// <cwd>/build/libasmtest_hwtrace.so — and dlsym()s the entry points into static
// function pointers. If the lib can't be resolved, HwTraceAvailable() returns
// false and the whole tier self-skips cleanly.
//
// libasmtest_hwtrace re-exports the trace handle + accessors (asmtest_trace_new
// and friends) from the same trace.o the emulator uses, so coverage is read back
// in the identical {Covered, BlocksLen, InsnsTotal} shape.
package asmtest

/*
#cgo linux LDFLAGS: -ldl
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <dlfcn.h>

// The one struct the API takes by pointer, redefined here so this file needs no
// header on the cgo include path (mirrors asmtest_hwtrace_options_t from
// include/asmtest_hwtrace.h). ASMTEST_HW_OK == 0. Default backend SINGLESTEP=3.
typedef struct {
    size_t struct_size; // ABI size negotiator (F27) — ALWAYS set to sizeof
    int backend;
    size_t aux_size;
    size_t data_size;
    int snapshot;
    const char *object_hint;
    int lbr_period;    // AMD LBR opt-in (0 = default sample_period=1)
    int branch_filter; // AMD LBR opt-in (0 = default BRANCH_ANY)
} asmtest_hwtrace_options_t;

// F29 machine-readable status (mirrors asmtest_hwtrace_status_t): code
// distinguishes EPERM (substrate present, permission denied) from EUNAVAIL.
typedef struct {
    int available;
    int code;
    int stage;
    int perf_event_paranoid;
    int probe_errno;
    char reason[160];
} asmtest_hwtrace_status_t;

// The cross-tier orchestrator's resolved-choice struct, redefined here (mirrors
// asmtest_trace_choice_t from include/asmtest_trace_auto.h): exactly four
// int-sized enum fields, no padding, so it marshals as four consecutive C ints
// (tier, backend, fidelity, mechanism — the 2026-07 flag day grew it).
typedef struct {
    int tier;
    int backend;
    int fidelity;
    int mechanism;
} asmtest_trace_choice_t;

// asmtest_hwtrace_scope_t (include/asmtest_hwtrace.h): an 8-byte per-scope capture
// handle. render_scope takes it BY VALUE (cgo marshals it directly — no packing).
typedef struct {
    uint32_t idx;
    uint32_t gen;
} asmtest_hwtrace_scope_t;

// Entry-point typedefs (one per exported symbol in libasmtest_hwtrace).
typedef int  (*hw_available_fn)(int);
typedef void (*hw_skip_reason_fn)(int, char *, size_t);
typedef int  (*hw_status_fn)(int, asmtest_hwtrace_status_t *);
typedef int  (*hw_paranoid_fn)(void);
typedef size_t (*hw_resolve_fn)(int, int *, size_t);
typedef int  (*hw_auto_fn)(int);
// Cross-tier orchestrator entry points (asmtest_trace_auto.h), from the SAME lib.
typedef size_t (*hw_trace_resolve_fn)(unsigned, asmtest_trace_choice_t *, size_t);
typedef int    (*hw_trace_auto_fn)(unsigned, asmtest_trace_choice_t *);
// Auto-escalating CALL-OWNING cross-tier trace (asmtest_trace_call_auto): owns the
// invocation, runs it under the fastest exact tier, and re-runs on a ceiling-free
// tier when the trace truncates. Self-manages the tier lifecycle internally.
typedef int    (*hw_trace_call_auto_fn)(const void *, size_t, const long *, int,
                                        unsigned, long *, void *,
                                        asmtest_trace_choice_t *);
typedef int  (*hw_init_fn)(const asmtest_hwtrace_options_t *);
typedef int  (*hw_register_fn)(const char *, void *, size_t, void *);
typedef void (*hw_marker_fn)(const char *);
typedef int  (*hw_try_begin_fn)(const char *);
typedef int  (*hw_render_fn)(const char *, char *, size_t);
typedef void (*hw_shutdown_fn)(void);
typedef int  (*hw_arm_tid_fn)(void);
typedef int  (*hw_exec_alloc_fn)(const void *, size_t, void **, size_t *);
typedef void (*hw_exec_free_fn)(void *, size_t);
typedef void *(*hw_trace_new_fn)(size_t, size_t);
typedef void  (*hw_trace_free_fn)(void *);
typedef int   (*hw_trace_covered_fn)(void *, uint64_t);
typedef unsigned long long (*hw_trace_blocks_len_fn)(void *);
typedef unsigned long long (*hw_trace_insns_total_fn)(void *);
typedef unsigned long long (*hw_trace_insns_len_fn)(void *);
typedef int                (*hw_trace_truncated_fn)(void *);
typedef unsigned long long (*hw_trace_block_at_fn)(void *, size_t);
typedef unsigned long long (*hw_trace_insn_at_fn)(void *, size_t);
// §1 registry-free lazy-arm call + handle-keyed render (the call_scoped path).
typedef int (*hw_call_scoped_ex_fn)(void *, size_t, void *, void *, const long *,
                                    int, long *, asmtest_hwtrace_scope_t *);
typedef int (*hw_render_scope_fn)(asmtest_hwtrace_scope_t, char *, size_t);

// The jitdump entry struct, redefined here (mirrors asmtest_jitdump_entry_t from
// include/asmtest_ptrace.h): exactly four uint64 fields, no padding, so it
// marshals as four consecutive little-endian u64s — a JIT method's load address,
// size, timestamp, and the JIT's unique index.
typedef struct {
    uint64_t code_addr;
    uint64_t code_size;
    uint64_t timestamp;
    uint64_t code_index;
} asmtest_jitdump_entry_t;

// The eBPF emission-event struct, redefined here (mirrors asmtest_codeimage_event_t
// from include/asmtest_codeimage.h): three u64s, three u32s, one i32 — no padding,
// so it marshals as a 40-byte record matching the kernel-side struct.
typedef struct {
    uint64_t addr;      // published base address (0 for a memfd hint)
    uint64_t len;       // byte length (0 for a memfd hint)
    uint64_t timestamp; // bpf_ktime_get_ns() at emission
    uint32_t pid;       // tgid that published
    uint32_t tid;       // thread that published
    uint32_t kind;      // ASMTEST_CI_KIND_*
    int32_t  fd;        // memfd fd, or -1
} asmtest_codeimage_event_t;

// asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit, from the
// SAME libasmtest_hwtrace. One typedef per exported symbol.
typedef int  (*pt_available_fn)(void);
typedef void (*pt_skip_reason_fn)(char *, size_t);
typedef int  (*pt_trace_call_fn)(const void *, size_t, const long *, int, long *, void *);
typedef int  (*pt_trace_attached_fn)(int, const void *, size_t, long *, void *);
typedef int  (*pt_trace_attached_versioned_fn)(int, const void *, size_t, void *, uint64_t, long *, void *);
typedef int  (*pt_run_to_fn)(int, const void *);
typedef int  (*proc_region_by_addr_fn)(int, const void *, void **, size_t *);
typedef int  (*proc_perfmap_symbol_fn)(int, const char *, void **, size_t *);
typedef int  (*jitdump_find_fn)(const char *, int, const char *, asmtest_jitdump_entry_t *, uint8_t *, size_t, size_t *);

// asmtest_codeimage.h — time-aware code-image recorder, from the SAME
// libasmtest_hwtrace. One typedef per exported symbol.
typedef int      (*ci_available_fn)(void);
typedef void     (*ci_skip_reason_fn)(char *, size_t);
typedef void *   (*ci_new_fn)(int);
typedef void     (*ci_free_fn)(void *);
typedef int      (*ci_track_fn)(void *, const void *, size_t);
typedef int      (*ci_refresh_fn)(void *);
typedef uint64_t (*ci_now_fn)(const void *);
typedef int      (*ci_bytes_at_fn)(const void *, const void *, uint64_t, const uint8_t **, size_t *);
typedef int      (*ci_bpf_available_fn)(void);
typedef void     (*ci_bpf_skip_reason_fn)(char *, size_t);
typedef int      (*ci_watch_bpf_fn)(void *);
typedef int      (*ci_poll_bpf_fn)(void *, int);
typedef int      (*ci_next_fn)(void *, asmtest_codeimage_event_t *);

// asmtest_ptrace.h — call descent (asmtest_descent_t): edges + nested frames, plus
// the three descending _ex trace entry points. From the SAME libasmtest_hwtrace. The
// resolver/denylist upcalls are C-callable Go functions (//export goDescent*Tramp in
// descent_cgo.go); declared extern here so the set-callback bridges below can install
// them, and so the linker binds them across the two cgo-generated C files.
extern int goDescentResolverTramp(uint64_t callee, void *user, uint64_t *base_out, uint64_t *len_out);
extern int goDescentDenylistTramp(uint64_t callee, void *user);

typedef void *   (*desc_new_fn)(int);
typedef void     (*desc_free_fn)(void *);
typedef void     (*desc_set_u32_fn)(void *, uint32_t);
typedef void     (*desc_set_u64_fn)(void *, uint64_t);
typedef int      (*desc_region_fn)(void *, const void *, size_t);
typedef void     (*desc_set_resolver_fn)(void *, int (*)(uint64_t, void *, uint64_t *, uint64_t *), void *);
typedef void     (*desc_set_denylist_fn)(void *, int (*)(uint64_t, void *), void *);
typedef size_t   (*desc_len_fn)(void *);
typedef uint64_t (*desc_u64_idx_fn)(void *, size_t);
typedef uint32_t (*desc_u32_idx_fn)(void *, size_t);
typedef int32_t  (*desc_i32_idx_fn)(void *, size_t);
typedef size_t   (*desc_count2_fn)(void *, size_t);
typedef uint64_t (*desc_u64_idx2_fn)(void *, size_t, size_t);
typedef int      (*desc_flag_fn)(void *);
typedef int (*pt_trace_call_ex_fn)(const void *, size_t, const long *, int, long *, void *, void *);
typedef int (*pt_trace_attached_ex_fn)(int, const void *, size_t, long *, void *, void *);
typedef int (*pt_trace_attached_versioned_ex_fn)(int, const void *, size_t, void *, uint64_t, long *, void *, void *);

static hw_available_fn        p_hw_available;
static hw_skip_reason_fn      p_hw_skip_reason;
static hw_status_fn           p_hw_status;
static hw_paranoid_fn         p_hw_paranoid;
static hw_resolve_fn          p_hw_resolve;
static hw_auto_fn             p_hw_auto;
static hw_trace_resolve_fn    p_trace_resolve;
static hw_trace_auto_fn       p_trace_auto;
static hw_trace_call_auto_fn  p_trace_call_auto;
static hw_init_fn             p_hw_init;
static hw_register_fn         p_hw_register;
static hw_marker_fn           p_hw_begin;
static hw_marker_fn           p_hw_end;
static hw_try_begin_fn        p_hw_try_begin;
static hw_render_fn           p_hw_render;
static hw_shutdown_fn         p_hw_shutdown;
static hw_arm_tid_fn          p_hw_arm_tid;
static hw_exec_alloc_fn       p_hw_exec_alloc;
static hw_exec_free_fn        p_hw_exec_free;
static hw_trace_new_fn        p_hw_trace_new;
static hw_trace_free_fn       p_hw_trace_free;
static hw_trace_covered_fn    p_hw_trace_covered;
static hw_trace_blocks_len_fn p_hw_trace_blocks_len;
static hw_trace_insns_total_fn p_hw_trace_insns_total;
static hw_trace_insns_len_fn  p_hw_trace_insns_len;
static hw_trace_truncated_fn  p_hw_trace_truncated;
static hw_trace_block_at_fn   p_hw_trace_block_at;
static hw_trace_insn_at_fn    p_hw_trace_insn_at;
static hw_call_scoped_ex_fn   p_hw_call_scoped_ex;
static hw_render_scope_fn     p_hw_render_scope;
// asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit.
static pt_available_fn        p_pt_available;
static pt_skip_reason_fn      p_pt_skip_reason;
static pt_trace_call_fn       p_pt_trace_call;
// BTF block-step tier: same shapes as the per-instruction trio above.
static pt_available_fn        p_pt_blockstep_available;
static pt_trace_call_fn       p_pt_trace_call_blockstep;
static pt_trace_attached_fn   p_pt_trace_attached_blockstep;
static pt_trace_attached_fn   p_pt_trace_attached;
static pt_trace_attached_versioned_fn p_pt_trace_attached_versioned;
static pt_run_to_fn           p_pt_run_to;
static proc_region_by_addr_fn p_proc_region_by_addr;
static proc_perfmap_symbol_fn p_proc_perfmap_symbol;
static jitdump_find_fn        p_jitdump_find;
// asmtest_codeimage.h — time-aware code-image recorder.
static ci_available_fn        p_ci_available;
static ci_skip_reason_fn      p_ci_skip_reason;
static ci_new_fn              p_ci_new;
static ci_free_fn             p_ci_free;
static ci_track_fn            p_ci_track;
static ci_refresh_fn          p_ci_refresh;
static ci_now_fn              p_ci_now;
static ci_bytes_at_fn         p_ci_bytes_at;
static ci_bpf_available_fn    p_ci_bpf_available;
static ci_bpf_skip_reason_fn  p_ci_bpf_skip_reason;
static ci_watch_bpf_fn        p_ci_watch_bpf;
static ci_poll_bpf_fn         p_ci_poll_bpf;
static ci_next_fn             p_ci_next;
// asmtest_ptrace.h — call descent (asmtest_descent_t) + descending _ex entry points.
static desc_new_fn            p_desc_new;
static desc_free_fn           p_desc_free;
static desc_set_u32_fn        p_desc_set_max_depth;
static desc_set_u64_fn        p_desc_set_insn_budget;
static desc_set_u32_fn        p_desc_set_watchdog_ms;
static desc_free_fn           p_desc_use_default_denylist;
static desc_region_fn         p_desc_allow_region;
static desc_region_fn         p_desc_deny_region;
static desc_set_resolver_fn   p_desc_set_resolver;
static desc_set_denylist_fn   p_desc_set_denylist;
static desc_len_fn            p_desc_edges_len;
static desc_u64_idx_fn        p_desc_edge_site;
static desc_u64_idx_fn        p_desc_edge_target;
static desc_u32_idx_fn        p_desc_edge_depth;
static desc_len_fn            p_desc_frames_len;
static desc_u64_idx_fn        p_desc_frame_base;
static desc_u64_idx_fn        p_desc_frame_len;
static desc_u32_idx_fn        p_desc_frame_depth;
static desc_i32_idx_fn        p_desc_frame_parent;
static desc_count2_fn         p_desc_frame_insn_count;
static desc_u64_idx2_fn       p_desc_frame_insn_at;
static desc_count2_fn         p_desc_frame_block_count;
static desc_u64_idx2_fn       p_desc_frame_block_at;
static desc_flag_fn           p_desc_truncated;
static desc_flag_fn           p_desc_depth_capped;
static pt_trace_call_ex_fn    p_pt_trace_call_ex;
static pt_trace_attached_ex_fn p_pt_trace_attached_ex;
static pt_trace_attached_versioned_ex_fn p_pt_trace_attached_versioned_ex;

static int g_hw_loaded;   // 1 once dlopen + every dlsym succeeded.
static char g_hw_path[4096]; // the lib string this process dlopen()ed (for LibraryPath).

// Resolve libasmtest_hwtrace from $ASMTEST_HWTRACE_LIB, else
// build/libasmtest_hwtrace.so (relative to the cwd, like the Python loader's last
// candidate). dlopen it RTLD_GLOBAL so the trampolines below find the symbols,
// then dlsym each entry point. On any miss the tier stays unloaded and
// asmtest_hw_is_loaded() is 0, so the binding self-skips rather than crashing.
// Idempotent.
static void asmtest_hw_resolve(void) {
    if (g_hw_loaded) return;
    const char *env = getenv("ASMTEST_HWTRACE_LIB");
    const char *name = env && *env ? env : "build/libasmtest_hwtrace.so";
    void *h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (!h) return;
    // Record the string we resolved (env override first, else the build/ default)
    // so LibraryPath() can report which candidate satisfied the load. This is the
    // string passed to dlopen — a source distribution has no bundled native/ dir.
    snprintf(g_hw_path, sizeof(g_hw_path), "%s", name);
    p_hw_available       = (hw_available_fn)dlsym(h, "asmtest_hwtrace_available");
    p_hw_skip_reason     = (hw_skip_reason_fn)dlsym(h, "asmtest_hwtrace_skip_reason");
    p_hw_status          = (hw_status_fn)dlsym(h, "asmtest_hwtrace_status");
    p_hw_paranoid        = (hw_paranoid_fn)dlsym(h, "asmtest_hwtrace_perf_event_paranoid");
    p_hw_resolve         = (hw_resolve_fn)dlsym(h, "asmtest_hwtrace_resolve");
    p_hw_auto            = (hw_auto_fn)dlsym(h, "asmtest_hwtrace_auto");
    p_trace_resolve      = (hw_trace_resolve_fn)dlsym(h, "asmtest_trace_resolve");
    p_trace_auto         = (hw_trace_auto_fn)dlsym(h, "asmtest_trace_auto");
    p_trace_call_auto    = (hw_trace_call_auto_fn)dlsym(h, "asmtest_trace_call_auto");
    p_hw_init            = (hw_init_fn)dlsym(h, "asmtest_hwtrace_init");
    p_hw_register        = (hw_register_fn)dlsym(h, "asmtest_hwtrace_register_region");
    p_hw_begin           = (hw_marker_fn)dlsym(h, "asmtest_hwtrace_begin");
    p_hw_end             = (hw_marker_fn)dlsym(h, "asmtest_hwtrace_end");
    p_hw_try_begin       = (hw_try_begin_fn)dlsym(h, "asmtest_hwtrace_try_begin");
    p_hw_render          = (hw_render_fn)dlsym(h, "asmtest_hwtrace_render");
    p_hw_shutdown        = (hw_shutdown_fn)dlsym(h, "asmtest_hwtrace_shutdown");
    p_hw_arm_tid         = (hw_arm_tid_fn)dlsym(h, "asmtest_hwtrace_arm_tid");
    p_hw_exec_alloc      = (hw_exec_alloc_fn)dlsym(h, "asmtest_hwtrace_exec_alloc");
    p_hw_exec_free       = (hw_exec_free_fn)dlsym(h, "asmtest_hwtrace_exec_free");
    p_hw_trace_new       = (hw_trace_new_fn)dlsym(h, "asmtest_trace_new");
    p_hw_trace_free      = (hw_trace_free_fn)dlsym(h, "asmtest_trace_free");
    p_hw_trace_covered   = (hw_trace_covered_fn)dlsym(h, "asmtest_trace_covered");
    p_hw_trace_blocks_len  = (hw_trace_blocks_len_fn)dlsym(h, "asmtest_emu_trace_blocks_len");
    p_hw_trace_insns_total = (hw_trace_insns_total_fn)dlsym(h, "asmtest_emu_trace_insns_total");
    p_hw_trace_insns_len   = (hw_trace_insns_len_fn)dlsym(h, "asmtest_emu_trace_insns_len");
    p_hw_trace_truncated   = (hw_trace_truncated_fn)dlsym(h, "asmtest_emu_trace_truncated");
    p_hw_trace_block_at    = (hw_trace_block_at_fn)dlsym(h, "asmtest_emu_trace_block_at");
    p_hw_trace_insn_at     = (hw_trace_insn_at_fn)dlsym(h, "asmtest_emu_trace_insn_at");
    p_hw_call_scoped_ex    = (hw_call_scoped_ex_fn)dlsym(h, "asmtest_hwtrace_call_scoped_ex");
    p_hw_render_scope      = (hw_render_scope_fn)dlsym(h, "asmtest_hwtrace_render_scope");
    // asmtest_ptrace.h — resolved from the same already-loaded handle.
    p_pt_available         = (pt_available_fn)dlsym(h, "asmtest_ptrace_available");
    p_pt_skip_reason       = (pt_skip_reason_fn)dlsym(h, "asmtest_ptrace_skip_reason");
    p_pt_trace_call        = (pt_trace_call_fn)dlsym(h, "asmtest_ptrace_trace_call");
    p_pt_blockstep_available  = (pt_available_fn)dlsym(h, "asmtest_ptrace_blockstep_available");
    p_pt_trace_call_blockstep = (pt_trace_call_fn)dlsym(h, "asmtest_ptrace_trace_call_blockstep");
    p_pt_trace_attached_blockstep = (pt_trace_attached_fn)dlsym(h, "asmtest_ptrace_trace_attached_blockstep");
    p_pt_trace_attached    = (pt_trace_attached_fn)dlsym(h, "asmtest_ptrace_trace_attached");
    p_pt_trace_attached_versioned = (pt_trace_attached_versioned_fn)dlsym(h, "asmtest_ptrace_trace_attached_versioned");
    p_pt_run_to            = (pt_run_to_fn)dlsym(h, "asmtest_ptrace_run_to");
    p_proc_region_by_addr  = (proc_region_by_addr_fn)dlsym(h, "asmtest_proc_region_by_addr");
    p_proc_perfmap_symbol  = (proc_perfmap_symbol_fn)dlsym(h, "asmtest_proc_perfmap_symbol");
    p_jitdump_find         = (jitdump_find_fn)dlsym(h, "asmtest_jitdump_find");
    // asmtest_codeimage.h — resolved from the same already-loaded handle.
    p_ci_available         = (ci_available_fn)dlsym(h, "asmtest_codeimage_available");
    p_ci_skip_reason       = (ci_skip_reason_fn)dlsym(h, "asmtest_codeimage_skip_reason");
    p_ci_new               = (ci_new_fn)dlsym(h, "asmtest_codeimage_new");
    p_ci_free              = (ci_free_fn)dlsym(h, "asmtest_codeimage_free");
    p_ci_track             = (ci_track_fn)dlsym(h, "asmtest_codeimage_track");
    p_ci_refresh           = (ci_refresh_fn)dlsym(h, "asmtest_codeimage_refresh");
    p_ci_now               = (ci_now_fn)dlsym(h, "asmtest_codeimage_now");
    p_ci_bytes_at          = (ci_bytes_at_fn)dlsym(h, "asmtest_codeimage_bytes_at");
    p_ci_bpf_available     = (ci_bpf_available_fn)dlsym(h, "asmtest_codeimage_bpf_available");
    p_ci_bpf_skip_reason   = (ci_bpf_skip_reason_fn)dlsym(h, "asmtest_codeimage_bpf_skip_reason");
    p_ci_watch_bpf         = (ci_watch_bpf_fn)dlsym(h, "asmtest_codeimage_watch_bpf");
    p_ci_poll_bpf          = (ci_poll_bpf_fn)dlsym(h, "asmtest_codeimage_poll_bpf");
    p_ci_next              = (ci_next_fn)dlsym(h, "asmtest_codeimage_next");
    // asmtest_ptrace.h — call descent + descending _ex entry points, same handle.
    p_desc_new             = (desc_new_fn)dlsym(h, "asmtest_descent_new");
    p_desc_free            = (desc_free_fn)dlsym(h, "asmtest_descent_free");
    p_desc_set_max_depth   = (desc_set_u32_fn)dlsym(h, "asmtest_descent_set_max_depth");
    p_desc_set_insn_budget = (desc_set_u64_fn)dlsym(h, "asmtest_descent_set_insn_budget");
    p_desc_set_watchdog_ms = (desc_set_u32_fn)dlsym(h, "asmtest_descent_set_watchdog_ms");
    p_desc_use_default_denylist = (desc_free_fn)dlsym(h, "asmtest_descent_use_default_denylist");
    p_desc_allow_region    = (desc_region_fn)dlsym(h, "asmtest_descent_allow_region");
    p_desc_deny_region     = (desc_region_fn)dlsym(h, "asmtest_descent_deny_region");
    p_desc_set_resolver    = (desc_set_resolver_fn)dlsym(h, "asmtest_descent_set_resolver");
    p_desc_set_denylist    = (desc_set_denylist_fn)dlsym(h, "asmtest_descent_set_denylist");
    p_desc_edges_len       = (desc_len_fn)dlsym(h, "asmtest_descent_edges_len");
    p_desc_edge_site       = (desc_u64_idx_fn)dlsym(h, "asmtest_descent_edge_site");
    p_desc_edge_target     = (desc_u64_idx_fn)dlsym(h, "asmtest_descent_edge_target");
    p_desc_edge_depth      = (desc_u32_idx_fn)dlsym(h, "asmtest_descent_edge_depth");
    p_desc_frames_len      = (desc_len_fn)dlsym(h, "asmtest_descent_frames_len");
    p_desc_frame_base      = (desc_u64_idx_fn)dlsym(h, "asmtest_descent_frame_base");
    p_desc_frame_len       = (desc_u64_idx_fn)dlsym(h, "asmtest_descent_frame_len");
    p_desc_frame_depth     = (desc_u32_idx_fn)dlsym(h, "asmtest_descent_frame_depth");
    p_desc_frame_parent    = (desc_i32_idx_fn)dlsym(h, "asmtest_descent_frame_parent");
    p_desc_frame_insn_count  = (desc_count2_fn)dlsym(h, "asmtest_descent_frame_insn_count");
    p_desc_frame_insn_at     = (desc_u64_idx2_fn)dlsym(h, "asmtest_descent_frame_insn_at");
    p_desc_frame_block_count = (desc_count2_fn)dlsym(h, "asmtest_descent_frame_block_count");
    p_desc_frame_block_at    = (desc_u64_idx2_fn)dlsym(h, "asmtest_descent_frame_block_at");
    p_desc_truncated       = (desc_flag_fn)dlsym(h, "asmtest_descent_truncated");
    p_desc_depth_capped    = (desc_flag_fn)dlsym(h, "asmtest_descent_depth_capped");
    p_pt_trace_call_ex     = (pt_trace_call_ex_fn)dlsym(h, "asmtest_ptrace_trace_call_ex");
    p_pt_trace_attached_ex = (pt_trace_attached_ex_fn)dlsym(h, "asmtest_ptrace_trace_attached_ex");
    p_pt_trace_attached_versioned_ex = (pt_trace_attached_versioned_ex_fn)dlsym(h, "asmtest_ptrace_trace_attached_versioned_ex");
    g_hw_loaded = p_hw_available && p_hw_skip_reason && p_hw_resolve &&
                  p_hw_auto && p_trace_resolve && p_trace_auto &&
                  p_trace_call_auto && p_hw_init &&
                  p_hw_register && p_hw_begin && p_hw_end && p_hw_shutdown &&
                  p_hw_exec_alloc && p_hw_exec_free && p_hw_trace_new &&
                  p_hw_trace_free && p_hw_trace_covered && p_hw_trace_blocks_len &&
                  p_hw_trace_insns_total && p_hw_trace_insns_len &&
                  p_hw_trace_truncated && p_hw_trace_block_at && p_hw_trace_insn_at &&
                  p_hw_call_scoped_ex && p_hw_render_scope &&
                  p_pt_available && p_pt_skip_reason && p_pt_trace_call &&
                  p_pt_blockstep_available && p_pt_trace_call_blockstep &&
                  p_pt_trace_attached_blockstep &&
                  p_pt_trace_attached && p_pt_trace_attached_versioned &&
                  p_pt_run_to && p_proc_region_by_addr &&
                  p_proc_perfmap_symbol && p_jitdump_find &&
                  p_ci_available && p_ci_skip_reason && p_ci_new && p_ci_free &&
                  p_ci_track && p_ci_refresh && p_ci_now && p_ci_bytes_at &&
                  p_ci_bpf_available && p_ci_bpf_skip_reason && p_ci_watch_bpf &&
                  p_ci_poll_bpf && p_ci_next &&
                  p_desc_new && p_desc_free && p_desc_set_max_depth &&
                  p_desc_set_insn_budget && p_desc_set_watchdog_ms &&
                  p_desc_use_default_denylist &&
                  p_desc_allow_region && p_desc_deny_region &&
                  p_desc_set_resolver && p_desc_set_denylist &&
                  p_desc_edges_len && p_desc_edge_site && p_desc_edge_target &&
                  p_desc_edge_depth && p_desc_frames_len && p_desc_frame_base &&
                  p_desc_frame_len && p_desc_frame_depth && p_desc_frame_parent &&
                  p_desc_frame_insn_count && p_desc_frame_insn_at &&
                  p_desc_frame_block_count && p_desc_frame_block_at &&
                  p_desc_truncated && p_desc_depth_capped &&
                  p_pt_trace_call_ex && p_pt_trace_attached_ex &&
                  p_pt_trace_attached_versioned_ex;
}

static int asmtest_hw_is_loaded(void) { return g_hw_loaded; }

// The lib string dlopen() resolved (empty until asmtest_hw_resolve succeeds).
static const char *asmtest_hw_go_library_path(void) { return g_hw_path; }

// static wrappers Go calls — each guards the (already-resolved) pointer so a
// partial load can never dereference NULL.
static int  asmtest_hw_go_available(int backend) { return p_hw_available ? p_hw_available(backend) : 0; }
static void asmtest_hw_go_skip_reason(int backend, char *buf, size_t buflen) {
    if (p_hw_skip_reason) p_hw_skip_reason(backend, buf, buflen);
    else if (buflen) buf[0] = 0;
}
static size_t asmtest_hw_go_resolve(int policy, int *out, size_t cap) {
    return p_hw_resolve ? p_hw_resolve(policy, out, cap) : 0;
}
static int  asmtest_hw_go_auto(int policy) {
    return p_hw_auto ? p_hw_auto(policy) : -3; // ASMTEST_HW_EUNAVAIL
}
// Cross-tier orchestrator bridges (asmtest_trace_auto.h).
static size_t asmtest_go_trace_resolve(unsigned policy, asmtest_trace_choice_t *out, size_t cap) {
    return p_trace_resolve ? p_trace_resolve(policy, out, cap) : 0;
}
static int  asmtest_go_trace_auto(unsigned policy, asmtest_trace_choice_t *out) {
    return p_trace_auto ? p_trace_auto(policy, out) : -3; // ASMTEST_HW_EUNAVAIL
}
static int  asmtest_go_trace_call_auto(const void *code, size_t len, const long *args,
                                       int nargs, unsigned policy, long *result,
                                       void *trace, asmtest_trace_choice_t *used) {
    return p_trace_call_auto
        ? p_trace_call_auto(code, len, args, nargs, policy, result, trace, used)
        : -3; // ASMTEST_HW_EUNAVAIL
}
static int  asmtest_hw_go_init(int backend) {
    // Zero-init (the old field-by-field init left lbr_period/branch_filter
    // as stack garbage — the F27 plan's noted go bug), then self-describe.
    asmtest_hwtrace_options_t o = {0};
    o.struct_size = sizeof o; // F27 ABI size negotiation
    o.backend = backend;
    return p_hw_init ? p_hw_init(&o) : -1;
}
// F29 machine-readable status + the paranoid reader (additive diagnostics; an
// older lib without them degrades to -3 / INT_MIN-style sentinels).
static int  asmtest_hw_go_status(int backend, asmtest_hwtrace_status_t *out) {
    return p_hw_status ? p_hw_status(backend, out) : -3; // ASMTEST_HW_EUNAVAIL
}
static int  asmtest_hw_go_paranoid(void) {
    return p_hw_paranoid ? p_hw_paranoid() : (-2147483647 - 1); // INT_MIN
}
static int  asmtest_hw_go_register(const char *name, void *base, size_t len, void *trace) {
    return p_hw_register ? p_hw_register(name, base, len, trace) : -1;
}
static void asmtest_hw_go_begin(const char *name) { if (p_hw_begin) p_hw_begin(name); }
static void asmtest_hw_go_end(const char *name)   { if (p_hw_end) p_hw_end(name); }
// Scoped-tracing shared core (§0/§1): error-returning begin (falls back to void
// begin -> 0 when absent) + render-on-close.
static int  asmtest_hw_go_try_begin(const char *name) {
    if (p_hw_try_begin) return p_hw_try_begin(name);
    if (p_hw_begin) { p_hw_begin(name); return 0; }
    return -1;
}
static int  asmtest_hw_go_render(const char *name, char *buf, size_t buflen) {
    return p_hw_render ? p_hw_render(name, buf, buflen) : -5;
}
static void asmtest_hw_go_shutdown(void) { if (p_hw_shutdown) p_hw_shutdown(); }
static int  asmtest_hw_go_arm_tid(void) { return p_hw_arm_tid ? p_hw_arm_tid() : -1; }
static int  asmtest_hw_go_exec_alloc(const void *bytes, size_t len, void **base_out, size_t *len_out) {
    return p_hw_exec_alloc ? p_hw_exec_alloc(bytes, len, base_out, len_out) : -1;
}
static void asmtest_hw_go_exec_free(void *base, size_t len) { if (p_hw_exec_free) p_hw_exec_free(base, len); }
// NOTE: insns capacity FIRST, blocks SECOND (matches asmtest_trace_new's signature).
static void *asmtest_hw_go_trace_new(size_t insns, size_t blocks) {
    return p_hw_trace_new ? p_hw_trace_new(insns, blocks) : NULL;
}
static void  asmtest_hw_go_trace_free(void *t) { if (p_hw_trace_free) p_hw_trace_free(t); }
static int   asmtest_hw_go_trace_covered(void *t, uint64_t off) {
    return p_hw_trace_covered ? p_hw_trace_covered(t, off) : 0;
}
static unsigned long long asmtest_hw_go_blocks_len(void *t) {
    return p_hw_trace_blocks_len ? p_hw_trace_blocks_len(t) : 0;
}
static unsigned long long asmtest_hw_go_insns_total(void *t) {
    return p_hw_trace_insns_total ? p_hw_trace_insns_total(t) : 0;
}
static unsigned long long asmtest_hw_go_insns_len(void *t) {
    return p_hw_trace_insns_len ? p_hw_trace_insns_len(t) : 0;
}
static int asmtest_hw_go_truncated(void *t) {
    return p_hw_trace_truncated ? p_hw_trace_truncated(t) : 0;
}
static unsigned long long asmtest_hw_go_block_at(void *t, size_t i) {
    return p_hw_trace_block_at ? p_hw_trace_block_at(t, i) : 0;
}
static unsigned long long asmtest_hw_go_insn_at(void *t, size_t i) {
    return p_hw_trace_insn_at ? p_hw_trace_insn_at(t, i) : 0;
}
static int asmtest_hw_go_call_scoped_ex(void *base, size_t len, void *trace, void *fn,
                                        const long *args, int nargs, long *result_out,
                                        asmtest_hwtrace_scope_t *out) {
    return p_hw_call_scoped_ex
        ? p_hw_call_scoped_ex(base, len, trace, fn, args, nargs, result_out, out)
        : -3; // ASMTEST_HW_EUNAVAIL (nested block comments would close the cgo preamble)
}
static int asmtest_hw_go_render_scope(asmtest_hwtrace_scope_t handle, char *buf, size_t buflen) {
    return p_hw_render_scope ? p_hw_render_scope(handle, buf, buflen) : -3;
}

// Trampoline that invokes the generated host-native code through a function
// pointer under the SysV integer ABI (two long args -> long result).
static long asmtest_hw_call2(void *p, long a, long b) {
    return ((long (*)(long, long))p)(a, b);
}

// asmtest_ptrace.h bridges — each NULL-guards its (already-resolved) pointer so a
// partial load can never dereference NULL.
static int asmtest_go_pt_available(void) { return p_pt_available ? p_pt_available() : 0; }
static void asmtest_go_pt_skip_reason(char *buf, size_t buflen) {
    if (p_pt_skip_reason) p_pt_skip_reason(buf, buflen);
    else if (buflen) buf[0] = 0;
}
static int asmtest_go_pt_blockstep_available(void) {
    return p_pt_blockstep_available ? p_pt_blockstep_available() : 0;
}
static int asmtest_go_pt_trace_call_blockstep(const void *code, size_t len,
                                              const long *args, int nargs,
                                              long *result, void *trace) {
    return p_pt_trace_call_blockstep
               ? p_pt_trace_call_blockstep(code, len, args, nargs, result, trace)
               : -1;
}
static int asmtest_go_pt_trace_call(const void *code, size_t len, const long *args,
                                    int nargs, long *result, void *trace) {
    return p_pt_trace_call ? p_pt_trace_call(code, len, args, nargs, result, trace) : -1;
}
static int asmtest_go_pt_trace_attached(int pid, const void *base, size_t len,
                                        long *result, void *trace) {
    return p_pt_trace_attached ? p_pt_trace_attached(pid, base, len, result, trace) : -1;
}
static int asmtest_go_pt_trace_attached_blockstep(int pid, const void *base,
                                                  size_t len, long *result,
                                                  void *trace) {
    return p_pt_trace_attached_blockstep
               ? p_pt_trace_attached_blockstep(pid, base, len, result, trace)
               : -1;
}
static int asmtest_go_pt_trace_attached_versioned(int pid, const void *base, size_t len,
                                                  void *img, uint64_t when,
                                                  long *result, void *trace) {
    return p_pt_trace_attached_versioned
        ? p_pt_trace_attached_versioned(pid, base, len, img, when, result, trace) : -1;
}
static int asmtest_go_pt_run_to(int pid, const void *addr) {
    return p_pt_run_to ? p_pt_run_to(pid, addr) : -1;
}
static int asmtest_go_proc_region_by_addr(int pid, const void *addr,
                                          void **base_out, size_t *len_out) {
    return p_proc_region_by_addr ? p_proc_region_by_addr(pid, addr, base_out, len_out) : -1;
}
static int asmtest_go_proc_perfmap_symbol(int pid, const char *name,
                                          void **base_out, size_t *len_out) {
    return p_proc_perfmap_symbol ? p_proc_perfmap_symbol(pid, name, base_out, len_out) : -1;
}
static int asmtest_go_jitdump_find(const char *path, int pid, const char *name,
                                   asmtest_jitdump_entry_t *out, uint8_t *bytes_out,
                                   size_t bytes_cap, size_t *bytes_len) {
    return p_jitdump_find ? p_jitdump_find(path, pid, name, out, bytes_out, bytes_cap, bytes_len) : -1;
}

// asmtest_codeimage.h bridges — each NULL-guards its (already-resolved) pointer so a
// partial load can never dereference NULL.
static int asmtest_go_ci_available(void) { return p_ci_available ? p_ci_available() : 0; }
static void asmtest_go_ci_skip_reason(char *buf, size_t buflen) {
    if (p_ci_skip_reason) p_ci_skip_reason(buf, buflen);
    else if (buflen) buf[0] = 0;
}
static void *asmtest_go_ci_new(int pid) { return p_ci_new ? p_ci_new(pid) : NULL; }
static void asmtest_go_ci_free(void *img) { if (p_ci_free) p_ci_free(img); }
static int asmtest_go_ci_track(void *img, const void *base, size_t len) {
    return p_ci_track ? p_ci_track(img, base, len) : -1;
}
static int asmtest_go_ci_refresh(void *img) { return p_ci_refresh ? p_ci_refresh(img) : -1; }
static uint64_t asmtest_go_ci_now(const void *img) { return p_ci_now ? p_ci_now(img) : 0; }
static int asmtest_go_ci_bytes_at(const void *img, const void *addr, uint64_t when,
                                  const uint8_t **out, size_t *out_len) {
    return p_ci_bytes_at ? p_ci_bytes_at(img, addr, when, out, out_len) : -1;
}
static int asmtest_go_ci_bpf_available(void) { return p_ci_bpf_available ? p_ci_bpf_available() : 0; }
static void asmtest_go_ci_bpf_skip_reason(char *buf, size_t buflen) {
    if (p_ci_bpf_skip_reason) p_ci_bpf_skip_reason(buf, buflen);
    else if (buflen) buf[0] = 0;
}
static int asmtest_go_ci_watch_bpf(void *img) { return p_ci_watch_bpf ? p_ci_watch_bpf(img) : -1; }
static int asmtest_go_ci_poll_bpf(void *img, int timeout_ms) {
    return p_ci_poll_bpf ? p_ci_poll_bpf(img, timeout_ms) : -1;
}
static int asmtest_go_ci_next(void *img, asmtest_codeimage_event_t *out) {
    return p_ci_next ? p_ci_next(img, out) : -1;
}

// asmtest_ptrace.h — call descent (asmtest_descent_t) bridges. Each NULL-guards its
// (already-resolved) pointer so a partial load can never dereference NULL. The two
// set-callback bridges install the //export'd Go trampolines (descent_cgo.go); `user`
// is the descent handle, the key the trampoline uses to find the Go closure.
static void *asmtest_go_desc_new(int level) { return p_desc_new ? p_desc_new(level) : NULL; }
static void asmtest_go_desc_free(void *d) { if (p_desc_free) p_desc_free(d); }
static void asmtest_go_desc_set_max_depth(void *d, uint32_t v) { if (p_desc_set_max_depth) p_desc_set_max_depth(d, v); }
static void asmtest_go_desc_set_insn_budget(void *d, uint64_t v) { if (p_desc_set_insn_budget) p_desc_set_insn_budget(d, v); }
static void asmtest_go_desc_set_watchdog_ms(void *d, uint32_t v) { if (p_desc_set_watchdog_ms) p_desc_set_watchdog_ms(d, v); }
static void asmtest_go_desc_use_default_denylist(void *d) { if (p_desc_use_default_denylist) p_desc_use_default_denylist(d); }
static int asmtest_go_desc_allow_region(void *d, const void *b, size_t n) { return p_desc_allow_region ? p_desc_allow_region(d, b, n) : -1; }
static int asmtest_go_desc_deny_region(void *d, const void *b, size_t n) { return p_desc_deny_region ? p_desc_deny_region(d, b, n) : -1; }
static void asmtest_go_desc_set_resolver(void *d, void *user) { if (p_desc_set_resolver) p_desc_set_resolver(d, goDescentResolverTramp, user); }
static void asmtest_go_desc_set_denylist(void *d, void *user) { if (p_desc_set_denylist) p_desc_set_denylist(d, goDescentDenylistTramp, user); }
static size_t asmtest_go_desc_edges_len(void *d) { return p_desc_edges_len ? p_desc_edges_len(d) : 0; }
static uint64_t asmtest_go_desc_edge_site(void *d, size_t i) { return p_desc_edge_site ? p_desc_edge_site(d, i) : 0; }
static uint64_t asmtest_go_desc_edge_target(void *d, size_t i) { return p_desc_edge_target ? p_desc_edge_target(d, i) : 0; }
static uint32_t asmtest_go_desc_edge_depth(void *d, size_t i) { return p_desc_edge_depth ? p_desc_edge_depth(d, i) : 0; }
static size_t asmtest_go_desc_frames_len(void *d) { return p_desc_frames_len ? p_desc_frames_len(d) : 0; }
static uint64_t asmtest_go_desc_frame_base(void *d, size_t f) { return p_desc_frame_base ? p_desc_frame_base(d, f) : 0; }
static uint64_t asmtest_go_desc_frame_len(void *d, size_t f) { return p_desc_frame_len ? p_desc_frame_len(d, f) : 0; }
static uint32_t asmtest_go_desc_frame_depth(void *d, size_t f) { return p_desc_frame_depth ? p_desc_frame_depth(d, f) : 0; }
static int32_t asmtest_go_desc_frame_parent(void *d, size_t f) { return p_desc_frame_parent ? p_desc_frame_parent(d, f) : -1; }
static size_t asmtest_go_desc_frame_insn_count(void *d, size_t f) { return p_desc_frame_insn_count ? p_desc_frame_insn_count(d, f) : 0; }
static uint64_t asmtest_go_desc_frame_insn_at(void *d, size_t f, size_t i) { return p_desc_frame_insn_at ? p_desc_frame_insn_at(d, f, i) : 0; }
static size_t asmtest_go_desc_frame_block_count(void *d, size_t f) { return p_desc_frame_block_count ? p_desc_frame_block_count(d, f) : 0; }
static uint64_t asmtest_go_desc_frame_block_at(void *d, size_t f, size_t i) { return p_desc_frame_block_at ? p_desc_frame_block_at(d, f, i) : 0; }
static int asmtest_go_desc_truncated(void *d) { return p_desc_truncated ? p_desc_truncated(d) : 0; }
static int asmtest_go_desc_depth_capped(void *d) { return p_desc_depth_capped ? p_desc_depth_capped(d) : 0; }
static int asmtest_go_pt_trace_call_ex(const void *code, size_t len, const long *args, int nargs,
                                       long *result, void *trace, void *descent) {
    return p_pt_trace_call_ex ? p_pt_trace_call_ex(code, len, args, nargs, result, trace, descent) : -1;
}
static int asmtest_go_pt_trace_attached_ex(int pid, const void *base, size_t len, long *result,
                                           void *trace, void *descent) {
    return p_pt_trace_attached_ex ? p_pt_trace_attached_ex(pid, base, len, result, trace, descent) : -1;
}
static int asmtest_go_pt_trace_attached_versioned_ex(int pid, const void *base, size_t len, void *img,
                                                     uint64_t when, long *result, void *trace, void *descent) {
    return p_pt_trace_attached_versioned_ex
        ? p_pt_trace_attached_versioned_ex(pid, base, len, img, when, result, trace, descent) : -1;
}
*/
import "C"

import (
	"fmt"
	"runtime"
	"unsafe"
)

// hwOK is the success status returned by the lifecycle / registration calls
// (mirrors the C macro ASMTEST_HW_OK).
const hwOK = 0

// HwEUnavail is the status HwTraceAuto returns when no hardware-trace backend is
// available on this host (mirrors the C macro ASMTEST_HW_EUNAVAIL).
const HwEUnavail = -3

// asmtest_trace_backend_t — the four hardware-trace backends. SINGLESTEP is the
// portable default that runs on any x86-64 Linux.
const (
	IntelPT    = 0
	CoreSight  = 1
	AmdLBR     = 2
	SingleStep = 3
)

// asmtest_hwtrace_policy_t — the backend auto-selection policy for HwTraceResolve
// / HwTraceAuto. BEST is the most faithful available backend; CEILING_FREE is the
// same but skips the one fixed-window backend (AMD LBR) — re-resolve under it
// after a trace comes back truncated.
const (
	Best        = 0
	CeilingFree = 1
)

// asmtest_trace_tier_t — the CROSS-TIER orchestrator's trace tiers (over the
// hardware + DynamoRIO + emulator tiers), most-faithful to least. See
// include/asmtest_trace_auto.h and the Python wrapper's resolve_tiers/auto_tier.
const (
	TierHwtrace   = 0 // HW branch trace / single-step (real CPU)
	TierDynamoRIO = 1 // in-process software DBI (real CPU)
	TierEmulator  = 2 // Unicorn virtual CPU (isolated guest)
)

// asmtest_trace_fidelity_t — execution fidelity of a resolved tier choice.
const (
	FidelityNative      = 0 // runs the real bytes on the real CPU in-process
	FidelityVirtual     = 1 // isolated guest on an emulated CPU
	FidelityStatistical = 2 // sampled survey (IBS/LBR sampling): NOT exact
)

// Cross-tier policy bitmask for ResolveTiers / AutoTier. TraceBest is the most-
// faithful available (emulator floor allowed); TraceCeilingFree drops the one
// fixed-window backend (AMD LBR); TraceNativeOnly forbids the native->emulator
// fidelity crossing (drops the emulator floor).
const (
	TraceBest        = 0x0
	TraceCeilingFree = 0x1
	TraceNativeOnly  = 0x2
)

// Resolve the optional hardware-trace tier the first time the package loads,
// exactly as drtrace.go resolves the DynamoRIO tier. A miss is silent —
// HwTraceAvailable() reports it.
func init() { C.asmtest_hw_resolve() }

// HwTraceAvailable reports whether the chosen backend can run on this host: the
// libasmtest_hwtrace lib loaded AND the backend's full detect-and-skip chain
// passes (asmtest_hwtrace_available(backend) == 1). It never panics, so callers
// (and the test) self-skip cleanly when the lib or the hardware is absent.
func HwTraceAvailable(backend int) bool {
	return C.asmtest_hw_is_loaded() != 0 && C.asmtest_hw_go_available(C.int(backend)) != 0
}

// HwTraceLibraryPath is the libasmtest_hwtrace string this process actually
// dlopen()ed — the $ASMTEST_HWTRACE_LIB override if set, else the build/ default
// (the resolver's search order, env-override first). It is the counterpart of the
// Python wrapper's hwtrace.library_path(), letting a clean-room test assert which
// candidate satisfied the load. Empty until the tier resolves; unlike the
// prebuilt-payload bindings this is a SOURCE distribution, so there is no bundled
// native/ directory — the reported path is whatever dlopen() was handed.
func HwTraceLibraryPath() string {
	return C.GoString(C.asmtest_hw_go_library_path())
}

// HwTraceSkipReason is a human-readable reason HwTraceAvailable(backend) is false
// (or "available"). Useful for the self-skip message.
func HwTraceSkipReason(backend int) string {
	if C.asmtest_hw_is_loaded() == 0 {
		return "libasmtest_hwtrace not loaded (set ASMTEST_HWTRACE_LIB or build with `make shared-hwtrace`)"
	}
	buf := make([]byte, 160)
	C.asmtest_hw_go_skip_reason(C.int(backend), (*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)))
	return C.GoString((*C.char)(unsafe.Pointer(&buf[0])))
}

// HwTraceResolve is this host's hardware-trace fallback cascade: the available
// backends, most-faithful first (IntelPT > AmdLBR > SingleStep > CoreSight),
// honoring policy. Empty only off x86-64 Linux (single-step is the floor there)
// or when libasmtest_hwtrace is not loaded. CeilingFree drops the ceiling-bounded
// backend (AMD LBR).
func HwTraceResolve(policy int) []int {
	var out [4]C.int
	n := C.asmtest_hw_go_resolve(C.int(policy), &out[0], C.size_t(len(out)))
	bes := make([]int, int(n))
	for i := range bes {
		bes[i] = int(out[i])
	}
	return bes
}

// HwTraceAuto is the single most-preferred available backend under policy (a
// backend enum >= 0, ready to pass to HwTraceInit), or HwEUnavail (-3) when no
// hardware-trace backend is available on this host.
func HwTraceAuto(policy int) int {
	return int(C.asmtest_hw_go_auto(C.int(policy)))
}

// TierChoice is one resolved cross-tier trace option: which Tier to use, which
// hardware Backend within it (meaningful only when Tier == TierHwtrace), the
// Fidelity class (FidelityNative / FidelityVirtual / FidelityStatistical), and
// the concrete capture Mechanism (Mech*) — for TraceCallAuto's Used, the
// escalation rung that actually won (F22/F26/F37). Mirrors
// asmtest_trace_choice_t / the Python wrapper's TierChoice.
type TierChoice struct {
	Tier      int
	Backend   int
	Fidelity  int
	Mechanism int
}

// asmtest_trace_mechanism_t — the escalation-rung/mechanism discriminator.
// Every value except MechStatistical is an EXACT producer.
const (
	MechNone        = 0 // no rung produced a trace (EUNAVAIL)
	MechHwBranch    = 1 // in-process HW branch record (PT / AMD LBR / CoreSight)
	MechTfStep      = 2 // in-process EFLAGS.TF #DB single-step
	MechMsrLbr      = 3 // in-process MSR-direct LBR re-run (rung 1b)
	MechBlockStep   = 4 // fork-isolated BTF block-step re-run
	MechPerInsn     = 5 // fork-isolated per-instruction ptrace re-run
	MechDbi         = 6 // in-process DynamoRIO code cache
	MechEmulator    = 7 // Unicorn virtual CPU (isolated guest)
	MechStatistical = 8 // sampled survey: never exact, never parity
)

// asmtest_hwtrace.h status codes / stages surfaced by HwTraceStatus (F29).
const (
	HwEperm      = -9 // perf capture permission denied (substrate present)
	HwStageOk    = 0  // no gate failed
	HwStageProbe = 4  // the perf open probe failed (EPERM vs EUNAVAIL in Code)
)

// HwStatus is the F29 machine-readable availability verdict for one backend:
// Code distinguishes HwEperm (substrate present, perf permission denied) from
// EUNAVAIL (hardware/decoder/PMU absent) — the split HwTraceAvailable's bool
// deliberately collapses. Reason is byte-identical to HwTraceSkipReason.
type HwStatus struct {
	Available         bool
	Code              int
	Stage             int
	PerfEventParanoid int
	ProbeErrno        int
	Reason            string
}

// HwTraceStatus fills the F29 verdict for backend (asmtest_hwtrace_status).
func HwTraceStatus(backend int) (HwStatus, error) {
	var raw C.asmtest_hwtrace_status_t
	if rc := C.asmtest_hw_go_status(C.int(backend), &raw); rc != hwOK {
		return HwStatus{}, fmt.Errorf("asmtest_hwtrace_status failed: %d", int(rc))
	}
	return HwStatus{
		Available:         raw.available != 0,
		Code:              int(raw.code),
		Stage:             int(raw.stage),
		PerfEventParanoid: int(raw.perf_event_paranoid),
		ProbeErrno:        int(raw.probe_errno),
		Reason:            C.GoString(&raw.reason[0]),
	}, nil
}

// HwTracePerfEventParanoid is the kernel's perf_event_paranoid level, or
// INT_MIN where the proc file is absent (non-Linux / masked /proc).
func HwTracePerfEventParanoid() int { return int(C.asmtest_hw_go_paranoid()) }

// ResolveTiers is this host's full CROSS-TIER cascade (asmtest_trace_resolve),
// most-faithful first: Intel PT -> AMD LBR -> DynamoRIO -> single-step ->
// CoreSight -> emulator, each included only if its tier is available, honoring
// policy. The emulator (TierEmulator, FidelityVirtual) is the universal floor and
// the last entry under TraceBest. TraceNativeOnly drops that floor (no
// native->emulator crossing); TraceCeilingFree drops AMD LBR. Empty only off a
// native host under TraceNativeOnly, or when libasmtest_hwtrace is not loaded.
func ResolveTiers(policy int) []TierChoice {
	var out [8]C.asmtest_trace_choice_t
	n := C.asmtest_go_trace_resolve(C.uint(policy), &out[0], C.size_t(len(out)))
	cs := make([]TierChoice, int(n))
	for i := range cs {
		cs[i] = TierChoice{
			Tier:      int(out[i].tier),
			Backend:   int(out[i].backend),
			Fidelity:  int(out[i].fidelity),
			Mechanism: int(out[i].mechanism),
		}
	}
	return cs
}

// AutoTier is the single most-preferred available cross-tier choice under policy
// (asmtest_trace_auto). The bool is false (meaning EUNAVAIL) when the cascade is
// empty — only off a native host under TraceNativeOnly, or when
// libasmtest_hwtrace is not loaded.
func AutoTier(policy int) (TierChoice, bool) {
	var c C.asmtest_trace_choice_t
	if rc := C.asmtest_go_trace_auto(C.uint(policy), &c); rc != hwOK {
		return TierChoice{}, false
	}
	return TierChoice{
		Tier:      int(c.tier),
		Backend:   int(c.backend),
		Fidelity:  int(c.fidelity),
		Mechanism: int(c.mechanism),
	}, true
}

// HwTraceInit selects a backend and initializes the tier. SingleStep is the
// portable default that runs on any x86-64 Linux.
func HwTraceInit(backend int) error {
	if rc := C.asmtest_hw_go_init(C.int(backend)); rc != hwOK {
		return fmt.Errorf("asmtest_hwtrace_init failed: %d", int(rc))
	}
	return nil
}

// HwTraceShutdown tears the tier down and returns it to the uninitialized state.
// Safe to call after a failed init.
func HwTraceShutdown() { C.asmtest_hw_go_shutdown() }

// HwTraceArmTid is the OS thread id (SYS_gettid) that armed the currently-active
// hwtrace capture, or -1 when none is active.
func HwTraceArmTid() int { return int(C.asmtest_hw_go_arm_tid()) }

// HwNativeCode is host-native machine code in real executable (W^X) memory,
// materialized at its runtime address so PC-relative and branch targets resolve.
// It is the hardware-tier counterpart of drtrace.go's NativeCode (a distinct type
// because the two tiers own their executable memory through different allocators —
// here asmtest_hwtrace_exec_alloc / _free). Release it with Free.
type HwNativeCode struct {
	base unsafe.Pointer
	len  C.size_t
	free bool
}

// HwNativeCodeFromBytes maps executable memory and copies the host-native machine
// code bytes into it (offset 0 is the entry point), via asmtest_hwtrace_exec_alloc.
func HwNativeCodeFromBytes(b []byte) (*HwNativeCode, error) {
	if C.asmtest_hw_is_loaded() == 0 {
		return nil, fmt.Errorf("hardware-trace tier unavailable")
	}
	nc := &HwNativeCode{}
	var p unsafe.Pointer
	if len(b) > 0 {
		p = unsafe.Pointer(&b[0])
	}
	if rc := C.asmtest_hw_go_exec_alloc(p, C.size_t(len(b)), &nc.base, &nc.len); rc != hwOK {
		return nil, fmt.Errorf("asmtest_hwtrace_exec_alloc failed: %d", int(rc))
	}
	return nc, nil
}

// Base is the runtime address of the executable mapping (offset 0 = entry).
func (c *HwNativeCode) Base() uintptr { return uintptr(c.base) }

// Len is the number of code bytes.
func (c *HwNativeCode) Len() int { return int(c.len) }

// Call invokes the code through a function pointer under the SysV integer ABI:
// two long args, a long result.
func (c *HwNativeCode) Call(a, b int64) int64 {
	return int64(C.asmtest_hw_call2(c.base, C.long(a), C.long(b)))
}

// Free unmaps the executable memory. Safe to call more than once.
func (c *HwNativeCode) Free() {
	if !c.free {
		C.asmtest_hw_go_exec_free(c.base, c.len)
		c.free = true
	}
}

// HwTrace is an app-owned coverage recorder for a registered native region,
// reading back basic-block coverage / the ordered instruction stream as the
// region runs under the hardware tier. Release it with Free.
type HwTrace struct{ h unsafe.Pointer }

// NewHwTrace allocates a trace handle. instructions > 0 records the ordered
// instruction stream; blocks > 0 records basic-block coverage. (The underlying
// asmtest_trace_new takes insns first, blocks second; this constructor takes
// blocks first to mirror the Python wrapper's new(blocks=, instructions=).)
func NewHwTrace(blocks, instructions int) *HwTrace {
	return &HwTrace{h: C.asmtest_hw_go_trace_new(C.size_t(instructions), C.size_t(blocks))}
}

// Register records a non-overlapping native code range under name, recording
// coverage into this trace. The name may be freed after the call (it is copied).
func (t *HwTrace) Register(name string, code *HwNativeCode) error {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	rc := C.asmtest_hw_go_register(cs, code.base, code.len, t.h)
	if rc != hwOK {
		return fmt.Errorf("register_region(%q) failed: %d", name, int(rc))
	}
	return nil
}

// Begin opens hardware AUX capture for the named region. Markers must be balanced
// with End — prefer Region, which balances them for you. (MVP: only one region
// may be active at a time.)
func (t *HwTrace) Begin(name string) {
	// Pin the goroutine to its OS thread for the region's lifetime (End releases
	// it). The single-step backend arms per-thread EFLAGS.TF, single-steps the
	// traced code, and handles the SIGTRAPs — all of which must happen on ONE OS
	// thread. Begin, the traced call, and End are three separate cgo calls with Go
	// code in between, so without this the Go scheduler can migrate the goroutine
	// mid-region: TF gets armed on one thread while the code runs on another (an
	// empty/partial trace — "offset 0 not covered"), or a stray SIGTRAP lands on a
	// thread later running Go and crashes with a wandering site. Harmless for the
	// PT/AMD backends (they capture per-thread hardware AUX and decode after End).
	runtime.LockOSThread()
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	C.asmtest_hw_go_begin(cs)
}

// End closes capture for the named region and decodes the captured trace, then
// releases the OS-thread pin taken in Begin.
func (t *HwTrace) End(name string) {
	defer runtime.UnlockOSThread()
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	C.asmtest_hw_go_end(cs)
}

// Region runs fn between a balanced Begin(name)/End(name) — the Go idiom for the
// scoped marker (Go has no RAII, so a func wrapper replaces Python's `with`). End
// runs even if fn panics.
func (t *HwTrace) Region(name string, fn func()) {
	t.Begin(name)
	defer t.End(name)
	fn()
}

// scopeName builds a call-site region name "basename:line" (basename dodges the
// 64-char C name ceiling and full-path aliasing under Core §0.4's by-name registry).
func scopeName(file string, line int) string {
	base := file
	for i := len(file) - 1; i >= 0; i-- {
		if file[i] == '/' {
			base = file[i+1:]
			break
		}
	}
	n := fmt.Sprintf("%s:%d", base, line)
	if len(n) > 63 {
		n = n[len(n)-63:]
	}
	return n
}

// renderRegion renders the named region's recorded instructions to assembly text
// (size-then-allocate); "" if the decoder is unavailable.
func renderRegion(name string) string {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	need := int(C.asmtest_hw_go_render(cs, (*C.char)(nil), 0))
	if need <= 0 {
		return ""
	}
	buf := make([]byte, need+1)
	C.asmtest_hw_go_render(cs, (*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)))
	return string(buf[:need])
}

// ScopedResult carries a Scope's outcome: the auto-generated region name, the
// rendered assembly listing, whether the scope armed, and the thread-scope honesty
// bit (truncated on a cross-thread close / capture overflow, Core §0.2/§1).
type ScopedResult struct {
	Name      string
	Path      string
	Armed     bool
	Truncated bool
}

// Scope traces fn over the native code, auto-naming from the call site
// (runtime.Caller) and returning the rendered assembly on close — the closure form of
// the *import + scope* surface (Go has no RAII). LockOSThread pins the goroutine so
// the per-thread single-step capture stays on ONE OS thread (released after End).
//
// WORK FANNED OUT via `go func()` inside fn runs on OTHER OS threads and is SILENTLY
// UNTRACED — the scope confines the trace to the arming thread. The §0.2 tid assert
// is a backstop only for an End that ran on the wrong thread, NOT a fan-out detector.
// End runs even if fn panics.
func (t *HwTrace) Scope(code *HwNativeCode, emit bool, fn func()) ScopedResult {
	_, file, line, _ := runtime.Caller(1)
	name := scopeName(file, line)
	res := ScopedResult{Name: name}
	// Register-then-begin under the auto-name (Core §0.4 idempotent-by-name).
	csr := C.CString(name)
	C.asmtest_hw_go_register(csr, code.base, code.len, t.h)
	C.free(unsafe.Pointer(csr))
	runtime.LockOSThread()
	cs := C.CString(name)
	res.Armed = C.asmtest_hw_go_try_begin(cs) == 0
	func() {
		defer func() {
			C.asmtest_hw_go_end(cs)
			C.free(unsafe.Pointer(cs))
			runtime.UnlockOSThread()
		}()
		fn()
	}()
	res.Path = renderRegion(name)
	res.Truncated = t.Truncated()
	if emit && res.Path != "" {
		fmt.Print(res.Path)
	}
	return res
}

// CallScopedResult carries a CallScoped outcome: the traced call's return value
// (Result, meaningful only when OK), whether the scope armed and ran (OK == rc 0),
// the executed body's disassembly (Path, "" when the decoder is absent), the
// thread-scope honesty bit (Truncated), and the raw ASMTEST_HW_* status (RC).
// Mirrors the Python/Ruby CallScopedResult.
type CallScopedResult struct {
	Result    int64
	OK        bool
	Path      string
	Truncated bool
	RC        int
}

// CallScoped traces ONE native call the managed-safe way: arm the single-step
// window, call code(args...) through the SysV integer ABI, and disarm — all in
// native code (asmtest_hwtrace_call_scoped_ex), a tighter window than Scope.
// REGISTRY-FREE (consumes no MAX_REGIONS slot), so it is safe in a tight loop.
// Owns and frees its own trace handle. Integer args (0-6). Requires an initialized
// single-step tier (HwTraceInit); RC is negative on a self-skip. Mirrors python
// HwTrace.call_scoped.
//
// LockOSThread pins the goroutine across BOTH cgo calls below: call_scoped_ex captures
// the scope into the CAPTURING OS thread's TLS range stack, and the handle-keyed
// render_scope must read it back on that SAME thread (the header pins the handle to the
// capturing thread). Without the pin, a goroutine->M migration between the two calls —
// which the slow single-stepped body makes likely under load — would make render_scope
// miss the handle and silently drop Path. (Result/Truncated/RC are handle-independent.)
func CallScoped(code *HwNativeCode, args ...int64) CallScopedResult {
	h := C.asmtest_hw_go_trace_new(256, 64) // insns=256, blocks=64
	if h == nil {
		return CallScopedResult{RC: -3} // ASMTEST_HW_EUNAVAIL
	}
	defer C.asmtest_hw_go_trace_free(h)
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	// A valid pointer even for 0 args (nargs, not the slice length, bounds the read).
	n := len(args)
	arr := make([]C.long, n+1)
	for i, a := range args {
		arr[i] = C.long(a)
	}
	var result C.long
	var scope C.asmtest_hwtrace_scope_t
	rc := int(C.asmtest_hw_go_call_scoped_ex(
		code.base, code.len, h, code.base,
		&arr[0], C.int(n), &result, &scope))
	if rc != hwOK {
		return CallScopedResult{RC: rc}
	}
	// Render the body from the just-captured (thread-local) scope handle, by value.
	path := ""
	if need := int(C.asmtest_hw_go_render_scope(scope, (*C.char)(nil), 0)); need > 0 {
		buf := make([]byte, need+1)
		C.asmtest_hw_go_render_scope(scope, (*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)))
		path = string(buf[:need])
	}
	truncated := C.asmtest_hw_go_truncated(h) != 0
	return CallScopedResult{Result: int64(result), OK: true, Path: path, Truncated: truncated, RC: rc}
}

// TraceCallAutoResult carries a TraceCallAuto outcome: the traced call's return
// value (Result, meaningful only when OK), the filled Trace (a queryable *HwTrace
// the CALLER frees via Trace.Free() — inspect it with Covered/BlockOffsets/etc.),
// the Used TierChoice that produced the final trace (inspect Used.Backend to see
// whether escalation fired), the completeness honesty bit (Truncated), whether
// some tier ran (OK == rc 0), and the raw ASMTEST_HW_* status (RC). On a self-skip
// (no call-owning native tier) Trace is nil, Used is the zero TierChoice, and RC
// is negative. Mirrors the Python TraceCallAutoResult.
type TraceCallAutoResult struct {
	Result    int64
	Trace     *HwTrace
	Used      TierChoice
	Truncated bool
	OK        bool
	RC        int
}

// TraceCallAuto is the auto-escalating CALL-OWNING cross-tier trace
// (asmtest_trace_call_auto): run code(args...) under the fastest exact tier and,
// when the trace comes back truncated, escalate to a ceiling-free tier and re-run —
// until the trace is complete or the tiers are exhausted. It OWNS the invocation, so
// code must be RE-RUNNABLE (invoked once per attempted tier: the in-process fast step,
// then the fork-isolated ptrace escalations). Integer args (0-6, SysV integer ABI).
// Starts under TraceBest. Returns a TraceCallAutoResult whose Trace the CALLER frees;
// RC is negative on a self-skip where no call-owning native tier is available. Mirrors
// python HwTrace.trace_call_auto.
//
// Unlike CallScoped's call_scoped_ex, asmtest_trace_call_auto SELF-MANAGES the full
// tier lifecycle (init -> begin -> invoke -> end -> shutdown) INTERNALLY, so it needs
// no HwTraceInit — a pre-arm here would double-init and leave the tier torn down. The
// whole lifecycle runs inside the single cgo call, so no LockOSThread pin is needed
// (the per-thread single-step step can never migrate mid-call).
func TraceCallAuto(code *HwNativeCode, args ...int64) TraceCallAutoResult {
	trace := NewHwTrace(64, 512) // blocks=64, instructions=512
	if trace.h == nil {
		return TraceCallAutoResult{RC: HwEUnavail}
	}
	// A valid pointer even for 0 args (nargs, not the slice length, bounds the read).
	n := len(args)
	arr := make([]C.long, n+1)
	for i, a := range args {
		arr[i] = C.long(a)
	}
	var result C.long
	var used C.asmtest_trace_choice_t
	rc := int(C.asmtest_go_trace_call_auto(
		code.base, code.len, &arr[0], C.int(n), C.uint(TraceBest),
		&result, trace.h, &used))
	if rc != hwOK {
		trace.Free()
		return TraceCallAutoResult{RC: rc}
	}
	return TraceCallAutoResult{
		Result: int64(result),
		Trace:  trace,
		Used: TierChoice{
			Tier:      int(used.tier),
			Backend:   int(used.backend),
			Fidelity:  int(used.fidelity),
			Mechanism: int(used.mechanism),
		},
		Truncated: trace.Truncated(),
		OK:        true,
		RC:        rc,
	}
}

// Covered reports whether the basic block at byte-offset off (from the region
// entry) was entered.
func (t *HwTrace) Covered(off uint64) bool {
	return C.asmtest_hw_go_trace_covered(t.h, C.uint64_t(off)) != 0
}

// BlocksLen is the number of distinct basic blocks recorded.
func (t *HwTrace) BlocksLen() uint64 { return uint64(C.asmtest_hw_go_blocks_len(t.h)) }

// InsnsTotal is the number of instructions executed (counts past the buffer cap).
func (t *HwTrace) InsnsTotal() uint64 { return uint64(C.asmtest_hw_go_insns_total(t.h)) }

// InsnsLen is the number of instruction offsets actually stored (up to the
// trace's instruction capacity; may be smaller than InsnsTotal).
func (t *HwTrace) InsnsLen() uint64 { return uint64(C.asmtest_hw_go_insns_len(t.h)) }

// Truncated reports whether the instruction stream overflowed its capacity (some
// executed offsets were not stored).
func (t *HwTrace) Truncated() bool { return C.asmtest_hw_go_truncated(t.h) != 0 }

// BlockOffsets is the distinct basic-block start offsets recorded, in first-seen
// order.
func (t *HwTrace) BlockOffsets() []uint64 {
	n := uint64(C.asmtest_hw_go_blocks_len(t.h))
	offs := make([]uint64, n)
	for i := uint64(0); i < n; i++ {
		offs[i] = uint64(C.asmtest_hw_go_block_at(t.h, C.size_t(i)))
	}
	return offs
}

// InsnOffsets is the ordered instruction-offset stream actually stored — each
// executed instruction's offset in execution order, up to the trace's insns
// capacity (InsnsLen, not the possibly-larger InsnsTotal).
func (t *HwTrace) InsnOffsets() []uint64 {
	n := uint64(C.asmtest_hw_go_insns_len(t.h))
	offs := make([]uint64, n)
	for i := uint64(0); i < n; i++ {
		offs[i] = uint64(C.asmtest_hw_go_insn_at(t.h, C.size_t(i)))
	}
	return offs
}

// Free releases the trace handle. Safe to call more than once.
func (t *HwTrace) Free() {
	if t.h != nil {
		C.asmtest_hw_go_trace_free(t.h)
		t.h = nil
	}
}

// ---- Out-of-process / foreign-process tracing (asmtest_ptrace.h) ----
//
// Single-step a forked or externally-attached target OUT OF BAND, and resolve
// the code region to trace from the OS — /proc/<pid>/maps, a JIT perf-map, or a
// binary jitdump. The managed-runtime path (JVM/.NET/Node on AMD, where Intel PT
// is unavailable and in-process DynamoRIO cannot seize the runtime's threads).
// Linux x86-64. Mirrors the Python wrapper's Ptrace class. These share the same
// already-dlopen'd libasmtest_hwtrace as the hardware-trace surface above.

// ptraceOK is the success status returned by the ptrace toolkit calls (mirrors
// the C macro ASMTEST_PTRACE_OK).
const ptraceOK = 0

// PtraceENOENT is the status meaning a region / symbol / method was not found
// (mirrors the C macro ASMTEST_PTRACE_ENOENT).
const PtraceENOENT = -7

// PtraceAvailable reports whether the out-of-process single-step tracer can run
// on this host (Linux x86-64) AND libasmtest_hwtrace loaded. It never panics, so
// callers (and the test) self-skip cleanly when the lib or the host is unfit.
func PtraceAvailable() bool {
	return C.asmtest_hw_is_loaded() != 0 && C.asmtest_go_pt_available() != 0
}

// PtraceSkipReason is a human-readable reason PtraceAvailable() is false (or
// "available"). Useful for the self-skip message.
func PtraceSkipReason() string {
	if C.asmtest_hw_is_loaded() == 0 {
		return "libasmtest_hwtrace not loaded (set ASMTEST_HWTRACE_LIB or build with `make shared-hwtrace`)"
	}
	buf := make([]byte, 160)
	C.asmtest_go_pt_skip_reason((*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)))
	return C.GoString((*C.char)(unsafe.Pointer(&buf[0])))
}

// PtraceTraceCall forks a tracee that calls the code at codeBase (codeLen bytes,
// already executable in this process — e.g. an HwNativeCode's Base()/Len()) with
// up to six integer args per the SysV ABI, single-steps it OUT OF PROCESS, and
// fills trace. It returns the routine's return value (the child's RAX at the ret).
func PtraceTraceCall(codeBase unsafe.Pointer, codeLen int, args []int64, trace *HwTrace) (int64, error) {
	n := len(args)
	// asmtest_ptrace_trace_call reads `nargs` longs; pass a valid pointer even
	// for the zero-arg case.
	arr := make([]C.long, n)
	if n == 0 {
		arr = make([]C.long, 1)
	}
	for i, a := range args {
		arr[i] = C.long(a)
	}
	var result C.long
	rc := C.asmtest_go_pt_trace_call(codeBase, C.size_t(codeLen), &arr[0],
		C.int(n), &result, trace.h)
	if rc != ptraceOK {
		return 0, fmt.Errorf("asmtest_ptrace_trace_call failed: %d", int(rc))
	}
	return int64(result), nil
}

// PtraceBlockstepAvailable reports whether the BTF block-step variant
// (PTRACE_SINGLEBLOCK — one #DB per TAKEN branch instead of one per instruction)
// can run here: x86-64 Linux with a functional PTRACE_SINGLEBLOCK and Capstone for
// the intra-block reconstruction. Hang-proof, cached; callers self-skip on false.
func PtraceBlockstepAvailable() bool {
	return C.asmtest_hw_is_loaded() != 0 && C.asmtest_go_pt_blockstep_available() != 0
}

// PtraceTraceCallBlockstep is the block-step variant of PtraceTraceCall: it drives
// PTRACE_SINGLEBLOCK (DEBUGCTL.BTF), stopping once per TAKEN branch and
// reconstructing the intra-block instructions with Capstone — the same
// insns/blocks stream as PtraceTraceCall at a fraction of the stops. Probe first
// with PtraceBlockstepAvailable. Complete at moderate overhead, NOT cheap: each
// block still costs a full ptrace round-trip.
func PtraceTraceCallBlockstep(codeBase unsafe.Pointer, codeLen int, args []int64, trace *HwTrace) (int64, error) {
	n := len(args)
	arr := make([]C.long, n)
	if n == 0 {
		arr = make([]C.long, 1)
	}
	for i, a := range args {
		arr[i] = C.long(a)
	}
	var result C.long
	rc := C.asmtest_go_pt_trace_call_blockstep(codeBase, C.size_t(codeLen), &arr[0],
		C.int(n), &result, trace.h)
	if rc != ptraceOK {
		return 0, fmt.Errorf("asmtest_ptrace_trace_call_blockstep failed: %d", int(rc))
	}
	return int64(result), nil
}

// PtraceTraceAttached traces a region [base, base+length) in a SEPARATE,
// already-ptrace-stopped process (the caller owns PTRACE_ATTACH/DETACH). The
// target's bytes are read via process_vm_readv. Returns the target's RAX at the
// region exit.
func PtraceTraceAttached(pid int, base uintptr, length int, trace *HwTrace) (int64, error) {
	var result C.long
	rc := C.asmtest_go_pt_trace_attached(C.int(pid), unsafe.Pointer(base),
		C.size_t(length), &result, trace.h)
	if rc != ptraceOK {
		return 0, fmt.Errorf("asmtest_ptrace_trace_attached failed: %d", int(rc))
	}
	return int64(result), nil
}

// PtraceTraceAttachedBlockstep is the block-step variant of PtraceTraceAttached:
// one #DB per TAKEN branch (intra-block instructions reconstructed with Capstone),
// same contract otherwise — the rootless managed-runtime completeness fallback at
// a fraction of the stops. Probe first with PtraceBlockstepAvailable.
func PtraceTraceAttachedBlockstep(pid int, base uintptr, length int, trace *HwTrace) (int64, error) {
	var result C.long
	rc := C.asmtest_go_pt_trace_attached_blockstep(C.int(pid), unsafe.Pointer(base),
		C.size_t(length), &result, trace.h)
	if rc != ptraceOK {
		return 0, fmt.Errorf("asmtest_ptrace_trace_attached_blockstep failed: %d", int(rc))
	}
	return int64(result), nil
}

// PtraceTraceAttachedVersioned is PtraceTraceAttached decoded against TIME-CORRECT
// bytes from a CodeImage recorder instead of a single live snapshot. For a JIT whose
// code at base was patched, freed, or had its address reused during the run, a fresh
// read returns the WRONG bytes; passing img + the logical timestamp when (0 = latest)
// the region was live at makes block normalization use the bytes that were actually
// executing. img must already be tracking a region covering [base, base+length)
// (CodeImage.Track); a nil img is exactly PtraceTraceAttached. Returns the target's
// RAX at the region exit.
func PtraceTraceAttachedVersioned(pid int, base uintptr, length int, img *CodeImage, when uint64, trace *HwTrace) (int64, error) {
	var imgPtr unsafe.Pointer
	if img != nil {
		imgPtr = img.h
	}
	var result C.long
	rc := C.asmtest_go_pt_trace_attached_versioned(C.int(pid), unsafe.Pointer(base),
		C.size_t(length), imgPtr, C.uint64_t(when), &result, trace.h)
	if rc != ptraceOK {
		return 0, fmt.Errorf("asmtest_ptrace_trace_attached_versioned failed: %d", int(rc))
	}
	return int64(result), nil
}

// PtraceRunTo runs an already-attached, ptrace-stopped target forward until it
// reaches addr (a software breakpoint that fires when the program itself next calls
// in), leaving it stopped there ready for PtraceTraceAttached — the step that makes a
// resolved JIT method traceable when you don't control call timing. It returns the
// status code (PTRACE_OK, or PTRACE_ENOENT if the target exited first). The caller
// owns PTRACE_ATTACH/DETACH.
func PtraceRunTo(pid int, addr uintptr) int {
	return int(C.asmtest_go_pt_run_to(C.int(pid), unsafe.Pointer(addr)))
}

// ProcRegionByAddr finds the executable mapping in /proc/<pid>/maps that contains
// addr and returns its extent (base, len). ok is false when no executable mapping
// contains addr (or on a read failure).
func ProcRegionByAddr(pid int, addr uintptr) (base, length uintptr, ok bool) {
	var b unsafe.Pointer
	var l C.size_t
	rc := C.asmtest_go_proc_region_by_addr(C.int(pid), unsafe.Pointer(addr), &b, &l)
	if rc != ptraceOK {
		return 0, 0, false
	}
	return uintptr(b), uintptr(l), true
}

// ProcPerfmapSymbol resolves a JIT method by name in /tmp/perf-<pid>.map and
// returns its extent (base, len). ok is false when there is no such symbol or no
// map file.
func ProcPerfmapSymbol(pid int, name string) (base, length uintptr, ok bool) {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	var b unsafe.Pointer
	var l C.size_t
	rc := C.asmtest_go_proc_perfmap_symbol(C.int(pid), cs, &b, &l)
	if rc != ptraceOK {
		return 0, 0, false
	}
	return uintptr(b), uintptr(l), true
}

// JitMethod is a JIT method resolved from a jitdump: its load address, size, the
// JIT's timestamp/index, and (optionally) the recorded native code bytes. Mirrors
// asmtest_jitdump_entry_t + the Python wrapper's JitMethod.
type JitMethod struct {
	CodeAddr  uint64
	CodeSize  uint64
	Timestamp uint64
	CodeIndex uint64
	Code      []byte
}

// JitdumpFind reads the Linux perf jitdump at path (or /tmp/jit-<pid>.dump when
// path is empty) and resolves a method by name to its load address, size,
// timestamp/index, and — when wantBytes > 0 — up to wantBytes of the recorded
// native code. The latest re-JIT body (highest timestamp) wins. ok is false when
// no such method / no file / not a jitdump.
func JitdumpFind(path string, name string, pid int, wantBytes int) (JitMethod, bool) {
	var cPath *C.char
	if path != "" {
		cPath = C.CString(path)
		defer C.free(unsafe.Pointer(cPath))
	}
	cName := C.CString(name)
	defer C.free(unsafe.Pointer(cName))

	var entry C.asmtest_jitdump_entry_t
	var bytesOut *C.uint8_t
	var blen C.size_t
	var buf []byte
	if wantBytes > 0 {
		buf = make([]byte, wantBytes)
		bytesOut = (*C.uint8_t)(unsafe.Pointer(&buf[0]))
	}
	rc := C.asmtest_go_jitdump_find(cPath, C.int(pid), cName, &entry, bytesOut,
		C.size_t(wantBytes), &blen)
	if rc != ptraceOK {
		return JitMethod{}, false
	}
	m := JitMethod{
		CodeAddr:  uint64(entry.code_addr),
		CodeSize:  uint64(entry.code_size),
		Timestamp: uint64(entry.timestamp),
		CodeIndex: uint64(entry.code_index),
	}
	if wantBytes > 0 {
		m.Code = append([]byte(nil), buf[:int(blen)]...)
	}
	return m, true
}

// ---- Time-aware code-image recorder (asmtest_codeimage.h) ----
//
// A userspace PERF_RECORD_TEXT_POKE: a TIMESTAMPED CODE-IMAGE TIMELINE for one
// target process. Track() snapshots a region's bytes (version 0) and arms
// write-protect-async on its pages; Refresh() re-snapshots only the pages written
// since the last arm, appending a new version stamped with the next monotonic
// sequence; BytesAt(addr, when) answers "what bytes were live at addr as of
// sequence when" — exactly what a branch-trace decoder or the W2 block-normalizer
// needs to reconstruct a JIT method whose address was reused mid-trace. Change
// detection is pure userspace (soft-dirty / PAGEMAP_SCAN) and works on a FOREIGN
// process; pid == 0 records THIS process. Shares the same already-dlopen'd
// libasmtest_hwtrace as the surfaces above. Mirrors the Python wrapper's CodeImage.

// ciOK is the success status returned by the code-image calls (mirrors the C macro
// ASMTEST_CI_OK).
const ciOK = 0

// CodeImageENOENT is the status meaning an address was never tracked, or there is no
// version at/before the requested sequence (mirrors the C macro ASMTEST_CI_ENOENT).
const CodeImageENOENT = -7

// CodeImage event kinds — how a code-emission event was observed by the eBPF detector
// (mirror the C macros ASMTEST_CI_KIND_*).
const (
	CodeImageKindMprotect = 1 // mprotect(...PROT_EXEC...) — the common JIT edge
	CodeImageKindMmap     = 2 // mmap(...PROT_EXEC...); addr is the real base
	CodeImageKindMemfd    = 3 // memfd_create — staging hint; correlate via fd
)

// CodeImageAvailable reports whether the userspace recorder can detect page changes
// on this host (PAGEMAP_SCAN, or the soft-dirty fallback) AND libasmtest_hwtrace
// loaded. It never panics, so callers (and the test) self-skip cleanly when the lib
// or the host is unfit.
func CodeImageAvailable() bool {
	return C.asmtest_hw_is_loaded() != 0 && C.asmtest_go_ci_available() != 0
}

// CodeImageSkipReason is a human-readable reason CodeImageAvailable() is false (or
// "available"). Useful for the self-skip message.
func CodeImageSkipReason() string {
	if C.asmtest_hw_is_loaded() == 0 {
		return "libasmtest_hwtrace not loaded (set ASMTEST_HWTRACE_LIB or build with `make shared-hwtrace`)"
	}
	buf := make([]byte, 160)
	C.asmtest_go_ci_skip_reason((*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)))
	return C.GoString((*C.char)(unsafe.Pointer(&buf[0])))
}

// CodeImageBpfAvailable reports whether the optional eBPF emission detector can load
// and attach on this host (built with libbpf, kernel BTF present, sufficient
// privilege) AND libasmtest_hwtrace loaded. The userspace poll path is the
// always-available fallback when this is false.
func CodeImageBpfAvailable() bool {
	return C.asmtest_hw_is_loaded() != 0 && C.asmtest_go_ci_bpf_available() != 0
}

// CodeImageBpfSkipReason is a human-readable reason CodeImageBpfAvailable() is false
// (or "available").
func CodeImageBpfSkipReason() string {
	if C.asmtest_hw_is_loaded() == 0 {
		return "libasmtest_hwtrace not loaded (set ASMTEST_HWTRACE_LIB or build with `make shared-hwtrace`)"
	}
	buf := make([]byte, 160)
	C.asmtest_go_ci_bpf_skip_reason((*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf)))
	return C.GoString((*C.char)(unsafe.Pointer(&buf[0])))
}

// CodeImageEvent is a code-emission event from the eBPF detector — when and where new
// executable code appeared for the target. Mirrors asmtest_codeimage_event_t.
type CodeImageEvent struct {
	Addr      uint64 // published base address (0 for a memfd hint)
	Len       uint64 // byte length (0 for a memfd hint)
	Timestamp uint64 // bpf_ktime_get_ns() at emission
	Pid       uint32 // tgid that published
	Tid       uint32 // thread that published
	Kind      uint32 // CodeImageKind*
	Fd        int32  // memfd fd, or -1
}

// CodeImage is a timestamped code-image timeline for one target process. Release it
// with Close (or the alias Free).
type CodeImage struct{ h unsafe.Pointer }

// NewCodeImage creates a timeline recording pid's memory (pid == 0 => this process).
// The caller owns any ptrace attach policy for a foreign pid; the recorder itself
// only reads memory and scans pagemap. Returns nil on allocation failure (or when
// libasmtest_hwtrace is not loaded).
func NewCodeImage(pid int) *CodeImage {
	if C.asmtest_hw_is_loaded() == 0 {
		return nil
	}
	h := C.asmtest_go_ci_new(C.int(pid))
	if h == nil {
		return nil
	}
	return &CodeImage{h: h}
}

// Track begins tracking [base, base+length) in the target: snapshot version 0 now and
// arm write-protect-async on its pages so the next Refresh sees changes. May be called
// for several disjoint regions.
func (c *CodeImage) Track(base uintptr, length int) error {
	rc := C.asmtest_go_ci_track(c.h, unsafe.Pointer(base), C.size_t(length))
	if rc != ciOK {
		return fmt.Errorf("asmtest_codeimage_track failed: %d", int(rc))
	}
	return nil
}

// Refresh scans the tracked ranges for pages changed since the last arm, re-snapshots
// each changed page as a NEW version, and re-arms write-protect. Returns the number of
// new versions recorded (>= 0), or an error on a negative status.
func (c *CodeImage) Refresh() (int, error) {
	rc := int(C.asmtest_go_ci_refresh(c.h))
	if rc < 0 {
		return rc, fmt.Errorf("asmtest_codeimage_refresh failed: %d", rc)
	}
	return rc, nil
}

// Now is the current capture sequence — a monotonic logical timestamp the caller
// stamps trace positions against. Advances by one for every version recorded (Track +
// each Refresh change). 0 before anything is tracked.
func (c *CodeImage) Now() uint64 { return uint64(C.asmtest_go_ci_now(c.h)) }

// BytesAt returns the bytes live at addr as of capture sequence when (when == 0 =>
// the latest version), as a freshly-copied slice from addr to the end of that
// version's region. It is nil when addr is not in any tracked region or there is no
// version at/before when (ASMTEST_CI_ENOENT) — and on any other failure.
func (c *CodeImage) BytesAt(addr uintptr, when uint64) []byte {
	var out *C.uint8_t
	var outLen C.size_t
	rc := C.asmtest_go_ci_bytes_at(c.h, unsafe.Pointer(addr), C.uint64_t(when), &out, &outLen)
	if rc != ciOK || out == nil {
		return nil
	}
	// *out points at borrowed bytes owned by the timeline; copy them out so the
	// returned slice does not alias C memory freed by Close.
	return C.GoBytes(unsafe.Pointer(out), C.int(outLen))
}

// WatchBpf loads the CO-RE eBPF program, filters it to this image's pid, and attaches
// it; subsequent PollBpf calls drain emission events. Returns the status code
// (ciOK on success, or a negative ASMTEST_CI_* status — ENOSYS without libbpf,
// EUNAVAIL without privilege/BTF, ELOAD on load/attach failure).
func (c *CodeImage) WatchBpf() int { return int(C.asmtest_go_ci_watch_bpf(c.h)) }

// PollBpf drains ready emission events from the BPF ring buffer into the image's
// internal queue. timeoutMs == 0 is a NON-BLOCKING drain (so it interleaves with a
// single-step loop); > 0 waits up to that long. Returns the number of events queued
// (>= 0), or a negative status.
func (c *CodeImage) PollBpf(timeoutMs int) int {
	return int(C.asmtest_go_ci_poll_bpf(c.h, C.int(timeoutMs)))
}

// NextEvent pops one queued emission event. ok is true when an event was returned,
// false when the queue is empty (or on a negative status).
func (c *CodeImage) NextEvent() (CodeImageEvent, bool) {
	var ev C.asmtest_codeimage_event_t
	if C.asmtest_go_ci_next(c.h, &ev) != 1 {
		return CodeImageEvent{}, false
	}
	return CodeImageEvent{
		Addr:      uint64(ev.addr),
		Len:       uint64(ev.len),
		Timestamp: uint64(ev.timestamp),
		Pid:       uint32(ev.pid),
		Tid:       uint32(ev.tid),
		Kind:      uint32(ev.kind),
		Fd:        int32(ev.fd),
	}, true
}

// Close frees the timeline and all recorded versions, detaching any eBPF watch. Safe
// to call more than once.
func (c *CodeImage) Close() {
	if c.h != nil {
		C.asmtest_go_ci_free(c.h)
		c.h = nil
	}
}

// Free is an alias for Close, matching the Free naming of the other handle types in
// this package (HwNativeCode, HwTrace).
func (c *CodeImage) Free() { c.Close() }

// ---- Call descent (asmtest_descent_t) ----
//
// Configure how the ptrace stepper handles the call-outs it would otherwise step
// over, and read back the recorded edges + nested frames. Four levels (the Descent*
// constants). Pass a *Descent to the *Ex trace entry points; frame 0 is the root
// region (a superset of the flat trace) and descended callees are frames 1..N. Mirrors
// the Python wrapper's Descent class.
//
// The resolver / denylist upcalls are GC-safe: their trampolines are //export'd
// C-callable Go functions (descent_cgo.go), and the Go closures they dispatch to are
// held in a package-level registry keyed by the handle for the handle's lifetime — so
// Go's moving GC can neither collect nor relocate them while the out-of-process
// single-stepper is calling back into them mid-step (the go(cgo) analogue of the
// Python wrapper keeping the CFUNCTYPE trampolines referenced).

// asmtest_descent_level_t — call-descent policy. OFF steps over and records nothing
// (today's behaviour); RECORD_EDGES records (call-site -> callee) edges and steps
// over; DESCEND_KNOWN single-steps INTO calls that resolve (allow-set / resolver),
// else records an edge; DESCEND_ALL steps INTO everything (denylist + budget +
// watchdog gated).
const (
	DescentOff          = 0
	DescentRecordEdges  = 1
	DescentDescendKnown = 2
	DescentDescendAll   = 3
)

// DescentEdge is one recorded (call-site -> callee) edge: Site is the call
// instruction's offset from the region entry, Target is the callee's ABSOLUTE
// address, Depth is the caller frame's depth (0 = frame 0).
type DescentEdge struct {
	Site   uint64
	Target uint64
	Depth  uint32
}

// DescentResolver decides, per call-out, whether to descend into callee and — when
// known — its extent. Return (true, base, length) to descend into the callee region
// [base, base+length); (false, 0, 0) to step over. A true with length 0 steps over
// (mirrors the Python wrapper, and the C engine ignores a zero-length resolution).
type DescentResolver func(callee uint64) (descend bool, base uint64, length uint64)

// DescentDenylist returns true to REFUSE descent into callee (step over it), false to
// allow it. Consulted in addition to the deny-region set.
type DescentDenylist func(callee uint64) bool

// Descent owns an asmtest_descent_t handle. Release it with Free (or the alias Close);
// both are idempotent — this wrapper NULLs the handle and the native free is itself
// NULL-safe, so a double free is a no-op (mirroring the trace-handle discipline).
type Descent struct{ h unsafe.Pointer }

// NewDescent allocates a descent handle at level (one of the Descent* constants).
// Returns nil when libasmtest_hwtrace is not loaded or the allocation fails.
func NewDescent(level int) *Descent {
	if C.asmtest_hw_is_loaded() == 0 {
		return nil
	}
	h := C.asmtest_go_desc_new(C.int(level))
	if h == nil {
		return nil
	}
	return &Descent{h: h}
}

// Free releases the handle and drops any registered resolver / denylist closures from
// the package registry. Safe to call more than once.
func (d *Descent) Free() {
	if d.h != nil {
		descentUnregister(d.h)
		C.asmtest_go_desc_free(d.h)
		d.h = nil
	}
}

// Close is an alias for Free, matching the CodeImage naming.
func (d *Descent) Close() { d.Free() }

// ---- configuration (in) ----

// SetMaxDepth caps nested descent depth (frame 0 is depth 0). 0 restores the default.
func (d *Descent) SetMaxDepth(maxDepth uint32) {
	C.asmtest_go_desc_set_max_depth(d.h, C.uint32_t(maxDepth))
}

// SetInsnBudget sets the total single-step instruction budget across all descended
// frames. 0 restores the default.
func (d *Descent) SetInsnBudget(budget uint64) {
	C.asmtest_go_desc_set_insn_budget(d.h, C.uint64_t(budget))
}

// SetWatchdogMs sets the real-time watchdog (milliseconds) for a descended run. 0
// restores the default.
func (d *Descent) SetWatchdogMs(ms uint32) {
	C.asmtest_go_desc_set_watchdog_ms(d.h, C.uint32_t(ms))
}

// UseDefaultDenylist arms the built-in L3 default denylist (PLT resolver /
// vdso / GC-JIT modules; plus blocking-libc entry points on the fork path).
func (d *Descent) UseDefaultDenylist() {
	C.asmtest_go_desc_use_default_denylist(d.h)
}

// AllowRegion adds [base, base+length) to the level-2 allow-set (descend into calls
// landing inside). Returns 0 on success, negative on OOM.
func (d *Descent) AllowRegion(base uintptr, length int) int {
	return int(C.asmtest_go_desc_allow_region(d.h, unsafe.Pointer(base), C.size_t(length)))
}

// DenyRegion adds [base, base+length) to the level-3 deny-set (never descend into it).
// Returns 0 on success, negative on OOM.
func (d *Descent) DenyRegion(base uintptr, length int) int {
	return int(C.asmtest_go_desc_deny_region(d.h, unsafe.Pointer(base), C.size_t(length)))
}

// SetResolver installs a level-2/3 resolver upcall. The closure is kept alive in a
// package-level registry keyed by this handle until Free, so a moving GC cannot
// collect or relocate it while the stepper calls back into it.
func (d *Descent) SetResolver(fn DescentResolver) {
	descentSetResolver(d.h, fn)
	C.asmtest_go_desc_set_resolver(d.h, d.h)
}

// SetDenylist installs a level-3 denylist upcall (kept alive like SetResolver).
func (d *Descent) SetDenylist(fn DescentDenylist) {
	descentSetDenylist(d.h, fn)
	C.asmtest_go_desc_set_denylist(d.h, d.h)
}

// ---- results (out) ----

// Edges is every stepped-over call (level >= 1) as (Site, Target, Depth).
func (d *Descent) Edges() []DescentEdge {
	n := uint64(C.asmtest_go_desc_edges_len(d.h))
	out := make([]DescentEdge, n)
	for i := uint64(0); i < n; i++ {
		out[i] = DescentEdge{
			Site:   uint64(C.asmtest_go_desc_edge_site(d.h, C.size_t(i))),
			Target: uint64(C.asmtest_go_desc_edge_target(d.h, C.size_t(i))),
			Depth:  uint32(C.asmtest_go_desc_edge_depth(d.h, C.size_t(i))),
		}
	}
	return out
}

// FramesLen is the number of recorded frames (frame 0 is the root region).
func (d *Descent) FramesLen() int { return int(C.asmtest_go_desc_frames_len(d.h)) }

// FrameBase is frame f's ABSOLUTE base address.
func (d *Descent) FrameBase(f int) uint64 {
	return uint64(C.asmtest_go_desc_frame_base(d.h, C.size_t(f)))
}

// FrameLen is frame f's byte length.
func (d *Descent) FrameLen(f int) uint64 {
	return uint64(C.asmtest_go_desc_frame_len(d.h, C.size_t(f)))
}

// FrameDepth is frame f's nesting depth (0 = frame 0).
func (d *Descent) FrameDepth(f int) uint32 {
	return uint32(C.asmtest_go_desc_frame_depth(d.h, C.size_t(f)))
}

// FrameParent is frame f's parent frame index, or -1 for the root.
func (d *Descent) FrameParent(f int) int32 {
	return int32(C.asmtest_go_desc_frame_parent(d.h, C.size_t(f)))
}

// FrameInsns is frame f's ordered instruction-offset stream.
func (d *Descent) FrameInsns(f int) []uint64 {
	n := uint64(C.asmtest_go_desc_frame_insn_count(d.h, C.size_t(f)))
	out := make([]uint64, n)
	for i := uint64(0); i < n; i++ {
		out[i] = uint64(C.asmtest_go_desc_frame_insn_at(d.h, C.size_t(f), C.size_t(i)))
	}
	return out
}

// FrameBlocks is frame f's distinct basic-block start offsets.
func (d *Descent) FrameBlocks(f int) []uint64 {
	n := uint64(C.asmtest_go_desc_frame_block_count(d.h, C.size_t(f)))
	out := make([]uint64, n)
	for i := uint64(0); i < n; i++ {
		out[i] = uint64(C.asmtest_go_desc_frame_block_at(d.h, C.size_t(f), C.size_t(i)))
	}
	return out
}

// Truncated reports whether a pool overflowed / a byte failed to decode (the record is
// incomplete).
func (d *Descent) Truncated() bool { return C.asmtest_go_desc_truncated(d.h) != 0 }

// DepthCapped reports whether descent stopped at a policy limit (max_depth / budget /
// recursion cap), as distinct from a pool overflow.
func (d *Descent) DepthCapped() bool { return C.asmtest_go_desc_depth_capped(d.h) != 0 }

// ---- Descending trace entry points (asmtest_ptrace_*_ex) ----

// PtraceTraceCallEx is the descending variant of PtraceTraceCall: it threads a
// *Descent through the single-step loop so call-outs are recorded as edges and (at
// level >= 2) descended as nested frames. trace (the flat frame-0 view) may be nil to
// record only into descent, and descent may be nil (then it is exactly
// PtraceTraceCall). CRITICAL: region is the TRACED region's byte length, NOT the whole
// allocation — pass it when the call target is an in-blob sibling that must stay
// OUTSIDE the region (otherwise the sibling falls inside and mis-records as recursion).
// region <= 0 defaults to codeLen (the whole allocation).
func PtraceTraceCallEx(codeBase unsafe.Pointer, codeLen int, args []int64, trace *HwTrace,
	descent *Descent, region int) (int64, error) {
	n := len(args)
	arr := make([]C.long, n)
	if n == 0 {
		arr = make([]C.long, 1)
	}
	for i, a := range args {
		arr[i] = C.long(a)
	}
	if region <= 0 {
		region = codeLen
	}
	var th, dh unsafe.Pointer
	if trace != nil {
		th = trace.h
	}
	if descent != nil {
		dh = descent.h
	}
	var result C.long
	rc := C.asmtest_go_pt_trace_call_ex(codeBase, C.size_t(region), &arr[0], C.int(n),
		&result, th, dh)
	if rc != ptraceOK {
		return 0, fmt.Errorf("asmtest_ptrace_trace_call_ex failed: %d", int(rc))
	}
	return int64(result), nil
}

// PtraceTraceAttachedEx is the descending variant of PtraceTraceAttached for an
// externally-attached, already-ptrace-stopped process. trace and/or descent may be nil.
func PtraceTraceAttachedEx(pid int, base uintptr, length int, trace *HwTrace,
	descent *Descent) (int64, error) {
	var th, dh unsafe.Pointer
	if trace != nil {
		th = trace.h
	}
	if descent != nil {
		dh = descent.h
	}
	var result C.long
	rc := C.asmtest_go_pt_trace_attached_ex(C.int(pid), unsafe.Pointer(base),
		C.size_t(length), &result, th, dh)
	if rc != ptraceOK {
		return 0, fmt.Errorf("asmtest_ptrace_trace_attached_ex failed: %d", int(rc))
	}
	return int64(result), nil
}

// PtraceTraceAttachedVersionedEx is the descending variant of
// PtraceTraceAttachedVersioned (decoded against code-image-versioned bytes). A nil img
// is exactly PtraceTraceAttachedEx; trace and/or descent may be nil.
func PtraceTraceAttachedVersionedEx(pid int, base uintptr, length int, img *CodeImage,
	when uint64, trace *HwTrace, descent *Descent) (int64, error) {
	var imgPtr, th, dh unsafe.Pointer
	if img != nil {
		imgPtr = img.h
	}
	if trace != nil {
		th = trace.h
	}
	if descent != nil {
		dh = descent.h
	}
	var result C.long
	rc := C.asmtest_go_pt_trace_attached_versioned_ex(C.int(pid), unsafe.Pointer(base),
		C.size_t(length), imgPtr, C.uint64_t(when), &result, th, dh)
	if rc != ptraceOK {
		return 0, fmt.Errorf("asmtest_ptrace_trace_attached_versioned_ex failed: %d", int(rc))
	}
	return int64(result), nil
}
