/*
 * mach_backend.c — macOS out-of-process single-step native-trace backend (W2, Darwin).
 * See asmtest_mach.h and docs/internal/implementations/macos-oop-mach-stepper.md.
 *
 * XNU cripples BSD ptrace for PC/flag edits (PT_STEP/PT_CONTINUE return ENOTSUP off the
 * `addr == (caddr_t)1` sentinel, by kernel-comment design, to force use of Mach SPIs) —
 * so unlike ptrace_backend.c's Linux tracer, this backend arms EFLAGS.TF through
 * `task_for_pid` + `thread_set_state` and receives each `#DB` as an `EXC_BREAKPOINT`
 * Mach exception message on a dedicated exception port (T2), single-steps through it
 * (T3), and layers trace_call / run_to (T4/T5) on that engine. This translation unit is
 * the T1 skeleton: the platform gate, the five-symbol shape, and one real function
 * (available/skip_reason). The Mach logic itself lands in T2-T5.
 */
#include "asmtest_mach.h"
#include "asmtest_trace.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__x86_64__) && defined(__APPLE__)

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_status.h>
#include <signal.h> /* kill/SIGCONT: lift a job-control SIGSTOP the caller used to park the target */
#include <stdlib.h> /* malloc/free: the owned region-bytes + stream buffers (T3) */

#include "mach_exc.h" /* MIG-generated (T2): the mach_exception_raise* wire types */

/* mach_exc.h declares the request/reply wire types and the catch_* callbacks it
 * expects us to define, but NOT the dispatcher itself — mach_excServer.c defines
 * mach_exc_server with external linkage (mig_external expands to extern) and no
 * matching prototype ships in the generated header, so this backend declares it. */
extern boolean_t mach_exc_server(mach_msg_header_t *InHeadP,
                                 mach_msg_header_t *OutHeadP);

/* Capture stream capacity — same bound as the Linux out-of-process tracer
 * (ptrace_backend.c's PTRACE_STREAM_CAP), so the two backends truncate at the same
 * trace size. */
#define MACH_STREAM_CAP (1u << 16)

/* ================================================================= */
/* T2 — task_for_pid + Mach exception-port receive loop.             */
/* ================================================================= */

/* What the receive loop is currently doing, read by catch_mach_exception_raise (the
 * MIG callback signature is fixed, so all state rides on this file-scope context —
 * single active trace at a time, exactly like the in-process stepper's MVP). */
typedef enum {
    MACH_MODE_IDLE = 0,
    MACH_MODE_STEP,  /* T3: TF armed; record in-region RIPs from each #DB */
    MACH_MODE_BREAK, /* T3/T5: an int3 planted at `target`; wait for that one hit */
} mach_mode_t;

typedef struct {
    mach_mode_t mode;
    task_t task;
    thread_act_t thread;
    mach_port_t exc_port_active; /* the port mach_receive_loop/_one read from */

    /* MACH_MODE_STEP */
    uint64_t base_ip;
    uint64_t region_len;
    uint64_t *stream;
    uint32_t stream_len;
    uint32_t stream_cap;
    int overflow;
    int entered;
    uint64_t last_rax;

    /* MACH_MODE_BREAK */
    uint64_t target;
    int hit;

    /* shared */
    int done;         /* catch_* sets this to end mach_receive_loop */
    kern_return_t err; /* KERN_SUCCESS unless catch_* hit an internal Mach failure */
} mach_trace_ctx_t;

static mach_trace_ctx_t g_mach_ctx;

/* task_for_pid failures are overwhelmingly the entitlement/root gate (KERN_FAILURE)
 * or a bad pid (KERN_INVALID_ARGUMENT); surface both as the soft EPERM gate per
 * asmtest_mach.h, everything else as ETRACE. */
static int mach_status_from_kr(kern_return_t kr) {
    if (kr == KERN_SUCCESS)
        return ASMTEST_MACH_OK;
    if (kr == KERN_FAILURE || kr == KERN_INVALID_ARGUMENT)
        return ASMTEST_MACH_EPERM;
    return ASMTEST_MACH_ETRACE;
}

