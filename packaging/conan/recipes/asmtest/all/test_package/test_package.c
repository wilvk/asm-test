/* Conan test_package consumer. libasmtest.a provides main() (the TAP runner);
 * a consumer supplies TEST() cases and links the packaged static lib. */
#include <asmtest.h>

TEST(conan, links_and_runs) {
    void *p = asmtest_guarded_alloc(64);
    ASSERT_TRUE(p != NULL);
    asmtest_guarded_free(p, 64);
    ASSERT_TRUE(ASMTEST_VERSION_NUM >= 10100);
}
