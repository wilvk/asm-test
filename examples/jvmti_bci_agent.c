/*
 * jvmti_bci_agent.c — a minimal JVMTI agent for TRUE per-address JVM bytecode-index
 * attribution (native-il-bytecode-attribution T6). Test-support only: it is NEVER
 * shipped in libasmtest and never linked into the library — it is loaded into a JVM
 * with -agentpath by the jit_trace `java-bci` lane, exactly like the perf project's
 * libperf-jvmti.so is loaded into HotSpot for the jitdump lane.
 *
 * HotSpot's own jitdump (via libperf-jvmti.so) carries JIT_CODE_LOAD records ONLY — no
 * source/bytecode info (verified: 0 DEBUG_INFO records). But JVMTI's CompiledMethodLoad
 * event delivers the JIT's OWN address->bytecode-index map. This agent captures it to a
 * plain text sidecar /tmp/asmtest-bci-<pid>.map, one line per PC:
 *
 *     <hex addr> <bci> <line> <class-signature><method-name>
 *
 * so the harness can ingest (addr - method_base) -> bci into an asmtest_srcreg and prove
 * a native address resolves to a bytecode index. Line numbers (via GetLineNumberTable)
 * are best-effort (-1 when absent). The sidecar is written LIVE, per event — no clean
 * shutdown dependency, unlike the perf agent's buffered jitdump.
 *
 * Inline-aware where the JIT provides it: if compile_info carries a
 * JVMTI_CMLR_INLINE_INFO record chain (jvmticmlr.h), we walk the PCStackInfo array and
 * write the LEAF frame (methods[0]/bcis[0]) per PC. Otherwise we fall back to the flat
 * jvmtiAddrLocationMap, whose `location` IS the bci on HotSpot
 * (GetJLocationFormat == JVMTI_JLOCATION_JVMBCI). The map may be NULL (it is optional) —
 * we then write nothing, never crash.
 */
#include <jvmti.h>
#include <jvmticmlr.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_out;

static FILE *sidecar(void) {
    if (g_out == NULL) {
        char path[64];
        snprintf(path, sizeof path, "/tmp/asmtest-bci-%d.map", (int)getpid());
        g_out = fopen(path, "w");
    }
    return g_out;
}

/* Map a bytecode index to a source line via the method's line-number table (the entry
 * with the largest start_location <= bci). -1 when unavailable. */
static jint line_for_bci(jvmtiEnv *jvmti, jmethodID method, jlocation bci) {
    jint count = 0;
    jvmtiLineNumberEntry *table = NULL;
    if ((*jvmti)->GetLineNumberTable(jvmti, method, &count, &table) !=
            JVMTI_ERROR_NONE ||
        table == NULL)
        return -1;
    jint line = -1;
    for (jint i = 0; i < count; i++)
        if (table[i].start_location <= bci)
            line = table[i].line_number;
        else
            break;
    (*jvmti)->Deallocate(jvmti, (unsigned char *)table);
    return line;
}

/* Emit one sidecar line for a (pc, bci, method) point. */
static void emit_point(FILE *f, jvmtiEnv *jvmti, const void *pc, jlocation bci,
                       jmethodID method) {
    char *msig = NULL, *mname = NULL, *csig = NULL;
    jclass decl = NULL;
    (*jvmti)->GetMethodName(jvmti, method, &mname, &msig, NULL);
    if ((*jvmti)->GetMethodDeclaringClass(jvmti, method, &decl) ==
            JVMTI_ERROR_NONE &&
        decl != NULL)
        (*jvmti)->GetClassSignature(jvmti, decl, &csig, NULL);
    jint line = line_for_bci(jvmti, method, bci);
    fprintf(f, "%llx %ld %d %s%s\n", (unsigned long long)(uintptr_t)pc,
            (long)bci, (int)line, csig != NULL ? csig : "",
            mname != NULL ? mname : "?");
    if (mname != NULL)
        (*jvmti)->Deallocate(jvmti, (unsigned char *)mname);
    if (msig != NULL)
        (*jvmti)->Deallocate(jvmti, (unsigned char *)msig);
    if (csig != NULL)
        (*jvmti)->Deallocate(jvmti, (unsigned char *)csig);
}

static void JNICALL on_compiled_method_load(jvmtiEnv *jvmti, jmethodID method,
                                            jint code_size,
                                            const void *code_addr,
                                            jint map_length,
                                            const jvmtiAddrLocationMap *map,
                                            const void *compile_info) {
    (void)code_size;
    (void)code_addr;
    pthread_mutex_lock(&g_lock);
    FILE *f = sidecar();
    if (f == NULL) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    /* Inline-aware path: walk the compile_info record chain for INLINE_INFO. */
    int wrote_inline = 0;
    for (const jvmtiCompiledMethodLoadRecordHeader *h =
             (const jvmtiCompiledMethodLoadRecordHeader *)compile_info;
         h != NULL; h = h->next) {
        if (h->kind != JVMTI_CMLR_INLINE_INFO)
            continue;
        const jvmtiCompiledMethodLoadInlineRecord *ir =
            (const jvmtiCompiledMethodLoadInlineRecord *)h;
        for (jint i = 0; i < ir->numpcs; i++) {
            const PCStackInfo *pc = &ir->pcinfo[i];
            if (pc->numstackframes > 0 && pc->methods != NULL &&
                pc->bcis != NULL) {
                emit_point(f, jvmti, pc->pc, pc->bcis[0],
                           pc->methods[0]); /* leaf */
                wrote_inline = 1;
            }
        }
    }

    /* Flat fallback: the address->location map, where location == bci on HotSpot. */
    if (!wrote_inline && map != NULL)
        for (jint i = 0; i < map_length; i++)
            emit_point(f, jvmti, map[i].start_address, map[i].location, method);

    fflush(f);
    pthread_mutex_unlock(&g_lock);
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *vm, char *options, void *reserved) {
    (void)options;
    (void)reserved;
    jvmtiEnv *jvmti = NULL;
    if ((*vm)->GetEnv(vm, (void **)&jvmti, JVMTI_VERSION_1_2) != JNI_OK ||
        jvmti == NULL)
        return JNI_ERR;

    jvmtiCapabilities caps;
    memset(&caps, 0, sizeof caps);
    caps.can_generate_compiled_method_load_events = 1;
    if ((*jvmti)->AddCapabilities(jvmti, &caps) != JVMTI_ERROR_NONE)
        return JNI_ERR;
    /* Best-effort line/source capabilities (ignore failure — bci still works). */
    jvmtiCapabilities extra;
    memset(&extra, 0, sizeof extra);
    extra.can_get_line_numbers = 1;
    extra.can_get_source_file_name = 1;
    (void)(*jvmti)->AddCapabilities(jvmti, &extra);

    jvmtiEventCallbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.CompiledMethodLoad = &on_compiled_method_load;
    if ((*jvmti)->SetEventCallbacks(jvmti, &cb, sizeof cb) != JVMTI_ERROR_NONE)
        return JNI_ERR;
    if ((*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
                                           JVMTI_EVENT_COMPILED_METHOD_LOAD,
                                           NULL) != JVMTI_ERROR_NONE)
        return JNI_ERR;
    return JNI_OK;
}

JNIEXPORT void JNICALL Agent_OnUnload(JavaVM *vm) {
    (void)vm;
    pthread_mutex_lock(&g_lock);
    if (g_out != NULL) {
        fclose(g_out);
        g_out = NULL;
    }
    pthread_mutex_unlock(&g_lock);
}
