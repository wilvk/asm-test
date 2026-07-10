/* syscall_log_win.c — Windows mirror of examples/syscall_log.c.
 *
 * Attaches to a process asm-test did NOT start and logs the DATA it sends/receives
 * across the kernel boundary, out of process. Windows has no PTRACE_SYSCALL and no
 * stable syscall numbers, so — instead of stopping on every syscall — it plants
 * int3 breakpoints on the ntdll Nt* stubs (the stable interception layer) via the
 * Debug API, then on each hit reads the arguments from CONTEXT + the pointed-to
 * buffers/paths via ReadProcessMemory. The breakpoint step-over/re-arm is the exact
 * analog of the Linux run_to step-over.
 *
 * Decodes NtCreateFile (path), NtWriteFile (outgoing data), NtClose. NtReadFile is
 * reported by length (its buffer is filled at RETURN; capturing it would need a
 * return breakpoint, the same technique applied once more). x86-64. Single-threaded
 * target assumed (the re-arm state is process-global).
 *
 * Usage:  syscall_log_win <pid> [max_events]
 * BUILDS with mingw; RUNS on real Windows (best-effort under Wine).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
/* clang-format off: windows.h must precede tlhelp32.h (it needs Win32 types). */
#include <windows.h>
#include <tlhelp32.h>
/* clang-format on */

#define TF 0x100u
#define DUMP_CAP 48

static HANDLE g_proc;

static int rdmem(uint64_t addr, void *buf, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(g_proc, (void *)(uintptr_t)addr, buf, n, &got) &&
                   got == n
               ? 0
               : -1;
}

/* Print up to DUMP_CAP bytes at target `addr` as a C-escaped quoted string. */
static void dump_data(uint64_t addr, uint32_t n) {
    uint32_t want = n > DUMP_CAP ? DUMP_CAP : n;
    unsigned char buf[DUMP_CAP];
    if (rdmem(addr, buf, want) != 0)
        want = 0;
    putchar('"');
    for (uint32_t i = 0; i < want; i++) {
        unsigned char c = buf[i];
        if (c == '\n')
            fputs("\\n", stdout);
        else if (c == '\t')
            fputs("\\t", stdout);
        else if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c >= 0x20 && c < 0x7f)
            putchar(c);
        else
            printf("\\x%02x", c);
    }
    putchar('"');
    if (n > want)
        fputs("...", stdout);
}

/* Print a UNICODE_STRING at target `us_addr` (Length@0 in bytes, Buffer@8). */
static void dump_wstr(uint64_t us_addr) {
    uint16_t blen = 0;
    uint64_t wbuf = 0;
    if (rdmem(us_addr + 0, &blen, 2) != 0 || rdmem(us_addr + 8, &wbuf, 8) != 0) {
        fputs("\"?\"", stdout);
        return;
    }
    uint32_t nch = blen / 2;
    if (nch > DUMP_CAP)
        nch = DUMP_CAP;
    putchar('"');
    for (uint32_t i = 0; i < nch; i++) {
        uint16_t wc = 0;
        if (rdmem(wbuf + 2 * i, &wc, 2) != 0)
            break;
        putchar(wc >= 0x20 && wc < 0x7f ? (int)wc : '?');
    }
    putchar('"');
    if (blen / 2 > nch)
        fputs("...", stdout);
}

