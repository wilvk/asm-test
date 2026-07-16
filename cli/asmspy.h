/*
 * asmspy.h — shared contract for the asmspy CLI (cli/).
 *
 * asmspy is a small ncurses front-end over the asm-test out-of-process tracer:
 * pick a running process, then watch either its live syscalls (with data) or a
 * live disassembly + call-graph of a chosen function — all out of band via the
 * ptrace attach seam (examples/attach_trace.c, examples/syscall_log.c).
 *
 * Three translation units share this header:
 *   asmspy_proc.c   — /proc enumeration + an ELF .symtab/.dynsym function resolver
 *   asmspy_engine.c — the ptrace engines (syscall stream + region sampler)
 *   asmspy.c        — main, the headless subcommands, and the ncurses TUI
 *
 * IMPORTANT (ptrace is per-thread): every ptrace call + waitpid for a tracee must
 * be issued by the SAME OS thread that PTRACE_ATTACHed it. The engine functions
 * below therefore run start-to-finish on ONE thread; the TUI runs them on a
 * dedicated tracer thread and never touches ptrace from the UI thread.
 */
#ifndef ASMSPY_H
#define ASMSPY_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "asmspy_treefilter.h" /* --tree: depth cap / symbol focus / module   */
#include "asmtest_ptrace.h"
#include "asmtest_trace.h"
#include "asmtest_valtrace.h" /* --dataflow: L0 value trace + L1 def-use graph */

/* ------------------------------------------------------------------ */
/* Process list (asmspy_proc.c)                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    pid_t pid;
    pid_t ppid;      /* parent pid (for the process-tree view)         */
    char user[24];   /* owner username (or numeric uid)           */
    char cmd[192];   /* cmdline (argv joined), or [comm] for a kthread */
    char runtime[8]; /* cheap runtime badge ("JVM"/"py"/"node"/"jit"/…),
                       * from argv0/comm + perf-map; "" if native/unknown */
    int attachable;  /* 1 if same euid as us (ptrace_scope=1 friendly) */
    unsigned long long cpu; /* CPU jiffies used during the sample window
                             * (ASMSPY_SORT_ACTIVE / _SCAN; 0 otherwise) */
    unsigned scan;          /* per-mille alphanumeric density of a memory sample
                    * (ASMSPY_SORT_SCAN only; 0 otherwise)              */
} asmspy_proc_t;

/* Process list ordering. */
typedef enum {
    ASMSPY_SORT_PID = 0, /* ascending pid (cheap, no sampling)          */
    ASMSPY_SORT_ACTIVE =
        1,                /* most recently active first (a ~150ms CPU sample) */
    ASMSPY_SORT_SCAN = 2, /* quick scan: string-rich memory first, then recency
                             * (samples each process's readable memory)        */
} asmspy_sort_t;

/* Scan /proc into a malloc'd array ordered per `sort`. Returns count (>=0) into
 * *out (caller frees with free()), or -1 on failure. ASMSPY_SORT_ACTIVE samples
 * per-process CPU time over a short window, so it briefly sleeps. */
int asmspy_proclist(asmspy_proc_t **out, size_t *count, asmspy_sort_t sort);

/* ------------------------------------------------------------------ */
/* Process fingerprint (asmspy_proc.c) — "what kind of process is this":*/
/* language runtime, thread makeup, notable modules, and ELF traits,   */
/* all from /proc + the mapped ELF (no ptrace), so it runs on any pid   */
/* we can read. Deeper than the cheap asmspy_proc_t.runtime badge.      */
/* ------------------------------------------------------------------ */
typedef struct {
    char runtime[24]; /* "JVM"/".NET"/"CPython"/"Node/V8"/"Ruby"/"Perl"/"Mono"/
                        * "Erlang/BEAM"/"Go"/"PHP"/"native"/"?"                */
    char
        evidence[96]; /* what identified it: "libjvm.so", ".note.go.buildid"  */
    int jitting; /* /tmp/perf-<pid>.map present (actively JIT-compiling)  */
    int threads; /* Threads: from /proc/<pid>/status (0 if unknown)      */
    unsigned long rss_kb; /* VmRSS in KiB (0 if unknown)                     */
    pid_t
        tracer_pid; /* TracerPid: 0 = none, else the tracer already attached */
    int seccomp;    /* Seccomp: 0 off, 1 strict, 2 filtered, -1 unknown     */
    char exe[192];  /* /proc/<pid>/exe target path ("" if unreadable)       */
    int elf_class;  /* 32 or 64, or 0 if the exe ELF couldn't be read       */
    int pie;        /* 1 = position-independent (ET_DYN) main executable    */
    int static_linked; /* 1 = no PT_INTERP (statically linked)                 */
    char interp[80]; /* PT_INTERP (dynamic loader) basename, "" if static    */
    char threadnames[6]
                    [20]; /* up to 6 distinct per-task comm names           */
    int n_threadnames;
    int more_threadnames; /* 1 if distinct names were dropped past the cap  */
    char modules[10][48]; /* up to 10 notable mapped-library basenames      */
    int n_modules;
    int more_modules; /* 1 if notable modules were dropped past the cap */
} asmspy_fingerprint_t;

