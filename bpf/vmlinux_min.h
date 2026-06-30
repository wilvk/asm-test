/*
 * vmlinux_min.h — minimal fallback for the CO-RE program when /sys/kernel/btf/vmlinux is
 * unavailable at build time (the mk/native-trace.mk rule generates the full vmlinux.h from
 * the running kernel's BTF first, and only falls back to this). It defines exactly the
 * base types and the two syscall-tracepoint context structs codeimage.bpf.c touches; the
 * syscall raw-tracepoint layout (trace_entry; long id; args[]/ret) is a stable ABI, so
 * fixed-offset access is correct even without CO-RE relocations.
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

#ifndef __ksym
#define __ksym __attribute__((section(".ksyms")))
#endif

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
