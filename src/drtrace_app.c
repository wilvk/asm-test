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

/* ------------------------------------------------------------------ */
/* DynamoRIO Application Interface, resolved via dlopen (see banner)    */
/* ------------------------------------------------------------------ */
static void *g_dr_handle;
static int (*p_dr_app_setup)(void);
static void (*p_dr_app_start)(void);
static void (*p_dr_app_stop)(void);
static void (*p_dr_app_stop_and_cleanup)(void);

/* Resolve the libdynamorio path: explicit option, env override, then a bare
 * soname (found via rpath / LD_LIBRARY_PATH). Writes into buf, returns it. */
static const char *dr_lib_path(const asmtest_drtrace_options_t *opts, char *buf,
                               size_t buflen) {
    const char *env = getenv("ASMTEST_DR_LIB");
    if (opts != NULL && opts->dynamorio_home != NULL &&
        opts->dynamorio_home[0] != '\0') {
        snprintf(buf, buflen, "%s/lib64/release/libdynamorio.so",
                 opts->dynamorio_home);
        return buf;
    }
    if (env != NULL && env[0] != '\0') {
        snprintf(buf, buflen, "%s", env);
        return buf;
    }
    snprintf(buf, buflen, "libdynamorio.so");
    return buf;
}

/* ------------------------------------------------------------------ */
/* Lifecycle state machine                                             */
/* ------------------------------------------------------------------ */
enum dr_state {
    ST_UNINIT = 0,
    ST_INIT,
    ST_STARTED,
    ST_STOPPED,
    ST_SHUTDOWN
};

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

int asmtest_dr_available(void) {
    /* A cheap, side-effect-free probe: is a libdynamorio path resolvable? We do
     * NOT dlopen here (that runs DR's constructor); we just check the file. A
     * bare soname (no path) is reported available and left to dlopen to settle. */
    char buf[1024];
    const char *env = getenv("ASMTEST_DR_LIB");
    if (env != NULL && env[0] != '\0')
        return access(env, R_OK) == 0;
    const char *home = getenv("DYNAMORIO_HOME");
    if (home != NULL && home[0] != '\0') {
        snprintf(buf, sizeof buf, "%s/lib64/release/libdynamorio.so", home);
        return access(buf, R_OK) == 0;
    }
    return 0;
}

int asmtest_dr_marker_error(void) { return g_marker_errors; }

/* ------------------------------------------------------------------ */
/* Init / start / stop / shutdown                                      */
/* ------------------------------------------------------------------ */

int asmtest_dr_init(const asmtest_drtrace_options_t *opts) {
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
    if (p_dr_app_setup == NULL || p_dr_app_start == NULL ||
        p_dr_app_stop == NULL || p_dr_app_stop_and_cleanup == NULL) {
        dlclose(g_dr_handle);
        g_dr_handle = NULL;
        pthread_mutex_unlock(&g_lock);
        return ASMTEST_DR_ENODR;
    }
    if (p_dr_app_setup() != 0) {
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
    pthread_mutex_lock(&g_lock);
    if ((g_state == ST_STARTED || g_state == ST_STOPPED || g_state == ST_INIT) &&
        p_dr_app_stop_and_cleanup != NULL) {
        p_dr_app_stop_and_cleanup();
    }
    for (int i = 0; i < g_nregions; i++)
        free(g_regions[i].name);
    g_nregions = 0;
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

int asmtest_dr_register_region(const char *name, void *base, size_t len,
                               asmtest_trace_t *trace) {
    if (name == NULL || base == NULL || len == 0 || trace == NULL)
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
    g_marker_sink += 0x33; /* distinct body (also read by the client at entry) */
    if (g_active_depth < ASMTEST_DR_MAX_DEPTH)
        g_active_stack[g_active_depth] = name;
    g_active_depth++;
}

void asmtest_trace_end(const char *name) {
    g_marker_sink += 0x44; /* distinct body */
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

int asmtest_exec_alloc(const uint8_t *bytes, size_t len,
                       asmtest_exec_code_t *out) {
    if (bytes == NULL || len == 0 || out == NULL)
        return ASMTEST_DR_EINVAL;
    /* W^X: map without permissions, flip to RW to copy, then to RX. */
    void *p = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return ASMTEST_DR_EINVAL;
    if (mprotect(p, len, PROT_READ | PROT_WRITE) != 0) {
        munmap(p, len);
        return ASMTEST_DR_EINVAL;
    }
    memcpy(p, bytes, len);
    if (mprotect(p, len, PROT_READ | PROT_EXEC) != 0) {
        munmap(p, len);
        return ASMTEST_DR_EINVAL;
    }
    __builtin___clear_cache((char *)p, (char *)p + len);
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
    /* Reserve the executable mapping so we know the real run address. */
    void *p = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
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
    if (mprotect(p, len, PROT_READ | PROT_WRITE) != 0) {
        asmtest_asm_free(&r1);
        munmap(p, len);
        return ASMTEST_DR_EINVAL;
    }
    memcpy(p, r1.bytes, r1.len);
    asmtest_asm_free(&r1);
    if (mprotect(p, len, PROT_READ | PROT_EXEC) != 0) {
        munmap(p, len);
        return ASMTEST_DR_EINVAL;
    }
    __builtin___clear_cache((char *)p, (char *)p + len);
    out->base = p;
    out->len = len;
    return ASMTEST_DR_OK;
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
