/* test_vmmap.c — the /proc/<pid>/maps parse + rank + JSON body encoder
 * (cli/asmspy_vmmap.h), over a STRING fixture.
 *
 * This test carries the real burden for the feature. The emission side needs a
 * live target and a serve session; the part that can actually be WRONG is the
 * parse and the cap discipline, and both are pure over a FILE* — so they run on
 * any host with no /proc, no ptrace and no victim. The cli-smoke lane is then
 * only responsible for the wiring. (Same discipline as test_autoregion.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asmspy_vmmap.h"

static int failures;
static void check(const char *what, int cond, const char *why) {
    if (!cond) {
        fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

/* A miniature but REAL maps table: file-backed exec, [heap], a shared library,
 * [stack], and a large anonymous reservation. The last is the row scan_modules
 * would silently drop, and the one a data trace is most likely to touch. */
static const char *kMaps =
    "00400000-00410000 r-xp 00000000 08:01 100 /tmp/victim\n"
    "00500000-00540000 rw-p 00000000 00:00 0 [heap]\n"
    "7f0000000000-7f0000200000 r-xp 00000000 08:01 200 /usr/lib/libc.so.6\n"
    "7ffd00000000-7ffd00021000 rw-p 00000000 00:00 0 [stack]\n"
    "7f9000000000-7f9020000000 rw-p 00000000 00:00 0 \n";