/* Find a loaded module's base address in the TARGET process (out of process). */
static uint64_t module_base(DWORD pid, const wchar_t *name) {
    HANDLE snap = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    MODULEENTRY32W me = {.dwSize = sizeof me};
    uint64_t base = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, name) == 0) {
                base = (uint64_t)(uintptr_t)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

enum { S_CREATE, S_WRITE, S_READ, S_CLOSE, S_N };
static const char *S_NAME[S_N] = {"NtCreateFile", "NtWriteFile", "NtReadFile",
                                  "NtClose"};
static const char *S_SYM[S_N] = {"NtCreateFile", "NtWriteFile", "NtReadFile",
                                 "NtClose"};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pid> [max_events]\n", argv[0]);
        return 2;
    }
    DWORD pid = (DWORD)strtoul(argv[1], NULL, 0);
    long max_events = argc > 2 ? strtol(argv[2], NULL, 0) : 16;
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Resolve each stub's OFFSET in our own ntdll, then rebase onto the target's
     * ntdll base — robust even if the two bases differ (e.g. under Wine). */
    HMODULE nt_local = GetModuleHandleW(L"ntdll.dll");
    uint64_t local_base = (uint64_t)(uintptr_t)nt_local;
    uint64_t tgt_base = module_base(pid, L"ntdll.dll");
    if (!tgt_base) {
        fprintf(stderr, "could not find ntdll in pid %lu\n", (unsigned long)pid);
        return 1;
    }
    uint64_t stub[S_N];
    BYTE orig[S_N];
    for (int i = 0; i < S_N; i++) {
        void *p = (void *)GetProcAddress(nt_local, S_SYM[i]);
        stub[i] = p ? tgt_base + ((uint64_t)(uintptr_t)p - local_base) : 0;
    }

    if (!DebugActiveProcess(pid)) {
        fprintf(stderr, "DebugActiveProcess(%lu) failed: %lu\n",
                (unsigned long)pid, GetLastError());
        return 1;
    }
    DebugSetProcessKillOnExit(FALSE);
    fprintf(stderr, "logging up to %ld ntdll calls of pid %lu (out of band)\n\n",
            max_events, (unsigned long)pid);

    uint64_t rearm = 0; /* stub addr whose int3 to re-plant after a step-over */
    int rearm_i = -1;
    long done = 0;
    int running = 1;

    DEBUG_EVENT ev;
    while (running) {
        if (!WaitForDebugEvent(&ev, 30000)) {
            fprintf(stderr, "WaitForDebugEvent timed out\n");
            break;
        }
        DWORD cont = DBG_CONTINUE;

        if (ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            g_proc = ev.u.CreateProcessInfo.hProcess;
            if (ev.u.CreateProcessInfo.hFile)
                CloseHandle(ev.u.CreateProcessInfo.hFile);
            for (int i = 0; i < S_N; i++) {
                BYTE cc = 0xCC;
                if (stub[i] && rdmem(stub[i], &orig[i], 1) == 0) {
                    WriteProcessMemory(g_proc, (void *)(uintptr_t)stub[i], &cc, 1,
                                       NULL);
                    FlushInstructionCache(g_proc, (void *)(uintptr_t)stub[i], 1);
                }
            }
        } else if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const EXCEPTION_RECORD *er = &ev.u.Exception.ExceptionRecord;
            uint64_t exaddr = (uint64_t)(uintptr_t)er->ExceptionAddress;
            HANDLE ht = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                       THREAD_QUERY_INFORMATION,
                                   FALSE, ev.dwThreadId);
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (ht)
                GetThreadContext(ht, &ctx);

            int which = -1;
            for (int i = 0; i < S_N; i++)
                if (er->ExceptionCode == EXCEPTION_BREAKPOINT &&
                    exaddr == stub[i])
                    which = i;

            if (which >= 0) {
                uint64_t rsp = ctx.Rsp;
                /* x64 args: RCX,RDX,R8,R9 then [RSP+0x28],[+0x30],[+0x38],... */
                switch (which) {
                case S_WRITE: {
                    uint64_t bufp = 0, length = 0;
                    rdmem(rsp + 0x30, &bufp, 8);   /* arg6 = Buffer */
                    rdmem(rsp + 0x38, &length, 8); /* arg7 = Length */
                    printf("NtWriteFile(handle=0x%llx, ",
                           (unsigned long long)ctx.Rcx);
                    dump_data(bufp, (uint32_t)length);
                    printf(", %lu)\n", (unsigned long)(uint32_t)length);
                    break;
                }
                case S_READ: {
                    uint64_t length = 0;
                    rdmem(rsp + 0x38, &length, 8);
                    printf("NtReadFile(handle=0x%llx, len=%lu)\n",
                           (unsigned long long)ctx.Rcx,
                           (unsigned long)(uint32_t)length);
                    break;
                }
                case S_CREATE: {
                    uint64_t oa = ctx.R8; /* arg3 = OBJECT_ATTRIBUTES* */
                    uint64_t objname = 0;
                    rdmem(oa + 16, &objname, 8); /* ObjectName@16 (x64) */
                    fputs("NtCreateFile(", stdout);
                    if (objname)
                        dump_wstr(objname);
                    else
                        fputs("\"?\"", stdout);
                    printf(", access=0x%llx)\n", (unsigned long long)ctx.Rdx);
                    break;
                }
                case S_CLOSE:
                    printf("NtClose(0x%llx)\n", (unsigned long long)ctx.Rcx);
                    break;
                }

                /* step over: restore byte, rewind RIP, single-step, re-arm */
                WriteProcessMemory(g_proc, (void *)(uintptr_t)stub[which],
                                   &orig[which], 1, NULL);
                FlushInstructionCache(g_proc, (void *)(uintptr_t)stub[which], 1);
                ctx.Rip = (DWORD64)stub[which];
                ctx.EFlags |= TF;
                SetThreadContext(ht, &ctx);
                rearm = stub[which];
                rearm_i = which;
                if (++done >= max_events)
                    running = 0;
            } else if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && rearm) {
                /* the step-over completed: re-plant the int3, clear TF */
                BYTE cc = 0xCC;
                WriteProcessMemory(g_proc, (void *)(uintptr_t)rearm, &cc, 1,
                                   NULL);
                FlushInstructionCache(g_proc, (void *)(uintptr_t)rearm, 1);
                ctx.EFlags &= ~(DWORD)TF;
                SetThreadContext(ht, &ctx);
                rearm = 0;
                rearm_i = -1;
            } else if (er->ExceptionCode == EXCEPTION_BREAKPOINT ||
                       er->ExceptionCode == EXCEPTION_SINGLE_STEP) {
                /* initial system breakpoint / stray step: swallow */
            } else {
                cont = DBG_EXCEPTION_NOT_HANDLED; /* real fault: hand back */
            }
            if (ht)
                CloseHandle(ht);
        } else if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            fprintf(stderr, "\n(target exited)\n");
            break;
        }

        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }

    /* detach cleanly: remove any breakpoints we still hold */
    for (int i = 0; i < S_N; i++)
        if (stub[i])
            WriteProcessMemory(g_proc, (void *)(uintptr_t)stub[i], &orig[i], 1,
                               NULL);
    DebugActiveProcessStop(pid);
    fprintf(stderr, "\ndetached; logged %ld ntdll calls\n", done);
    (void)S_NAME;
    (void)rearm_i;
    return 0;
}