static kern_return_t mach_get_task(pid_t pid, task_t *out) {
    return task_for_pid(mach_task_self(), pid, out);
}

/* The task's exception-port registration for EXC_MASK_BREAKPOINT as it was BEFORE we
 * overwrote it, so a long-lived target (a real JVM/CoreCLR/Node process — the whole
 * point of this backend) comes back exactly as we found it once tracing ends, rather
 * than being left with TF stuck on and a port pointing at a receive right we've since
 * deallocated. Sized to EXC_TYPES_COUNT per Apple's own task_get_exception_ports
 * examples, though EXC_MASK_BREAKPOINT (one bit) can only ever fill one entry. */
typedef struct {
    exception_mask_t masks[EXC_TYPES_COUNT];
    mach_port_t ports[EXC_TYPES_COUNT];
    exception_behavior_t behaviors[EXC_TYPES_COUNT];
    thread_state_flavor_t flavors[EXC_TYPES_COUNT];
    mach_msg_type_number_t count;
} mach_saved_ports_t;

/* Allocate a receive port, grant it a send right (task_set_exception_ports needs one
 * to hand to the kernel), save the task's CURRENT EXC_MASK_BREAKPOINT registration
 * into *saved, then register our port for the 64-bit mach_exc_* message variants
 * (MACH_EXCEPTION_CODES). */
static kern_return_t mach_setup_exception_port(task_t task,
                                               mach_port_t *exc_port_out,
                                               mach_saved_ports_t *saved) {
    saved->count = EXC_TYPES_COUNT;
    if (task_get_exception_ports(task, EXC_MASK_BREAKPOINT, saved->masks,
                                 &saved->count, saved->ports, saved->behaviors,
                                 saved->flavors) != KERN_SUCCESS)
        saved->count = 0; /* best-effort: proceed with nothing to restore later */

    mach_port_t exc_port = MACH_PORT_NULL;
    kern_return_t kr =
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &exc_port);
    if (kr != KERN_SUCCESS)
        return kr;
    kr = mach_port_insert_right(mach_task_self(), exc_port, exc_port,
                                MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), exc_port);
        return kr;
    }
    kr = task_set_exception_ports(task, EXC_MASK_BREAKPOINT, exc_port,
                                  EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES,
                                  THREAD_STATE_NONE);
    if (kr != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), exc_port);
        return kr;
    }
    *exc_port_out = exc_port;
    return KERN_SUCCESS;
}

/* Hand the task's exception-port registration back exactly as mach_setup_exception_port
 * found it (a no-op array when nothing was registered before), then release our own
 * receive right. task_set_exception_ports copies the port argument (COPY_SEND), so the
 * saved send rights task_get_exception_ports handed us are ours to release after. */
static void mach_teardown_exception_port(task_t task, mach_port_t exc_port,
                                         const mach_saved_ports_t *saved) {
    for (mach_msg_type_number_t i = 0; i < saved->count; i++) {
        task_set_exception_ports(task, saved->masks[i], saved->ports[i],
                                 saved->behaviors[i], saved->flavors[i]);
        if (saved->ports[i] != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), saved->ports[i]);
    }
    if (exc_port != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), exc_port);
}

/* Receive and dispatch exactly one message on exc_port through the MIG-generated
 * mach_exc_server, replying with whatever catch_mach_exception_raise* returned.
 * Returning KERN_SUCCESS in that reply is what resumes the faulting thread — the
 * whole stop/continue rhythm rides on this one round trip. */
