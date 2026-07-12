/*
 * branchsnap.bpf.c — the §D3/AMD-P0 deterministic LBR snapshot program.
 *
 * Attached to PERF_TYPE_BREAKPOINT (hardware execution breakpoint) perf events planted
 * at the traced region's EXITS (P5: the SAME program is attached once per exit
 * breakpoint — up to the 4 x86 debug registers — via N bpf_links; all instances feed
 * the one ringbuf below): when execution reaches a boundary the breakpoint fires, this
 * program runs, and bpf_get_branch_snapshot() reads the frozen branch stack AT THAT
 * DETERMINISTIC POINT — no sample_period=1 flood, no "richest window" guessing. The raw
 * perf_branch_entry array is pushed to a ringbuf the userspace loader drains and hands to
 * the existing amd_replay (which already filters to in-region branches, so any
 * kernel-entry entries are dropped there).
 *
 * GPL: bpf_ringbuf + the snapshot helper require a GPL-compatible program.
 */
#include "vmlinux.h"

#include <bpf/bpf_helpers.h>

#include "branchsnap_event.h"

char LICENSE[] SEC("license") = "GPL";

/* Boundary snapshots, drained by ring_buffer__consume() in snapshot_end(). 256 KiB. */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 18);
} snaps SEC(".maps");

/* F13 — boundary hits DROPPED on ringbuf saturation (bpf_ringbuf_reserve returned
 * NULL). Userspace reads this .bss counter after the drain and flags the trace
 * TRUNCATED on any drop: the lost hit may have been the richest in-region window, so a
 * silent drop could misreport a partial reconstruction as complete. Plain (non-atomic)
 * increment is safe: every breakpoint is a per-thread event on the one armed thread, so
 * program instances never run concurrently. */
__u64 asmtest_bsnap_drops;

SEC("perf_event")
int asmtest_branchsnap(void *ctx) {
    struct bsnap_event *ev = bpf_ringbuf_reserve(&snaps, sizeof(*ev), 0);
    if (!ev) {
        asmtest_bsnap_drops++;
        return 0;
    }
    /* P5: each per-exit bpf_link carries the ABSOLUTE breakpoint address as its attach
     * cookie, so the drain knows WHICH boundary froze this window (0 without cookie
     * support — the drain then skips the boundary-edge completion, never guesses). */
    ev->exit_ip = bpf_get_attach_cookie(ctx);
    /* Fill the raw buffer with the current LBR stack. Returns BYTES written (or a
     * negative errno, e.g. -EOPNOTSUPP where the arch/kernel has no snapshot path). */
    long got = bpf_get_branch_snapshot(ev->raw, sizeof(ev->raw), 0);
    ev->nr = got > 0 ? (__u64)got / BSNAP_ENTRY_SZ : 0;
    bpf_ringbuf_submit(ev, 0);
    return 0;
}
