/* watch_victim.c — a victim for the asmspy --watch (hardware data-watchpoint) smoke.
 *
 * A WORKER thread (NOT the thread-group leader) stores a known magic value into a
 * known 8-byte global in a loop. Two things make this the right test for --watch:
 *
 *   1. Known location + known value: main() prints &g_watch_target so the smoke
 *      can pass it to `--watch <pid> 0xADDR`, and every store writes WATCH_MAGIC,
 *      so the smoke can assert asmspy captured the exact written value.
 *
 *   2. PER-THREAD arming: the writes happen on the worker, not the leader. A
 *      tracer that armed the debug registers only on the group leader (the easy
 *      bug) would trap NONE of these writes; catching them proves asmspy armed
 *      the watchpoint on every task under /proc/<pid>/task. main() prints the
 *      worker's tid so the smoke can confirm the reported hit came from it.
 *
 * Opts in via PR_SET_PTRACER_ANY like the other example victims so attach works
 * in a plain container.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

/* Distinctive, greppable value the worker stores into the watched field. */
#define WATCH_MAGIC 0xd15ea5eddeadbeefULL

/* The watched field: 8-byte, 8-aligned (an x86 watchpoint needs a length-aligned
 * address). volatile so each store really executes (not hoisted out of the loop). */
static volatile uint64_t g_watch_target __attribute__((aligned(8)));

/* The worker thread writes the magic into the watched field ~20x/second — spaced
 * enough that the tracer attaches + arms before catching writes, and fast enough
 * that a small hit budget completes quickly. Non-static + noinline so it keeps a
 * resolvable STT_FUNC symbol the faulting PC lands in (like the other victims). */
__attribute__((noinline)) void *writer(void *arg) {
    (void)arg;
    fprintf(stderr, "watch worker_tid=%d\n", (int)syscall(SYS_gettid));
    fflush(stderr);
    for (;;) {
        g_watch_target = WATCH_MAGIC; /* the WATCHED store */
        usleep(50 * 1000);
    }
    return NULL;
}

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "watch_victim pid=%d watch_target=%p\n", (int)getpid(),
            (void *)&g_watch_target);
    fflush(stderr);
    pthread_t th;
    if (pthread_create(&th, NULL, writer, NULL) != 0)
        return 1;
    pthread_join(th,
                 NULL); /* the leader just idles; the worker does the writes */
    return 0;
}
