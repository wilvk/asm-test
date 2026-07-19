/*
 * pin_apx_decode.c — the UNGATED half of the APX fixture (T8). Runs on ANY x86-64
 * host (it never executes the APX bytes, only XED-decodes them), so every lane run
 * proves pin/pin_apx_fixture.h's bytes are genuinely APX before the execution
 * halves gate off on silicon.
 *
 * Two modes:
 *   (no args)  TAP decode assertion: xed_decode each instruction of APX_ROUTINE
 *              and assert xed_classify_apx() (Pin kit's XED) tags the REX2/EGPR
 *              ones — the kit-verified "genuinely APX" proof. (The source doc
 *              suggested asserting the extension enum is XED_EXTENSION_APXEVEX/
 *              APXLEGACY, but the 4.2 kit tags a REX2-promoted legacy MOV/ADD as
 *              iclass MOV/ADD, ext BASE; xed_classify_apx is the purpose-built
 *              predicate — "True for APX instructions ... EGPRs, REX2 and
 *              encodings treated as illegal on non-APX systems".)
 *   cpuid      Silent CPUID APX_F probe: exit 0 iff this CPU supports APX, else 1.
 *              The pintool-apx-test recipe branches on it to gate the execution
 *              halves (a REAL hardware gate; running APX code on a non-APX CPU is
 *              a #UD). APX_F is CPUID.(EAX=07H,ECX=1):EDX[21] per the Intel APX
 *              Architecture Specification.
 *
 * Compiled with the Pin kit's XED headers/lib (-I$PIN_HOME/extras/xed-intel64/...).
 */
#include "xed/xed-interface.h"

#include "pin_apx_fixture.h"

#include <cpuid.h>
#include <stdio.h>
#include <string.h>

/* CPUID.(EAX=07H,ECX=1):EDX[21] = APX_F. */
static int host_has_apx(void) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid_count(7, 1, &a, &b, &c, &d))
        return 0;
    return (d & (1u << 21)) != 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "cpuid") == 0)
        return host_has_apx() ? 0 : 1;

    xed_tables_init();

    int checks = 0, failures = 0, decoded = 0, apx = 0, decode_err = 0;
    unsigned off = 0;
    while (off < sizeof APX_ROUTINE) {
        xed_decoded_inst_t x;
        xed_decoded_inst_zero(&x);
        xed_decoded_inst_set_mode(&x, XED_MACHINE_MODE_LONG_64,
                                  XED_ADDRESS_WIDTH_64b);
        xed_error_enum_t e =
            xed_decode(&x, APX_ROUTINE + off, sizeof APX_ROUTINE - off);
        if (e != XED_ERROR_NONE) {
            decode_err++;
            break;
        }
        decoded++;
        if (xed_classify_apx(&x))
            apx++;
        off += xed_decoded_inst_get_length(&x);
    }

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        checks++;                                                              \
        printf((cond) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, msg);     \
        if (!(cond))                                                           \
            failures++;                                                        \
    } while (0)

    CHECK(decode_err == 0 && decoded == APX_INSN_COUNT,
          "APX_ROUTINE decodes cleanly under the Pin kit's XED (4 insns)");
    CHECK(
        apx == APX_APX_INSN_COUNT,
        "APX_ROUTINE decodes as APX (XED xed_classify_apx: 3 EGPR/REX2 insns)");
#undef CHECK

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
