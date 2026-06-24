/*
 * corpus_routines.c — name -> address lookup for the conformance routines.
 *
 * Lets a binding obtain a routine pointer with one ordinary call
 * (asmtest_corpus_routine("add_signed")) instead of each FFI's symbol-address
 * mechanism. Compiled into the libasmtest_corpus fixture lib alongside the
 * routine objects (examples/{add,flags,fp,simd}.s).
 */
#include <string.h>

extern long add_signed(long, long);
extern long sum_via_rbx(long, long);
extern long clobbers_rbx(long, long);
extern long set_carry(void);
extern long clear_carry(void);
extern double fp_add(double, double);
extern void vec_add4f(void);

void *asmtest_corpus_routine(const char *name) {
    if (!name)
        return (void *)0;
    if (!strcmp(name, "add_signed"))
        return (void *)add_signed;
    if (!strcmp(name, "sum_via_rbx"))
        return (void *)sum_via_rbx;
    if (!strcmp(name, "clobbers_rbx"))
        return (void *)clobbers_rbx;
    if (!strcmp(name, "set_carry"))
        return (void *)set_carry;
    if (!strcmp(name, "clear_carry"))
        return (void *)clear_carry;
    if (!strcmp(name, "fp_add"))
        return (void *)fp_add;
    if (!strcmp(name, "vec_add4f"))
        return (void *)vec_add4f;
    return (void *)0;
}
