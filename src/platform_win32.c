/* platform_win32.c — Win32 implementations of the POSIX primitives the runner
 * relies on, for the native Win64 tier (see docs/plans/win64-native-tier-plan.md,
 * Phase 4). Kept separate from src/asmtest.c (which stays POSIX-only) so the
 * working {x86-64, AArch64} x {Linux, macOS} build is untouched; this file is
 * compiled only for the Win64 target and provides the same exported symbols.
 *
 * Ported so far:
 *   - the guard-page allocator (mmap + mprotect(PROT_NONE) -> VirtualAlloc +
 *     VirtualProtect(PAGE_NOACCESS));
 *   - isolated test execution with crash containment + timeout (fork + waitpid +
 *     alarm -> CreateProcess + WaitForSingleObject + TerminateProcess).
 *
 * Still POSIX-only in src/asmtest.c (later Phase 4 slices): the parallel `-jN`
 * pool (WaitForMultipleObjects over the per-test children), in-process
 * crash-to-failure for `--no-fork` (signals -> SEH/vectored handler), and the
 * --filter glob (fnmatch).
 */
#if defined(_WIN32)

#include <stddef.h>
#include <stdio.h>
#include <windows.h>

#include "asmtest.h"
#include "platform_win32.h"

static size_t win32_page_size(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t)si.dwPageSize;
}

static size_t round_up_page(size_t n, size_t pg) {
    size_t usable = ((n + pg - 1) / pg) * pg;
    return usable == 0 ? pg : usable;
}

/* A buffer whose last byte abuts a trailing PAGE_NOACCESS guard page, so a
 * one-past-the-end write faults. Mirrors the POSIX asmtest_guarded_alloc. */
