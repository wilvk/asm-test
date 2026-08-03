/* test_serve_record.c — a serve session must record as ONE .asmtrace file: one
 * header, one end footer, and the engine's own events, with the wire protocol's
 * control lines left out.
 *
 * Before this, each `start` opened its own recorder, so a session that changed
 * engine wrote a header PER ENGINE and the result was not loadable as a
 * recording at all (load_recording_file reads line 1). That is what stopped a
 * capture carrying a code plane, a worldline AND kernel crossings from
 * existing: no single engine emits all three (`trace` gives codeimage + trace,
 * `log` gives syscalls), so such a capture must span two. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static void check(const char *what, int cond, const char *why) {
    if (!cond) {
        fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

static int count_lines(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    char buf[65536];
    int n = 0;
    if (!f)
        return -1;
    while (fgets(buf, sizeof buf, f))
        if (strstr(buf, needle))
            n++;
    fclose(f);
    return n;
}

static int first_line_is_header(const char *path) {
    FILE *f = fopen(path, "r");
    char buf[65536];
    int ok = 0;
    if (!f)
        return 0;
    if (fgets(buf, sizeof buf, f))
        ok = strstr(buf, "\"asmtrace\":1") != NULL;
    fclose(f);
    return ok;
}

int main(void) {
    const char *out = "build/test-serve-record.asmtrace";
    char cmd[1024];
    remove(out);

    /* `launch` (fork + PTRACE_TRACEME) needs no CAP_SYS_PTRACE, so this runs in
     * any lane. The two-engine union is exercised by the 61 T7c fixture
     * recording, which needs an attach. */
    snprintf(cmd, sizeof cmd,
             "{ printf '{\"cmd\":\"launch\",\"mode\":\"trace\","
             "\"argv\":[\"build/serve_record_target\"],\"func\":\"work\","
             "\"max\":20}\\n'; sleep 3; "
             "printf '{\"cmd\":\"stop\"}\\n'; sleep 1; "
             "printf '{\"cmd\":\"quit\"}\\n'; } | "
             "build/asmspy --serve --record=%s >/dev/null 2>&1",
             out);
    if (system(cmd) != 0)
        fprintf(stderr, "note: serve exited non-zero (checked below anyway)\n");

    check("the session wrote a recording at all", access(out, R_OK) == 0,
          "--serve --record produced no file");
    if (access(out, R_OK) != 0) {
        fprintf(stderr, "test_serve_record: %d failure(s)\n", ++failures);
        return 1;
    }

    check("line 1 is the recording header", first_line_is_header(out),
          "load_recording_file reads line 1; a protocol line there makes the "
          "file unloadable");
    check("exactly ONE header for the whole session",
          count_lines(out, "\"asmtrace\":1") == 1,
          "a header per engine is what made a multi-engine capture unloadable");
    check("exactly ONE end footer", count_lines(out, "\"k\":\"end\"") == 1,
          "a footer per engine claims the recording ended more than once");
    check("the wire protocol's control lines stay OUT of the recording",
          count_lines(out, "\"k\":\"cmd\"") == 0 &&
              count_lines(out, "\"k\":\"session\"") == 0,
          "cmd/session are the serve PROTOCOL, not recorded events: the client "
          "stream keeps them, the file must not");
    check("the engine's own events are in the file",
          count_lines(out, "\"k\":\"trace\"") > 0,
          "the recording carries none of the engine's events");

    if (failures) {
        fprintf(stderr, "test_serve_record: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_serve_record: all checks passed\n");
    return 0;
}