/* Fill *out for `pid`, best-effort (unknown fields left zero/empty). Reads only
 * /proc and the mapped ELF — no ptrace, no attach. Always returns 0. */
int asmspy_fingerprint(pid_t pid, asmspy_fingerprint_t *out);

/* ------------------------------------------------------------------ */
/* Function-symbol resolver (asmspy_proc.c)                            */
/*                                                                     */
/* No ELF symbol reader exists in the library, so this parses the      */
/* .symtab/.dynsym of every ELF mapped into the target and offsets     */
/* each STT_FUNC by that module's load bias (from /proc/<pid>/maps).   */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t addr; /* runtime start address in the target             */
    uint64_t size; /* symbol size in bytes (0 if unknown)             */
    char *name;    /* function name; C++-demangled if mangled (owned) */
    char *module;  /* basename of the backing file (owned)            */
} asmspy_sym_t;

typedef struct {
    asmspy_sym_t *v;
    size_t n;
} asmspy_symtab_t;

/* Load every STT_FUNC symbol from the target's mapped ELF files, sorted by
 * runtime address. Returns 0 on success (even if n==0), -1 on hard failure. */
int asmspy_symtab_load(pid_t pid, asmspy_symtab_t *out);
void asmspy_symtab_free(asmspy_symtab_t *t);

/* Forward lookup: first function whose name equals `name` (exact), else NULL. */
const asmspy_sym_t *asmspy_symtab_by_name(const asmspy_symtab_t *t,
                                          const char *name);
/* Reverse lookup: the function whose [addr, addr+size) (or nearest addr<=)
 * contains `addr`, else NULL. */
const asmspy_sym_t *asmspy_symtab_at(const asmspy_symtab_t *t, uint64_t addr);

/* ------------------------------------------------------------------ */
/* JIT resolver (asmspy_proc.c) — two sources, one map                 */
/*                                                                     */
/* Managed runtimes name their JIT-compiled code (which lives in       */
/* anonymous executable mappings the ELF symtab can't see) in either   */
/* of two standard perf formats, and this map reads BOTH:              */
/*                                                                     */
/*  1. the BINARY jitdump file `jit-<pid>.dump` (perf's richer format: */
/*     sized, timestamped JIT_CODE_LOAD/MOVE records; emitted by       */
/*     LLVM/Julia/OpenJDK-jvmti/…). Discovered the way perf discovers  */
/*     it — the emitter mmaps the file's header page so it shows in    */
/*     /proc/<pid>/maps — with /tmp/jit-<pid>.dump (next to the perf   */
/*     map) and the target's cwd as fallbacks. Little-endian x86-64.   */
/*  2. the TEXT perf-map /tmp/perf-<pid>.map — "<addr> <size> <name>"  */
/*     lines (Node/V8 --perf-basic-prof, .NET DOTNET_PerfMapEnabled=1, */
/*     OpenJDK via perf-map-agent).                                    */
/*                                                                     */
/* When both sources name an address the jitdump entry WINS (it        */
/* carries exact code sizes and survives tiered recompiles/moves; the  */
/* text map is the lowest common denominator). Unlike the ELF symtab   */
/* this table is REFRESHED during the trace, since a running JIT keeps */
/* compiling new methods; addresses are absolute (no load bias).       */
/* ------------------------------------------------------------------ */
typedef struct {
    asmspy_sym_t
        *v; /* sorted by addr; `name` owned, `module` the shared "jit" */
    size_t n, cap;
    pid_t pid; /* whose perf-map / jitdump to read                  */
    unsigned
        miss_budget;     /* refresh-on-miss rate limiter (see asmspy_resolve) */
    char dump_path[256]; /* discovered jit-<pid>.dump path ("" until found)   */
} asmspy_jitmap_t;

