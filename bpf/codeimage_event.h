/*
 * codeimage_event.h — the code-emission event record shared between the eBPF program
 * (bpf/codeimage.bpf.c, which includes vmlinux.h for __u64/__s32) and the userspace
 * recorder (src/codeimage.c). The userspace public mirror is asmtest_codeimage_event_t in
 * include/asmtest_codeimage.h; src/codeimage.c carries a _Static_assert that the two have
 * the same size. KEEP THE THREE IN SYNC.
 *
 * Fixed-width, naturally aligned, no implicit padding: 8+8+8+4+4+4+4 = 40 bytes.
 */
#ifndef ASMTEST_CODEIMAGE_EVENT_H
#define ASMTEST_CODEIMAGE_EVENT_H

/* Mirror of the ASMTEST_CI_KIND_* values in asmtest_codeimage.h. */
#define ASMTEST_CI_EV_MPROTECT 1 /* mprotect(...PROT_EXEC...)              */
#define ASMTEST_CI_EV_MMAP 2     /* mmap(...PROT_EXEC...); addr from exit  */
#define ASMTEST_CI_EV_MEMFD 3    /* memfd_create; fd from exit            */

struct asmtest_ci_event {
    __u64 addr;      /* published base address (0 for a memfd hint) */
    __u64 len;       /* byte length (0 for a memfd hint)            */
    __u64 timestamp; /* bpf_ktime_get_ns() at emission              */
    __u32 pid;       /* tgid that published                         */
    __u32 tid;       /* thread that published                       */
    __u32 kind;      /* ASMTEST_CI_EV_*                             */
    __s32 fd;        /* memfd fd, or -1                             */
};

#endif /* ASMTEST_CODEIMAGE_EVENT_H */