static kern_return_t mach_receive_one(mach_port_t exc_port) {
    union {
        mach_msg_header_t head;
        char pad[sizeof(mach_msg_header_t) + 512];
    } req, rep;

    kern_return_t kr = mach_msg(&req.head, MACH_RCV_MSG, 0, sizeof req, exc_port,
                                MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS)
        return kr;

    if (!mach_exc_server(&req.head, &rep.head))
        return KERN_FAILURE;

    return mach_msg(&rep.head, MACH_SEND_MSG, rep.head.msgh_size, 0, MACH_PORT_NULL,
                    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

/* Drive mach_receive_one until the active catch_* callback sets g_mach_ctx.done (a
 * trace/breakpoint completed) or a Mach call fails. The single-step policy that
 * decides WHEN to set `done` lives entirely in catch_mach_exception_raise below —
 * this loop is pure plumbing, unchanged by T3's policy fill-in. */
static kern_return_t mach_receive_loop(void) {
    g_mach_ctx.done = 0;
    g_mach_ctx.err = KERN_SUCCESS;
    kern_return_t kr;
    do {
        kr = mach_receive_one(g_mach_ctx.exc_port_active);
    } while (kr == KERN_SUCCESS && !g_mach_ctx.done);
    if (kr != KERN_SUCCESS)
        return kr;
    return g_mach_ctx.err;
}

/* ================================================================= */
/* Register access (thread_get_state/thread_set_state, x86_THREAD_STATE64) —  */
/* the remote analog of ss_backend.c's SS_RIP/SS_SET_TF ucontext shims.        */
/* ================================================================= */

#define MACH_TF 0x100ULL /* EFL_TF, RFLAGS bit 8 — identical to ss_backend.c's SS_TF */

static kern_return_t mach_get_regs(thread_act_t thread,
                                   x86_thread_state64_t *out) {
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    return thread_get_state(thread, x86_THREAD_STATE64, (thread_state_t)out,
                            &count);
}

static kern_return_t mach_set_regs(thread_act_t thread,
                                   const x86_thread_state64_t *in) {
    return thread_set_state(thread, x86_THREAD_STATE64, (thread_state_t)in,
                            x86_THREAD_STATE64_COUNT);
}

static kern_return_t mach_arm_tf(thread_act_t thread) {
    x86_thread_state64_t s;
    kern_return_t kr = mach_get_regs(thread, &s);
    if (kr != KERN_SUCCESS)
        return kr;
    s.__rflags |= MACH_TF;
    return mach_set_regs(thread, &s);
}

static kern_return_t mach_clear_tf(thread_act_t thread) {
    x86_thread_state64_t s;
    kern_return_t kr = mach_get_regs(thread, &s);
    if (kr != KERN_SUCCESS)
        return kr;
    s.__rflags &= ~MACH_TF;
    return mach_set_regs(thread, &s);
}

/* ================================================================= */
/* catch_mach_exception_raise* — the MIG server's fixed callback signatures.   */
/* T2 supplies the plumbing (mode dispatch, resume/park); T3 fills in the      */
/* STEP policy (record in-region RIPs); BREAK (run_until) also lands here     */
/* since both share the one active-trace context and receive loop.            */
/* ================================================================= */

/* Leave the faulting thread genuinely parked (not merely "exception resolved"):
 * bump its Mach suspend count before we reply KERN_SUCCESS below, so it does not
 * actually run again until a later thread_resume (mirrors how a ptrace-stopped
 * tracee stays stopped between calls). */
static void mach_park_thread(thread_act_t thread) { thread_suspend(thread); }

kern_return_t catch_mach_exception_raise(mach_port_t exception_port,
                                         mach_port_t thread, mach_port_t task,
                                         exception_type_t exception,
                                         mach_exception_data_t code,
                                         mach_msg_type_number_t codeCnt) {
    (void)exception_port;
    (void)task;
    g_mach_ctx.thread = thread;

    if (exception != EXC_BREAKPOINT || codeCnt < 1) {
        g_mach_ctx.err = KERN_FAILURE;
        g_mach_ctx.done = 1;
        mach_park_thread(thread);
        return KERN_SUCCESS;
    }
    int64_t subcode = code[0]; /* EXC_I386_SGL=1 (single-step #DB), EXC_I386_BPT=2 (int3) */

    if (g_mach_ctx.mode == MACH_MODE_BREAK) {
        if (subcode != 2 /* EXC_I386_BPT */) {
            mach_park_thread(thread);
            return KERN_SUCCESS; /* not our breakpoint; ignore and keep the loop open */
        }
        x86_thread_state64_t s;
        if (mach_get_regs(thread, &s) != KERN_SUCCESS) {
            g_mach_ctx.err = KERN_FAILURE;
            g_mach_ctx.done = 1;
            mach_park_thread(thread);
            return KERN_SUCCESS;
        }
        /* int3 traps ONE byte past the hit (like x86 ptrace); rewind to the planted
         * address, matching run_until's ptrace twin exactly. */
        s.__rip = g_mach_ctx.target;
        if (mach_set_regs(thread, &s) != KERN_SUCCESS)
            g_mach_ctx.err = KERN_FAILURE;
        g_mach_ctx.hit = 1;
        g_mach_ctx.done = 1;
        mach_park_thread(thread);
        return KERN_SUCCESS;
    }

    /* MACH_MODE_STEP: subcode must be the single-step trap; anything else (e.g. a
     * stray int3 the traced code itself executed) ends the trace honestly rather
     * than mis-recording it as a step. */
    if (subcode != 1 /* EXC_I386_SGL */) {
        g_mach_ctx.err = KERN_FAILURE;
        g_mach_ctx.done = 1;
        mach_park_thread(thread);
        return KERN_SUCCESS;
    }

    x86_thread_state64_t s;
    if (mach_get_regs(thread, &s) != KERN_SUCCESS) {
        g_mach_ctx.err = KERN_FAILURE;
        g_mach_ctx.done = 1;
        mach_park_thread(thread);
        return KERN_SUCCESS;
    }
    uint64_t rip = s.__rip;

    if (rip >= g_mach_ctx.base_ip && rip < g_mach_ctx.base_ip + g_mach_ctx.region_len) {
        g_mach_ctx.entered = 1;
        if (g_mach_ctx.stream_len < g_mach_ctx.stream_cap)
            g_mach_ctx.stream[g_mach_ctx.stream_len++] = rip - g_mach_ctx.base_ip;
        else
            g_mach_ctx.overflow = 1;
        /* Re-assert TF so the reply resumes stepping (in-region or a stepped-over
         * callee's single instructions before mach_run_until takes over for a
         * detected call-out — see asmtest_mach_trace_attached). */
        s.__rflags |= MACH_TF;
        if (mach_set_regs(thread, &s) != KERN_SUCCESS)
            g_mach_ctx.err = KERN_FAILURE;
        return KERN_SUCCESS;
    }

    if (!g_mach_ctx.entered) {
        /* Still in call-setup glue before reaching the region (e.g. the fork/PLT
         * path to a fork-and-trace blob's entry) — keep stepping silently, exactly
         * as the Linux out-of-process tracer's pre-entry steps are un-recorded. */
        s.__rflags |= MACH_TF;
        if (mach_set_regs(thread, &s) != KERN_SUCCESS) {
            g_mach_ctx.err = KERN_FAILURE;
            g_mach_ctx.done = 1;
            mach_park_thread(thread);
        }
        return KERN_SUCCESS;
    }

    /* Entered, now stepped out: stop the loop here and let
     * asmtest_mach_trace_attached classify the exit (return vs. call-out) — it owns
     * the region-bytes buffer classify_region_exit-equivalent needs, which this MIG
     * callback does not have access to. */
    g_mach_ctx.last_rax = s.__rax;
    g_mach_ctx.done = 1;
    mach_park_thread(thread);
    return KERN_SUCCESS;
}

/* Never configured: task_set_exception_ports above requests EXCEPTION_DEFAULT, not
 * EXCEPTION_STATE(_IDENTITY), so the kernel never sends this message shape. MIG's
 * dispatcher still needs the symbol to link (it is generated for all three routines
 * mach_exc.defs declares); reaching this in practice is an unexpected-behavior bug,
 * not a normal path, hence the unconditional failure. */
kern_return_t catch_mach_exception_raise_state(
    mach_port_t exception_port, exception_type_t exception,
    const mach_exception_data_t code, mach_msg_type_number_t codeCnt, int *flavor,
    const thread_state_t old_state, mach_msg_type_number_t old_stateCnt,
    thread_state_t new_state, mach_msg_type_number_t *new_stateCnt) {
    (void)exception_port;
    (void)exception;
    (void)code;
    (void)codeCnt;
    (void)flavor;
    (void)old_state;
    (void)old_stateCnt;
    (void)new_state;
    (void)new_stateCnt;
    return KERN_FAILURE;
}

kern_return_t catch_mach_exception_raise_state_identity(
    mach_port_t exception_port, mach_port_t thread, mach_port_t task,
    exception_type_t exception, mach_exception_data_t code,
    mach_msg_type_number_t codeCnt, int *flavor, thread_state_t old_state,
    mach_msg_type_number_t old_stateCnt, thread_state_t new_state,
    mach_msg_type_number_t *new_stateCnt) {
    (void)exception_port;
    (void)thread;
    (void)task;
    (void)exception;
    (void)code;
    (void)codeCnt;
    (void)flavor;
    (void)old_state;
    (void)old_stateCnt;
    (void)new_state;
    (void)new_stateCnt;
    return KERN_FAILURE;
}

/* ================================================================= */
/* T3 — trace_attached single-step engine.                           */
/* ================================================================= */

/* mach_run_until: the Darwin analog of ptrace_backend.c's run_until (software-int3
 * arm only; T5 adds the x86_DEBUG_STATE64 hardware-breakpoint W^X fallback this same
 * seam uses). Plants 0xCC at `target` in the task's memory, resumes the thread
 * through the exception-port loop until THAT breakpoint is hit, restores the
 * original byte, and rewinds RIP back to `target` — leaving the thread parked
 * exactly there (see mach_park_thread). Returns ASMTEST_MACH_OK, ASMTEST_MACH_ENOENT
 * (the task is gone), or ASMTEST_MACH_ETRACE. */
static int mach_run_until(task_t task, thread_act_t thread, mach_port_t exc_port,
                          uint64_t target) {
    mach_vm_address_t page = (mach_vm_address_t)target;
    vm_offset_t orig_data = 0;
    mach_msg_type_number_t orig_cnt = 0;
    kern_return_t kr =
        mach_vm_read(task, page, 1, &orig_data, &orig_cnt);
    if (kr != KERN_SUCCESS)
        return ASMTEST_MACH_ETRACE;
    uint8_t orig_byte = *(uint8_t *)orig_data;
    mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)orig_data, orig_cnt);

    kr = mach_vm_protect(task, page, 1, FALSE,
                         VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS)
        return ASMTEST_MACH_ETRACE; /* T5 adds the hardware-breakpoint fallback here */
    uint8_t bp = 0xCC;
    kr = mach_vm_write(task, page, (vm_offset_t)&bp, 1);
    if (kr != KERN_SUCCESS)
        return ASMTEST_MACH_ETRACE;

    g_mach_ctx.mode = MACH_MODE_BREAK;
    g_mach_ctx.task = task;
    g_mach_ctx.thread = thread;
    g_mach_ctx.target = target;
    g_mach_ctx.hit = 0;
    g_mach_ctx.exc_port_active = exc_port;

    thread_resume(thread); /* counter mach_park_thread from whatever stopped it last */
    kr = mach_receive_loop();

    mach_vm_write(task, page, (vm_offset_t)&orig_byte, 1); /* best-effort restore */

    if (kr != KERN_SUCCESS)
        return ASMTEST_MACH_ETRACE;
    return g_mach_ctx.hit ? ASMTEST_MACH_OK : ASMTEST_MACH_ENOENT;
}

