/*
 * drtrace_app.c — application-facing side of the optional DynamoRIO native-trace
 * tier (libasmtest_drapp). See include/asmtest_drtrace.h and docs/native-tracing.md.
 *
 * Responsibilities:
 *   - the lifecycle state machine (UNINIT -> INIT -> STARTED -> STOPPED ->
 *     SHUTDOWN) over DynamoRIO's Application Interface (dr_app_setup/start/...);
 *   - configuring the in-process client load and bringing DynamoRIO up;
 *   - the exported begin/end/register markers the client resolves by PC and
 *     instruments — kept as real functions, with distinct bodies so the compiler
 *     and linker cannot fold the otherwise-identical empty ones to one address;
 *   - host-native executable code allocation (W^X mmap) for tracing generated
 *     routines.
 *
 * Client loading: libdynamorio reads DYNAMORIO_OPTIONS at *library-load* time
 * (its constructor), which is before main(). So this library does NOT link
 * libdynamorio; instead asmtest_dr_init() sets DYNAMORIO_OPTIONS and only THEN
 * dlopen()s libdynamorio, so the constructor sees the freshly-set client config.
 * The dr_app_* entry points are then resolved with dlsym. This is also the load
 * pattern the language bindings use. No DynamoRIO headers are needed (dr_app.h
 * pulls in dr_defines.h, whose `bool` clashes with <stdbool.h>).
 */
#define _GNU_SOURCE                                                            \
    1 /* dladdr / Dl_info — locate a package-bundled libdynamorio */
#include "asmtest_drtrace.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef ASMTEST_HAVE_KEYSTONE
#include "asmtest_assemble.h"
#endif

/* DynamoRIO's runtime is a Mach-O dylib on Darwin (built from the pinned
 * source fork — upstream ships no macOS release asset) and an ELF .so
 * everywhere else. One name, used by every resolution site below. */
#if defined(__APPLE__)
#define DR_LIBNAME "libdynamorio.dylib"
#else
#define DR_LIBNAME "libdynamorio.so"
#endif

/* ------------------------------------------------------------------ */
/* DynamoRIO Application Interface, resolved via dlopen (see banner)    */
/* ------------------------------------------------------------------ */
static void *g_dr_handle;
static int (*p_dr_app_setup)(void);
static void (*p_dr_app_start)(void);
static void (*p_dr_app_stop)(void);
static void (*p_dr_app_stop_and_cleanup)(void);
static char (*p_dr_app_running)(
    void); /* dr_app_running_under_dynamorio (bool) */

/* When drapp is shipped inside a package payload (wheel/gem/jar/...), libdynamorio
 * is vendored right next to it. dlopen() does NOT consult drapp's own RUNPATH for
 * that call, so locate the sibling explicitly via dladdr on one of our own symbols
 * and write "<drapp-dir>/" DR_LIBNAME into buf (return NULL if undeterminable
 * or the sibling is absent). Lets a bundled tier self-locate with no env/opts. */
static const char *dr_bundled_lib(char *buf, size_t buflen) {
    Dl_info info;
    if (dladdr((void *)dr_bundled_lib, &info) && info.dli_fname != NULL) {
        const char *slash = strrchr(info.dli_fname, '/');
        if (slash != NULL) {
            snprintf(buf, buflen, "%.*s/" DR_LIBNAME,
                     (int)(slash - info.dli_fname), info.dli_fname);
            if (access(buf, R_OK) == 0)
                return buf;
        }
    }
    return NULL;
}

/* Resolve the libdynamorio path: explicit option, the ASMTEST_DR_LIB env var, a
 * copy bundled next to drapp (a published package), the DYNAMORIO_HOME env var,
 * then a bare soname (found via rpath / ldconfig / LD_LIBRARY_PATH). Writes into
 * buf. asmtest_dr_available() probes the SAME cascade minus this final
 * bare-soname fallback — it can't verify a bare soname without dlopen (which
 * runs DR's constructor), so init may still succeed via that fallback after
 * available() reports 0 (see dr_probe). */
