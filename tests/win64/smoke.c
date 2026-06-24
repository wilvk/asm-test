/* tests/win64/smoke.c — Phase 0 substrate smoke for the native Win64 tier.
 *
 * Cross-compiled with x86_64-w64-mingw32-gcc into a Windows PE and run under
 * Wine. Calls a NASM `-f win64` leaf through the real Microsoft x64 ABI and
 * checks the result — proving cross-assemble -> cross-link -> Wine works end to
 * end on the existing Linux CI with no Windows host, before the trampoline lands.
 *
 * Note: Windows is LLP64, so `long` is 32-bit here; the native Win64 tier uses a
 * fixed 64-bit type for the register-width contract. We use `long long` (64-bit
 * on Win64) so this smoke matches the RAX/RCX widths the asm leaf actually uses.
 */
#include <stdio.h>

extern long long win64_add3(long long x);

int main(void) {
    long long got = win64_add3(39);
    if (got != 42) {
        printf("win64 smoke: FAIL win64_add3(39) = %lld, want 42\n", got);
        return 1;
    }
    printf("win64 smoke: ok — win64_add3(39) = %lld (Microsoft x64 ABI via Wine)\n",
           got);
    return 0;
}