/* Bind an (empty) JIT map to `pid`. Pairs with asmspy_jitmap_free. */
void asmspy_jitmap_init(asmspy_jitmap_t *j, pid_t pid);
/* Re-read the target's jitdump file AND /tmp/perf-<pid>.map, replacing the map
 * (sorted by addr; jitdump entries shadow text entries they cover). Cheap next
 * to single-stepping. Returns the method count, or -1 if neither source exists
 * (the map is then emptied). Safe to call repeatedly during a trace. */
int asmspy_jitmap_refresh(asmspy_jitmap_t *j);
/* Reverse lookup: the JIT method whose [addr, addr+size) contains `addr`, else
 * NULL. Does NOT refresh (pure) — use asmspy_resolve for refresh-on-miss. */
const asmspy_sym_t *asmspy_jitmap_at(const asmspy_jitmap_t *j, uint64_t addr);
void asmspy_jitmap_free(asmspy_jitmap_t *j);

/* The single resolution chokepoint for the whole-process single-step engines:
 * try the ELF symtab first, then the JIT map; on a double-miss refresh the
 * perf-map (rate-limited via `jit->miss_budget`, so an unmapped/non-JIT target
 * doesn't re-read the file on every unknown address) and retry the JIT tier.
 * Returns the owning asmspy_sym_t (ELF or JIT tier) or NULL. `jit` may be NULL
 * (ELF-only). The returned pointer is valid until the next asmspy_resolve /
 * asmspy_jitmap_refresh on the same map. */
const asmspy_sym_t *asmspy_resolve(const asmspy_symtab_t *syms,
                                   asmspy_jitmap_t *jit, uint64_t addr);

/* ------------------------------------------------------------------ */
/* Tracer engines (asmspy_engine.c) — each runs entirely on its caller */
/* thread (the ptrace-per-thread rule). `stop` may be NULL; when       */
/* non-NULL the engine returns promptly once it becomes true (the UI    */
/* sets it and pthread_kill(SIGALRM)s the tracer thread to unblock a    */
/* pending waitpid). `max` bounds the event/sample count (<0 = until    */
/* stop / target exit).                                                 */
/* ------------------------------------------------------------------ */

/* One decoded syscall: `line` is the full strace-ish line; `str` is just the
 * decoded string payload it carried (a path or a read/write buffer), or NULL. */
typedef void (*asmspy_syscall_sink)(void *ctx, const char *line,
                                    const char *str);

/* Attach to `pid` and ALL its threads (PTRACE_SEIZE every task, follow threads
 * it later spawns via PTRACE_O_TRACECLONE), stream their syscalls with decoded
 * data via `sink`, detach. When more than one thread is followed each `line` is
 * prefixed "[tid] " so the interleaved streams stay distinguishable. Returns 0
 * on clean detach, negative (an ASMTEST_PTRACE_* spirit code) on attach failure. */
int asmspy_engine_syscalls(pid_t pid, int follow, long max, atomic_bool *stop,
                           asmspy_syscall_sink sink, void *ctx);

/* One captured invocation of the traced region, handed to the front-end to
 * render. `code`/`len` are the region bytes (read from the target) so the sink
 * can disassemble; `desc` carries the call-out edges (functions called). */
typedef void (*asmspy_region_sink)(void *ctx, unsigned sample_no, long result,
                                   const asmtest_trace_t *tr,
                                   const asmtest_descent_t *desc,
                                   const uint8_t *code, size_t len,
                                   uint64_t base);

/* Returned by asmspy_engine_region when it detached cleanly but NEVER observed the
 * region execute (zero samples, not user-cancelled). Distinct positive value (the
 * engine's failure codes are the negative ASMTEST_PTRACE_*), so callers can tell
 * "nothing ran here" from a real attach failure. Since the engine races EVERY thread
 * to the entry, this now means the region genuinely did not execute in the sample
 * window — it no longer implies "it ran on a worker thread we weren't watching". */