int main(void) {
    FILE *f = fmemopen((void *)kMaps, strlen(kMaps), "r");
    asmspy_vmspan_t *sp = NULL;
    size_t total = 0;
    int n, i, anon = 0, heap = 0, stack = 0;
    char buf[64 * 1024];
    char tiny[64];

    check("fixture/opened", f != NULL, "fmemopen failed");
    if (!f)
        return 1;
    n = asmspy_vmmap_parse(f, &sp, &total);
    fclose(f);
    check("parse/count", n == 5, "five rows in the fixture");
    check("parse/total", total == 5, "total counts rows SEEN, before any cap");

    /* Anonymous and bracket rows are KEPT. Dropping them is exactly the
     * scan_modules behaviour this replaces, and they are most of what a data
     * trace touches. */
    for (i = 0; i < n; i++) {
        if (sp[i].name[0] == '\0')
            anon++;
        if (strcmp(sp[i].name, "[heap]") == 0)
            heap++;
        if (strcmp(sp[i].name, "[stack]") == 0)
            stack++;
    }
    check("parse/keeps-anon", anon == 1, "an anonymous mapping must survive");
    check("parse/keeps-heap", heap == 1, "[heap] must survive");
    check("parse/keeps-stack", stack == 1, "[stack] must survive");

    /* name is the BASENAME of a path; path is the whole thing. */
    for (i = 0; i < n; i++)
        if (sp[i].base == 0x7f0000000000ULL) {
            check("parse/basename", strcmp(sp[i].name, "libc.so.6") == 0,
                  sp[i].name);
            check("parse/path", strcmp(sp[i].path, "/usr/lib/libc.so.6") == 0,
                  sp[i].path);
            check("parse/len", sp[i].len == 0x200000ULL, "hi-lo");
        }

    /* Ranked executable-first, so a cap can never drop libc while keeping a
     * larger anonymous reservation. */
    check("rank/exec-first", sp[0].perms[2] == 'x', "row 0 must be executable");
    /* NB: `sp` stays live — the body checks below still read it, and it is
     * freed once at the end. The cap block next owns its own allocation. */

    /* The body is valid JSON and states cap, flag AND total. */
    check("body/ok",
          asmspy_vmmap_body(sp, (size_t)n, total, 1, 0, buf, sizeof buf) == 0,
          "a small map must fit");
    check("body/has-total", strstr(buf, "\"spans_total\":5") != NULL, buf);
    check("body/has-flag", strstr(buf, "\"spans_truncated\":false") != NULL, buf);
    check("body/readable", strstr(buf, "\"maps_readable\":true") != NULL, buf);
    check("body/hex-base", strstr(buf, "\"base\":\"0x500000\"") != NULL,
          "addresses are hex STRINGS, never JSON numbers");
    check("body/len-number", strstr(buf, "\"len\":262144") != NULL,
          "lengths are magnitudes, so they stay numbers");
    check("body/no-leading-comma", buf[0] == '\"',
          "asmtrace_emit prepends {\"k\":\"vmmap\", so the body must not lead "
          "with a comma");
    check("body/anon-omits-name", strstr(buf, "\"name\":\"\"") == NULL,
          "an anonymous row omits `name` rather than carrying an empty one");

    /* Overflow REFUSES rather than truncating mid-token: a half-token is
     * syntactically invalid JSON that a reader reports as a corrupt recording
     * rather than as our bug. */
    check("body/refuses-overflow",
          asmspy_vmmap_body(sp, (size_t)n, total, 1, 0, tiny, sizeof tiny) == -1,
          "a body that will not fit must refuse, never emit a half-token");

    free(sp);

    /* RANK BEFORE CAP — the assertion that matters. Build a table of CAP+1
     * enormous anonymous mappings plus ONE small executable one, so a naive
     * "cap first, then rank" drops the executable row (it is last in file order
     * and the smallest). This is the procinfo failure mode its own comment
     * records: capping first "dropped libc itself while keeping dozens of
     * zero-symbol rows". */
    {
        char *big = malloc(1 << 20);
        size_t o = 0;
        FILE *bf;
        asmspy_vmspan_t *bs = NULL;
        size_t btotal = 0;
        int bn, kept_libc = 0, k;
        check("cap/alloc", big != NULL, "malloc");
        if (!big)
            return 1;
        for (k = 0; k < ASMSPY_VMMAP_CAP + 1; k++)
            o += (size_t)snprintf(big + o, (1u << 20) - o,
                                  "%llx000-%llx000 rw-p 00000000 00:00 0 \n",
                                  (unsigned long long)(0x10000 + k),
                                  (unsigned long long)(0x10100 + k));
        snprintf(big + o, (1u << 20) - o,
                 "7fff00000000-7fff00001000 r-xp 00000000 08:01 9 "
                 "/lib/libc.so.6\n");
        bf = fmemopen(big, strlen(big), "r");
        bn = asmspy_vmmap_parse(bf, &bs, &btotal);
        fclose(bf);
        check("cap/enforced", bn == ASMSPY_VMMAP_CAP,
              "the cap must bound what is KEPT");
        check("cap/total-is-pre-cap", btotal == (size_t)ASMSPY_VMMAP_CAP + 2,
              "spans_total must count rows SEEN, or the truncation magnitude is "
              "unrecoverable — the stated v1 gap in procinfo's `modules`");
        for (k = 0; k < bn; k++)
            if (strcmp(bs[k].name, "libc.so.6") == 0)
                kept_libc = 1;
        check("cap/ranks-before-capping", kept_libc,
              "the one executable row must survive a cap full of larger "
              "anonymous ones — rank over the FULL table, THEN cap");
        /* And the flag must fire when the cap actually bit. */
        check("cap/flags-truncation",
              asmspy_vmmap_body(bs, (size_t)bn, btotal, 1, 0, buf,
                                sizeof buf) == 0 &&
                  strstr(buf, "\"spans_truncated\":true") != NULL,
              "a capped body must say so");
        free(bs);
        free(big);
    }

    /* THE CHANGE GATE. An unchanged address space must digest identically even
     * though the version ordinal has moved on — otherwise the gate never
     * suppresses anything and every invocation re-emits the whole table. This
     * regressed once already: the first implementation hashed the emitted BODY,
     * which carries "version", and live capture re-emitted 23 identical
     * mappings. */
    {
        FILE *f1 = fmemopen((void *)kMaps, strlen(kMaps), "r");
        FILE *f2 = fmemopen((void *)kMaps, strlen(kMaps), "r");
        asmspy_vmspan_t *s1 = NULL, *s2 = NULL;
        size_t t1 = 0, t2 = 0;
        int n1 = asmspy_vmmap_parse(f1, &s1, &t1);
        int n2 = asmspy_vmmap_parse(f2, &s2, &t2);
        char d1[65], d2[65], b1[64 * 1024], b2[64 * 1024];
        fclose(f1);
        fclose(f2);
        asmspy_vmmap_digest(s1, (size_t)n1, t1, 1, d1);
        asmspy_vmmap_digest(s2, (size_t)n2, t2, 1, d2);
        check("gate/stable-across-versions", strcmp(d1, d2) == 0,
              "the same map must digest the same, whatever the version ordinal");
        /* And prove the version really does differ in the body, so the check
         * above is testing what it claims to. */
        asmspy_vmmap_body(s1, (size_t)n1, t1, 1, 0, b1, sizeof b1);
        asmspy_vmmap_body(s2, (size_t)n2, t2, 1, 1, b2, sizeof b2);
        check("gate/body-does-differ-by-version", strcmp(b1, b2) != 0,
              "if the bodies were equal this test would pass vacuously");

        /* A map that genuinely MOVED must digest differently — the gate has to
         * still fire when it should. */
        s2[0].len += 4096;
        asmspy_vmmap_digest(s2, (size_t)n2, t2, 1, d2);
        check("gate/detects-a-real-change", strcmp(d1, d2) != 0,
              "a changed mapping must not be suppressed");
        free(s1);
        free(s2);
    }

    /* A malformed line is skipped, not fatal, and does not count toward total. */
    {
        const char *junk = "not a maps line at all\n"
                           "00400000-00410000 r-xp 00000000 08:01 100 /a\n";
        FILE *jf = fmemopen((void *)junk, strlen(junk), "r");
        asmspy_vmspan_t *js = NULL;
        size_t jt = 0;
        int jn = asmspy_vmmap_parse(jf, &js, &jt);
        fclose(jf);
        check("parse/skips-garbage", jn == 1 && jt == 1,
              "an unparseable line is skipped and not counted");
        free(js);
    }

    if (failures) {
        fprintf(stderr, "test_vmmap: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_vmmap: all checks passed\n");
    return 0;
}
