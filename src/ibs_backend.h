/*
 * ibs_backend.h — INTERNAL window primitives for the statistical AMD IBS-Op lane.
 *
 * NOT a public header: it ships nothing in include/ and carries no ABI promise
 * (mirroring src/amd_backend.h / src/stealth_helper.h). The PUBLIC IBS surface is
 * include/asmtest_ibs.h (available/decode/survey_pid/survey_process). This header
 * adds the two primitives hwtrace.c needs for its Zen-2 F6 fallback and that do NOT
 * fit the survey_* shape: a window that ARMS IBS-Op on the calling thread, lets the
 * caller run its own workload inline, then DRAINS — the same begin/run/end shape as
 * asmtest_hwtrace_sample_begin_amd/_end_amd, so the branch-stack survey and its IBS
 * fallback share one control flow.
 *
 * Both are Linux/x86-64/AMD-only; everywhere else they self-skip (EUNAVAIL), like the
 * rest of the lane. They reuse asmtest_ibs_opts_t / asmtest_ibs_survey_t (freed with
 * asmtest_ibs_survey_free) from the public header.
 */
#ifndef ASMTEST_IBS_BACKEND_H
#define ASMTEST_IBS_BACKEND_H

#include "asmtest_ibs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Arm IBS-Op on the CALLING thread and return an opaque window handle in *ctx_out.
 * Sampling is live on return, so the caller runs its window body next and calls
 * asmtest_ibs_window_end to stop + collect. Returns ASMTEST_IBS_OK, ASMTEST_IBS_EINVAL
 * on a NULL out-param, or ASMTEST_IBS_EUNAVAIL when IBS-Op is unavailable / cannot open.
 * `opts` may be NULL (defaults). Pair EVERY OK with exactly one asmtest_ibs_window_end
 * (which frees the handle). */
int asmtest_ibs_window_begin(const asmtest_ibs_opts_t *opts, void **ctx_out);

/* Disable the window `ctx` (from asmtest_ibs_window_begin), drain its ring into the
 * aggregated edge histogram *out (sorted by descending count, same shape as a survey),
 * and free the handle. *out is always zero-initialised first (free it with
 * asmtest_ibs_survey_free). Returns ASMTEST_IBS_OK, or ASMTEST_IBS_EINVAL on a NULL
 * argument. */
int asmtest_ibs_window_end(void *ctx, asmtest_ibs_survey_t *out);

#ifdef __cplusplus
}
#endif
#endif /* ASMTEST_IBS_BACKEND_H */