#define ASMSPY_REGION_NEVER_RAN 1

/* SEIZE every thread of `pid`, then repeatedly race them all to the region
 * [base, base+len) — sampling whichever thread ARRIVES FIRST — invoking `sink` per
 * captured invocation, until `max` samples, `stop`, or the target exits; detach so
 * the target survives. `only_tid` (0 = any) pins the sample to one thread; it filters
 * the race rather than narrowing the SEIZE, because the shared entry breakpoint would
 * kill an unseized thread that reached it. Returns 0 on clean detach,
 * ASMSPY_REGION_NEVER_RAN if the region was never seen executing, or a negative
 * ASMTEST_PTRACE_* on an attach/availability failure.
 *
 * Worker threads ARE covered (asmspy-plan Theme B): a function that runs only off the
 * leader — as managed methods almost always do — is sampled like any other. A
 * genuinely W^X-enforced JIT page refuses the POKETEXT entry breakpoint and self-skips
 * via ASMTEST_PTRACE_ETRACE (no DR0 fallback here yet, the same gap the data-flow tier
 * carries) rather than being traced wrong. */
int asmspy_engine_region(pid_t pid, pid_t only_tid, uint64_t base, size_t len,
                         long max, atomic_bool *stop, asmspy_region_sink sink,
                         void *ctx);

/* ------------------------------------------------------------------ */
/* Data-flow value capture (asmspy_engine.c) — the scoped L0 value      */
/* trace + L1 def-use of a live region, wrapping the ptrace single-step  */
/* producer (src/dataflow_ptrace.c). Like the region sampler above it     */
/* scopes to [base,base+len), but instead of WHICH instructions ran it    */
/* records HOW DATA MOVED: each executed step's operand VALUES (L0) and    */
/* the last-writer def-use graph (L1) built over them. Single-shot and     */
/* expensive (~10^3-10^5x per stepped instruction), so it is scoped by     */
/* design — one invocation, not a live stream.                            */
/* ------------------------------------------------------------------ */

/* Returned by asmspy_engine_dataflow when the value PRODUCER is unavailable on
 * this build/host (off Linux x86-64, or built without Capstone) — a clean skip,
 * distinct from the negative hard-failure codes and from the other engines'
 * REGION_NEVER_RAN (1) / SAMPLE_UNAVAIL (2) positives. */
#define ASMSPY_DATAFLOW_UNAVAIL 3

/* One captured invocation's data flow, handed to the front-end to render: the L0
 * value trace `vt` (per-step operand read/write values) and the L1 last-writer
 * def-use graph `g` built over it (may be NULL if the build failed), plus the
 * region bytes (read from the target) and its `base` so the sink can disassemble
 * each step. `result` is the region's return value (rax at the region exit). */
typedef void (*asmspy_dataflow_sink)(void *ctx, long result,
                                     const asmtest_valtrace_t *vt,
                                     const asmtest_defuse_t *g,
                                     const uint8_t *code, size_t len,
                                     uint64_t base);

/* Attach to `pid`, run the JIT-aware scoped ptrace L0 value producer
 * (asmtest_dataflow_ptrace_attach_jit) over [base, base+len) for ONE invocation,
 * build the def-use graph, hand both to `sink`, and DETACH so the target SURVIVES.
 * SEIZEs EVERY thread and steps whichever one first enters the region, so a routine
 * that runs on a worker thread — as managed methods almost always do — is reached,
 * not just the leader; `only_tid` (non-0, the --tid convention) pins exactly that
 * thread instead. The target's own trap (a JIT self-check, a debugger breakpoint)
 * is detected and DELIVERED rather than swallowed or single-stepped through (the
 * signal split), so native and managed/JIT targets share one path — no runtime
 * gate. Two known, separately-tracked gaps: a region patched/re-JIT'd MID-capture
 * decodes the live snapshot (versioned decode is not wired in here yet), and a
 * genuinely W^X-enforced JIT page refusing the int3 entry breakpoint self-skips
 * cleanly via a negative ASMTEST_PTRACE_ETRACE (no hardware-breakpoint fallback
 * here yet) rather than crashing anything. `max` (>0) bounds the in-region steps
 * captured (the producer's max_insns); <=0 captures until the region returns
 * (bounded by the producer's step backstop). `stop` may be NULL (checked once
 * before the capture; the producer's own run-to / step loop is not interruptible
 * mid-capture — the tier is single-shot and fast). Returns 0 on a clean capture
 * (sink invoked once), ASMSPY_DATAFLOW_UNAVAIL if the producer is unavailable, or
 * a negative ASMTEST_PTRACE_* on an attach/permission failure. A region that never
 * executes on ANY thread blocks the producer at its entry breakpoint — bound live
 * use accordingly (the CLI timeout-guards its smoke). */
