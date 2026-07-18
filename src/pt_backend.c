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
/* no libipt packet stream. (End-to-end libipt decode is NOT forward-    */
/* look: asmtest_pt_encode_fixture below synthesizes a valid PT packet   */
/* stream with libipt's own encoder, and test_wholewindow_decode drives  */
/* both decode entries through it on any host. What stays hardware-      */
/* gated is PT CAPTURE — and with it the facade dispatch, which cannot   */
/* run at all until asmtest_hwtrace_available(INTEL_PT) is true.) */
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

/* Drain any pending events before asking for the next instruction. libipt's
 * instruction-flow decoder REQUIRES that pending events (tracing enabled/disabled,
 * exec-mode change, paging, overflow, ...) be consumed via pt_insn_event before the
 * next pt_insn_next() — otherwise pt_insn_next() returns -pte_bad_query ("expected
 * tracing enabled event"). Every real PT stream begins with a trace-enable event, so
 * a decode loop that never drains events yields ZERO instructions on any valid input.
 * Advances *status in place; returns 0 on success (or a clean drain) or the negative
 * libipt error from pt_insn_event. Mirrors libipt's own ptxed reference loop. */
static int drain_events(struct pt_insn_decoder *dec, int *status) {
    while (*status & pts_event_pending) {
        struct pt_event event;
        *status = pt_insn_event(dec, &event, sizeof event);
        if (*status < 0)
            return *status;
    }
    return 0;
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
            if (drain_events(dec, &status) < 0)
                break;
            if (status & pts_eos)
                break;
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
                             asmtest_trace_t *trace, uint64_t *base_ip_out) {
    if (base_ip_out != NULL)
        *base_ip_out = 0;
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
            if (drain_events(dec, &status) < 0)
                break;
            if (status & pts_eos)
                break;
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
    if (base_ip_out != NULL)
        *base_ip_out = base_ip; /* the window's absolute offset origin */
    if (decoded == 0)
        return ASMTEST_HW_EDECODE;
    return ASMTEST_HW_OK;
}

/* ------------------------------------------------------------------ */
/* Synthetic fixture generator — a valid Intel PT packet stream WITHOUT any PT  */
/* hardware, built with libipt's own packet ENCODER (userspace). Emits exactly  */
/* the stream a CPU would produce for the canonical ROUTINE                     */
/*   mov rax,rdi ; add rax,rsi ; cmp rax,100 ; jle +3 ; dec rax ; ret           */
/* on EITHER side of its `jle`: a PSB header (psb, mode.exec 64-bit, psbend),   */
/* trace-enable at `base_ip` (tip.pge), one 1-bit TNT for the jle, then trace-  */
/* disable at the ret (tip.pgd).                                                */
/*                                                                              */
/* `taken` selects the TNT payload and is the fixture's DISCRIMINATING knob:    */
/*   taken != 0 (args like 20,22 -> 42 <= 100): jle TAKEN, the dec at +0xe is   */
/*              SKIPPED -> insns {0x0,0x3,0x6,0xc,0x11},    blocks {0x0,0x11}   */
/*   taken == 0 (args like 200,1 -> 201 > 100): jle NOT taken, the dec at +0xe  */
/*              RUNS    -> insns {0x0,0x3,0x6,0xc,0xe,0x11}, blocks {0x0,0xe}   */
/* The taken walk is the SAME ground truth the AMD / CoreSight / DynamoRIO      */
/* backends reconstruct for these bytes. Emitting BOTH is what makes the decode */
/* test non-vacuous: a one-sided fixture cannot distinguish a decoder that      */
/* actually FOLLOWS the TNT from one whose expected answer is merely baked in — */
/* the two walks differ exactly at the 0xe `dec`. Lets                          */
/* asmtest_pt_decode[_window] run end-to-end in CI (test_wholewindow_decode)    */
/* with no intel_pt PMU. Writes the raw AUX bytes into buf[0,cap); *out_len gets */
/* the byte count. Returns ASMTEST_HW_OK, ASMTEST_HW_EINVAL on a bad argument, or */
/* ASMTEST_HW_EDECODE on an encoder error. */
int asmtest_pt_encode_fixture(uint8_t *buf, size_t cap, uint64_t base_ip,
                              int taken, size_t *out_len) {
    if (buf == NULL || cap == 0 || out_len == NULL)
        return ASMTEST_HW_EINVAL;
    memset(buf, 0, cap);
    struct pt_config config;
    pt_config_init(&config);
    config.begin = buf;
    config.end = buf + cap;
    struct pt_encoder *enc = pt_alloc_encoder(&config);
    if (enc == NULL)
        return ASMTEST_HW_EDECODE;

    /* Emit the taken-jle walk packet by packet; pt_enc_next writes at the encoder's
     * cursor and advances it. Any negative return is an encode error (rc set). */
    struct pt_packet pkt;
    int rc = 0;
    memset(&pkt, 0, sizeof pkt); /* PSB — synchronization point */
    pkt.type = ppt_psb;
    rc |= pt_enc_next(enc, &pkt) < 0;
    memset(&pkt, 0, sizeof pkt); /* MODE.EXEC — declare 64-bit mode */
    pkt.type = ppt_mode;
    pkt.payload.mode.leaf = pt_mol_exec;
    pkt.payload.mode.bits.exec = pt_set_exec_mode(ptem_64bit);
    rc |= pt_enc_next(enc, &pkt) < 0;
    memset(&pkt, 0, sizeof pkt); /* PSBEND — close the PSB header */
    pkt.type = ppt_psbend;
    rc |= pt_enc_next(enc, &pkt) < 0;
    memset(&pkt, 0,
           sizeof pkt); /* TIP.PGE — enable tracing at the first insn */
    pkt.type = ppt_tip_pge;
    pkt.payload.ip.ipc = pt_ipc_full;
    pkt.payload.ip.ip = base_ip;
    rc |= pt_enc_next(enc, &pkt) < 0;
    memset(&pkt, 0, sizeof pkt); /* TNT — one conditional (jle), taken or not */
    pkt.type = ppt_tnt_8;
    pkt.payload.tnt.bit_size = 1;
    pkt.payload.tnt.payload = (taken != 0) ? 1 : 0;
    rc |= pt_enc_next(enc, &pkt) < 0;
    memset(&pkt, 0, sizeof pkt); /* TIP.PGD — disable tracing at the ret */
    pkt.type = ppt_tip_pgd;
    pkt.payload.ip.ipc = pt_ipc_full;
    pkt.payload.ip.ip =
        0; /* out of region: a DISABLE never fetches the target */
    rc |= pt_enc_next(enc, &pkt) < 0;

    uint64_t used = 0;
    if (rc == 0 && pt_enc_get_offset(enc, &used) < 0)
        rc = 1;
    pt_free_encoder(enc);
    if (rc != 0)
        return ASMTEST_HW_EDECODE;
    *out_len = (size_t)used;
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
                             asmtest_trace_t *trace, uint64_t *base_ip_out) {
    (void)aux;
    (void)aux_len;
    (void)img;
    (void)when;
    (void)trace;
    if (base_ip_out != NULL)
        *base_ip_out = 0;
    return ASMTEST_HW_ENOSYS;
}

int asmtest_pt_encode_fixture(uint8_t *buf, size_t cap, uint64_t base_ip,
                              int taken, size_t *out_len) {
    (void)buf;
    (void)cap;
    (void)base_ip;
    (void)taken;
    (void)out_len;
    return ASMTEST_HW_ENOSYS;
}

#endif /* ASMTEST_HAVE_LIBIPT */