static const char *dr_lib_path(const asmtest_drtrace_options_t *opts, char *buf,
                               size_t buflen) {
    const char *env = getenv("ASMTEST_DR_LIB");
    const char *home = getenv("DYNAMORIO_HOME");
    if (opts != NULL && opts->dynamorio_home != NULL &&
        opts->dynamorio_home[0] != '\0') {
        snprintf(buf, buflen, "%s/lib64/release/" DR_LIBNAME,
                 opts->dynamorio_home);
        return buf;
    }
    if (env != NULL && env[0] != '\0') {
        snprintf(buf, buflen, "%s", env);
        return buf;
    }
    if (dr_bundled_lib(buf, buflen) != NULL)
        return buf;
    if (home != NULL && home[0] != '\0') {
        snprintf(buf, buflen, "%s/lib64/release/" DR_LIBNAME, home);
        return buf;
    }
    snprintf(buf, buflen, DR_LIBNAME);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Lifecycle state machine                                             */
/* ------------------------------------------------------------------ */
enum dr_state { ST_UNINIT = 0, ST_INIT, ST_STARTED, ST_STOPPED, ST_SHUTDOWN };

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static enum dr_state g_state = ST_UNINIT;
static pthread_t g_setup_thread;
static int g_marker_errors = 0;

/* App-side region bookkeeping (the client keeps its own copy; this side tracks
 * names for unregister and to validate registration). */
#define ASMTEST_DR_MAX_REGIONS 32
typedef struct {
    char *name;
    void *base;
    size_t len;
    asmtest_trace_t *trace;
} app_region_t;
static app_region_t g_regions[ASMTEST_DR_MAX_REGIONS];
static int g_nregions = 0;

/* Per-thread active-region name stack, for marker balance checking. The client
 * independently toggles recording; this side only validates begin/end pairing. */
#define ASMTEST_DR_MAX_DEPTH 16
static _Thread_local const char *g_active_stack[ASMTEST_DR_MAX_DEPTH];
static _Thread_local int g_active_depth = 0;

/* A distinct volatile sink each marker perturbs, so their bodies differ and are
 * never merged to a single PC (which would make begin indistinguishable from
 * end to the PC-resolving client). */
static volatile unsigned long g_marker_sink;

/* fork-after-start is unsupported (DR took over all threads). A pthread_atfork
 * child handler disables the tier in the forked child (e.g. Python multiprocessing
 * with the default 'fork' start method) so it degrades safely instead of running
 * with a broken DR state inherited across fork. */
static volatile int g_disabled_in_child = 0;
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;
static void dr_atfork_child(void) {
    g_disabled_in_child = 1;
    g_active_depth = 0;
}
static void install_atfork(void) {
    pthread_atfork(NULL, NULL, dr_atfork_child);
}

static void dr_reason(char *buf, size_t buflen, const char *msg) {
    if (buf != NULL && buflen > 0) {
        strncpy(buf, msg, buflen - 1);
        buf[buflen - 1] = '\0';
    }
}

/* Shared availability probe behind asmtest_dr_available() and _skip_reason().
 * Cheap and side-effect-free: it does NOT dlopen (that runs DR's constructor),
 * so it resolves the SAME cascade as dr_lib_path() EXCEPT the final bare-soname
 * fallback, which only dlopen can settle. Returns 1 when an explicit path
 * (ASMTEST_DR_LIB / a bundled copy / DYNAMORIO_HOME) is readable; else 0, with a
 * human-readable reason in *why (when non-NULL). A libdynamorio reachable ONLY
 * via the loader search path (ldconfig / LD_LIBRARY_PATH) reports 0 here yet
 * still loads at init — uncommon, so DR-gated tests self-skip conservatively
 * rather than error mid-init. */
static int dr_probe(char *why, size_t wn) {
    char buf[1024];
    const char *env = getenv("ASMTEST_DR_LIB");
    if (env != NULL && env[0] != '\0') {
        if (access(env, R_OK) == 0) {
            dr_reason(why, wn, "available");
            return 1;
        }
        dr_reason(why, wn, "ASMTEST_DR_LIB is set but not readable");
        return 0;
    }
    if (dr_bundled_lib(buf, sizeof buf) != NULL) { /* package-bundled copy */
        dr_reason(why, wn, "available");
        return 1;
    }
    const char *home = getenv("DYNAMORIO_HOME");
    if (home != NULL && home[0] != '\0') {
        snprintf(buf, sizeof buf, "%s/lib64/release/" DR_LIBNAME, home);
        if (access(buf, R_OK) == 0) {
            dr_reason(why, wn, "available");
            return 1;
        }
        dr_reason(why, wn,
                  "DYNAMORIO_HOME set but lib64/release/" DR_LIBNAME " not "
                  "found under it");
        return 0;
    }
    dr_reason(why, wn,
              "no libdynamorio resolved: set ASMTEST_DR_LIB or DYNAMORIO_HOME, "
              "or bundle it (a bare " DR_LIBNAME " on the loader path is still "
              "tried at init)");
    return 0;
}

int asmtest_dr_available(void) { return dr_probe(NULL, 0); }

void asmtest_dr_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    (void)dr_probe(buf, buflen);
}

