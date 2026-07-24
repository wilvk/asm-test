// view_test.h — the shared harness for the replay-view tests
// (docs/internal/gui/04-replay-views.md T3-T8). Header-only, so it adds no
// object to any link line and cannot drag a dependency into a test binary.
//
// Two things every view test needs: check helpers in the cli/test_view.c idiom
// (a `failures` counter, a printed FAIL line, no early exit — one run reports
// every broken rule, not just the first), and a byte-exact golden compare with
// UPDATE_GOLDEN=1 regeneration.
#ifndef ASMDESK_TEST_VIEW_TEST_H
#define ASMDESK_TEST_VIEW_TEST_H

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "doc/recording.h"
#include "doc/streams.h"

#ifndef ASMTEST_GOLDEN_DIR
#error "ASMTEST_GOLDEN_DIR must be defined by the build (mk/desktop.mk)"
#endif
#ifndef ASMTEST_EXPECTED_DIR
#error "ASMTEST_EXPECTED_DIR must be defined by the build (mk/desktop.mk)"
#endif

namespace vt {

inline int failures = 0;

inline void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}

inline void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

template <typename A, typename B>
void eq(const std::string &what, const A &got, const B &want) {
    if (!(got == want)) {
        std::ostringstream o;
        o << "got " << got << ", want " << want;
        fail(what, o.str());
    }
}

// Load a fixture from the golden corpus and decode its streams. A fixture that
// will not load is a hard failure, not a skipped assertion.
inline asmdesk::Streams load(const std::string &relpath) {
    std::string path = std::string(ASMTEST_GOLDEN_DIR) + "/" + relpath;
    std::string err;
    auto rec = asmdesk::load_recording_file(path, err);
    if (!rec) {
        fail("load " + relpath, err);
        return asmdesk::Streams{};
    }
    return asmdesk::decode_streams(*rec);
}

#ifdef ASMTEST_FIXTURE_DIR
// Load a fixture as a RECORDING rather than decoded streams. The live Observer
// views (08-observer-views.md) build straight off the document model, because
// the kinds they read — syscall, watch, topo, call, codeimage — are not part of
// 04's Streams decode and inventing a second decode for them would put the same
// fields in two places.
inline asmdesk::Recording load_fixture(const std::string &relpath) {
    std::string path = std::string(ASMTEST_FIXTURE_DIR) + "/" + relpath;
    std::string err;
    auto rec = asmdesk::load_recording_file(path, err);
    if (!rec) {
        fail("load " + relpath, err);
        return asmdesk::Recording{};
    }
    return *rec;
}
#endif

// Byte-compare `got` against desktop/test/expected/<name>. UPDATE_GOLDEN=1
// rewrites it — review the diff, the dump IS the contract.
inline void golden(const std::string &name, const std::string &got) {
    std::string path = std::string(ASMTEST_EXPECTED_DIR) + "/" + name;
    const char *update = std::getenv("UPDATE_GOLDEN");
    if (update != nullptr && std::string(update) == "1") {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            fail("golden " + name, "cannot write " + path);
            return;
        }
        out << got;
        std::printf("  regenerated %s\n", path.c_str());
        return;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fail("golden " + name,
             "no expected file at " + path +
                 " — regenerate with UPDATE_GOLDEN=1 make desktop-test");
        return;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    if (buf.str() == got)
        return;
    fail("golden " + name, "the dump differs from " + path);
    // Print the first differing line: a 200-line diff in a test log helps
    // nobody, and the first divergence is nearly always the cause.
    std::istringstream a(buf.str()), b(got);
    std::string la, lb;
    int line = 1;
    for (;; line++) {
        bool ga = static_cast<bool>(std::getline(a, la));
        bool gb = static_cast<bool>(std::getline(b, lb));
        if (!ga && !gb)
            break;
        if (!ga)
            la = "(end of file)";
        if (!gb)
            lb = "(end of file)";
        if (la != lb) {
            std::fprintf(stderr,
                         "     line %d\n     expected: %s\n     got:      %s\n",
                         line, la.c_str(), lb.c_str());
            break;
        }
    }
}

inline int report(const char *name) {
    if (failures) {
        std::fprintf(stderr, "%d %s check(s) failed\n", failures, name);
        return 1;
    }
    std::printf("%s: all checks passed\n", name);
    return 0;
}

} // namespace vt
#endif // ASMDESK_TEST_VIEW_TEST_H
