/* packaging/smoke/consumer.c — the minimal pkg-config consumer shared by every
 * system-package Docker verification lane (distribution-packaging.md T8-T12).
 *
 * libasmtest.a is a unit-testing framework whose static lib *provides* main()
 * (the TAP test runner); a consumer supplies TEST() cases and links the lib.
 * So this file deliberately declares no main of its own — one trivial case is
 * enough to prove the whole chain a system package must get right: the installed
 * <asmtest.h> resolves, libasmtest.a links, and a real framework symbol runs.
 * asmtest_guarded_alloc/_free live in the core static lib and need no setup.
 *
 * Build + run exactly as the lanes do:
 *   cc consumer.c $(pkg-config --cflags --libs asmtest) -o consumer && ./consumer
 */
#include <asmtest.h>

TEST(pkgconfig, links_and_runs) {
    void *p = asmtest_guarded_alloc(64);
    ASSERT_TRUE(p != NULL);
    asmtest_guarded_free(p, 64);
    ASSERT_TRUE(ASMTEST_VERSION_NUM >= 10100);
}
