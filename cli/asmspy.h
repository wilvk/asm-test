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

#include "asmtest_ptrace.h"
#include "asmtest_trace.h"

/* ------------------------------------------------------------------ */
/* Process list (asmspy_proc.c)                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    pid_t pid;
    pid_t ppid;       /* parent pid (for the process-tree view)         */
    char user[24];    /* owner username (or numeric uid)           */
    char cmd[192];    /* cmdline (argv joined), or [comm] for a kthread */
    int attachable;   /* 1 if same euid as us (ptrace_scope=1 friendly) */
    unsigned long long cpu; /* CPU jiffies used during the sample window
                             * (ASMSPY_SORT_ACTIVE / _SCAN; 0 otherwise) */
    unsigned scan; /* per-mille alphanumeric density of a memory sample
                    * (ASMSPY_SORT_SCAN only; 0 otherwise)              */
} asmspy_proc_t;

/* Process list ordering. */
typedef enum {
    ASMSPY_SORT_PID = 0,    /* ascending pid (cheap, no sampling)          */
    ASMSPY_SORT_ACTIVE = 1, /* most recently active first (a ~150ms CPU sample) */
    ASMSPY_SORT_SCAN = 2,   /* quick scan: string-rich memory first, then recency
                             * (samples each process's readable memory)        */
} asmspy_sort_t;

/* Scan /proc into a malloc'd array ordered per `sort`. Returns count (>=0) into
 * *out (caller frees with free()), or -1 on failure. ASMSPY_SORT_ACTIVE samples
 * per-process CPU time over a short window, so it briefly sleeps. */
int asmspy_proclist(asmspy_proc_t **out, size_t *count, asmspy_sort_t sort);

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
/* JIT / perf-map resolver (asmspy_proc.c)                             */
/*                                                                     */
/* Managed runtimes emit /tmp/perf-<pid>.map — a text table of         */
/* "<hex addr> <hex size> <name>" lines naming their JIT-compiled code */
/* (Node/V8 with --perf-basic-prof, .NET with DOTNET_PerfMapEnabled=1, */
/* and OpenJDK via perf-map-agent all use this one format). That code  */
/* lives in anonymous executable mappings the ELF symtab can't see, so */
/* without this every managed frame renders "0x..". Unlike the ELF     */
/* symtab this table is REFRESHED during the trace, since a running    */
/* JIT keeps compiling new methods; addresses are absolute (no bias).  */
/* ------------------------------------------------------------------ */
typedef struct {
    asmspy_sym_t *v; /* sorted by addr; `name` owned, `module` the shared "jit" */
    size_t n, cap;
    pid_t pid;             /* whose /tmp/perf-<pid>.map to read                 */
    unsigned miss_budget;  /* refresh-on-miss rate limiter (see asmspy_resolve) */
} asmspy_jitmap_t;