typedef enum {
    MACH_EXIT_RETURNED,
    MACH_EXIT_CALLOUT_RESUMED,
    MACH_EXIT_CALLOUT_LOST,
} mach_exit_kind_t;

/* Darwin twin of ptrace_backend.c's classify_region_exit: decide whether stepping
 * out of [base,len) is the routine's own return, or a call-out to a helper whose
 * return address still lands back in the region (stepped over at native speed via
 * mach_run_until, recording resumes). Needs Capstone's is-call query; without it
 * every exit reads as a return (leaf-only). */
static mach_exit_kind_t mach_classify_region_exit(task_t task, thread_act_t thread,
                                                   mach_port_t exc_port,
                                                   const uint8_t *code, size_t len,
                                                   uint64_t base_ip,
                                                   uint64_t last_off,
                                                   uint64_t *resume_off) {
    if (!asmtest_disas_available() ||
        !asmtest_disas_is_call(ASMTEST_ARCH_X86_64, code, len, last_off))
        return MACH_EXIT_RETURNED;
    size_t cl =
        asmtest_disas(ASMTEST_ARCH_X86_64, code, len, base_ip, last_off, NULL, 0);
    uint64_t ret_off = last_off + cl;
    if (cl == 0 || ret_off >= len)
        return MACH_EXIT_RETURNED;
    if (mach_run_until(task, thread, exc_port, base_ip + ret_off) != ASMTEST_MACH_OK)
        return MACH_EXIT_CALLOUT_LOST;
    *resume_off = ret_off;
    return MACH_EXIT_CALLOUT_RESUMED;
}