void *asmtest_guarded_alloc(size_t n) {
    size_t pg = win32_page_size();
    size_t usable = round_up_page(n, pg);
    size_t total = usable + pg;
    unsigned char *base = (unsigned char *)VirtualAlloc(
        NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (base == NULL)
        return NULL;
    DWORD old;
    if (!VirtualProtect(base + usable, pg, PAGE_NOACCESS, &old)) {
        VirtualFree(base, 0, MEM_RELEASE);
        return NULL;
    }
    /* Place the buffer so its last byte abuts the guard page. */
    return base + (usable - n);
}

void asmtest_guarded_free(void *p, size_t n) {
    if (p == NULL)
        return;
    size_t pg = win32_page_size();
    size_t usable = round_up_page(n, pg);
    unsigned char *base = (unsigned char *)p - (usable - n);
    VirtualFree(base, 0, MEM_RELEASE); /* releases the whole reservation */
}

/* A buffer preceded by a leading PAGE_NOACCESS guard page, so a one-before-the
 * -start read/write (underrun) faults. Mirrors asmtest_guarded_alloc_under. */
void *asmtest_guarded_alloc_under(size_t n) {
    size_t pg = win32_page_size();
    size_t usable = round_up_page(n, pg);
    size_t total = pg + usable;
    unsigned char *base = (unsigned char *)VirtualAlloc(
        NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (base == NULL)
        return NULL;
    DWORD old;
    if (!VirtualProtect(base, pg, PAGE_NOACCESS, &old)) { /* leading guard */
        VirtualFree(base, 0, MEM_RELEASE);
        return NULL;
    }
    /* Buffer starts right after the guard page, so buf[-1] faults. */
    return base + pg;
}

void asmtest_guarded_free_under(void *p, size_t n) {
    if (p == NULL)
        return;
    size_t pg = win32_page_size();
    unsigned char *base = (unsigned char *)p - pg;
    (void)n;
    VirtualFree(base, 0, MEM_RELEASE);
}

/* ------------------------------------------------------------------ */
/* Isolated execution: crash containment + per-test timeout            */
/* ------------------------------------------------------------------ */

/* A process that dies from an unhandled hardware exception exits with the
 * NTSTATUS exception code (e.g. 0xC0000005 for an access violation), which lives
 * in the error-severity range. Distinguish those from ordinary exit codes. */
static int is_crash_code(DWORD code) { return code >= 0xC0000000u; }

asmtest_win32_run_t asmtest_win32_run(const char *cmdline, unsigned timeout_ms,
                                      unsigned long *exit_code) {
    /* CreateProcessA may modify its command-line buffer, so copy it. */
    char buf[2048];
    size_t i = 0;
    for (; cmdline[i] != '\0' && i + 1 < sizeof buf; i++)
        buf[i] = cmdline[i];
    buf[i] = '\0';

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);

    if (!CreateProcessA(NULL, buf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return ASMTEST_W32_SPAWN_FAIL;

    DWORD waited = WaitForSingleObject(pi.hProcess,
                                       timeout_ms == 0 ? INFINITE : timeout_ms);

    asmtest_win32_run_t result;
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE); /* reap */
        result = ASMTEST_W32_TIMEOUT;
    } else {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        if (exit_code != NULL)
            *exit_code = code;
        result = is_crash_code(code) ? ASMTEST_W32_CRASH : ASMTEST_W32_OK;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}

/* One in-flight child in the pool. */
typedef struct {
    HANDLE handle;
    int idx;            /* index into the caller's arrays */
    ULONGLONG deadline; /* GetTickCount64 deadline; 0 = no timeout */
} pool_slot;

/* Spawn cmdline into *s (idx, deadline filled in). Returns 1 on success. */
static int pool_spawn(const char *cmdline, pool_slot *s, int idx,
                      unsigned timeout_ms) {
    char buf[2048];
    size_t i = 0;
    for (; cmdline[i] != '\0' && i + 1 < sizeof buf; i++)
        buf[i] = cmdline[i];
    buf[i] = '\0';

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessA(NULL, buf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    s->handle = pi.hProcess;
    s->idx = idx;
    s->deadline = (timeout_ms == 0) ? 0 : GetTickCount64() + timeout_ms;
    return 1;
}

int asmtest_win32_run_pool(const char *const *cmdlines, int n, int jobs,
                           unsigned timeout_ms, asmtest_win32_run_t *results,
                           unsigned long *exit_codes) {
    if (jobs < 1)
        jobs = 1;
    if (jobs > MAXIMUM_WAIT_OBJECTS)
        jobs = MAXIMUM_WAIT_OBJECTS;

    pool_slot slots[MAXIMUM_WAIT_OBJECTS];
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    int nslots = 0;
    int next = 0; /* next task to launch */

    /* Fill the initial slots; a task that fails to spawn is recorded at once. */
    while (nslots < jobs && next < n) {
        if (pool_spawn(cmdlines[next], &slots[nslots], next, timeout_ms)) {
            nslots++;
        } else {
            results[next] = ASMTEST_W32_SPAWN_FAIL;
            if (exit_codes != NULL)
                exit_codes[next] = 0;
        }
        next++;
    }

    /* Retire exactly one slot per iteration (a child finished, or one timed
     * out), then refill that slot with the next pending task. */
    while (nslots > 0) {
        DWORD wait_ms = INFINITE;
        if (timeout_ms != 0) {
            ULONGLONG now = GetTickCount64();
            ULONGLONG min_left = (ULONGLONG)-1;
            for (int i = 0; i < nslots; i++) {
                ULONGLONG left =
                    slots[i].deadline > now ? slots[i].deadline - now : 0;
                if (left < min_left)
                    min_left = left;
            }
            wait_ms = (min_left > 0x7fffffffULL) ? 0x7fffffff : (DWORD)min_left;
        }
        for (int i = 0; i < nslots; i++)
            handles[i] = slots[i].handle;

        DWORD w =
            WaitForMultipleObjects((DWORD)nslots, handles, FALSE, wait_ms);

        int retire; /* slot index to retire */
        asmtest_win32_run_t outcome;
        DWORD code = 0;

        if (w == WAIT_TIMEOUT) {
            ULONGLONG now = GetTickCount64();
            retire = -1;
            for (int i = 0; i < nslots; i++) {
                if (slots[i].deadline != 0 && slots[i].deadline <= now) {
                    retire = i;
                    break;
                }
            }
            if (retire < 0)
                continue; /* deadline not actually reached yet; re-wait */
            TerminateProcess(slots[retire].handle, 1);
            WaitForSingleObject(slots[retire].handle, INFINITE);
            outcome = ASMTEST_W32_TIMEOUT;
        } else if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + (DWORD)nslots) {
            retire = (int)(w - WAIT_OBJECT_0);
            GetExitCodeProcess(slots[retire].handle, &code);
            outcome = is_crash_code(code) ? ASMTEST_W32_CRASH : ASMTEST_W32_OK;
        } else {
            /* WAIT_FAILED or abandoned: terminate everything and bail. */
            for (int i = 0; i < nslots; i++) {
                TerminateProcess(slots[i].handle, 1);
                CloseHandle(slots[i].handle);
            }
            return -1;
        }

        CloseHandle(slots[retire].handle);
        results[slots[retire].idx] = outcome;
        if (exit_codes != NULL)
            exit_codes[slots[retire].idx] = code;

        /* Refill this slot with the next task (skipping any that fail to spawn);
         * if there are none left, compact the slot away. */
        int filled = 0;
        while (next < n) {
            if (pool_spawn(cmdlines[next], &slots[retire], next, timeout_ms)) {
                next++;
                filled = 1;
                break;
            }
            results[next] = ASMTEST_W32_SPAWN_FAIL;
            if (exit_codes != NULL)
                exit_codes[next] = 0;
            next++;
        }
        if (!filled) {
            slots[retire] = slots[nslots - 1];
            nslots--;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* In-process crash-to-failure (the `--no-fork` path)                  */
/* ------------------------------------------------------------------ */

/* __builtin_setjmp uses a 5-word buffer. Thread-local so concurrent runners on
 * different threads each have their own recovery point. */
static __thread void *tls_recover[5];
static __thread asmtest_win32_fault_t tls_fault;
static __thread volatile int tls_armed;

static int is_fatal_exc(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return 1;
    default:
        return 0; /* incl. EXCEPTION_STACK_OVERFLOW: not safely recoverable */
    }
}

/* Finding #34: restore ABI-required CPU state in a resumed/redirected context.
 * A POSIX signal handler is entered with the kernel-cleared flags (DF=0), but a
 * Windows VEH / SetThreadContext resumes with the *faulting* frame's RFLAGS and
 * SSE control word. The Microsoft x64 ABI requires the direction flag to be clear
 * on entry and return, and the CRT/compilers emit `rep movs/stos` assuming DF=0;
 * a routine under test may have executed `std` (a descending string copy is
 * exactly the kind of assembly this framework exercises, and overrunning a guard
 * page with DF=1 is a designed-for event) or perturbed MxCsr before it faulted.
 * Normalise both before diverting control into recovered C code. */
static void win32_normalize_abi_state(CONTEXT *ctx) {
    ctx->EFlags &= ~(DWORD)0x400; /* clear DF (RFLAGS bit 10) */
    ctx->MxCsr = 0x1F80;          /* default SSE control/status word */
    ctx->FltSave.MxCsr = 0x1F80;  /* the FXSAVE-area mirror actually restored */
}

/* Reached via a redirected instruction pointer after a caught fault; jumps back
 * to the guard's recovery point without SEH unwinding. */
static void asmtest_win32_landing(void) { __builtin_longjmp(tls_recover, 1); }

static LONG CALLBACK asmtest_win32_veh(EXCEPTION_POINTERS *info) {
    if (!tls_armed)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (!is_fatal_exc(code))
        return EXCEPTION_CONTINUE_SEARCH;

    tls_fault.code = code;
    tls_fault.address =
        (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
            ? (void *)info->ExceptionRecord->ExceptionInformation[1]
            : info->ExceptionRecord->ExceptionAddress;
    tls_armed = 0;

    /* Resume the thread at the landing pad (normal context, not nested in the
     * exception dispatch) with a 16-aligned stack (rsp ≡ 8 mod 16 at entry) and
     * ABI-normalised flags/SSE state (finding #34). */
    win32_normalize_abi_state(info->ContextRecord);
    DWORD64 sp = info->ContextRecord->Rsp;
    sp = (sp & ~(DWORD64)15) - 8;
    info->ContextRecord->Rsp = sp;
    info->ContextRecord->Rip = (DWORD64)(ULONG_PTR)asmtest_win32_landing;
    return EXCEPTION_CONTINUE_EXECUTION;
}

int asmtest_win32_guard(void (*fn)(void *), void *arg,
                        asmtest_win32_fault_t *fault) {
    PVOID handler = AddVectoredExceptionHandler(1, asmtest_win32_veh);
    int faulted;

    if (__builtin_setjmp(tls_recover) == 0) {
        tls_armed = 1;
        fn(arg);
        tls_armed = 0;
        faulted = 0;
    } else {
        faulted = 1; /* arrived here via the landing pad */
    }

    if (handler != NULL)
        RemoveVectoredExceptionHandler(handler);
    if (faulted && fault != NULL)
        *fault = tls_fault;
    return faulted;
}

/* --- runner per-test facility (single global recovery; tests run serially) --- */

void *asmtest_win32_test_recover[5];
volatile int asmtest_win32_test_reason;
asmtest_win32_fault_t asmtest_win32_test_fault;

static volatile LONG rt_armed;
static volatile LONG rt_finishing; /* main thread has entered test_end (#35) */
static volatile LONG
    rt_landing_reached; /* landing pad executed (watchdog, #36) */
static PVOID rt_veh;
static HANDLE rt_timer_queue;
static HANDLE rt_timer;
static HANDLE rt_test_thread;

/* Finding #36: the watchdog redirects a hung thread via SetThreadContext, which
 * only takes effect when the thread next returns to user mode. A spin-loop hang
 * reaches the landing pad almost immediately; a thread wedged in a kernel wait
 * (WaitForSingleObject on a never-signaled handle, Sleep(INFINITE), a deadlocked
 * lock) never returns and so is unrecoverable in-process. After redirecting, the
 * watchdog polls for the landing pad up to this bound before failing hard. */
enum { RT_LANDING_POLL_MS = 5, RT_LANDING_POLL_ITERS = 200 }; /* ~1s ceiling */

static void rt_landing(void) {
    InterlockedExchange(&rt_landing_reached,
                        1); /* watched by rt_timer_cb (#36) */
    __builtin_longjmp(asmtest_win32_test_recover, 1);
}

static LONG CALLBACK rt_veh_cb(EXCEPTION_POINTERS *info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (!is_fatal_exc(code))
        return EXCEPTION_CONTINUE_SEARCH;
    if (!InterlockedExchange(&rt_armed, 0))
        return EXCEPTION_CONTINUE_SEARCH; /* already retired (timeout/assert) */

    asmtest_win32_test_fault.code = code;
    asmtest_win32_test_fault.address =
        (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
            ? (void *)info->ExceptionRecord->ExceptionInformation[1]
            : info->ExceptionRecord->ExceptionAddress;
    asmtest_win32_test_reason = ASMTEST_WIN32_REASON_CRASH;

    win32_normalize_abi_state(info->ContextRecord); /* clear DF etc. (#34) */
    DWORD64 sp = info->ContextRecord->Rsp;
    sp = (sp & ~(DWORD64)15) - 8;
    info->ContextRecord->Rsp = sp;
    info->ContextRecord->Rip = (DWORD64)(ULONG_PTR)rt_landing;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Watchdog: on the deadline, hijack the (hung) test thread's context to the
 * landing pad — the same recovery a fault uses — with a TIMEOUT verdict. */
static VOID CALLBACK rt_timer_cb(PVOID param, BOOLEAN fired) {
    (void)param;
    (void)fired;
    if (!InterlockedExchange(&rt_armed, 0))
        return;
    /* Finding #35: if the main thread has already entered test_end it is
     * finishing (and may be blocked inside DeleteTimerQueueTimer waiting for
     * THIS callback); hijacking it there would abandon that delete mid-call and
     * double-free the timer, and flip a passing test to a bogus TIMEOUT. The
     * test effectively completed — leave it alone. */
    if (rt_finishing)
        return;
    asmtest_win32_test_reason = ASMTEST_WIN32_REASON_TIMEOUT;
    if (rt_test_thread == NULL)
        return;

    InterlockedExchange(&rt_landing_reached, 0);
    SuspendThread(rt_test_thread);
    CONTEXT ctx;
    /* CONTROL for Rip/Rsp/EFlags, FLOATING_POINT so MxCsr can be normalised. */
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_FLOATING_POINT;
    if (GetThreadContext(rt_test_thread, &ctx)) {
        win32_normalize_abi_state(&ctx); /* clear DF etc. (#34) */
        DWORD64 sp = ctx.Rsp;
        sp = (sp & ~(DWORD64)15) - 8;
        ctx.Rsp = sp;
        ctx.Rip = (DWORD64)(ULONG_PTR)rt_landing;
        SetThreadContext(rt_test_thread, &ctx);
    }
    ResumeThread(rt_test_thread);

    /* Finding #36: wait for the redirect to actually take effect. A user-mode
     * (spin-loop) hang reaches rt_landing at once; a kernel-blocked thread never
     * does. If it does not land within the bound, it is unrecoverable in-process
     * — fail hard with a distinctive exit code instead of wedging the whole run
     * (the default forked mode contains such hangs by killing the child). */
    for (int i = 0; i < RT_LANDING_POLL_ITERS; i++) {
        if (InterlockedCompareExchange(&rt_landing_reached, 0, 0))
            return; /* diverted successfully */
        Sleep(RT_LANDING_POLL_MS);
    }
    fprintf(stderr,
            "asmtest: --no-fork test blocked in an unrecoverable kernel wait; "
            "forcing timeout exit (%d). Use the default forked mode to contain "
            "such hangs.\n",
            ASMTEST_WIN32_HANG_EXIT);
    fflush(stderr);
    ExitProcess(ASMTEST_WIN32_HANG_EXIT);
}

void asmtest_win32_test_begin(unsigned timeout_ms) {
    asmtest_win32_test_reason = 0;
    rt_timer = NULL;
    rt_test_thread = NULL;
    rt_finishing = 0;
    rt_landing_reached = 0;
    rt_armed = 1;
    rt_veh = AddVectoredExceptionHandler(1, rt_veh_cb);
    if (timeout_ms != 0) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                        GetCurrentProcess(), &rt_test_thread, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        if (rt_timer_queue == NULL)
            rt_timer_queue = CreateTimerQueue();
        CreateTimerQueueTimer(&rt_timer, rt_timer_queue, rt_timer_cb, NULL,
                              timeout_ms, 0, WT_EXECUTEONLYONCE);
    }
}

void asmtest_win32_test_disarm(void) { InterlockedExchange(&rt_armed, 0); }

/* Re-arm the crash/timeout facility for the next run_one phase (finding #33):
 * after an assertion the assertion path disarmed it, but teardown must run with
 * the same crash/timeout protection so a fault or hang there is folded into the
 * outcome (mirroring the always-installed POSIX signal handler + pending alarm).
 * The one-shot watchdog timer, if it has not yet fired, keeps covering the whole
 * test deadline across the re-arm. */
void asmtest_win32_test_rearm(void) {
    asmtest_win32_test_reason = 0;
    InterlockedExchange(&rt_armed, 1);
}

void asmtest_win32_test_end(void) {
    /* Finding #35: mark that we are wrapping up BEFORE retiring the arm, so a
     * watchdog that fires now sees rt_finishing and declines to hijack us. */
    rt_finishing = 1;
    InterlockedExchange(&rt_armed, 0);
    /* Swap the timer handle out atomically so exactly one caller ever deletes it.
     * If a watchdog hijack abandons this DeleteTimerQueueTimer mid-call and
     * run_one re-enters test_end after the longjmp, the second call sees NULL and
     * skips — no double-delete / use-after-free of the timer object. */
    HANDLE t =
        (HANDLE)InterlockedExchangePointer((PVOID volatile *)&rt_timer, NULL);
    if (t != NULL)
        /* INVALID_HANDLE_VALUE: wait for any in-flight callback to finish. */
        DeleteTimerQueueTimer(rt_timer_queue, t, INVALID_HANDLE_VALUE);
    if (rt_test_thread != NULL) {
        CloseHandle(rt_test_thread);
        rt_test_thread = NULL;
    }
    if (rt_veh != NULL) {
        RemoveVectoredExceptionHandler(rt_veh);
        rt_veh = NULL;
    }
}

#endif /* _WIN32 */
