/* cpp_victim.cpp — a C++ victim whose hot function has a MANGLED ELF symbol, for
 * asmspy's C++-demangling smoke. demo::hot_loop(int) mangles to
 * _ZN4demo8hot_loopEi; asmspy's resolver must render it demangled
 * ("demo::hot_loop(int)"), not leak the raw mangled name. Opts in via
 * PR_SET_PTRACER_ANY like the other example victims so attach works in a plain
 * container.
 */
#include <cstdio>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

namespace demo {
/* noinline so it survives as a distinct STT_FUNC symbol to resolve + demangle */
__attribute__((noinline)) long hot_loop(int n) {
    long s = 0;
    for (int i = 0; i < n; i++)
        s += (long)i * i;
    return s;
}
} // namespace demo

int main() {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    std::fprintf(stderr, "cpp_victim pid=%d\n", (int)getpid());
    std::fflush(stderr);
    volatile long sink = 0;
    for (;;) {
        sink += demo::hot_loop(1000);
        usleep(200 * 1000);
    }
    return 0;
}
