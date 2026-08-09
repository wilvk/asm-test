/*
 * asmtrace_sha256.h — a self-contained SHA-256, for the `.asmtrace` `code`
 * header's routine-identity hash (docs/internal/archive/gui/28-schema-freeze-completion.md
 * R1 T1).
 *
 * WHY THIS IS HERE and not a dependency. The `.asmtrace` writer TU is PURE C11 +
 * stdio by design (asmtrace_ndjson.h) so the Author-mode corpus recorder compiles
 * it anywhere the emulator runs. A `code` header names a routine by the SHA-256 of
 * its bytes, so two recordings can be proven the same routine (or refused as
 * different) — and a field named `sha256` must actually contain a SHA-256, not a
 * cheaper content hash a cross-language reader would misread. The repo carried no
 * SHA-256 (only asmspy_ghash.h's splitmix64 finalizer, a hash-table router, not a
 * digest), so this vendors one: ~110 lines of the public FIPS-180-4 algorithm,
 * header-only static-inline exactly like asmspy_ghash.h / asmspy_dataview.h, no
 * external dependency, no link surface. cli/test_sha256.c pins it against the
 * published FIPS test vectors so a typo cannot pass silently.
 *
 * Not a security primitive: this hashes a routine's own bytes for IDENTITY, never
 * a secret or a MAC. SHA-256 is used because it is the standard content digest a
 * reader in any language already has, so the `code.sha256` field is portable.
 */
#ifndef ASMTRACE_SHA256_H
#define ASMTRACE_SHA256_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t h[8];
    uint64_t nbits;
    uint8_t buf[64];
    size_t nbuf;
} asmtrace_sha256_t;

static inline uint32_t asmtrace_sha256_ror(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

static inline void asmtrace_sha256_init(asmtrace_sha256_t *s) {
    s->h[0] = 0x6a09e667u;
    s->h[1] = 0xbb67ae85u;
    s->h[2] = 0x3c6ef372u;
    s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu;
    s->h[5] = 0x9b05688cu;
    s->h[6] = 0x1f83d9abu;
    s->h[7] = 0x5be0cd19u;
    s->nbits = 0;
    s->nbuf = 0;
}

static inline void asmtrace_sha256_block(asmtrace_sha256_t *s,
                                         const uint8_t *p) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
        0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
        0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
        0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
        0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
        0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
        0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
        0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = asmtrace_sha256_ror(w[i - 15], 7) ^
                      asmtrace_sha256_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = asmtrace_sha256_ror(w[i - 2], 17) ^
                      asmtrace_sha256_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4],
             f = s->h[5], g = s->h[6], hh = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = asmtrace_sha256_ror(e, 6) ^ asmtrace_sha256_ror(e, 11) ^
                      asmtrace_sha256_ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + k[i] + w[i];
        uint32_t S0 = asmtrace_sha256_ror(a, 2) ^ asmtrace_sha256_ror(a, 13) ^
                      asmtrace_sha256_ror(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += hh;
}

static inline void asmtrace_sha256_update(asmtrace_sha256_t *s,
                                          const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    s->nbits += (uint64_t)len * 8;
    while (len) {
        size_t n = 64 - s->nbuf;
        if (n > len)
            n = len;
        for (size_t i = 0; i < n; i++)
            s->buf[s->nbuf + i] = p[i];
        s->nbuf += n;
        p += n;
        len -= n;
        if (s->nbuf == 64) {
            asmtrace_sha256_block(s, s->buf);
            s->nbuf = 0;
        }
    }
}

/* Finalize into 32 raw bytes. */
static inline void asmtrace_sha256_final(asmtrace_sha256_t *s,
                                         uint8_t out[32]) {
    uint64_t nbits = s->nbits;
    uint8_t pad = 0x80;
    asmtrace_sha256_update(s, &pad, 1);
    pad = 0x00;
    while (s->nbuf != 56)
        asmtrace_sha256_update(s, &pad, 1);
    uint8_t lenbe[8];
    for (int i = 0; i < 8; i++)
        lenbe[i] = (uint8_t)(nbits >> (56 - i * 8));
    asmtrace_sha256_update(s, lenbe, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(s->h[i]);
    }
}

/* Hash `len` bytes of `data` and write the 64-char lowercase-hex digest (plus a
 * NUL) into `out`, which must be at least 65 bytes. The one entry point the
 * producers call. */
static inline void asmtrace_sha256_hex(const void *data, size_t len,
                                       char out[65]) {
    asmtrace_sha256_t s;
    uint8_t d[32];
    asmtrace_sha256_init(&s);
    asmtrace_sha256_update(&s, data, len);
    asmtrace_sha256_final(&s, d);
    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", d[i]);
    out[64] = '\0';
}

#endif /* ASMTRACE_SHA256_H */