/* Darwin twin of ptrace_backend.c's normalize: block boundaries are region entry and
 * any recorded offset that is not immediately-previous-offset + previous-insn-length
 * (matches PT/DR/Unicorn/ss_backend's partition). */
static void mach_normalize(asmtest_trace_t *t, const uint8_t *base,
                           uint64_t base_ip, size_t len, const uint64_t *stream,
                           uint32_t n, int overflow) {
    if (t == NULL)
        return;
    int have_prev = 0;
    uint64_t expected_next = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t off = stream[i];
        if (!have_prev || off != expected_next)
            trace_append_block(t, off);
        trace_append_insn(t, off);
        size_t l = asmtest_disas(ASMTEST_ARCH_X86_64, base, len, base_ip, off, NULL, 0);
        if (l == 0) {
            t->truncated = true;
            return;
        }
        expected_next = off + l;
        have_prev = 1;
    }
    if (overflow)
        t->truncated = true;
}

int asmtest_mach_trace_attached(pid_t pid, const void *base, size_t len,
                                long *result, asmtest_trace_t *trace) {
    if (base == NULL || len == 0 || trace == NULL)
        return ASMTEST_MACH_EINVAL;

    task_t task = MACH_PORT_NULL;
    kern_return_t kr = mach_get_task(pid, &task);
    if (kr != KERN_SUCCESS)
        return mach_status_from_kr(kr);

    thread_act_array_t threads = NULL;
    mach_msg_type_number_t nthreads = 0;
    kr = task_threads(task, &threads, &nthreads);
    if (kr != KERN_SUCCESS || nthreads < 1) {
        mach_port_deallocate(mach_task_self(), task);
        return ASMTEST_MACH_ETRACE;
    }
    thread_act_t thread = threads[0];
    for (mach_msg_type_number_t i = 1; i < nthreads; i++)
        mach_port_deallocate(mach_task_self(), threads[i]);
    mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)threads,
                      nthreads * sizeof(thread_act_t));

    mach_port_t exc_port = MACH_PORT_NULL;
    mach_saved_ports_t saved_ports;
    kr = mach_setup_exception_port(task, &exc_port, &saved_ports);
    if (kr != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), thread);
        mach_port_deallocate(mach_task_self(), task);
        return mach_status_from_kr(kr);
    }

    uint8_t *owned = (uint8_t *)malloc(len);
    uint64_t *stream = (uint64_t *)malloc((size_t)MACH_STREAM_CAP * sizeof(uint64_t));
    int rc = ASMTEST_MACH_OK;
    uint32_t n = 0;
    int overflow = 0, entered = 0, returned = 0;
    uint64_t retval = 0;
    const uint64_t base_ip = (uint64_t)(uintptr_t)base;

    if (owned == NULL || stream == NULL) {
        rc = ASMTEST_MACH_ETRACE;
        goto out;
    }
    {
        mach_vm_size_t got = 0;
        vm_offset_t data = 0;
        mach_msg_type_number_t data_cnt = 0;
        kr = mach_vm_read(task, (mach_vm_address_t)base_ip, (mach_vm_size_t)len,
                          &data, &data_cnt);
        if (kr != KERN_SUCCESS || data_cnt < len) {
            if (kr == KERN_SUCCESS)
                mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)data,
                                  data_cnt);
            rc = ASMTEST_MACH_ETRACE;
            goto out;
        }
        got = data_cnt;
        memcpy(owned, (void *)data, len);
        mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)data, got);
    }

    /* Establish a KNOWN park under our own thread-suspend count before touching the
     * BSD stop state, so there is no window where the target could run uncontrolled
     * between lifting a SIGSTOP and arming TF below. thread_resume (after TF is
     * armed) cancels exactly this suspend — net zero — it exists only to bridge
     * safely from "unknown incoming stop mechanism" to "known running". */
    thread_suspend(thread);
    kill(pid, SIGCONT); /* lift a job-control SIGSTOP the caller used to park the
                         * target (asmtest_mach_run_to / a raw kill(pid, SIGSTOP));
                         * harmless if it was not stopped that way. */

    g_mach_ctx.mode = MACH_MODE_STEP;
    g_mach_ctx.task = task;
    g_mach_ctx.thread = thread;
    g_mach_ctx.base_ip = base_ip;
    g_mach_ctx.region_len = len;
    g_mach_ctx.stream = stream;
    g_mach_ctx.stream_len = 0;
    g_mach_ctx.stream_cap = MACH_STREAM_CAP;
    g_mach_ctx.overflow = 0;
    g_mach_ctx.entered = 0;
    g_mach_ctx.exc_port_active = exc_port;

    /* Record the PC BEFORE the first step iff the caller left the target stopped
     * EXACTLY at region entry (asmtest_mach_run_to's contract): the loop below only
     * records the RIP AFTER each step, so offset 0 would otherwise never be
     * captured. A before-the-region start (call-setup glue still ahead) leaves this
     * a no-op, matching the Linux out-of-process tracer's identical pre-loop check. */
    {
        x86_thread_state64_t s0;
        if (mach_get_regs(thread, &s0) == KERN_SUCCESS && s0.__rip >= base_ip &&
            s0.__rip < base_ip + len) {
            g_mach_ctx.entered = 1;
            stream[g_mach_ctx.stream_len++] = s0.__rip - base_ip;
        }
    }

    kr = mach_arm_tf(thread);
    if (kr != KERN_SUCCESS) {
        rc = ASMTEST_MACH_ETRACE;
        goto out;
    }
    thread_resume(thread); /* counter the caller's task_suspend/SIGSTOP park */

    for (;;) {
        kr = mach_receive_loop();
        if (kr != KERN_SUCCESS) {
            rc = ASMTEST_MACH_ETRACE;
            break;
        }
        n = g_mach_ctx.stream_len;
        overflow = g_mach_ctx.overflow;
        entered = g_mach_ctx.entered;

        if (g_mach_ctx.err != KERN_SUCCESS && !entered) {
            /* Never reached the region at all (or a non-#DB/#BP exception fired
             * before entry) — nothing to classify; report it and stop. */
            rc = ASMTEST_MACH_ETRACE;
            break;
        }
        /* g_mach_ctx.err == KERN_SUCCESS and entered: the step that ended the
         * receive loop landed OUT of region — classify it. (catch_* only sets done
         * without an error on exactly that condition; any other exit already broke
         * out above via the err-and-not-entered check.) */
        if (g_mach_ctx.err == KERN_SUCCESS && entered) {
            uint64_t resume_off = 0;
            mach_exit_kind_t k = mach_classify_region_exit(
                task, thread, exc_port, owned, len, base_ip, stream[n - 1],
                &resume_off);
            if (k == MACH_EXIT_CALLOUT_RESUMED) {
                if (n < MACH_STREAM_CAP)
                    stream[n++] = resume_off;
                else
                    overflow = 1;
                g_mach_ctx.stream_len = n;
                g_mach_ctx.overflow = overflow;
                /* mach_run_until (inside classify) left mode == MACH_MODE_BREAK for
                 * its own breakpoint wait; switch back to single-step recording
                 * before the next receive_loop iteration. */
                g_mach_ctx.mode = MACH_MODE_STEP;
                g_mach_ctx.exc_port_active = exc_port;
                kr = mach_arm_tf(thread);
                if (kr != KERN_SUCCESS) {
                    rc = ASMTEST_MACH_ETRACE;
                    break;
                }
                thread_resume(thread); /* counter mach_run_until's park-on-hit */
                continue;
            }
            if (k == MACH_EXIT_CALLOUT_LOST) {
                overflow = 1;
                break;
            }
            retval = g_mach_ctx.last_rax;
            returned = 1;
            break;
        }
        rc = ASMTEST_MACH_ETRACE;
        break;
    }

    if (rc == ASMTEST_MACH_OK) {
        if (result != NULL && returned)
            *result = (long)retval;
        mach_normalize(trace, owned, base_ip, len, stream, n, overflow);
    }

