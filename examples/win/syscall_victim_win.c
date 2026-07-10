/* syscall_victim_win.c — Windows mirror of examples/syscall_victim.c.
 *
 * A separate process (asm-test does NOT start it) that does real file I/O in a
 * loop, so the out-of-band logger (syscall_log_win) has data to capture. The
 * Win32 CreateFile/WriteFile/ReadFile calls descend through ntdll's
 * NtCreateFile/NtWriteFile/NtReadFile/NtClose — the boundary the logger taps
 * (Windows' stable interception layer, the analog of the Linux syscall boundary).
 */
#include <stdio.h>
#include <windows.h>

int main(void) {
    fprintf(stderr, "victim pid=%lu\n", (unsigned long)GetCurrentProcessId());
    fflush(stderr);

    /* Write under the temp dir so the demo never pollutes the working tree. */
    char path[MAX_PATH];
    DWORD tn = GetTempPathA(sizeof path, path);
    _snprintf(path + tn, sizeof path - tn, "asmtest_syscall_demo.txt");
    for (int n = 0;; n++) {
        char msg[64];
        int len = _snprintf(msg, sizeof msg, "tick %d from pid %lu\n", n,
                            (unsigned long)GetCurrentProcessId());

        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            WriteFile(h, msg, (DWORD)len, &wrote, NULL); /* outgoing data */
            CloseHandle(h);
        }
        h = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            char in[64];
            DWORD got = 0;
            ReadFile(h, in, sizeof in, &got, NULL); /* incoming data */
            CloseHandle(h);
        }
        Sleep(300); /* ~3 Hz */
    }
    return 0;
}
