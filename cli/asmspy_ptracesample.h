/* asmspy_ptracesample.h — the PERF-FREE region picker's tunables and its one
 * pure decision. The entry point itself (asmspy_ptrace_sample) is declared in
 * libasmspy.h, beside the resolver whose symbols it ranks; this header carries
 * the design note, the numbers the design was tuned to, and the Capstone
 * disposition that has to be testable on a host that HAS Capstone.
 *
 * ===========================================================================
 * WHY A THIRD SAMPLER EXISTS
 *
 * `asmspy --dataflow <pid> --auto` needs a hot region before it can arm
 * anything. Both samplers that predate this one — AMD IBS-Op and the portable
 * software clock — reach the SAME perf_event_open (src/ibs_backend.c:369). On a
 * stock Ubuntu box that syscall is refused: kernel.perf_event_paranoid = 4 is a
 * COMPILED-IN default that no file in /etc sets, so `--auto` records zero
 * events and `--sampler=sw` is not an escape hatch. Everything downstream of
 * the picker is already perf-free — given an explicit `func`, the same host
 * emits codeimage + df_step + regstate + df_edge — so the gate is exclusively
 * here. This module removes it: ptrace and /proc, nothing else.
 *
 * ===========================================================================
 * THREE PHASES, BECAUSE RESIDENCY ALONE IS MEASURABLY WRONG
 *
 *   Phase 1  RESIDENCY. SEIZE every tid, PTRACE_INTERRUPT at ~997 Hz, read the
 *            PC, resume. Yields an IP histogram, folded by
 *            asmspy_autoregion_rank_ip (the same pure rank the software-clock
 *            sampler uses — one ranking rule, not two).
 *
 *   Phase 2  CALL-TARGET EXPANSION. A residency histogram is dominated by the
 *            functions that were entered once and never return — main, an event
 *            loop, auto_victim's grind_forever (MEASURED 394:5 against the
 *            correct answer). Arming an entry breakpoint on one of those is the
 *            hang the whole feature exists to avoid. So each shortlisted body is
 *            scanned for DIRECT calls (asmtest_disas_call_target,
 *            src/disasm.c:419-425) and their targets admitted as candidates:
 *            that is what reaches a callee residency never samples.
 *
 *   Phase 3  ARRIVAL CONFIRMATION. An int3 at each candidate's entry, and the
 *            clock started. A candidate that is never reached is DROPPED — not
 *            demoted — because the property the caller needs is exactly "the
 *            entry breakpoint fires promptly", and this phase measures that
 *            property directly instead of predicting it.
 *
 * ===========================================================================
 * THE SIX CORRECTIONS, each one a defect a prototype actually shipped:
 *
 *  1. SIGNALS ARE RE-INJECTED. The prototype waitpid()ed and unconditionally
 *     PTRACE_CONTed with sig=0. man 2 ptrace: "If sig is 0, then a signal is not
 *     delivered." Against a 100 Hz ITIMER_REAL victim that destroyed 89% of the
 *     target's SIGALRMs and collapsed its throughput ~99% (2 utime ticks and 0
 *     forward-progress lines per 2 s, against 200/93 both at baseline and after
 *     detach). The "~1% cost, no perturbation" figure holds only for a
 *     signal-FREE spinner. See ps_resume_stop in the .c.
 *  2. PTRACE_O_TRACECLONE, OR YOU KILL THE USER'S PROCESS. The phase-3 int3 is
 *     shared process text; cli/asmspy_engine.c:2954-2957 spells out what happens
 *     to a thread that was never seized and reaches it — "would take a SIGTRAP
 *     with no tracer and DIE". The prototype passed options 0.
 *  3. /proc/<tid>/stat's utime+stime CANNOT DISCRIMINATE at this window. CLK_TCK
 *     is 100 (10 ms ticks) against a 400 ms window: measured, EVERY thread of
 *     threads_victim shows a delta of 0. /proc/self/schedstat reads "0 0 1"
 *     (kernel.sched_schedstats=0, needs root). The discriminator that works is
 *     /proc/<tid>/syscall — readable precisely because we are the tracer — which
 *     prints the literal "running" for an executing thread and a syscall number
 *     for a blocked one.
 *  4. RANK ON TIME-TO-FIRST-ARRIVAL, not a censored count. Under a per-candidate
 *     hit budget the counts saturate and TIE: measured, tiny_callee (50 arrivals,
 *     residency 1) tied with libc's sched_yield (50 arrivals, residency 1) —
 *     broken only by the very metric the entry rule exists to replace.
 *     Time-to-first separates them cleanly: 105 us vs 6125 us.
 *  5. asmtest_disas_call_target IS CAPSTONE-GATED and returns 0 SILENTLY without
 *     it, which from phase 2's side is indistinguishable from "this function
 *     makes no direct calls". So it must SAY so — asmspy_ps_expand_note below.
 *  6. THE 'R' STATE FILTER BOUNDS COST, NOT CORRECTNESS. 100 threads cost the
 *     same as 1 (0.27% of the window), but with the filter ON the ranking still
 *     put clock_nanosleep above worker 11:3. It is kept for cost. It is not
 *     credited with accuracy — phase 3 is what buys accuracy.
 */