int asmspy_engine_dataflow(pid_t pid, pid_t only_tid, uint64_t base, size_t len,
                           long max, atomic_bool *stop,
                           asmspy_dataflow_sink sink, void *ctx);

/* One executed instruction, formatted "<function+off [module]>  <disasm>". */
typedef void (*asmspy_stream_sink)(void *ctx, const char *line);

/* Attach to `pid` and single-step it, streaming EVERY executed instruction
 * (disassembled, plus the function it lands in, resolved via `syms`) through
 * `sink`, until `max` instructions, `stop`, or the target exits; detach. Truly
 * whole-process: SEIZEs every thread and steps them all (following threads
 * spawned later), tagging each line "[tid]" once more than one is followed.
 * Single-stepping is slow, so the target crawls while streamed (and resumes full
 * speed on detach). `only_tid` (non-0) restricts the trace to that ONE thread —
 * only that tid is seized/stepped, so the process's other threads keep running at
 * full speed (isolation + no per-step slowdown on them); 0 = whole process (all
 * threads). Returns 0 on clean detach, negative on an attach/availability
 * failure. `syms` may be NULL (raw addrs). */
int asmspy_engine_stream(pid_t pid, pid_t only_tid, int follow, long max,
                         atomic_bool *stop, const asmspy_symtab_t *syms,
                         asmspy_stream_sink sink, void *ctx);

/* One node of the whole-process call graph: a function seen as a caller and/or
 * a callee. `invocations` = how many times it was CALLED; `out_calls` = how many
 * calls it MADE; `fanout` = how many DISTINCT functions it calls. `external` is
 * 1 for a shared/system-library function, 0 for the target's own executable. */
typedef struct {
    uint64_t addr;   /* function entry address in the target  */
    char name[128];  /* resolved symbol name, or "0x…"         */
    char module[64]; /* backing module basename ("?" if unknown) */
    int external;    /* 1 = external library, 0 = internal exe */
    unsigned long long invocations; /* times this function was called         */
    unsigned long long out_calls;   /* total calls this function made         */
    unsigned fanout;                /* number of distinct functions it calls  */
} asmspy_gnode_t;

/* One caller->callee edge of the call graph, keyed by the endpoints' entry
 * ADDRESSES (not node-array indices) so a consumer can sort/filter the nodes
 * without invalidating edges. `count` is how many times that call was seen. */
typedef struct {
    uint64_t caller_addr; /* entry addr of the calling function */
    uint64_t callee_addr; /* entry addr of the called function  */
    unsigned long long count;
} asmspy_gedge_t;

/* Snapshot sink: `nodes[0..nn)` and `edges[0..ne)` are the current whole-process
 * call graph, owned by the engine and valid only for THIS call (copy what you
 * keep). Invoked periodically as the graph grows and once more just before
 * detach. `edges` may be NULL (with ne==0) if the engine could not build them. */
typedef void (*asmspy_graph_sink)(void *ctx, const asmspy_gnode_t *nodes,
                                  size_t nn, const asmspy_gedge_t *edges,
                                  size_t ne);

/* Attach to `pid` and ALL its threads, single-step them, and build a
 * caller->callee call graph: each observed CALL adds/thickens an edge and bumps
 * the callee's invocation count and the caller's out-call count / fan-out.
 * Streams snapshots through `sink` until `max` CALLS are recorded, `stop`, or the
 * target exits; detach. `max` counts recorded calls (<0 = until stop / exit).
 * `syms` names the nodes and drives the internal/external split (NULL -> raw
 * addresses, all internal). Direct calls are resolved exactly; an indirect call
 * is attributed to wherever the next step lands, so the graph is best-effort at
 * signal boundaries. Whole-process single-stepping is slow — the target crawls
 * while traced. `only_tid` (non-0) restricts the trace to that one thread (see
 * asmspy_engine_stream). Returns 0 on clean detach, negative on an attach/
 * availability failure. */
