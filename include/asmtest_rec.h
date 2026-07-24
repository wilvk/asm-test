/*
 * asmtest_rec.h — record-mode producer glue for the emulator tier
 * (docs/internal/gui/06-doors-and-learning.md T7).
 *
 * The runner arms recording (asmtest_record_path / asmtest_note_recording in
 * asmtest.h) but records nothing itself — it links no engine, and that is the
 * whole scoping mechanism: FRAMEWORK_OBJS stays engine-free, and only a suite
 * that ALREADY links the emulator tier can link this glue and produce
 * recordings. Everywhere else `--record-dir` is accepted and simply produces
 * nothing, which is the honest degrade rather than a silent one.
 *
 * Dependency tiers, deliberately: this header is pure (asmtest_trace.h +
 * asmtest_emu.h, both plain declarations); the TU behind it links the shared
 * `.asmtrace` writer (cli/asmtrace_ndjson.h), so field order has one owner and
 * a suite's recording and the golden corpus cannot spell a `trace` event
 * differently.
 */
#ifndef ASMTEST_REC_H
#define ASMTEST_REC_H

#include <stddef.h>

#include "asmtest_emu.h"
#include "asmtest_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serialize one emulator run to the CURRENT test's recording path.
 *
 * A NO-OP returning 0 when asmtest_record_path() is NULL (recording is not
 * armed, or no test is running) — so a suite can call this unconditionally.
 * Otherwise it writes the trace, the run's result and the tier's provenance
 * through the shared writer, calls asmtest_note_recording(path, -1), and
 * returns 1. Returns -1 on an I/O failure, with nothing noted: a recording that
 * could not be written must not be named in a failure report.
 *
 * `code`/`code_len` are the routine's bytes as they were handed to the
 * emulator; they are what makes the recorded offsets resolvable, and where
 * Capstone is linked they also supply the per-step `disasm` text (D10).
 *
 * The step id is -1 in v1 — "this test wrote this recording, and which step is
 * to blame is not known". A viewer opens the last step when a failure carries
 * no id; the Wave-2 blame producer supplies real ids through this same seam.
 */
int asmtest_rec_emu(const emu_trace_t *tr, const emu_result_t *res,
                    const void *code, size_t code_len);

#ifdef __cplusplus
}
#endif
#endif /* ASMTEST_REC_H */
