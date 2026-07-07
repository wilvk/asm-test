/*
 * corpus_routines.c — name -> address lookup for the conformance routines.
 *
 * Lets a binding obtain a routine pointer with one ordinary call
 * (asmtest_corpus_routine("add_signed")) instead of each FFI's symbol-address
 * mechanism. Compiled into the libasmtest_corpus fixture lib alongside the
 * routine objects (examples/{add,args,flags,fp,simd,structs}.s).
 */
#include <string.h>

extern long add_signed(long, long);
extern long sum_via_rbx(long, long);
extern long clobbers_rbx(long, long);
extern long set_carry(void);
extern long clear_carry(void);
extern double fp_add(double, double);
extern void vec_add4f(void);
extern long read_fault(const long *);
extern double int_to_double(long);
extern double mix_scale(long, double); /* mixed int+FP args (capture_mix) */
extern long sum3(long, long, long);
extern long sum8(long, long, long, long, long, long, long, long);
extern long sum10(long, long, long, long, long, long, long, long, long, long);
extern void make_big(void);  /* struct big(24B) via hidden sret ptr; address only */
extern void make_pair(void); /* struct pair(16B) in registers; address only */
#if defined(__x86_64__)
extern void vec_add4d(void); /* AVX2 256-bit (Track D); x86-64 only */
extern void vec_add8d(void); /* AVX-512 512-bit (Track D); x86-64 only */
#endif

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
#if defined(__x86_64__)
    if (!strcmp(name, "vec_add4d"))
        return (void *)vec_add4d;
    if (!strcmp(name, "vec_add8d"))
        return (void *)vec_add8d;
#endif
    if (!strcmp(name, "read_fault"))
        return (void *)read_fault;
    if (!strcmp(name, "int_to_double"))
        return (void *)int_to_double;
    if (!strcmp(name, "mix_scale"))
        return (void *)mix_scale;
    if (!strcmp(name, "sum3"))
        return (void *)sum3;
    if (!strcmp(name, "sum8"))
        return (void *)sum8;
    if (!strcmp(name, "sum10"))
        return (void *)sum10;
    if (!strcmp(name, "make_big"))
        return (void *)make_big;
    if (!strcmp(name, "make_pair"))
        return (void *)make_pair;
    return (void *)0;
}
