/*
 * debug.h — INTERNAL env-gated debug logging for the AMD/hwtrace trace tier
 * (docs/internal/plans/amd-tracing-followup-plan.md Phase 4). NOT a public header: it
 * ships nothing in include/ and carries no ABI promise, mirroring the src/amd_backend.h
 * and src/stealth_helper.h internal-header pattern.
 *
 * With this many stacked gates (vendor -> PMU -> perfmon_v2 -> kernel -> caps -> freeze
 * bit) a self-skip is opaque; this makes the skip, the backend/mode chosen, the Tier-A
 * vs Tier-B pick, and every truncation reason diagnosable. It is ZERO overhead when the
 * env var is unset: the ASMTEST_HWDBG macro short-circuits on a cached flag before it
 * ever touches the varargs formatter. Precedent: src/codeimage.c's ASMTEST_CODEIMAGE_DEBUG.
 *
 * Enable with ASMTEST_HWTRACE_DEBUG=1 (tier-wide) or ASMTEST_AMD_DEBUG=1 (AMD alias);
 * lines go to stderr, one per event, prefixed "[asmtest hwtrace] <func>: ".
 *
 * SIGNAL-SAFETY: NOT async-signal-safe (stdio + getenv). Never call ASMTEST_HWDBG from
 * the single-step SIGTRAP handler (ss_backend.c ss_on_sigtrap); every current call site
 * is ordinary thread context.
 */
#ifndef ASMTEST_HWTRACE_DEBUG_H
#define ASMTEST_HWTRACE_DEBUG_H

/* 1 when logging is enabled (getenv is read ONCE and cached), else 0. */
int asmtest_hwtrace_debug_enabled(void);

/* printf-family stderr logger; `fn` is the calling function name (the line prefix).
 * The printf format attribute makes the compiler check the varargs at every call. */
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void asmtest_hwtrace_debugf(const char *fn, const char *fmt, ...);

/* One-line, key=val by convention. No trailing newline in the format — debugf adds it. */
#define ASMTEST_HWDBG(...)                                                     \
    do {                                                                       \
        if (asmtest_hwtrace_debug_enabled())                                   \
            asmtest_hwtrace_debugf(__func__, __VA_ARGS__);                     \
    } while (0)

#endif /* ASMTEST_HWTRACE_DEBUG_H */
