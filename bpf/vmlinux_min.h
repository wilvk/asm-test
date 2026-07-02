/*
 * vmlinux_min.h — minimal fallback for the CO-RE program when /sys/kernel/btf/vmlinux is
 * unavailable at build time (the mk/native-trace.mk rule generates the full vmlinux.h from
 * the running kernel's BTF first, and only falls back to this). It defines the base types,
 * the two syscall-tracepoint context structs codeimage.bpf.c touches, and the BPF UAPI
 * constants/structs/typedefs its map definitions and helper calls need (enum bpf_map_type,
 * the map-update flags, struct bpf_pidns_info, and the __be/__wsum aliases that
 * bpf_helper_defs.h references) — so the BTF-less fallback build compiles. The syscall
 * raw-tracepoint layout (trace_entry; long id; args[]/ret) is a stable ABI, so fixed-offset
 * access is correct even without CO-RE relocations; the enum values below match the kernel
 * UAPI (include/uapi/linux/bpf.h) so libbpf reads the intended map types.
 */
#ifndef __VMLINUX_H__
#define __VMLINUX_H__

typedef unsigned char __u8;
typedef signed char __s8;
typedef unsigned short __u16;
typedef short __s16;
typedef unsigned int __u32;
typedef int __s32;
typedef unsigned long long __u64;
typedef long long __s64;

typedef __u8 u8;
typedef __u16 u16;
typedef __u32 u32;
typedef __u64 u64;

/* Endian/checksum aliases referenced by bpf_helper_defs.h helper prototypes. */
typedef __u16 __le16;
typedef __u16 __be16;
typedef __u32 __le32;
typedef __u32 __be32;
typedef __u16 __sum16;
typedef __u32 __wsum;

#ifndef __ksym
#define __ksym __attribute__((section(".ksyms")))
#endif

/* BPF map types — numeric values match include/uapi/linux/bpf.h so libbpf
 * creates the intended map kinds from the SEC(".maps") definitions. */
enum bpf_map_type {
    BPF_MAP_TYPE_UNSPEC = 0,
    BPF_MAP_TYPE_HASH = 1,
    BPF_MAP_TYPE_ARRAY = 2,
    BPF_MAP_TYPE_PROG_ARRAY = 3,
    BPF_MAP_TYPE_PERF_EVENT_ARRAY = 4,
    BPF_MAP_TYPE_PERCPU_HASH = 5,
    BPF_MAP_TYPE_PERCPU_ARRAY = 6,
    BPF_MAP_TYPE_STACK_TRACE = 7,
    BPF_MAP_TYPE_CGROUP_ARRAY = 8,
    BPF_MAP_TYPE_LRU_HASH = 9,
    BPF_MAP_TYPE_LRU_PERCPU_HASH = 10,
    BPF_MAP_TYPE_LPM_TRIE = 11,
    BPF_MAP_TYPE_ARRAY_OF_MAPS = 12,
    BPF_MAP_TYPE_HASH_OF_MAPS = 13,
    BPF_MAP_TYPE_DEVMAP = 14,
    BPF_MAP_TYPE_SOCKMAP = 15,
    BPF_MAP_TYPE_CPUMAP = 16,
    BPF_MAP_TYPE_XSKMAP = 17,
    BPF_MAP_TYPE_SOCKHASH = 18,
    BPF_MAP_TYPE_CGROUP_STORAGE = 19,
    BPF_MAP_TYPE_REUSEPORT_SOCKARRAY = 20,
    BPF_MAP_TYPE_PERCPU_CGROUP_STORAGE = 21,
    BPF_MAP_TYPE_QUEUE = 22,
    BPF_MAP_TYPE_STACK = 23,
    BPF_MAP_TYPE_SK_STORAGE = 24,
    BPF_MAP_TYPE_DEVMAP_HASH = 25,
    BPF_MAP_TYPE_STRUCT_OPS = 26,
    BPF_MAP_TYPE_RINGBUF = 27,
    BPF_MAP_TYPE_INODE_STORAGE = 28,
    BPF_MAP_TYPE_TASK_STORAGE = 29,
};

/* Flags for bpf_map_update_elem(). */
enum {
    BPF_ANY = 0,
    BPF_NOEXIST = 1,
    BPF_EXIST = 2,
    BPF_F_LOCK = 4,
};

/* Result of bpf_get_ns_current_pid_tgid(). */
struct bpf_pidns_info {
    __u32 pid;
    __u32 tgid;
};

struct trace_entry {
    unsigned short type;
    unsigned char flags;
    unsigned char preempt_count;
    int pid;
};

struct trace_event_raw_sys_enter {
    struct trace_entry ent;
    long int id;
    unsigned long args[6];
    char __data[0];
};

struct trace_event_raw_sys_exit {
    struct trace_entry ent;
    long int id;
    long int ret;
    char __data[0];
};

#endif /* __VMLINUX_H__ */
