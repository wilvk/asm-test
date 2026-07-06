/*
 * branchsnap.bpf.c — the §D3/AMD-P0 deterministic LBR snapshot program.
 *
 * Attached to a PERF_TYPE_BREAKPOINT (hardware execution breakpoint) perf event planted
 * at the traced region's EXIT: when execution reaches the boundary the breakpoint fires,
 * this program runs, and bpf_get_branch_snapshot() reads the frozen branch stack AT THAT
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

/* Boundary snapshots, drained by ring_buffer__poll() in userspace. 256 KiB. */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 18);
} snaps SEC(".maps");

SEC("perf_event")
int asmtest_branchsnap(void *ctx) {
    struct bsnap_event *ev = bpf_ringbuf_reserve(&snaps, sizeof(*ev), 0);
    if (!ev)
        return 0;
    /* Fill the raw buffer with the current LBR stack. Returns BYTES written (or a
     * negative errno, e.g. -EOPNOTSUPP where the arch/kernel has no snapshot path). */
    long got = bpf_get_branch_snapshot(ev->raw, sizeof(ev->raw), 0);
    ev->nr = got > 0 ? (__u64)got / BSNAP_ENTRY_SZ : 0;
    bpf_ringbuf_submit(ev, 0);
    return 0;
}