/* Bind an (empty) JIT map to `pid`. Pairs with asmspy_jitmap_free. */
void asmspy_jitmap_init(asmspy_jitmap_t *j, pid_t pid);
/* Re-read /tmp/perf-<pid>.map, replacing the map (sorted by addr). Cheap next to
 * single-stepping. Returns the method count, or -1 if the file is absent (the
 * map is then emptied). Safe to call repeatedly during a trace. */
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
int asmspy_engine_syscalls(pid_t pid, long max, atomic_bool *stop,
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
 * "nothing ran here" from a real attach failure — most often a multi-threaded target
 * whose function runs on a worker thread (this engine attaches only the leader). */
#define ASMSPY_REGION_NEVER_RAN 1

/* Attach to `pid`, then repeatedly run_to(base) + trace_attached the region
 * [base, base+len), invoking `sink` per captured invocation, until `max`
 * samples, `stop`, or the target exits; detach. Returns 0 on clean detach,
 * ASMSPY_REGION_NEVER_RAN if the region was never seen executing, or a negative
 * ASMTEST_PTRACE_* on an attach/availability failure. NOTE: attaches only the
 * thread-group leader — a function that runs only on a worker thread yields
 * ASMSPY_REGION_NEVER_RAN (unlike the whole-process syscall/stream engines). */
int asmspy_engine_region(pid_t pid, uint64_t base, size_t len, long max,
                         atomic_bool *stop, asmspy_region_sink sink, void *ctx);

/* One executed instruction, formatted "<function+off [module]>  <disasm>". */
typedef void (*asmspy_stream_sink)(void *ctx, const char *line);

/* Attach to `pid` and single-step it, streaming EVERY executed instruction
 * (disassembled, plus the function it lands in, resolved via `syms`) through
 * `sink`, until `max` instructions, `stop`, or the target exits; detach. Truly
 * whole-process: SEIZEs every thread and steps them all (following threads
 * spawned later), tagging each line "[tid]" once more than one is followed.
 * Single-stepping is slow, so the target crawls while streamed (and resumes full
 * speed on detach). Returns 0 on clean detach, negative on an attach/
 * availability failure. `syms` may be NULL (raw addrs). */
int asmspy_engine_stream(pid_t pid, long max, atomic_bool *stop,
                         const asmspy_symtab_t *syms, asmspy_stream_sink sink,
                         void *ctx);

/* One node of the whole-process call graph: a function seen as a caller and/or
 * a callee. `invocations` = how many times it was CALLED; `out_calls` = how many
 * calls it MADE; `fanout` = how many DISTINCT functions it calls. `external` is
 * 1 for a shared/system-library function, 0 for the target's own executable. */
typedef struct {
    uint64_t addr;                  /* function entry address in the target  */
    char name[128];                 /* resolved symbol name, or "0x…"         */
    char module[64];                /* backing module basename ("?" if unknown) */
    int external;                   /* 1 = external library, 0 = internal exe */
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
 * while traced. Returns 0 on clean detach, negative on an attach/availability
 * failure. */
int asmspy_engine_graph(pid_t pid, long max, atomic_bool *stop,
                        const asmspy_symtab_t *syms, asmspy_graph_sink sink,
                        void *ctx);

/* One call-tree entry handed to the front-end: `line` is the indented
 * "-> function [module]" text; `addr` is the callee's entry address (0 if
 * unresolved) so a front-end can disassemble that function on demand. */
typedef void (*asmspy_tree_sink)(void *ctx, const char *line, uint64_t addr);

/* Attach to `pid` and ALL its threads, single-step them, and stream a live,
 * indented call TREE through `sink` (one entry per function entry, indented by
 * the calling thread's live call depth, with the callee address). Depth
 * is a per-thread shadow counter — push on CALL, pop on RET (clamped at 0), so a
 * tail-call/longjmp/signal can drift the indentation but never desync fatally.
 * `max` bounds the call lines emitted (<0 = until stop / exit). Whole-process
 * single-stepping is slow, so the target crawls while traced. Each line is
 * prefixed "[tid] " once more than one thread is followed. Returns 0 on clean
 * detach, negative on an attach/availability failure. `syms` may be NULL. */
int asmspy_engine_tree(pid_t pid, long max, atomic_bool *stop,
                       const asmspy_symtab_t *syms, asmspy_tree_sink sink,
                       void *ctx);

/* What the process/thread topology counts per task. */
typedef enum {
    ASMSPY_COUNT_SYSCALLS = 0, /* syscalls made (PTRACE_SYSCALL — fast, crash-safe) */
    ASMSPY_COUNT_CALLS = 1,    /* CALL instructions (single-step — rich, slow)      */
} asmspy_count_t;

/* One task (thread) in the traced process tree, with its invocation count. A
 * process is the set of tasks sharing `tgid`; its leader has tid == tgid. */
typedef struct {
    pid_t tid;              /* task (thread) id                                */
    pid_t tgid;            /* thread-group (process) id                       */
    pid_t ppid;            /* parent process id (for the process forest)      */
    int is_leader;         /* 1 if tid == tgid (the process's main thread)    */
    char comm[24];         /* task name (/proc/<tid>/comm)                    */
    char exe[64];          /* process exe basename (leader tasks only)        */
    unsigned long long inv; /* invocation count: syscalls or calls per `mode`  */
} asmspy_task_t;

/* Topology snapshot sink: `tasks[0..n)` is every tracked task (threads of the
 * target and of every child process it forked), owned by the engine and valid
 * only for THIS call. Invoked periodically and once more just before detach. */
typedef void (*asmspy_topo_sink)(void *ctx, const asmspy_task_t *tasks, size_t n);

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

/* Human-readable one-liner for an engine/attach failure code, into buf. */
void asmspy_strerror(int rc, char *buf, size_t buflen);

/* ------------------------------------------------------------------ */
/* Interactive TUI entry point (asmspy.c). Returns a process exit code. */
/* ------------------------------------------------------------------ */
int asmspy_tui(void);

#endif /* ASMSPY_H */
