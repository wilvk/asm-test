/*
 * pt_backend.c — Intel PT decode backend for the hardware-trace tier (libipt).
 * See asmtest_hwtrace.h and docs/native-tracing.md.
 *
 * Intel PT records branch *decisions* only; the per-instruction and per-block
 * stream is *reconstructed* by replaying asm-test's own registered code bytes
 * between branch waypoints. We use libipt's instruction decoder (pt_insn_next),
 * which yields each executed instruction's IP and class, then:
 *   - record each in-range instruction offset into insns[];
 *   - NORMALIZE basic blocks to match the Unicorn/DynamoRIO partition: a libipt
 *     "block" can span direct branches, so instead we mark a block start at the
 *     first instruction and after every branch instruction (a DR basic block ends
 *     at a control transfer; the next executed instruction begins a new block).
 * Speculative/aborted instructions are filtered before recording.
 *
 * Gated on -DASMTEST_HAVE_LIBIPT (pkg-config libipt). Without libipt this TU
 * still compiles, reporting the decoder absent so asmtest_hwtrace_available()
 * self-skips.
 */
#include "asmtest_codeimage.h"
#include "asmtest_trace.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ASMTEST_HW_OK      0
#define ASMTEST_HW_EINVAL  (-1)
#define ASMTEST_HW_ENOSYS  (-5)
#define ASMTEST_HW_EDECODE (-8)

/* ------------------------------------------------------------------ */
/* §2 — recorder-backed image read (libipt-independent, host-testable) */
/*                                                                     */
/* Serve the bytes live at `ip` as of trace position `when` from a      */
/* self/foreign code-image recorder, so an in-process PT capture of the  */
/* WHOLE window decodes against the JIT's own live bytes (the temporal-  */
/* bytes rule) rather than a single pre-registered range. Returns the    */
/* number of bytes copied (> 0), or a negative ASMTEST_HW_* on a miss    */
/* (no version at/before `when`, or `ip` outside every tracked region).  */
/* The libipt pt_image callback (read_recorder, below) adapts this to    */
/* pt_image's signature when libipt is present; this plain form is       */
/* exercised directly by test_pt_image_from_codeimage — no PT hardware,  */
/* no libipt packet stream (unlike AMD/CS, Intel PT has no synthetic     */
/* packet fixture in the tree, so end-to-end libipt decode is forward-   */
/* look on real hardware). */
int asmtest_pt_read_codeimage(const asmtest_codeimage_t *img, uint64_t when,
                              uint64_t ip, uint8_t *buffer, size_t size) {
    if (img == NULL || buffer == NULL || size == 0)
        return ASMTEST_HW_EINVAL;
    const uint8_t *bytes = NULL;
    size_t avail = 0;
    int rc = asmtest_codeimage_bytes_at(img, (const void *)(uintptr_t)ip, when,
                                        &bytes, &avail);
    if (rc != ASMTEST_CI_OK || bytes == NULL || avail == 0)
        return ASMTEST_HW_EDECODE; /* not mapped at this (ip, when) */
    size_t n = (size < avail) ? size : avail;
    memcpy(buffer, bytes, n);
    return (int)n;
}

#ifdef ASMTEST_HAVE_LIBIPT
#include <intel-pt.h>

int asmtest_pt_decoder_present(void) { return 1; }

/* Context for the image read callback: serve bytes from the registered region. */
typedef struct {
    const uint8_t *base;
    uint64_t base_ip;
    size_t len;
} pt_read_ctx_t;

static int read_region(uint8_t *buffer, size_t size, const struct pt_asid *asid,
                       uint64_t ip, void *context) {
    (void)asid;
    pt_read_ctx_t *c = (pt_read_ctx_t *)context;
    if (ip < c->base_ip || ip >= c->base_ip + c->len)
        return -pte_nomap;
    uint64_t avail = c->base_ip + c->len - ip;
    size_t n = (size < avail) ? size : (size_t)avail;
    memcpy(buffer, c->base + (ip - c->base_ip), n);
    return (int)n;
}

