/*
 * test_sha256.c — pins cli/asmtrace_sha256.h against the published FIPS-180-4
 * SHA-256 test vectors. The `.asmtrace` `code` header's routine identity is only
 * as trustworthy as this digest, and a transcription typo in the round constants
 * or the message schedule would produce a WRONG-but-stable hash that the golden
 * corpus would then bless — so the algorithm is checked against the standard's
 * own answers, not against itself.
 */
#include <stdio.h>
#include <string.h>

#include "asmtrace_sha256.h"

static int failures;

static void expect(const char *label, const char *input, size_t len,
                   const char *want) {
    char got[65];
    asmtrace_sha256_hex(input, len, got);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s\n  want %s\n  got  %s\n", label, want, got);
        failures++;
    }
}

int main(void) {
    /* FIPS-180-4 / NIST CSRC example vectors. */
    expect("empty", "", 0,
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect("abc", "abc", 3,
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    expect("two-block",
           "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    /* A 1,000,000-'a' message exercises the streaming update across many blocks
     * (the standard's third example), so a length or padding bug in the final
     * block shows here and not only on tiny inputs. */
    {
        asmtrace_sha256_t s;
        unsigned char d[32];
        char hex[65];
        asmtrace_sha256_init(&s);
        for (int i = 0; i < 1000000; i++) {
            unsigned char a = 'a';
            asmtrace_sha256_update(&s, &a, 1);
        }
        asmtrace_sha256_final(&s, d);
        for (int i = 0; i < 32; i++)
            snprintf(hex + i * 2, 3, "%02x", d[i]);
        hex[64] = '\0';
        static const char *million_a =
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
        if (strcmp(hex, million_a) != 0) {
            fprintf(stderr, "FAIL million-a\n  got %s\n", hex);
            failures++;
        }
    }

    if (failures) {
        fprintf(stderr, "test_sha256: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_sha256: PASS\n");
    return 0;
}