int asmspy_engine_graph(pid_t pid, pid_t only_tid, int follow, long max,
                        atomic_bool *stop, const asmspy_symtab_t *syms,
                        asmspy_graph_sink sink, void *ctx);

/* One call-tree entry, structured — so a front-end can do more than print the
 * pre-rendered line (the TUI disassembles `addr`; the headless --json/--dot
 * exporters need the raw tid/depth/name instead of re-parsing indentation).
 * `name`/`module` are transient (valid only for the sink call — copy to keep). */
typedef struct {
    pid_t tid;        /* thread that made the call                          */
    int depth;        /* EFFECTIVE call depth at entry (0 = top). Equals the
                       * thread's live call depth, except under a --focus
                       * filter, which re-bases it so the focused function
                       * sits at depth 0 (see asmspy_treefilter.h).         */
    uint64_t addr;    /* callee entry address                               */
    const char *name; /* resolved symbol name, or "0x…" if unresolved       */
    const char *module; /* backing module basename, "jit", or "?" if unknown  */
} asmspy_tree_call_t;

/* One call-tree entry handed to the front-end: `line` is the indented
 * "-> function [module]" text; `call` carries the same entry structured
 * (see above) so a front-end can disassemble/export without re-parsing. */
typedef void (*asmspy_tree_sink)(void *ctx, const char *line,
                                 const asmspy_tree_call_t *call);

/* Attach to `pid` and ALL its threads, single-step them, and stream a live,
 * indented call TREE through `sink` (one entry per function entry, indented by
 * the calling thread's live call depth, with the callee address). Depth
 * is a per-thread shadow counter — push on CALL, pop on RET (clamped at 0), so a
 * tail-call/longjmp/signal can drift the indentation but never desync fatally.
 * `max` bounds the call lines emitted (<0 = until stop / exit). Whole-process
 * single-stepping is slow, so the target crawls while traced. Each line is
 * prefixed "[tid] " once more than one thread is followed. `only_tid` (non-0)
 * restricts the trace to that one thread (see asmspy_engine_stream).
 *
 * `filter` (NULL = unfiltered) caps the depth / focuses on a symbol's subtree /
 * restricts to a module — the difference between a readable tree and a firehose
 * on a busy process (asmspy_treefilter.h). It bounds only what is EMITTED: every
 * call and return is still tracked, so the depths of the surviving lines stay
 * true, and `max` therefore counts SURVIVING lines (a capped run reaches its
 * budget in filtered lines, not raw calls).
 *
 * Returns 0 on clean detach, negative on an attach/availability failure. `syms`
 * may be NULL. */
int asmspy_engine_tree(pid_t pid, pid_t only_tid, int follow, long max,
                       atomic_bool *stop, const asmspy_symtab_t *syms,
                       const asmspy_tree_filter_t *filter,
                       asmspy_tree_sink sink, void *ctx);

/* What the process/thread topology counts per task. */
typedef enum {
    ASMSPY_COUNT_SYSCALLS =
        0, /* syscalls made (PTRACE_SYSCALL — fast, crash-safe) */
    ASMSPY_COUNT_CALLS =
        1, /* CALL instructions (single-step — rich, slow)      */
} asmspy_count_t;

/* One task (thread) in the traced process tree, with its invocation count. A
 * process is the set of tasks sharing `tgid`; its leader has tid == tgid. */
typedef struct {
    pid_t tid;     /* task (thread) id                                */
    pid_t tgid;    /* thread-group (process) id                       */
    pid_t ppid;    /* parent process id (for the process forest)      */
    int is_leader; /* 1 if tid == tgid (the process's main thread)    */
    char comm[24]; /* task name (/proc/<tid>/comm)                    */
    char exe[64];  /* process exe basename (leader tasks only)        */
    unsigned long long
        inv; /* invocation count: syscalls or calls per `mode`  */
} asmspy_task_t;

/* Topology snapshot sink: `tasks[0..n)` is every tracked task (threads of the
 * target and of every child process it forked), owned by the engine and valid
 * only for THIS call. Invoked periodically and once more just before detach. */
typedef void (*asmspy_topo_sink)(void *ctx, const asmspy_task_t *tasks,
                                 size_t n);