/* Context for the recorder-backed pt_image callback (§2 whole-window mode). */
typedef struct {
    const asmtest_codeimage_t *img;
    uint64_t when;
} pt_recorder_ctx_t;

/* pt_image read callback backed by the code-image recorder: serves bytes for ANY
 * executed address (temporal-correct as of `when`), so the decoder no longer stops
 * at the first out-of-region IP the way read_region does. Distinct from read_region
 * — selected by mode — so the shipped region-scoped decode stays byte-identical. */
static int read_recorder(uint8_t *buffer, size_t size,
                         const struct pt_asid *asid, uint64_t ip,
                         void *context) {
    (void)asid;
    pt_recorder_ctx_t *c = (pt_recorder_ctx_t *)context;
    int n = asmtest_pt_read_codeimage(c->img, c->when, ip, buffer, size);
    return (n > 0) ? n : -pte_nomap;
}

static int is_branch(enum pt_insn_class c) {
    switch (c) {
    case ptic_call:
    case ptic_return:
    case ptic_jump:
    case ptic_cond_jump:
    case ptic_far_call:
    case ptic_far_return:
    case ptic_far_jump:
        return 1;
    default:
        return 0;
    }
}

int asmtest_pt_decode(const uint8_t *aux, size_t aux_len, const void *base,
                      size_t len, asmtest_trace_t *trace) {
    if (aux == NULL || aux_len == 0 || base == NULL || len == 0 ||
        trace == NULL)
        return ASMTEST_HW_EDECODE;

    struct pt_config config;
    pt_config_init(&config);
    config.begin = (uint8_t *)aux;
    config.end = (uint8_t *)aux + aux_len;
    /* pt_config_init zeroes config.cpu (vendor unknown); libipt then applies no
     * CPU errata workarounds, which is correct for a faithful trace. The capture
     * side could fill config.cpu from perf's AUXTRACE_INFO for errata coverage. */

    struct pt_insn_decoder *dec = pt_insn_alloc_decoder(&config);
    if (dec == NULL)
        return ASMTEST_HW_EDECODE;

    pt_read_ctx_t ctx = {(const uint8_t *)base, (uint64_t)(uintptr_t)base, len};
    struct pt_image *image = pt_image_alloc("asmtest");
    if (image == NULL) {
        pt_insn_free_decoder(dec);
        return ASMTEST_HW_EDECODE;
    }
    pt_image_set_callback(image, read_region, &ctx);
    pt_insn_set_image(dec, image);

    /* Sync to each PSB and walk instructions; normalize blocks at branch edges. */
    size_t inregion = 0; /* in-region instructions actually decoded */
    for (;;) {
        int status = pt_insn_sync_forward(dec);
        if (status < 0)
            break;               /* -pte_eos: no more sync points */
        int prev_was_branch = 1; /* first insn after a sync starts a block */
        for (;;) {
            struct pt_insn insn;
            status = pt_insn_next(dec, &insn, sizeof insn);
            if (status < 0)
                break;
            if (!insn.speculative && insn.ip >= ctx.base_ip &&
                insn.ip < ctx.base_ip + ctx.len) {
                uint64_t off = insn.ip - ctx.base_ip;
                trace_append_insn(trace,
                                  off); /* no-op if insns recording off */
                if (prev_was_branch)
                    trace_append_block(trace, off);
                prev_was_branch = is_branch(insn.iclass);
                inregion++;
            } else if (!insn.speculative) {
                /* Left the region (e.g. a call out); the next in-range insn
                 * begins a fresh block. */
                prev_was_branch = 1;
            }
            if (status & pts_eos)
                break;
        }
    }

    pt_insn_free_decoder(dec);
    pt_image_free(image);
    /* A non-empty AUX stream that yields zero in-region instructions is not a
     * complete empty trace — without a PT address filter the decoder dies with
     * -pte_nomap at the first out-of-region IP (enable-time PSB is in libc/ioctl
     * glue), so it never reaches the region. Report a decode failure so end()
     * flags the trace truncated instead of "complete". (The full fix also programs
     * a PERF_EVENT_IOC_SET_FILTER region filter on the capture side, which requires
     * Intel PT hardware to validate — see docs/native-tracing.md.) */
    if (inregion == 0)
        return ASMTEST_HW_EDECODE;
    return ASMTEST_HW_OK;
}

