/*
 * jvmti_jitdump_agent.c — a minimal ATTACH-CAPABLE JVMTI agent that emits a perf
 * jitdump (JIT_CODE_LOAD records) for HotSpot's compiled methods, so jitdump emission
 * can be turned on in an ALREADY-RUNNING JVM (`jcmd <pid> JVMTI.agent_load`) and a
 * method's recorded bytes recovered with asmtest_jitdump_find — WITHOUT having launched
 * the JVM with a tracing flag (intel-pt-attach-foreign-pid T3). Test-support only: it is
 * NEVER shipped in libasmtest and never linked into the library — it is loaded into a
 * HotSpot at runtime by the jit_trace `java-attach-jitdump` lane.
 *
 * WHY a bespoke agent (verified 2026-07-19, doc correction): the linux-tools perf agent
 * libperf-jvmti.so exports ONLY Agent_OnLoad — it has NO Agent_OnAttach — so
 * `jcmd <pid> JVMTI.agent_load libperf-jvmti.so` is refused by HotSpot with
 * "Agent_OnAttach is not available in .../libperf-jvmti.so". It is a launch-only
 * (-agentpath) agent and cannot serve the attach case the doc prescribes. This agent
 * exports BOTH entry points sharing one init and, on ATTACH, replays every already-
 * compiled nmethod via GenerateEvents(COMPILED_METHOD_LOAD) — so a method JIT'd BEFORE
 * the attach is still captured, exactly the property the launch-time -agentpath agent
 * gets for free by being present from the first compile.
 *
 * It writes the standard Linux perf jitdump binary (magic 'JiTD', v1) to
 * /tmp/jit-<pid>.dump — the same path CoreCLR/V8 use and asmtest_jitdump_find resolves
 * for path==NULL. The agent runs INSIDE the JVM, so a compiled method's bytes are copied
 * directly from code_addr (an in-process pointer). Records are flushed per event (no
 * clean-shutdown-flush dependency, unlike the buffered perf agent).
 */
#include <jvmti.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* jitdump on-disk constants (little-endian, matches asmtest_jitdump_find / perf v1). */
#define JITDUMP_MAGIC   0x4A695444u /* 'JiTD' */
#define JIT_CODE_LOAD   0u
#define ELF_MACH_X86_64 62u /* EM_X86_64 */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_dump;
static uint64_t g_index; /* code_index, monotonic per emitted method */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }

/* Open /tmp/jit-<pid>.dump and write the 40-byte v1 file header once. */
static FILE *dump_open(void) {
    if (g_dump != NULL)
        return g_dump;
    char path[64];
    snprintf(path, sizeof path, "/tmp/jit-%d.dump", (int)getpid());
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return NULL;
    put32(f, JITDUMP_MAGIC);      /* magic                       */
    put32(f, 1);                  /* version                     */
    put32(f, 40);                 /* total_size (this header)    */
    put32(f, ELF_MACH_X86_64);    /* elf_mach                    */
    put32(f, 0);                  /* pad1                        */
    put32(f, (uint32_t)getpid()); /* pid                         */
    put64(f, now_ns());           /* timestamp                   */
    put64(f, 0);                  /* flags                       */
    fflush(f);
    g_dump = f;
    return f;
}

/* CompiledMethodLoad: append one JIT_CODE_LOAD record. Fires live for methods compiled
 * after attach AND, on the attach replay (GenerateEvents), for every already-compiled
 * method. The record layout is exactly what asmtest_jitdump_find parses:
 *   prefix(16) = id, total_size, timestamp
 *   body(40)   = pid, tid, vma, code_addr, code_size, code_index
 *   name (NUL-terminated) + code_size raw bytes; total = 56 + name_len + code_size. */