/* Attach to `pid`, follow every thread (CLONE) and every child PROCESS
 * (FORK/VFORK, recursively) and exec, and count per task either syscalls
 * (`mode` == ASMSPY_COUNT_SYSCALLS, via PTRACE_SYSCALL — near full speed, safe on
 * any target) or CALL instructions (ASMSPY_COUNT_CALLS, via single-step — richer
 * but the whole tree crawls). Streams topology snapshots through `sink` until
 * `max` counted events, `stop`, or the whole tree exits; detach. `max` counts
 * invocations (<0 = until stop / exit). Returns 0 on clean detach, negative on an
 * attach/availability failure. */
int asmspy_engine_procs(pid_t pid, long max, atomic_bool *stop,
                        asmspy_count_t mode, asmspy_topo_sink sink, void *ctx);

/* ------------------------------------------------------------------ */
/* Statistical hot-edge sampler (asmspy_engine.c) — the ONLY rich view */
/* that is SAFE ON ANY TARGET. It reads AMD IBS-Op branch samples OUT   */
/* OF BAND (no ptrace, no single-step), so a JIT / managed runtime      */
/* keeps running at full speed and is never at risk of the single-step  */
/* crash the stream/graph/tree views carry. Needs an AMD IBS-Op host    */
/* (asmtest_ibs.h); self-skips everywhere else. STATISTICAL, never       */
/* exact: it proves an edge was SEEN, never that one was not taken.     */
/* ------------------------------------------------------------------ */

/* One statistical hot control-flow edge {from -> to}, endpoints resolved to
 * function names (ELF symtab + JIT perf-map), aggregated over the sample window
 * and sorted by descending sample count. */
typedef struct {
    uint64_t from_addr, to_addr;
    char from[160]; /* "func+0xNN [module]", or "0x…" if unresolved */
    char to[160];
    unsigned long long count; /* IBS-Op samples aggregated on this edge */
    unsigned mispred;         /* of those, how many were mispredicted   */
    unsigned is_return;       /* of those, how many retired a return    */
} asmspy_sample_edge_t;

/* Snapshot sink: `edges[0..n)` are the current hot edges (sorted by count), plus
 * honest provenance — total samples drained, retired-taken-branch samples, dropped
 * samples, and whether the kernel throttled the sample rate. Owned by the engine,
 * valid only for THIS call. Invoked after each sample window and once before
 * return. */
typedef void (*asmspy_sample_sink)(void *ctx, const asmspy_sample_edge_t *edges,
                                   size_t n, uint64_t samples,
                                   uint64_t branch_samples, uint64_t lost,
                                   int throttled);

/* Returned by asmspy_engine_sample when IBS-Op is unavailable on this host (not an
 * AMD IBS box, or perf blocked it) — a clean skip, distinct from the negative hard-
 * failure codes. Call asmtest_ibs_skip_reason() for the human reason. */
#define ASMSPY_SAMPLE_UNAVAIL 2

/* Attach AMD IBS-Op to EVERY thread of `pid` OUT OF BAND (no ptrace, no single-
 * step — the target runs unperturbed), sampling retired taken branches into a
 * statistical hot-edge histogram whose endpoints are resolved via `syms` (ELF) and
 * `jit` (the perf-map, so managed Node/.NET/Java frames are named — the payoff of
 * this view over single-stepping a JIT). Each `ms`-millisecond window's histogram
 * is streamed through `sink`; when `stop` is NULL the engine runs exactly ONE
 * window and returns (headless), otherwise it loops until `*stop`. Returns 0 on
 * success, ASMSPY_SAMPLE_UNAVAIL if IBS is unavailable, or a negative code on a
 * hard capture failure. `syms`/`jit` may be NULL (raw addresses). */
int asmspy_engine_sample(pid_t pid, unsigned ms, atomic_bool *stop,
                         const asmspy_symtab_t *syms, asmspy_jitmap_t *jit,
                         asmspy_sample_sink sink, void *ctx);

