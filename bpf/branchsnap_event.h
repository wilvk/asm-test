/*
 * branchsnap_event.h — shared layout for the AMD-P0 software-event LBR snapshot.
 * The BPF program (bpf/branchsnap.bpf.c) fills the raw buffer via
 * bpf_get_branch_snapshot(); userspace casts it back to struct perf_branch_entry[].
 * Kept type-free (builtin types only — no <stdint.h>, which the BPF target lacks) so it
 * compiles identically in the BPF program (clang -target bpf) and userspace.
 */
#ifndef ASMTEST_BRANCHSNAP_EVENT_H
#define ASMTEST_BRANCHSNAP_EVENT_H

/* AMD LbrExtV2 is 16-deep; a little header room absorbs kernel-entry entries. */
#define BSNAP_MAX 32
/* sizeof(struct perf_branch_entry): {u64 from; u64 to; u64 flags-bitfield}. */
#define BSNAP_ENTRY_SZ 24u

struct bsnap_event {
    unsigned long long nr;                       /* valid perf_branch_entry count */
    unsigned char raw[BSNAP_MAX * BSNAP_ENTRY_SZ]; /* perf_branch_entry[nr], recast */
};

#endif /* ASMTEST_BRANCHSNAP_EVENT_H */