int asmtest_dr_marker_error(void) { return g_marker_errors; }

int asmtest_dr_under_dynamorio(void) {
    return (p_dr_app_running != NULL && p_dr_app_running()) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Init / start / stop / shutdown                                      */
/* ------------------------------------------------------------------ */

int asmtest_dr_init(const asmtest_drtrace_options_t *opts) {
    if (g_disabled_in_child)
        return ASMTEST_DR_ENODR; /* tier disabled in a forked child */
    pthread_once(&g_atfork_once, install_atfork);
    pthread_mutex_lock(&g_lock);
    if (g_state == ST_INIT || g_state == ST_STARTED || g_state == ST_STOPPED) {
        pthread_mutex_unlock(&g_lock);
        return ASMTEST_DR_OK; /* second init is a no-op */
    }
    if (g_state != ST_UNINIT) {
        pthread_mutex_unlock(&g_lock);
        return ASMTEST_DR_ESTATE;
    }
    /* Resolve the client library path: explicit option, else env var. */
    const char *client = (opts != NULL) ? opts->client_path : NULL;
    if (client == NULL || client[0] == '\0')
        client = getenv("ASMTEST_DRCLIENT");
    if (client == NULL || client[0] == '\0') {
        pthread_mutex_unlock(&g_lock);
        return ASMTEST_DR_EINVAL; /* no client to load */
    }
    const char *extra = (opts != NULL && opts->client_options != NULL)
                            ? opts->client_options
                            : "";
    /* Configure the in-process client load BEFORE dlopen'ing libdynamorio (its
     * constructor reads DYNAMORIO_OPTIONS). */
    char buf[2048];
    snprintf(buf, sizeof buf, "-code_api -client_lib '%s;0;%s'", client, extra);
    setenv("DYNAMORIO_OPTIONS", buf, 1);

    /* Load libdynamorio once and cache the handle + entry points; a re-init
     * after shutdown reuses them (DR re-attach) rather than dlclose/reload, which
     * keeps DR's load-time DYNAMORIO_OPTIONS read intact and avoids unloading the
     * engine. */
    if (g_dr_handle == NULL) {
        char libpath[1024];
        dr_lib_path(opts, libpath, sizeof libpath);
        g_dr_handle = dlopen(libpath, RTLD_NOW | RTLD_GLOBAL);
        if (g_dr_handle == NULL) {
            pthread_mutex_unlock(&g_lock);
            return ASMTEST_DR_ENODR;
        }
        p_dr_app_setup = (int (*)(void))dlsym(g_dr_handle, "dr_app_setup");
        p_dr_app_start = (void (*)(void))dlsym(g_dr_handle, "dr_app_start");
        p_dr_app_stop = (void (*)(void))dlsym(g_dr_handle, "dr_app_stop");
        p_dr_app_stop_and_cleanup =
            (void (*)(void))dlsym(g_dr_handle, "dr_app_stop_and_cleanup");
        p_dr_app_running = /* optional: only the managed-host probe uses it */
            (char (*)(void))dlsym(g_dr_handle,
                                  "dr_app_running_under_dynamorio");
        if (p_dr_app_setup == NULL || p_dr_app_start == NULL ||
            p_dr_app_stop == NULL || p_dr_app_stop_and_cleanup == NULL) {
            dlclose(g_dr_handle);
            g_dr_handle = NULL;
            p_dr_app_setup = NULL;
            p_dr_app_start = NULL;
            p_dr_app_stop = NULL;
            p_dr_app_stop_and_cleanup = NULL;
            pthread_mutex_unlock(&g_lock);
            return ASMTEST_DR_ENODR;
        }
    }
    if (p_dr_app_setup() != 0) {
        /* Leave the cached handle/pointers in place — a retry reuses them rather
         * than dlopen'ing a second time; state stays UNINIT so a retry is valid. */
        pthread_mutex_unlock(&g_lock);
        return ASMTEST_DR_ENODR;
    }
    g_setup_thread = pthread_self();
    g_state = ST_INIT;
    g_marker_errors = 0;
    pthread_mutex_unlock(&g_lock);
    return ASMTEST_DR_OK;
}

int asmtest_dr_start(void) {
    if (g_disabled_in_child)
        return ASMTEST_DR_ENODR;
    pthread_mutex_lock(&g_lock);
    if (g_state != ST_INIT && g_state != ST_STOPPED) {
        pthread_mutex_unlock(&g_lock);
        return ASMTEST_DR_ESTATE;
    }
    if (!pthread_equal(pthread_self(), g_setup_thread)) {
        pthread_mutex_unlock(&g_lock);
        return ASMTEST_DR_ETHREAD; /* DR requires start on the setup thread */
    }
    p_dr_app_start();
    g_state = ST_STARTED;
    pthread_mutex_unlock(&g_lock);
    return ASMTEST_DR_OK;
}

int asmtest_dr_stop(void) {
    pthread_mutex_lock(&g_lock);
    if (g_state != ST_STARTED) {
        pthread_mutex_unlock(&g_lock);
        return ASMTEST_DR_ESTATE;
    }
    p_dr_app_stop();
    g_state = ST_STOPPED;
    pthread_mutex_unlock(&g_lock);
    return ASMTEST_DR_OK;
}

void asmtest_dr_shutdown(void) {
    /* Surface "shutdown while a region is still active on this thread" as data
     * (the markers are void and cannot return it inline). */
    if (g_active_depth > 0)
        g_marker_errors++;
    pthread_mutex_lock(&g_lock);
    if ((g_state == ST_STARTED || g_state == ST_STOPPED ||
         g_state == ST_INIT) &&
        p_dr_app_stop_and_cleanup != NULL) {
        p_dr_app_stop_and_cleanup();
    }
    for (int i = 0; i < g_nregions; i++)
        free(g_regions[i].name);
    g_nregions = 0;
    /* SHUTDOWN is terminal for the process: a subsequent asmtest_dr_init returns
     * ASMTEST_DR_ESTATE. DynamoRIO's in-process re-attach (dr_app_setup after
     * dr_app_stop_and_cleanup) is unreliable in practice — it can crash inside
     * DR — so we do not offer it. Trace again from a FRESH process. */
    g_state = ST_SHUTDOWN;
    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------ */
/* Region registration markers                                         */
/* ------------------------------------------------------------------ */

/* The register/unregister markers do real app-side bookkeeping AND are wrapped
 * by the client (which reads their argument registers at entry). */
__attribute__((noinline, visibility("default"))) void
asmtest_dr_register_region_marker(const char *name, void *base, size_t len,
                                  asmtest_trace_t *trace) {
    g_marker_sink += 0x11 + (unsigned long)(uintptr_t)name;
    (void)base;
    (void)len;
    (void)trace;
}

__attribute__((noinline, visibility("default"))) void
asmtest_dr_unregister_region_marker(const char *name) {
    g_marker_sink += 0x22 + (unsigned long)(uintptr_t)name;
}

/* A real exported function for symbol-mode tracing (Phase 7): traced by NAME with
 * no begin/end markers. noinline + default visibility so it has a stable entry PC
 * the client can resolve via dr_get_proc_address across all loaded modules. Lives
 * in libasmtest_drapp so every language binding (which dlopens drapp) shares one
 * resolvable symbol. (3, 4) -> 10. */
__attribute__((noinline, visibility("default"))) long
asmtest_symbol_demo(long a, long b) {
    long r = a * 2 + b;
    if (r > 1000)
        r -= 7;
    return r;
}

int asmtest_dr_register_region(const char *name, void *base, size_t len,
                               asmtest_trace_t *trace) {
    if (name == NULL || base == NULL || len == 0 || trace == NULL)
        return ASMTEST_DR_EINVAL;
    /* The client stores names in a fixed 64-byte buffer and matches against the
     * full name; a longer name would be truncated client-side and never match,
     * silently disabling recording. Reject it here so the two sides agree. */
    if (strlen(name) >= 64)
        return ASMTEST_DR_EINVAL;
    pthread_mutex_lock(&g_lock);
    if (g_state != ST_STARTED) {
        pthread_mutex_unlock(&g_lock);
        return ASMTEST_DR_ESTATE;
    }
    if (g_nregions >= ASMTEST_DR_MAX_REGIONS) {
        pthread_mutex_unlock(&g_lock);
        return ASMTEST_DR_EFULL;
    }
    char *dup = strdup(name);
    if (dup == NULL) {
        pthread_mutex_unlock(&g_lock);
        return ASMTEST_DR_EINVAL;
    }
    g_regions[g_nregions].name = dup;
    g_regions[g_nregions].base = base;
    g_regions[g_nregions].len = len;
    g_regions[g_nregions].trace = trace;
    g_nregions++;
    pthread_mutex_unlock(&g_lock);
    /* Hand the range to the client (it reads the args at the marker entry). */
    asmtest_dr_register_region_marker(name, base, len, trace);
    return ASMTEST_DR_OK;
}

/* Symbol-mode marker (native-trace Phase 7): the client resolves the symbol's
 * entry PC and records its [entry, entry+max_len) range always-on. Distinct body
 * so it is not merged with the other markers. */
__attribute__((noinline, visibility("default"))) void
asmtest_dr_register_symbol_marker(const char *symbol, size_t max_len,
                                  asmtest_trace_t *trace) {
    g_marker_sink += 0x55 + (unsigned long)(uintptr_t)symbol;
    (void)max_len;
    (void)trace;
}

int asmtest_dr_register_symbol(const char *symbol, size_t max_len,
                               asmtest_trace_t *trace) {
    if (symbol == NULL || max_len == 0 || trace == NULL)
        return ASMTEST_DR_EINVAL;
    if (strlen(symbol) >= 64) /* must fit the client's fixed name buffer */
        return ASMTEST_DR_EINVAL;
    pthread_mutex_lock(&g_lock);
    if (g_state != ST_STARTED) {
        pthread_mutex_unlock(&g_lock);
        return ASMTEST_DR_ESTATE;
    }
    pthread_mutex_unlock(&g_lock);
    asmtest_dr_register_symbol_marker(symbol, max_len, trace);
    return ASMTEST_DR_OK;
}

int asmtest_dr_unregister_region(const char *name) {
    if (name == NULL)
        return ASMTEST_DR_EINVAL;
    pthread_mutex_lock(&g_lock);
    int found = -1;
    for (int i = 0; i < g_nregions; i++)
        if (strcmp(g_regions[i].name, name) == 0) {
            found = i;
            break;
        }
    if (found < 0) {
        pthread_mutex_unlock(&g_lock);
        return ASMTEST_DR_ENOENT;
    }
    free(g_regions[found].name);
    g_regions[found] = g_regions[--g_nregions];
    pthread_mutex_unlock(&g_lock);
    asmtest_dr_unregister_region_marker(name);
    return ASMTEST_DR_OK;
}

/* ------------------------------------------------------------------ */
/* begin / end markers                                                 */
/* ------------------------------------------------------------------ */

void asmtest_trace_begin(const char *name) {
    g_marker_sink +=
        0x33; /* distinct body (also read by the client at entry) */
    if (g_disabled_in_child)
        return;
    if (g_active_depth < ASMTEST_DR_MAX_DEPTH)
        g_active_stack[g_active_depth] = name;
    g_active_depth++;
}

void asmtest_trace_end(const char *name) {
    g_marker_sink += 0x44; /* distinct body */
    if (g_disabled_in_child)
        return;
    if (g_active_depth <= 0) {
        g_marker_errors++; /* end without begin */
        return;
    }
    g_active_depth--;
    const char *top = (g_active_depth < ASMTEST_DR_MAX_DEPTH)
                          ? g_active_stack[g_active_depth]
                          : NULL;
    if (name != NULL && top != NULL && strcmp(name, top) != 0)
        g_marker_errors++; /* mismatched end (not the innermost region) */
}

/* ------------------------------------------------------------------ */
/* Host-native executable code (W^X)                                   */
/* ------------------------------------------------------------------ */

/* Platform seam (macos-dynamorio-port.md T7). Apple Silicon forbids
 * simultaneously writable+executable pages for every process: JIT memory must
 * be mapped RWX with MAP_JIT and toggled between the per-thread RW/RX views
 * with pthread_jit_write_protect_np — mprotect must never touch the region.
 * Everywhere else (Linux, macOS x86-64) the classic
 * PROT_NONE -> mprotect(RW) -> copy -> mprotect(RX) dance applies unchanged.
 *
 * exec_alloc_platform() yields a writable len-byte mapping (so callers know
 * the run address before the bytes exist — the two-pass assembler needs it);
 * exec_copy() writes src_len bytes and seals the whole map_len mapping
 * executable (the assemble path copies fewer bytes than it mapped). Both
 * return 0 on success, -1 on failure with nothing left protected-wrong;
 * the caller owns the munmap on any post-alloc failure. */
#if defined(__APPLE__) && defined(__aarch64__)
static int exec_alloc_platform(size_t len, void **outp) {
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    *outp = p;
    return 0;
}
static int exec_copy(void *dst, const uint8_t *src, size_t src_len,
                     size_t map_len) {
    pthread_jit_write_protect_np(0); /* this thread: write view open */
    memcpy(dst, src, src_len);
    pthread_jit_write_protect_np(1); /* close: executable view */
    __builtin___clear_cache((char *)dst, (char *)dst + map_len);
    return 0;
}
#else /* Linux + macOS x86-64: PROT_NONE -> RW -> RX */
static int exec_alloc_platform(size_t len, void **outp) {
    void *p = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    if (mprotect(p, len, PROT_READ | PROT_WRITE) != 0) {
        munmap(p, len);
        return -1;
    }
    *outp = p;
    return 0;
}
static int exec_copy(void *dst, const uint8_t *src, size_t src_len,
                     size_t map_len) {
    memcpy(dst, src, src_len);
    if (mprotect(dst, map_len, PROT_READ | PROT_EXEC) != 0)
        return -1;
    __builtin___clear_cache((char *)dst, (char *)dst + map_len);
    return 0;
}
#endif

int asmtest_exec_alloc(const uint8_t *bytes, size_t len,
                       asmtest_exec_code_t *out) {
    if (bytes == NULL || len == 0 || out == NULL)
        return ASMTEST_DR_EINVAL;
    void *p;
    if (exec_alloc_platform(len, &p) != 0)
        return ASMTEST_DR_EINVAL;
    if (exec_copy(p, bytes, len, len) != 0) {
        munmap(p, len);
        return ASMTEST_DR_EINVAL;
    }
    out->base = p;
    out->len = len;
    return ASMTEST_DR_OK;
}

int asmtest_asm_exec_native(const char *src, int syntax,
                            asmtest_exec_code_t *out) {
#ifndef ASMTEST_HAVE_KEYSTONE
    (void)src;
    (void)syntax;
    (void)out;
    return ASMTEST_DR_ENOSYS;
#else
#if !defined(__x86_64__)
    /* Host-native assembly is x86-64-only: the hardcoded ASM_X86_64 below
     * would hand an arm64 host x86-64 bytes. arm64 Keystone host-native
     * assembly is explicitly out of scope (macos-dynamorio-port.md T7). */
    (void)src;
    (void)syntax;
    (void)out;
    return ASMTEST_DR_ENOSYS;
#else
    if (src == NULL || out == NULL)
        return ASMTEST_DR_EINVAL;
    /* Pass 1: assemble at address 0 to learn the byte length. */
    asm_result_t r0;
    if (!asmtest_assemble(ASM_X86_64, (asm_syntax_t)syntax, src, 0, &r0)) {
        asmtest_asm_free(&r0);
        return ASMTEST_DR_EINVAL;
    }
    size_t len = r0.len;
    asmtest_asm_free(&r0);
    if (len == 0)
        return ASMTEST_DR_EINVAL;
    /* Reserve the mapping first so we know the real run address. */
    void *p;
    if (exec_alloc_platform(len, &p) != 0)
        return ASMTEST_DR_EINVAL;
    /* Pass 2: re-assemble at the real address so PC-relative targets resolve. */
    asm_result_t r1;
    if (!asmtest_assemble(ASM_X86_64, (asm_syntax_t)syntax, src,
                          (uint64_t)(uintptr_t)p, &r1) ||
        r1.len > len) {
        asmtest_asm_free(&r1);
        munmap(p, len);
        return ASMTEST_DR_EINVAL;
    }
    if (exec_copy(p, r1.bytes, r1.len, len) != 0) {
        asmtest_asm_free(&r1);
        munmap(p, len);
        return ASMTEST_DR_EINVAL;
    }
    asmtest_asm_free(&r1);
    out->base = p;
    out->len = len;
    return ASMTEST_DR_OK;
#endif
#endif
}

void asmtest_exec_free(asmtest_exec_code_t *code) {
    if (code == NULL || code->base == NULL)
        return;
    /* Best-effort: callers that registered the range should
     * asmtest_dr_unregister_region() first so the client flushes the cached
     * translation before the memory is unmapped. */
    munmap(code->base, code->len);
    code->base = NULL;
    code->len = 0;
}
