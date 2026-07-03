/*
 * asmtest_drtrace.h — optional in-process native runtime tracing, backed by
 * DynamoRIO's Application Interface (dr_app_setup / dr_app_start / ...).
 *
 * Where the Unicorn emulator tier (asmtest_emu.h) traces isolated guest bytes,
 * this tier traces code as it runs **natively inside the real process**: the app
 * initializes DynamoRIO at startup, marks a trace region with begin/end markers,
 * runs native or generated host-native code, and reads back the covered basic
 * blocks (and, optionally, the ordered instruction stream) as `asmtest_trace_t`
 * offsets — the same shape the emulator and hardware-trace tiers produce.
 *
 * Two cooperating libraries:
 *   - libasmtest_drapp   — this app-facing API; initializes DR, owns the
 *                          lifecycle state machine, exposes the begin/end markers
 *                          and region registration.
 *   - libasmtest_drclient — a DynamoRIO client (.so) loaded in-process that
 *                          observes the markers, instruments registered ranges,
 *                          and reconciles coverage into the app-owned trace.
 *
 * The tier is built only when DynamoRIO is available (DYNAMORIO_DIR/HOME) and is
 * kept entirely out of the core libasmtest and the libasmtest_emu superset.
 * Linux x86-64 only for now. See docs/native-tracing.md.
 */
#ifndef ASMTEST_DRTRACE_H
#define ASMTEST_DRTRACE_H

#include <stddef.h>
#include <stdint.h>

#include "asmtest_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes returned by the lifecycle and registration calls. */
#define ASMTEST_DR_OK      0
#define ASMTEST_DR_EINVAL  (-1) /* bad argument                              */
#define ASMTEST_DR_ESTATE  (-2) /* called in the wrong lifecycle state       */
#define ASMTEST_DR_ETHREAD (-3) /* start not on the setup thread             */
#define ASMTEST_DR_ENODR   (-4) /* built without DynamoRIO / DR unavailable  */
#define ASMTEST_DR_ENOSYS  (-5) /* feature not compiled in (e.g. no Keystone)*/
#define ASMTEST_DR_EFULL   (-6) /* region table full                         */
#define ASMTEST_DR_ENOENT  (-7) /* named region not found                    */

/* Recording mode. The process-init default; per-trace recording is actually
 * driven by the capacities of the registered asmtest_trace_t (instruction
 * recording when insns_cap > 0, block recording when blocks_cap > 0). EVENTS is
 * reserved and unimplemented. */
typedef enum {
    ASMTEST_DRTRACE_BLOCKS = 0,
    ASMTEST_DRTRACE_INSNS = 1,
    ASMTEST_DRTRACE_EVENTS = 2, /* reserved; not implemented */
} asmtest_drtrace_mode_t;

typedef struct {
    const char
        *dynamorio_home;     /* DR runtime root (optional; diagnostics)     */
    const char *client_path; /* path to libasmtest_drclient.so              */
    const char *client_options; /* extra client options (optional)            */
    asmtest_drtrace_mode_t
        mode; /* process-init default recording mode        */
} asmtest_drtrace_options_t;

/* 1 if this build includes the DynamoRIO tier AND libdynamorio is loadable at
 * runtime, else 0. Callers (and the smoke test) use this to self-skip cleanly,
 * mirroring asm_available()/emu disas_available(). */
int asmtest_dr_available(void);

/* Lifecycle (state machine UNINIT -> INIT -> STARTED -> STOPPED -> SHUTDOWN):
 *   init     performs dr_app_setup + client configuration (no takeover yet);
 *   start    performs dr_app_start (DR takes over) — MUST run on the setup
 *            thread, from INIT or STOPPED;
 *   stop     performs dr_app_stop (back to native), from STARTED;
 *   shutdown performs dr_app_stop_and_cleanup; SHUTDOWN is TERMINAL.
 * A second init is a no-op returning ASMTEST_DR_OK. Out-of-order calls return a
 * defined error rather than invoking DR (whose ordering violations are UB).
 * ONE lifecycle per process: after shutdown, init returns ASMTEST_DR_ESTATE —
 * DynamoRIO's in-process re-attach is unreliable, so trace again from a fresh
 * process. start/stop may be cycled repeatedly while single-threaded (the
 * bracket model); doing so under concurrent threads is the fragile case. */
int asmtest_dr_init(const asmtest_drtrace_options_t *opts);
int asmtest_dr_start(void);
int asmtest_dr_stop(void);
void asmtest_dr_shutdown(void);

