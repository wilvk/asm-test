/*
 * asmtest_addr_channel.h — the §D3 cross-process JIT-address channel.
 *
 * The concealed out-of-process ptrace-stealth stepper (asmtest_hwtrace_stealth_*)
 * runs in a SEPARATE process from the managed runtime it traces, so it does not see
 * the runtime's own MethodLoadVerbose events — it cannot know where a live JIT put a
 * method body. This is the load-bearing gap the managed plan's §D3 flags: the parent's
 * in-process listener knows the addresses; the helper needs them.
 *
 * This is that channel — a single-producer / single-consumer lock-free ring, sized to
 * live in the shared memfd the stealth stepper already maps into both processes. The
 * PARENT (the runtime, in its MethodLoadVerbose callback) publishes each JIT'd method's
 * (base, len, version); the HELPER (the stepper) drains it between steps and adds the
 * region to the set it records + follows. Header-only inline so the same definition
 * compiles into both the library and the bundled standalone helper binary with no
 * separate object.
 *
 * Concurrency: exactly one producer thread and one consumer thread. `head` is written
 * only by the producer (release), read by the consumer (acquire); `tail` is written
 * only by the consumer. No lock, no CAS — safe for the "runtime publishes while the
 * stepper drains" pattern. On overrun (producer laps the consumer past CAP) the
 * consumer skips to the oldest still-live record and flags it, never reads a torn slot.
 */
#ifndef ASMTEST_ADDR_CHANNEL_H
#define ASMTEST_ADDR_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One published JIT region: where a method body was emitted, how big, and the
 * code-image version live when it was published (0 if the publisher tracks none). */
typedef struct {
    uint64_t base;
    uint64_t len;
    uint64_t version;
} asmtest_addr_rec_t;

#ifndef ASMTEST_ADDR_CHAN_CAP
#define ASMTEST_ADDR_CHAN_CAP 256u /* in-flight records before the ring wraps */
#endif

typedef struct {
    volatile uint32_t
        head; /* producer publishes; next slot index (monotonic)   */
    volatile uint32_t
        tail; /* consumer drains; next slot to read (monotonic)     */
    volatile uint32_t
        overrun; /* set once if the producer ever lapped the ring   */
    uint32_t _pad;
    asmtest_addr_rec_t recs[ASMTEST_ADDR_CHAN_CAP];
} asmtest_addr_channel_t;

/* Zero a freshly-mapped channel. Call once, in the producer, before the consumer
 * process is forked (so both see an initialized ring). */
static inline void asmtest_addr_channel_init(asmtest_addr_channel_t *c) {
    if (c == NULL)
        return;
    c->head = 0;
    c->tail = 0;
    c->overrun = 0;
    c->_pad = 0;
}

/* PRODUCER: publish one region. Writes the slot, then bumps `head` with a release
 * store so a draining consumer that observes the new head also sees the slot contents.
 * Never blocks; on a full ring the oldest unread record is overwritten and `overrun`
 * is set (the consumer then honestly skips the lost span). */
static inline void asmtest_addr_channel_publish(asmtest_addr_channel_t *c,
                                                uint64_t base, uint64_t len,
                                                uint64_t version) {
    if (c == NULL)
        return;
    uint32_t h = c->head;
    uint32_t slot = h % ASMTEST_ADDR_CHAN_CAP;
    c->recs[slot].base = base;
    c->recs[slot].len = len;
    c->recs[slot].version = version;
    __atomic_store_n(&c->head, h + 1u, __ATOMIC_RELEASE);
}

/* CONSUMER: drain up to `max` newly-published records into `out`, returning the count
 * (0 if none). Acquire-loads `head` to pair with the producer's release. If the
 * producer lapped the ring since the last drain (more than CAP unread), the consumer
 * skips to the oldest still-live record and sets `overrun` — a disclosed gap, never a
 * torn read. */
static inline uint32_t asmtest_addr_channel_drain(asmtest_addr_channel_t *c,
                                                  asmtest_addr_rec_t *out,
                                                  uint32_t max) {
    if (c == NULL || out == NULL || max == 0)
        return 0;
    uint32_t h = __atomic_load_n(&c->head, __ATOMIC_ACQUIRE);
    uint32_t t = c->tail;
    if (h - t > ASMTEST_ADDR_CHAN_CAP) {
        t = h -
            ASMTEST_ADDR_CHAN_CAP; /* producer lapped us: jump to oldest live */
        c->overrun = 1;
    }
    uint32_t n = 0;
    while (t != h && n < max) {
        out[n++] = c->recs[t % ASMTEST_ADDR_CHAN_CAP];
        t++;
    }
    c->tail = t;
    return n;
}

/* Total records published so far (monotonic). Lets a consumer tell "nothing yet" from
 * "drained everything" without draining. */
static inline uint32_t
asmtest_addr_channel_published(const asmtest_addr_channel_t *c) {
    return c != NULL ? __atomic_load_n(&c->head, __ATOMIC_ACQUIRE) : 0;
}

/* Exported (non-inline) FFI shims over the inline API above — for bindings (Node/.NET/
 * …) that cannot call static-inline functions across the FFI boundary. new() mallocs +
 * inits a PROCESS-LOCAL channel (free with free()); publish_rec() wraps the inline
 * publish. Enough for the fork-internal asmtest_ptrace_trace_window_call, where the
 * caller pre-publishes regions and the forked child inherits a copy. Implemented in
 * src/ptrace_backend.c. */
asmtest_addr_channel_t *asmtest_addr_channel_new(void);
void asmtest_addr_channel_publish_rec(asmtest_addr_channel_t *c, uint64_t base,
                                      uint64_t len, uint64_t version);
void asmtest_addr_channel_free(asmtest_addr_channel_t *c);

/* SHARED-memory channel (MAP_SHARED) for the §D3 whole-window stepper: the runtime's JIT
 * listener publishes into it WHILE the forked helper drains it live, so methods JIT'd
 * mid-window are captured. The fork preserves the address, so the same pointer is valid in
 * both processes. Free with asmtest_addr_channel_free_shared (munmap). */
asmtest_addr_channel_t *asmtest_addr_channel_new_shared(void);
void asmtest_addr_channel_free_shared(asmtest_addr_channel_t *c);

#ifdef __cplusplus
}
#endif

#endif /* ASMTEST_ADDR_CHANNEL_H */
