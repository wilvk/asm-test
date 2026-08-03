/* asmspy_syscall_name.h — syscall-number -> name lookup, shared.
 *
 * The compiling host's own <sys/syscall.h> names every syscall it knows:
 * cli/gen-syscall-names.sh emits one ready-to-use designated-initializer
 * entry per __NR_name macro into the generated
 * $(BUILD)/asmspy_syscall_names.inc, so the NUMBER always comes from the
 * SAME headers as the name — this cannot drift out of step with the kernel
 * the way a hand-written table would. Sparse: a number with no entry yields
 * NULL, and the caller renders the decimal instead (an unknown number is
 * honest; an invented name is not).
 *
 * Header-only because TWO translation units need this table and must NOT
 * share a link edge:
 *   - cli/asmspy_engine.c's --log syscall decoder (the table's original
 *     owner, before this split);
 *   - cli/asmspy_proc.c's attach-free process snapshot (asmspy_procinfo),
 *     whose /proc/<tid>/syscall reader wants the same name beside the same
 *     number.
 * cli/test_procinfo.c links ONLY asmspy_proc.o (mk/cli.mk) — deliberately,
 * since the whole point of that test is proving the snapshot never ptraces.
 * Linking asmspy_engine.o for this table would drag every ptrace engine into
 * an attach-free test, exactly backwards. A header both TUs include instead
 * compiles the (small, sparse) table twice — once per TU, each copy `static`
 * — rather than forcing an unwanted link edge or a two-definitions clash.
 * Same file-extraction discipline as the other cli/asmspy_*.h view-model
 * modules (asmspy_arch.h, asmspy_ghash.h, asmspy_treefilter.h, …).
 *
 * The generated .inc is #included directly inside the array literal below
 * with no local macro of this TU's own: an earlier draft defined a small
 * `#define`-based helper here to expand the .inc's entries, but a
 * function-like macro whose replacement starts with an open bracket makes
 * clang-format's language guesser misdetect a .h file as Objective-C (a
 * measured tool quirk, specific to the .h extension — the identical text in
 * a .c file never triggers it). Moving the bracket syntax into the generator
 * output removes the only such macro from this file, sidestepping the
 * misdetection entirely rather than fighting it.
 */
#ifndef ASMSPY_SYSCALL_NAME_H
#define ASMSPY_SYSCALL_NAME_H

#include <stddef.h>
#include <sys/syscall.h>

/* Sparse, number-indexed. Numbers come from __NR_* (this compiling host's own
 * kernel headers, via sys/syscall.h); names from the generated .inc. A number
 * with no syscall at it stays NULL. `static`: each including TU gets its own
 * private copy, so linking both together never collides. */
static const char *const asmspy_syscall_names_tbl[] = {
#include "asmspy_syscall_names.inc"
};

/* nr -> its name ("read", "openat", ...), or NULL when `nr` is negative, out
 * of range, or the compiling host's headers named no syscall there. */
static inline const char *asmspy_syscall_name(long nr) {
    if (nr < 0 || (size_t)nr >= sizeof asmspy_syscall_names_tbl /
                                    sizeof asmspy_syscall_names_tbl[0])
        return NULL;
    return asmspy_syscall_names_tbl[nr];
}

#endif /* ASMSPY_SYSCALL_NAME_H */
