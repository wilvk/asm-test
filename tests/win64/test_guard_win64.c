/* test_guard_win64.c — Phase 4 slice: the Win32 guard-page allocator under Wine.
 *
 * Verifies asmtest_guarded_alloc / _alloc_under (src/platform_win32.c): the
 * usable region is committed read-write, and the abutting guard page is
 * PAGE_NOACCESS (so an overrun/underrun faults). Protection is checked with
 * VirtualQuery — deterministic, and it needs no SEH crash handler (that is a
 * later Phase 4 slice). Builds as a real PE and runs under Wine.
 */
#include <stdio.h>
#include <windows.h>

#include "asmtest.h"

static int fails = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            printf("ok   - %s\n", (msg));                                      \
        } else {                                                               \
            printf("FAIL - %s\n", (msg));                                      \
            fails++;                                                           \
        }                                                                      \
    } while (0)

static DWORD protect_at(const void *addr) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof mbi) == 0)
        return 0;
    if (mbi.State != MEM_COMMIT)
        return 0;
    return mbi.Protect;
}

int main(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size_t pg = (size_t)si.dwPageSize;
    const size_t n = 100;

    /* Trailing guard: buffer's last byte abuts a PAGE_NOACCESS page. */
    unsigned char *p = (unsigned char *)asmtest_guarded_alloc(n);
    CHECK(p != NULL, "asmtest_guarded_alloc returns a buffer");
    if (p != NULL) {
        p[0] = 0xAB;
        p[n - 1] = 0xCD; /* in-bounds writes must not fault */
        CHECK(p[0] == 0xAB && p[n - 1] == 0xCD,
              "guarded_alloc: usable region is writable");
        CHECK(protect_at(p) == PAGE_READWRITE,
              "guarded_alloc: usable region is committed PAGE_READWRITE");
        CHECK(protect_at(p + n) == PAGE_NOACCESS,
              "guarded_alloc: trailing guard page is PAGE_NOACCESS");
        asmtest_guarded_free(p, n);
    }

    /* Leading guard: a PAGE_NOACCESS page precedes the buffer (underrun trap). */
    unsigned char *u = (unsigned char *)asmtest_guarded_alloc_under(n);
    CHECK(u != NULL, "asmtest_guarded_alloc_under returns a buffer");
    if (u != NULL) {
        u[0] = 0x12;
        u[n - 1] = 0x34;
        CHECK(u[0] == 0x12 && u[n - 1] == 0x34,
              "guarded_alloc_under: usable region is writable");
        CHECK(protect_at(u) == PAGE_READWRITE,
              "guarded_alloc_under: usable region is committed PAGE_READWRITE");
        CHECK(protect_at(u - pg) == PAGE_NOACCESS,
              "guarded_alloc_under: leading guard page is PAGE_NOACCESS");
        asmtest_guarded_free_under(u, n);
    }

    /* A size within a page of SIZE_MAX makes round_up_page's rounding wrap;
     * the allocators must reject it instead of handing back a pointer inside
     * the guard page (code-review-plausible-triage T3). */
    CHECK(asmtest_guarded_alloc((size_t)-1) == NULL,
          "guarded_alloc rejects a size that overflows page rounding");
    CHECK(asmtest_guarded_alloc((size_t)-4096) == NULL,
          "guarded_alloc rejects a size within a page of SIZE_MAX");
    CHECK(asmtest_guarded_alloc_under((size_t)-1) == NULL,
          "guarded_alloc_under rejects a size that overflows page rounding");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
