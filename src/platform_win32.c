/* platform_win32.c — Win32 implementations of the POSIX primitives the runner
 * relies on, for the native Win64 tier (see docs/plans/win64-native-tier-plan.md,
 * Phase 4). Kept separate from src/asmtest.c (which stays POSIX-only) so the
 * working {x86-64, AArch64} x {Linux, macOS} build is untouched; this file is
 * compiled only for the Win64 target and provides the same exported symbols.
 *
 * Ported so far:
 *   - the guard-page allocator (mmap + mprotect(PROT_NONE) -> VirtualAlloc +
 *     VirtualProtect(PAGE_NOACCESS)).
 *
 * Still POSIX-only in src/asmtest.c (later Phase 4 slices): per-test isolation
 * (fork -> CreateProcess), crash-to-failure (signals -> SEH/vectored handler),
 * timeout (alarm -> timer queue), and the --filter glob (fnmatch).
 */
#if defined(_WIN32)

#include <stddef.h>
#include <windows.h>

#include "asmtest.h"

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
    unsigned char *base =
        (unsigned char *)VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT,
                                      PAGE_READWRITE);
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
    unsigned char *base =
        (unsigned char *)VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT,
                                      PAGE_READWRITE);
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

#endif /* _WIN32 */