static void JNICALL on_compiled_method_load(jvmtiEnv *jvmti, jmethodID method,
                                            jint code_size,
                                            const void *code_addr,
                                            jint map_length,
                                            const jvmtiAddrLocationMap *map,
                                            const void *compile_info) {
    (void)map_length;
    (void)map;
    (void)compile_info;
    if (code_addr == NULL || code_size <= 0)
        return;

    /* Build the JVM-descriptor name form the perf agent uses and asmtest_jitdump_find
     * matches: <class-signature><method-name><method-signature>, e.g. LHot;asmtjit(II)I. */
    char *mname = NULL, *msig = NULL, *csig = NULL;
    jclass decl = NULL;
    (*jvmti)->GetMethodName(jvmti, method, &mname, &msig, NULL);
    if ((*jvmti)->GetMethodDeclaringClass(jvmti, method, &decl) ==
            JVMTI_ERROR_NONE &&
        decl != NULL)
        (*jvmti)->GetClassSignature(jvmti, decl, &csig, NULL);
    char nm[512];
    snprintf(nm, sizeof nm, "%s%s%s", csig != NULL ? csig : "",
             mname != NULL ? mname : "?", msig != NULL ? msig : "");
    size_t name_len = strlen(nm) + 1; /* the record's name is NUL-terminated */

    pthread_mutex_lock(&g_lock);
    FILE *f = dump_open();
    if (f != NULL) {
        uint64_t addr = (uint64_t)(uintptr_t)code_addr;
        uint64_t csz = (uint64_t)code_size;
        uint32_t total = (uint32_t)(16 + 40 + name_len + csz);
        put32(f, JIT_CODE_LOAD);              /* id                       */
        put32(f, total);                      /* total_size               */
        put64(f, now_ns());                   /* timestamp                */
        put32(f, (uint32_t)getpid());         /* pid                      */
        put32(f, (uint32_t)getpid());         /* tid (reader ignores)     */
        put64(f, addr);                       /* vma                      */
        put64(f, addr);                       /* code_addr                */
        put64(f, csz);                        /* code_size                */
        put64(f, ++g_index);                  /* code_index               */
        fwrite(nm, 1, name_len, f);           /* NUL-terminated name      */
        fwrite(code_addr, 1, (size_t)csz, f); /* raw code bytes (in-proc) */
        fflush(f);
    }
    pthread_mutex_unlock(&g_lock);

    if (mname != NULL)
        (*jvmti)->Deallocate(jvmti, (unsigned char *)mname);
    if (msig != NULL)
        (*jvmti)->Deallocate(jvmti, (unsigned char *)msig);
    if (csig != NULL)
        (*jvmti)->Deallocate(jvmti, (unsigned char *)csig);
}

/* Shared init for both entry points: add the capability, wire the callback, enable the
 * event. On attach, replay methods already compiled before we attached. */
static jint init(JavaVM *vm, int replay) {
    jvmtiEnv *jvmti = NULL;
    if ((*vm)->GetEnv(vm, (void **)&jvmti, JVMTI_VERSION_1_2) != JNI_OK ||
        jvmti == NULL)
        return JNI_ERR;

    jvmtiCapabilities caps;
    memset(&caps, 0, sizeof caps);
    caps.can_generate_compiled_method_load_events = 1;
    if ((*jvmti)->AddCapabilities(jvmti, &caps) != JVMTI_ERROR_NONE)
        return JNI_ERR;

    jvmtiEventCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.CompiledMethodLoad = &on_compiled_method_load;
    if ((*jvmti)->SetEventCallbacks(jvmti, &cb, sizeof cb) != JVMTI_ERROR_NONE)
        return JNI_ERR;
    if ((*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
                                           JVMTI_EVENT_COMPILED_METHOD_LOAD,
                                           NULL) != JVMTI_ERROR_NONE)
        return JNI_ERR;

    /* Attach replay: fire CompiledMethodLoad for every already-compiled nmethod, so a
     * method JIT'd BEFORE we attached lands in the dump too. A launch-time -agentpath
     * agent gets these live from the first compile; an attach agent must ask for them. */
    if (replay)
        (*jvmti)->GenerateEvents(jvmti, JVMTI_EVENT_COMPILED_METHOD_LOAD);
    return JNI_OK;
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *vm, char *options, void *reserved) {
    (void)options;
    (void)reserved;
    return init(vm, /*replay=*/0);
}

/* The reason this agent exists: HotSpot calls Agent_OnAttach for a jcmd JVMTI.agent_load
 * against a running JVM. libperf-jvmti.so has no such entry point, so it cannot attach. */
JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM *vm, char *options,
                                      void *reserved) {
    (void)options;
    (void)reserved;
    return init(vm, /*replay=*/1);
}

JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *vm) {
    (void)vm;
    pthread_mutex_lock(&g_lock);
    if (g_dump != NULL) {
        fclose(g_dump);
        g_dump = NULL;
    }
    pthread_mutex_unlock(&g_lock);
}