/* ------------------------------------------------------------------ */
/* Hardware DATA-watchpoint engine (asmspy_engine.c) — the TARGETED,   */
/* near-zero-perturbation data-flow view. It arms an x86 debug register */
/* (DR0-3) in WRITE / READ-WRITE mode — not the EXECUTE mode the rest   */
/* of the tracer uses — on a chosen address in EVERY thread of the      */
/* target, PTRACE_CONTs it (NATIVE speed between hits: no single-step,   */
/* no code patch), and at each #DB reports who touched the field and     */
/* with what value. It answers "who wrote this field, and what did they  */
/* write" that nothing else in asmspy can. x86-64 only (debug registers);*/
/* self-skips elsewhere and where arming is refused (qemu / permission). */
/* ------------------------------------------------------------------ */

/* Returned by asmspy_engine_watch when the host refuses debug-register arming
 * (PTRACE_POKEUSER on DR0/DR7 rejected — permission / seccomp / no real debug
 * registers) or off x86-64 entirely — a clean skip, distinct from the negative
 * attach-failure codes and the other engines' positives (1/2/3). */
#define ASMSPY_WATCH_UNAVAIL 4

/* One data-watchpoint hit handed to the front-end. Thread `tid` accessed the
 * watched location `addr` at instruction `pc` (resolved to `func`+`off` in
 * `module`; `func`/`module` are NULL when unresolved). `is_write` is 1 for a
 * store (the only kind a write-only watch traps), 0 for a load, -1 if the
 * direction could not be decoded (a read+write watch whose faulting instruction
 * did not decode). `value` holds the `value_len` (<=8) bytes read back from the
 * watched location AFTER the access (host-endian) when `value_ok`. `hit_no` is
 * the 1-based hit index. `func`/`module` are transient — valid only for the sink
 * call (copy to keep). */
typedef struct {
    unsigned long hit_no;
    pid_t tid;
    uint64_t pc;        /* the accessing instruction (or the following one)  */
    uint64_t addr;      /* the watched location                              */
    int is_write;       /* 1 write, 0 read, -1 unknown                       */
    int value_ok;       /* the watched bytes were read back                  */
    unsigned value_len; /* bytes valid in `value` (== the watch length, <=8) */
    uint64_t value;     /* the watched bytes, read post-access               */
    const char *func;   /* resolved symbol name, or NULL                     */
    const char *module; /* backing module basename, or NULL                  */
    uint64_t off;       /* pc - func base (0 when func == NULL)              */
} asmspy_watch_hit_t;

typedef void (*asmspy_watch_sink)(void *ctx, const asmspy_watch_hit_t *hit);

/* Arm an x86 hardware DATA watchpoint on [addr, addr+len) in EVERY thread of
 * `pid` — debug registers are PER-THREAD, so a live multi-threaded target needs
 * every task armed (this iterates /proc/<pid>/task and arms threads spawned later
 * via PTRACE_O_TRACECLONE too) — then PTRACE_CONT the whole process. Between hits
 * the target runs at NATIVE speed. At each #DB the access is reported through
 * `sink`: the faulting thread + PC (resolved via `syms`, then the JIT perf-map),
 * the value read back from the watched bytes, and — for a read+write watch — read
 * vs write decoded from the faulting instruction. `rw` selects the trapped
 * condition (0 = writes only, self-labelling; 1 = reads AND writes). `len` is
 * 1/2/4/8 and `addr` must be `len`-aligned (an x86 hardware requirement). Runs
 * until `max` hits (<0 = until stop / target exit), `stop`, or the target exits,
 * then DISARMS the debug registers on every thread and detaches so the target
 * SURVIVES. Returns 0 on clean detach (any number of hits, including 0),
 * ASMSPY_WATCH_UNAVAIL if arming is refused or off x86-64, ASMTEST_PTRACE_EINVAL
 * on a bad length/alignment, or a negative ASMTEST_PTRACE_* on an attach failure.
 * `syms` may be NULL (raw addresses). */
int asmspy_engine_watch(pid_t pid, uint64_t addr, int rw, int len, long max,
                        atomic_bool *stop, const asmspy_symtab_t *syms,
                        asmspy_watch_sink sink, void *ctx);

/* Human-readable one-liner for an engine/attach failure code, into buf. */
void asmspy_strerror(int rc, char *buf, size_t buflen);

/* ------------------------------------------------------------------ */
/* Interactive TUI entry point (asmspy.c). Returns a process exit code. */
/* ------------------------------------------------------------------ */
int asmspy_tui(void);

#endif /* ASMSPY_H */