out:
    /* Leave the target stopped past the region for the caller (the Linux
     * out-of-process tracer's identical postcondition): the terminal catch_*
     * callback already parked it via mach_park_thread, so no thread_resume here —
     * only clear TF so a LATER resume (by the caller, or a future trace_attached
     * call) does not immediately retrap on an instruction nobody armed on purpose. */
    if (thread != MACH_PORT_NULL)
        mach_clear_tf(thread);
    mach_teardown_exception_port(task, exc_port, &saved_ports);
    if (thread != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), thread);
    if (task != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), task);
    free(owned);
    free(stream);
    return rc;
}

/* asmtest_disas_available() gates block normalization (Capstone length-decoder), the
 * same dependency ss_backend.c's in-process stepper uses — see asmtest_mach.h. */
int asmtest_mach_available(void) { return asmtest_disas_available() ? 1 : 0; }

void asmtest_mach_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *msg = asmtest_mach_available()
                          ? "available"
                          : "built without Capstone (mach block normalization)";
    strncpy(buf, msg, buflen - 1);
    buf[buflen - 1] = '\0';
}

/* TODO(T4): fork-and-trace a self-contained blob through the T3 engine. */
int asmtest_mach_trace_call(const void *code, size_t len, const long *args,
                            int nargs, long *result, asmtest_trace_t *trace) {
    (void)code;
    (void)len;
    (void)args;
    (void)nargs;
    (void)result;
    (void)trace;
    return ASMTEST_MACH_ENOSYS;
}

