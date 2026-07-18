/*
 * bs_recon.h — INTERNAL block-step terminator scanner, shared by the region and
 * foreign-memory (windowed) ptrace block-step reconstructors in src/ptrace_backend.c
 * AND by the in-process BTF pairing post-pass (src/ss_btf.c, W3). NOT a public header:
 * it ships nothing in include/ and carries no ABI promise, mirroring src/amd_backend.h.
 *
 * Extracted verbatim from ptrace_backend.c (no behavior change): the only signature
 * change is the leading `arch` parameter, replacing that file's TU-local
 * PTRACE_TRACE_ARCH macro (which does not exist here). Every caller passes
 * PTRACE_TRACE_ARCH or ASMTEST_ARCH_X86_64 unchanged.
 */
#ifndef ASMTEST_BS_RECON_H
#define ASMTEST_BS_RECON_H

#include <stddef.h>
#include <stdint.h>

#include "asmtest_trace.h" /* asmtest_arch_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Outcome of a block reconstruction. AMBIGUOUS is honest degradation, not an error:
 * what definitely ran is recorded and the caller flags the capture truncated. */
#define ASMTEST_BS_FAIL                                                        \
    0                   /* undecodable / unreadable / desync / no terminator  */
#define ASMTEST_BS_OK 1 /* terminator proven unique; the run is exact         */
#define ASMTEST_BS_AMBIGUOUS                                                   \
    2 /* >1 candidate terminator: prefix recorded, truncate */

/* How one instruction of a straight-line run relates to the observed next stop. */
typedef enum {
    BR_NONE = 0,    /* not a branch: the run continues through it            */
    BR_COND_MISS,   /* conditional, static target != next-stop: NOT taken    */
    BR_COND_HIT,    /* conditional, static target == next-stop: a CANDIDATE  */
    BR_HARD_HIT,    /* always-taken direct branch, target == next-stop       */
    BR_HARD_MISS,   /* always-taken direct branch, target != next-stop       */
    BR_HARD_UNKNOWN /* ret / indirect: always taken, target not static       */
} br_kind_t;

/* Classify the instruction at `code[off]` (running at base_addr + off) against the
 * observed next stop. *len_out gets its byte length, 0 if undecodable. Shared by both
 * reconstructors: the region form passes its whole snapshot with base_addr = base_ip and
 * off = the region offset; the windowed form passes a foreign byte window with
 * base_addr = the absolute address and off = 0. Both then get ABSOLUTE branch targets. */
br_kind_t classify_branch(asmtest_arch_t arch, const uint8_t *code,
                          size_t code_len, uint64_t base_addr, uint64_t off,
                          uint64_t next_pc, size_t *len_out);

/* Find the instruction that terminated the straight-line run starting at `from_off`, as
 * the amd-tracing-plan's "Same-target-conditional ambiguity -> truncated" rule requires.
 *
 * A block-step #DB is TRAP-class: the stop RIP is the branch TARGET, so the run went from
 * `from_off` up to some taken branch reaching `next_pc`. The old greedy rule stopped at
 * the FIRST direct branch whose static target == next_pc — which silently drops
 * instructions on the `||` / dual-guard shape (`je T; …; je T`, first not taken, second
 * taken), because both statically target T and BTF gives no signal for which was taken.
 *
 * So: scan the run counting CANDIDATES (direct branches whose static target == next_pc),
 * hard-stopping at the first always-taken instruction, since nothing past it can run.
 *   - exactly 1 candidate  -> proven terminator (the old behaviour, now proven not guessed)
 *   - 0 candidates         -> the hard stop is it, iff its target is not static
 *                             (ret/indirect). An always-taken DIRECT branch going
 *                             somewhere other than next_pc contradicts the observation.
 *   - more than 1          -> AMBIGUOUS: *term_out = the first, so the caller can still
 *                             record the prefix that definitely ran, and truncate.
 * Note a `ret`/indirect only HARD-STOPS the scan; it is not itself counted as a candidate
 * (it targets nothing statically), so a taken conditional followed by the function's ret
 * stays unambiguous. The residual: a conditional that was NOT taken, followed by a
 * ret/indirect whose runtime target happens to equal that conditional's static target,
 * resolves to the conditional. That coincidence is not statically decidable at all, and is
 * far narrower than the dual-guard shape this closes.
 *
 * Anything that stops the scan from PROVING uniqueness once a candidate is in hand —
 * undecodable bytes (the fall-through may be jump-table data, not code), running off the
 * region, or the walk ceiling — is reported AMBIGUOUS, never OK: an honest truncated beats
 * a silently short stream. Returns ASMTEST_BS_FAIL / ASMTEST_BS_OK / ASMTEST_BS_AMBIGUOUS. */
int asmtest_bs_scan_terminator(asmtest_arch_t arch, const uint8_t *code,
                               size_t len, uint64_t base_ip, uint64_t from_off,
                               uint64_t next_pc, uint64_t *term_out);

/* W3 (src/ss_btf.c): pair a raw stream of BTF stop addresses into (from,to) branch
 * waypoints via asmtest_bs_scan_terminator, for asmtest_amd_decode_stitched. Guarded
 * (not just declared unconditionally) because struct perf_branch_entry is Linux/x86-64
 * only — the guard must match where the definition lives (ss_btf.c's own #if), not just
 * where the type exists, or this would name a symbol with no body elsewhere. */
#if defined(__linux__) && defined(__x86_64__)
#include <linux/perf_event.h> /* struct perf_branch_entry */

/* Pair adjacent stops into waypoints: skip leading stops outside [base_ip,base_ip+len)
 * (glue between arming and the call into the region — the call itself is a taken
 * branch, so the first in-region stop is normally offset 0), then for each adjacent
 * pair (prev,s) resolve the terminator between them. ASMTEST_BS_OK appends
 * {.from = base_ip+term, .to = s} and continues; ASMTEST_BS_AMBIGUOUS/_FAIL sets *gap
 * and stops pairing (a lost trap leaves the next stop unreachable — the context-switch
 * signature). The first out-of-region s ends pairing with that final (exit) pair
 * included. Returns the pair count, newest-first (out[0] = the last pair formed) —
 * the order asmtest_amd_stitch's own reversal produces and amd_replay consumes. Every
 * entry is zero-initialized before from/to are set (only those two fields are
 * meaningful; an uninitialized rest could be misread as spec/mispred metadata by the
 * replay filters). Pure — no MSR I/O, no signals — host-testable with scripted streams. */
size_t asmtest_ss_btf_pair_stops(const uint8_t *code, size_t len,
                                 uint64_t base_ip, const uint64_t *stops,
                                 size_t nstops, struct perf_branch_entry *out,
                                 size_t out_cap, int *gap);
#endif

#ifdef __cplusplus
}
#endif
#endif /* ASMTEST_BS_RECON_H */
