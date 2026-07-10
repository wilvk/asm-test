/* attach_trace_win.c — Windows mirror of examples/attach_trace.c.
 *
 * Attaches to a process asm-test did NOT start and single-steps ONE call of a
 * region, out of process, via the Win32 Debug API — the direct analog of the
 * Linux ptrace path:
 *     DebugActiveProcess   ~ PTRACE_ATTACH        WriteProcessMemory 0xCC ~ run_to
 *     WaitForDebugEvent    ~ waitpid              GetThreadContext.Rip   ~ read RIP
 *     CONTEXT.EFlags |= TF ~ PTRACE_SINGLESTEP    ReadProcessMemory      ~ process_vm_readv
 *     DebugActiveProcessStop ~ PTRACE_DETACH
 *
 * This is Capstone-free (self-contained: only <windows.h>), so — like the Linux
 * demo's no-Capstone fallback — it reports ordered instruction offsets and a
 * discontinuity-based block count, not a disassembly. It traces a LEAF region
 * (the region calls nothing): the first step that leaves [base,base+len) is the
 * return, and RAX there is the result — matching the pre-Capstone Linux contract.
 *
 * Usage:  attach_trace_win <pid> <hex-addr> <len>
 * BUILDS with mingw; RUNS on real Windows (and best-effort under Wine).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define INSN_CAP 8192
#define TF 0x100u /* EFLAGS trap flag */

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <pid> <hex-addr> <len>\n", argv[0]);
        return 2;
    }
    DWORD pid = (DWORD)strtoul(argv[1], NULL, 0);
    uintptr_t base = (uintptr_t)strtoull(argv[2], NULL, 0);
    size_t len = (size_t)strtoull(argv[3], NULL, 0);
    fprintf(stderr, "tracing [0x%llx, +%zu) in pid %lu\n",
            (unsigned long long)base, len, (unsigned long)pid);

    if (!DebugActiveProcess(pid)) {
        fprintf(stderr,
                "DebugActiveProcess(%lu) failed: %lu (need same session / "
                "SeDebugPrivilege)\n",
                (unsigned long)pid, GetLastError());
        return 1;
    }
    DebugSetProcessKillOnExit(FALSE); /* detach leaves the target running */

    HANDLE hProc = NULL;
    BYTE orig = 0;
    int entered = 0, stepping = 0, done = 0;
    long ret_val = 0;
    uint64_t *insns = (uint64_t *)malloc(INSN_CAP * sizeof(uint64_t));
    size_t n_insns = 0;
    uint64_t total = 0;
    int truncated = 0;

    DEBUG_EVENT ev;
    while (!done) {
        if (!WaitForDebugEvent(&ev, 30000)) {
            fprintf(stderr, "WaitForDebugEvent timed out\n");
            break;
        }
        DWORD cont = DBG_CONTINUE;

        if (ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            hProc = ev.u.CreateProcessInfo.hProcess;
            if (ev.u.CreateProcessInfo.hFile)
                CloseHandle(ev.u.CreateProcessInfo.hFile);
            /* plant int3 at the region entry — the run_to analog */
            if (ReadProcessMemory(hProc, (void *)base, &orig, 1, NULL)) {
                BYTE cc = 0xCC;
                WriteProcessMemory(hProc, (void *)base, &cc, 1, NULL);
                FlushInstructionCache(hProc, (void *)base, 1);
            } else {
                fprintf(stderr, "cannot read region entry: %lu\n",
                        GetLastError());
            }
        } else if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            const EXCEPTION_RECORD *er = &ev.u.Exception.ExceptionRecord;
            uintptr_t exaddr = (uintptr_t)er->ExceptionAddress;
            HANDLE ht = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                       THREAD_QUERY_INFORMATION,
                                   FALSE, ev.dwThreadId);
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (ht)
                GetThreadContext(ht, &ctx);

            if (er->ExceptionCode == EXCEPTION_BREAKPOINT && exaddr == base &&
                !entered) {
                /* region entry: restore byte, rewind RIP, begin single-stepping */
                WriteProcessMemory(hProc, (void *)base, &orig, 1, NULL);
                FlushInstructionCache(hProc, (void *)base, 1);
                ctx.Rip = (DWORD64)base;
                ctx.EFlags |= TF;
                SetThreadContext(ht, &ctx);
                entered = stepping = 1;
                insns[n_insns++] = 0; /* offset 0 = entry */
                total++;
            } else if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && stepping) {
                uintptr_t rip = (uintptr_t)ctx.Rip;
                if (rip >= base && rip < base + len) {
                    total++;
                    if (n_insns < INSN_CAP)
                        insns[n_insns++] = (uint64_t)(rip - base);
                    else
                        truncated = 1;
                    ctx.EFlags |= TF; /* keep stepping */
                    SetThreadContext(ht, &ctx);
                } else {
                    /* left the region — leaf return. RAX is the result. */
                    ret_val = (long)ctx.Rax;
                    ctx.EFlags &= ~(DWORD)TF;
                    SetThreadContext(ht, &ctx);
                    stepping = 0;
                    done = 1;
                }
            } else if (er->ExceptionCode == EXCEPTION_BREAKPOINT ||
                       er->ExceptionCode == EXCEPTION_SINGLE_STEP) {
                /* initial system breakpoint / stray step: swallow */
            } else {
                /* a real fault in the target: let its own handler see it */
                cont = DBG_EXCEPTION_NOT_HANDLED;
            }
            if (ht)
                CloseHandle(ht);
        } else if (ev.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            fprintf(stderr, "(target exited before the region ran)\n");
            done = 1;
        }

        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }

    DebugActiveProcessStop(pid); /* detach; the target keeps running */

    if (!entered) {
        fprintf(stderr, "region was never entered\n");
        free(insns);
        return 1;
    }

    printf("\nreturn value : %ld\n", ret_val);
    printf("instructions : %llu executed, %zu recorded%s\n",
           (unsigned long long)total, n_insns, truncated ? "  (TRUNCATED)" : "");
    size_t blocks = 0; /* block ENTRIES by control-flow discontinuity */
    for (size_t i = 0; i < n_insns; i++) {
        if (i == 0 || insns[i] <= insns[i - 1] || insns[i] - insns[i - 1] > 15)
            blocks++;
    }
    printf("basic blocks : %zu entries (by CF discontinuity; no disassembler)\n\n",
           blocks);
    printf("ordered instruction offsets:\n  ");
    for (size_t i = 0; i < n_insns; i++)
        printf("+0x%llx ", (unsigned long long)insns[i]);
    printf("\n");

    free(insns);
    return 0;
}
