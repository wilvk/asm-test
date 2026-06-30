/*
 * codeimage.bpf.c — CO-RE eBPF program: detect JIT code-EMISSION edges for a target PID.
 *
 * SIDEBAND only: it reports WHEN/WHERE new executable code appears (mprotect(PROT_EXEC),
 * mmap(PROT_EXEC), memfd_create) so the userspace recorder (src/codeimage.c) can snapshot
 * the freshly-published bytes on the emission edge instead of polling. It does NOT, and
 * cannot, trace the instruction stream (eBPF is event-driven and the verifier forbids
 * per-instruction following) — hardware/ptrace remains the source of the executed stream.
 *
 * Hooks: classic syscall raw tracepoints (sys_enter_mprotect / sys_enter_mmap +
 * sys_exit_mmap / sys_enter_memfd_create + sys_exit_memfd_create), chosen over kprobes or
 * fentry for ABI stability across kernels 5.8 -> 6.17. The target tgid is filtered
 * in-kernel from a 1-entry config map; events are submitted to a BPF ring buffer the
 * userspace side drains non-blockingly.
 *
 * Built only in the eBPF container lane (clang + libbpf + bpftool); see mk/native-trace.mk.
 */
#include "vmlinux.h"

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "codeimage_event.h"

/* GPL: bpf_ringbuf / tracepoint attach require a GPL-compatible program. */
char LICENSE[] SEC("license") = "GPL";

#define CI_PROT_EXEC 0x4 /* PROT_EXEC (UAPI-stable; not pulled from a header) */

/* Watch config (key 0). The target is identified by its PID NAMESPACE (dev+ino of
 * /proc/<pid>/ns/pid) plus the namespace-local tgid — NOT a bare pid, because
 * bpf_get_current_pid_tgid() returns the GLOBAL (init-ns) tgid, which does not match a
 * container-local getpid(). bpf_get_ns_current_pid_tgid() translates into the target's
 * namespace so the filter is correct on the host AND inside a PID namespace. pid 0 means
 * "watch every process in that namespace". */
struct ci_cfg {
    __u64 dev;
    __u64 ino;
    __u64 pid; /* namespace-local tgid; 0 = all */
};
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct ci_cfg);
} cfg SEC(".maps");

/* Emission events, drained by ring_buffer__poll() in userspace. 1 MiB. */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 20);
} events SEC(".maps");

/* sys_enter_mmap stashes (len, prot) keyed by tid so sys_exit_mmap can emit with the real
 * mapped base (mmap's return value, unknown at enter). */
struct mmap_scratch {
    __u64 len;
};
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, struct mmap_scratch);
} mmap_pending SEC(".maps");

/* sys_enter_memfd_create marks the tid so sys_exit_memfd_create can emit with the fd. */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, __u8);
} memfd_pending SEC(".maps");

static __always_inline int ci_watched(__u32 *tgid_out, __u32 *tid_out) {
    __u32 k = 0;
    struct ci_cfg *c = bpf_map_lookup_elem(&cfg, &k);
    if (!c)
        return 0;
    struct bpf_pidns_info ns = {};
    /* Translate the current task into the target's PID namespace. Fails (non-zero) if the
     * current task is not in that namespace — i.e. not the process we are watching. */
    if (bpf_get_ns_current_pid_tgid(c->dev, c->ino, &ns, sizeof(ns)) != 0)
        return 0;
    if (c->pid != 0 && ns.tgid != (__u32)c->pid)
        return 0;
    *tgid_out = ns.tgid; /* report the namespace-local ids (match userspace getpid()) */
    *tid_out = ns.pid;
    return 1;
}

static __always_inline void ci_emit(__u64 addr, __u64 len, __u32 kind, __s32 fd,
                                    __u32 tgid, __u32 tid) {
    struct asmtest_ci_event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        return; /* ring full — drop, never block */
    e->addr = addr;
    e->len = len;
    e->timestamp = bpf_ktime_get_ns();
    e->pid = tgid;
    e->tid = tid;
    e->kind = kind;
    e->fd = fd;
    bpf_ringbuf_submit(e, 0);
}

SEC("tracepoint/syscalls/sys_enter_mprotect")
int ci_mprotect(struct trace_event_raw_sys_enter *ctx) {
    __u32 tgid, tid;
    if (!ci_watched(&tgid, &tid))
        return 0;
    unsigned long prot = (unsigned long)ctx->args[2];
    if (!(prot & CI_PROT_EXEC))
        return 0;
    ci_emit((__u64)ctx->args[0], (__u64)ctx->args[1], ASMTEST_CI_EV_MPROTECT, -1, tgid, tid);
    return 0;
}

SEC("tracepoint/syscalls/sys_enter_mmap")
int ci_mmap_enter(struct trace_event_raw_sys_enter *ctx) {
    __u32 tgid, tid;
    if (!ci_watched(&tgid, &tid))
        return 0;
    unsigned long prot = (unsigned long)ctx->args[2];
    if (!(prot & CI_PROT_EXEC))
        return 0;
    struct mmap_scratch s = {.len = (__u64)ctx->args[1]};
    bpf_map_update_elem(&mmap_pending, &tid, &s, BPF_ANY);
    return 0;
}

SEC("tracepoint/syscalls/sys_exit_mmap")
int ci_mmap_exit(struct trace_event_raw_sys_exit *ctx) {
    __u32 tgid, tid;
    if (!ci_watched(&tgid, &tid))
        return 0;
    struct mmap_scratch *s = bpf_map_lookup_elem(&mmap_pending, &tid);
    if (!s)
        return 0;
    __u64 len = s->len;
    bpf_map_delete_elem(&mmap_pending, &tid);
    long ret = (long)ctx->ret;
    if (ret < 0)
        return 0; /* mmap failed */
    ci_emit((__u64)ret, len, ASMTEST_CI_EV_MMAP, -1, tgid, tid);
    return 0;
}

SEC("tracepoint/syscalls/sys_enter_memfd_create")
int ci_memfd_enter(struct trace_event_raw_sys_enter *ctx) {
    __u32 tgid, tid;
    if (!ci_watched(&tgid, &tid))
        return 0;
    __u8 one = 1;
    bpf_map_update_elem(&memfd_pending, &tid, &one, BPF_ANY);
    return 0;
}

SEC("tracepoint/syscalls/sys_exit_memfd_create")
int ci_memfd_exit(struct trace_event_raw_sys_exit *ctx) {
    __u32 tgid, tid;
    if (!ci_watched(&tgid, &tid))
        return 0;
    if (!bpf_map_lookup_elem(&memfd_pending, &tid))
        return 0;
    bpf_map_delete_elem(&memfd_pending, &tid);
    long ret = (long)ctx->ret;
    if (ret < 0)
        return 0;
    ci_emit(0, 0, ASMTEST_CI_EV_MEMFD, (__s32)ret, tgid, tid);
    return 0;
}