/* 1 if the calling thread is currently executing under DynamoRIO's control
 * (dr_app_running_under_dynamorio), else 0. Used by the managed-host gate to
 * measure thread-takeover scope after asmtest_dr_start(). */
int asmtest_dr_under_dynamorio(void);

/* Register a non-overlapping native code range under `name`, recording coverage
 * into the app-owned `trace` (allocate it with asmtest_trace_new). The app may
 * free its copy of `name` after the call. Returns ASMTEST_DR_OK or an error. */
int asmtest_dr_register_region(const char *name, void *base, size_t len,
                               asmtest_trace_t *trace);
int asmtest_dr_unregister_region(const char *name);

/* Region markers. Real exported functions (the client resolves their PCs and
 * wraps them), NOT macros. begin(name) opens recording for the named region on
 * the calling thread; end(name) closes it. They return void; balance errors
 * (end without begin, mismatched end) are surfaced out-of-band via
 * asmtest_dr_marker_error(). Markers must be balanced — prefer a scoped wrapper
 * in each binding. */
void asmtest_trace_begin(const char *name);
void asmtest_trace_end(const char *name);

/* Count of illegal marker operations observed (end without matching begin, or a
 * mismatched end) since init. 0 means every marker was balanced. */
int asmtest_dr_marker_error(void);

/* Symbol/function mode (native-trace Phase 7): trace a named exported function
 * WITHOUT explicit begin/end markers. The client resolves `symbol`'s entry PC
 * with dr_get_proc_address and records every execution of blocks in
 * [entry, entry+max_len) into `trace` — recording is always-on for the range, so
 * no begin/end is needed. `max_len` bounds the function (pass its size, or a
 * generous upper bound; ranges must not overlap). Best-effort: less robust for
 * inlined or generated code than explicit markers, where the symbol may not have
 * a single stable entry PC. Returns ASMTEST_DR_OK or an error. */
int asmtest_dr_register_symbol(const char *symbol, size_t max_len,
                               asmtest_trace_t *trace);

/* A real exported function used to exercise symbol mode: trace it by NAME with
 * asmtest_dr_register_symbol("asmtest_symbol_demo", ...) and call it with no
 * begin/end markers. noinline + default visibility give it a stable entry PC the
 * client resolves via dr_get_proc_address. Computes a*2 + b (small enough to fit
 * the entry block); the language-binding native-trace tests trace it so every
 * binding shares one resolvable symbol. Returns 10 for (3, 4). */
long asmtest_symbol_demo(long a, long b);

/* ------------------------------------------------------------------ */
/* Host-native executable code (native-trace Phase 4)                  */
/*                                                                     */
/* DynamoRIO traces code running natively, so it needs real executable */
/* host memory — distinct from the emulator's guest load address. This */
/* maps W^X-correct executable memory, copies the bytes, and (when the */
/* range is registered) lets the caller invoke it through a function    */
/* pointer and trace the call.                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    void *base; /* executable mapping holding the bytes (offset 0 = entry) */
    size_t len; /* number of code bytes                                    */
} asmtest_exec_code_t;

/* Map executable memory and copy `len` bytes of host-native machine code into it
 * (mmap PROT_NONE -> mprotect RW to copy -> mprotect RX, icache flushed). The
 * code is materialized at its actual runtime address so PC-relative and branch
 * targets resolve. Returns ASMTEST_DR_OK; *out holds {base,len} on success. */
int asmtest_exec_alloc(const uint8_t *bytes, size_t len,
                       asmtest_exec_code_t *out);

/* Assemble host-native assembly text (x86-64) with Keystone and materialize it
 * via asmtest_exec_alloc. `syntax` matches asmtest_assemble.h's asm_syntax_t.
 * Returns ASMTEST_DR_ENOSYS when this build has no Keystone. */
int asmtest_asm_exec_native(const char *src, int syntax,
                            asmtest_exec_code_t *out);

/* Unmap the executable memory. This does NOT unregister the range — regions are
 * keyed by name and this function only has {base,len} — so if the range was
 * registered, the caller MUST asmtest_dr_unregister_region(name) FIRST (which
 * makes the client drop its cached translation), then call this. Order:
 * unregister, then free. */
void asmtest_exec_free(asmtest_exec_code_t *code);

#ifdef __cplusplus
}
#endif

#endif /* ASMTEST_DRTRACE_H */