/* §2 whole-window decode: like asmtest_pt_decode, but the pt_image is backed by the
 * code-image recorder (bytes for ANY address, temporal-correct as of `when`) and NO
 * in-region IP filter is applied — every executed instruction is recorded, at an
 * offset from the first decoded IP. A distinct callback + record policy from the
 * region-scoped decode, selected by mode. Needs Intel PT hardware to validate live
 * (self-skips as the rest of the PT capture path does); the recorder-backed image
 * adapter it rides (asmtest_pt_read_codeimage) is host-tested directly. */
int asmtest_pt_decode_window(const uint8_t *aux, size_t aux_len,
                             const asmtest_codeimage_t *img, uint64_t when,
                             asmtest_trace_t *trace) {
    if (aux == NULL || aux_len == 0 || img == NULL || trace == NULL)
        return ASMTEST_HW_EDECODE;
    struct pt_config config;
    pt_config_init(&config);
    config.begin = (uint8_t *)aux;
    config.end = (uint8_t *)aux + aux_len;
    struct pt_insn_decoder *dec = pt_insn_alloc_decoder(&config);
    if (dec == NULL)
        return ASMTEST_HW_EDECODE;
    pt_recorder_ctx_t ctx = {img, when};
    struct pt_image *image = pt_image_alloc("asmtest-window");
    if (image == NULL) {
        pt_insn_free_decoder(dec);
        return ASMTEST_HW_EDECODE;
    }
    pt_image_set_callback(image, read_recorder, &ctx);
    pt_insn_set_image(dec, image);

    size_t decoded = 0;
    uint64_t base_ip = 0; /* first decoded IP: the window's offset origin */
    for (;;) {
        int status = pt_insn_sync_forward(dec);
        if (status < 0)
            break;
        int prev_was_branch = 1;
        for (;;) {
            struct pt_insn insn;
            status = pt_insn_next(dec, &insn, sizeof insn);
            if (status < 0)
                break;
            if (!insn.speculative) {
                if (decoded == 0)
                    base_ip = insn.ip;
                uint64_t off = insn.ip - base_ip;
                trace_append_insn(trace, off);
                if (prev_was_branch)
                    trace_append_block(trace, off);
                prev_was_branch = is_branch(insn.iclass);
                decoded++;
            }
            if (status & pts_eos)
                break;
        }
    }
    pt_insn_free_decoder(dec);
    pt_image_free(image);
    if (decoded == 0)
        return ASMTEST_HW_EDECODE;
    return ASMTEST_HW_OK;
}

#else /* !ASMTEST_HAVE_LIBIPT */

int asmtest_pt_decoder_present(void) { return 0; }

int asmtest_pt_decode(const uint8_t *aux, size_t aux_len, const void *base,
                      size_t len, asmtest_trace_t *trace) {
    (void)aux;
    (void)aux_len;
    (void)base;
    (void)len;
    (void)trace;
    return ASMTEST_HW_ENOSYS;
}

int asmtest_pt_decode_window(const uint8_t *aux, size_t aux_len,
                             const asmtest_codeimage_t *img, uint64_t when,
                             asmtest_trace_t *trace) {
    (void)aux;
    (void)aux_len;
    (void)img;
    (void)when;
    (void)trace;
    return ASMTEST_HW_ENOSYS;
}

#endif /* ASMTEST_HAVE_LIBIPT */