/* TODO(T5): expose mach_run_until publicly + add the x86_DEBUG_STATE64 W^X fallback. */
int asmtest_mach_run_to(pid_t pid, const void *addr) {
    (void)pid;
    (void)addr;
    return ASMTEST_MACH_ENOSYS;
}

#else /* not x86-64 Darwin: harmless no-op everywhere else (Linux, Apple Silicon). */

int asmtest_mach_available(void) { return 0; }

void asmtest_mach_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *msg = "mach stepper is x86-64 macOS only";
    strncpy(buf, msg, buflen - 1);
    buf[buflen - 1] = '\0';
}

int asmtest_mach_trace_call(const void *code, size_t len, const long *args,
                            int nargs, long *result, asmtest_trace_t *trace) {
    (void)code;
    (void)len;
    (void)args;
    (void)nargs;
    (void)result;
    (void)trace;
    return ASMTEST_MACH_ENOSYS;
}

int asmtest_mach_trace_attached(pid_t pid, const void *base, size_t len,
                                long *result, asmtest_trace_t *trace) {
    (void)pid;
    (void)base;
    (void)len;
    (void)result;
    (void)trace;
    return ASMTEST_MACH_ENOSYS;
}

int asmtest_mach_run_to(pid_t pid, const void *addr) {
    (void)pid;
    (void)addr;
    return ASMTEST_MACH_ENOSYS;
}

#endif
