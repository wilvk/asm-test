/* trace_auto.h — INTERNAL seams for the escalation cascade (no ABI promise).
 * Mirrors the amd_backend.h / ibs_backend.h host-testable-seam convention. */
#ifndef ASMTEST_TRACE_AUTO_INTERNAL_H
#define ASMTEST_TRACE_AUTO_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Rung 1b (MSR-direct LBR) commit decision, extracted so hosts without the
 * LbrExtV2 MSRs can still pin it: an MSR read commits when it succeeded and
 * recorded something — a nonempty truncated partial is returned exactly like
 * a fast-tier truncated partial (HW_OK + truncated), honouring the header's
 * "HW_OK when some tier ran". Only the genuinely-empty truncated read
 * (nothing in-region) falls through to the steppers. 1 = commit. */
int asmtest_trace_auto_msr_commits(int rc, int truncated, int nonempty);

#ifdef __cplusplus
}
#endif
#endif /* ASMTEST_TRACE_AUTO_INTERNAL_H */