#ifndef ASMSPY_PTRACESAMPLE_H
#define ASMSPY_PTRACESAMPLE_H

#include "libasmspy.h" /* asmspy_symtab_t, asmspy_autocand_t, the entry point */

/* Interrupt rate. 997 Hz rather than 1000: a prime-ish rate cannot phase-lock
 * with a target's own 100/250/1000 Hz timer and sample the same point of its
 * loop forever, which is the classic way a "statistical" profiler becomes a
 * deterministic one. */
#define ASMSPY_PS_HZ 997

/* The default residency window when a caller passes window_ms <= 0. 400 ms is
 * AUTO_WINDOW_MS, the same window the other two samplers use — the point of the
 * chain is that the three answer the same question, not three different ones. */
#define ASMSPY_PS_DEFAULT_WINDOW_MS 400

/* How many candidates reach phase 3. Phase 1's shortlist plus phase 2's
 * expansion, deduped. Every one of them costs up to ASMSPY_PS_CONFIRM_MS of
 * wall time when it never arrives, so this bounds the confirm phase. */
#define ASMSPY_PS_MAX_CAND 16

/* Phase 1 shortlist depth. Wider than the handful a caller wants back, because
 * phase 2 expands from these: the residency winner is usually the WRONG answer
 * whose CALLEE is the right one, so the value of a row here is what it calls,
 * not what it is. */
#define ASMSPY_PS_SHORTLIST 6

/* Per-candidate phase-3 budget. A candidate that has not been entered within
 * this long is treated as not-arriving; the correct pick on every measured
 * victim arrives in tens of MICROseconds, and the losers are the ones that burn
 * the whole slice. */
#define ASMSPY_PS_CONFIRM_MS 50

/* Per-candidate arrival budget: stop counting after this many. Deliberately
 * SMALL, and correction 4 is why — the count saturates here by construction, so
 * it is evidence that the entry is live, never the ranking key. Each arrival
 * costs the target a trap + a single-step + a re-plant, so a large budget would
 * buy nothing and charge the user for it. */
#define ASMSPY_PS_HIT_BUDGET 4

/* Phase 2's disposition, as a pure decision (correction 5).
 *
 * Returns NULL when call-target expansion can run, or the sentence the caller
 * should report when it cannot. Pure and separate from the phase itself so that
 * BOTH branches are pinned by cli/test_ptracesample.c on a host that has
 * Capstone and could never otherwise reach the refusal — a silent degradation
 * to a residency-only ranking is precisely the failure this names, so "we could
 * not test it here" would have been the same bug one level up. */
static inline const char *asmspy_ps_expand_note(int have_disas) {
    return have_disas ? NULL
                      : "no Capstone — call-target expansion skipped, so this "
                        "ranking is residency-only";
}

/* MAY PHASE 3 ARM? The single most dangerous decision in this module, as a pure
 * one.
 *
 * Phase 3's int3 is one byte in SHARED process text. Every thread of the target
 * can execute it, so every thread of the target must be OURS — a task that
 * reaches it without a tracer takes a SIGTRAP whose default action kills the
 * whole process (cli/asmspy_engine.c:2954-2957 states exactly this). "We seized
 * most of them" is not a weaker version of safety, it is the absence of it.
 *
 * `fully_seized` is false whenever ANY task of the target may be untraced, OR
 * may become so before the window ends: a PTRACE_SEIZE refused for a reason
 * other than the task having exited, a seize scan that never converged, a
 * followed child dropped for want of table space — and a thread count sitting
 * within PS_ARM_HEADROOM of the PS_MAX_THREADS cap, because a target one
 * thread-pool growth away from an untraced task over an 800 ms window is not
 * safe to arm either. It is re-read AFTER the window as well as before: a run
 * that LOST coverage part-way confirmed only some candidates against only some
 * of the thread set, which is not a measurement to present as one.
 *
 * Returns NULL when arming is safe, or the sentence the caller must report when
 * it is not. Pure, so cli/test_ptracesample.c pins both branches — including the
 * refusal, which on a healthy 4-thread victim is otherwise unreachable. */
static inline const char *asmspy_ps_arm_note(int fully_seized) {
    return fully_seized
               ? NULL
               : "the target's thread set could not be fully seized, so the "
                 "int3 arrival check was skipped and these candidates are "
                 "UNCONFIRMED — a process-wide trap over a task we do not "
                 "trace "
                 "kills the process";
}

#endif /* ASMSPY_PTRACESAMPLE_H */
