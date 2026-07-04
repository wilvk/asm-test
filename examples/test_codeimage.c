/*
 * test_codeimage.c — live test for the time-aware code-image recorder
 * (asmtest_codeimage.h). The Phase-A temporal test runs on ANY Linux with soft-dirty
 * page tracking (no privilege) — it is the same-address-different-bytes proof the
 * recorder exists for. The Phase-C eBPF emission test self-skips (exit 0) unless built
 * with libbpf and run with CAP_BPF (the `make docker-hwtrace-codeimage` lane).
 */
#include "asmtest_codeimage.h"
#include "asmtest_trace.h" /* asmtest_disas: the bytes_at-through-decoder round-trip */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* Two distinct 7-byte routines that share an address: A = add, B = sub. They differ at
 * one byte (0x01 vs 0x29), so a recorder that conflates them by address is caught.
 *   A: mov rax,rdi ; add rax,rsi ; ret
 *   B: mov rax,rdi ; sub rax,rsi ; ret */
static const unsigned char BLOB_A[] = {0x48, 0x89, 0xf8, 0x48,
                                       0x01, 0xf0, 0xc3};
static const unsigned char BLOB_B[] = {0x48, 0x89, 0xf8, 0x48,
                                       0x29, 0xf0, 0xc3};

/* Phase A: the temporal correctness proof. Track a region, rewrite it IN PLACE with
 * different bytes (a re-JIT at a reused address), and assert the timeline still answers
 * the OLD bytes for the OLD logical time — where a single late snapshot returns only the
 * new bytes. Runs live, in-process, no privilege. */
static void test_codeimage_temporal(void) {
#if defined(__linux__)
    if (!asmtest_codeimage_available()) {
        char why[200];
        asmtest_codeimage_skip_reason(why, sizeof why);
        printf("# SKIP codeimage temporal: %s\n", why);
        return;
    }
    long ps = sysconf(_SC_PAGESIZE);
    unsigned char *p =
        (unsigned char *)mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP codeimage temporal: mmap failed\n");
        return;
    }

    memcpy(p, BLOB_A, sizeof BLOB_A); /* publish A */

    asmtest_codeimage_t *img = asmtest_codeimage_new(0); /* this process */
    CHECK(img != NULL, "codeimage: new()");

    int rc = asmtest_codeimage_track(img, p, sizeof BLOB_A);
    CHECK(rc == ASMTEST_CI_OK,
          "codeimage: track() snapshots v0 and arms soft-dirty");
    uint64_t t0 = asmtest_codeimage_now(img);
    CHECK(t0 >= 1, "codeimage: now() advanced past 0 after track");

    memcpy(p, BLOB_B,
           sizeof BLOB_B); /* re-JIT in place: same address, new bytes */

    int nv = asmtest_codeimage_refresh(img);
    CHECK(nv >= 1,
          "codeimage: refresh() detected the in-place rewrite (new version)");
    uint64_t t1 = asmtest_codeimage_now(img);
    CHECK(t1 > t0, "codeimage: now() advanced after the change");

    const uint8_t *b = NULL;
    size_t bl = 0;
    rc = asmtest_codeimage_bytes_at(img, p, t0, &b, &bl);
    CHECK(rc == ASMTEST_CI_OK && bl == sizeof BLOB_A &&
              memcmp(b, BLOB_A, sizeof BLOB_A) == 0,
          "codeimage: bytes_at(t0) returns the OLD bytes A — the temporal fix");

    rc = asmtest_codeimage_bytes_at(img, p, t1, &b, &bl);
    CHECK(rc == ASMTEST_CI_OK && bl == sizeof BLOB_B &&
              memcmp(b, BLOB_B, sizeof BLOB_B) == 0,
          "codeimage: bytes_at(t1) returns the NEW bytes B");

    rc = asmtest_codeimage_bytes_at(img, p, 0, &b, &bl);
    CHECK(rc == ASMTEST_CI_OK && memcmp(b, BLOB_B, sizeof BLOB_B) == 0,
          "codeimage: bytes_at(0 = latest) returns B");

    /* The live page now holds B, so a naive late process_vm_readv would miss A entirely —
     * which is exactly the bug the timeline fixes. */
    CHECK(memcmp(p, BLOB_B, sizeof BLOB_B) == 0,
          "codeimage: live page holds B (a late snapshot alone would lose A)");

    /* Querying inside the region but past `addr` clamps the length to the region end. */
    rc = asmtest_codeimage_bytes_at(img, p + 3, t0, &b, &bl);
    CHECK(rc == ASMTEST_CI_OK && bl == sizeof BLOB_A - 3 &&
              memcmp(b, BLOB_A + 3, sizeof BLOB_A - 3) == 0,
          "codeimage: bytes_at(addr+3) returns the tail of A");

    rc = asmtest_codeimage_bytes_at(img, p + ps, 0, &b, &bl);
    CHECK(rc == ASMTEST_CI_ENOENT, "codeimage: untracked address -> ENOENT");

    /* §2 round-trip: disassemble the bytes_at-served bytes and confirm the temporal
     * version drives a version-correct decode. A's insn at offset 3 is `add`, B's is
     * `sub`, so the two versions must disassemble differently at the SAME address. */
    if (asmtest_disas_available()) {
        const uint8_t *ba = NULL, *bb = NULL;
        size_t la = 0, lb = 0;
        asmtest_codeimage_bytes_at(img, p, t0, &ba, &la);
        asmtest_codeimage_bytes_at(img, p, t1, &bb, &lb);
        char da[64], db[64];
        uint64_t ip = (uint64_t)(uintptr_t)p;
        asmtest_disas(ASMTEST_ARCH_X86_64, ba, la, ip, 3, da, sizeof da);
        asmtest_disas(ASMTEST_ARCH_X86_64, bb, lb, ip, 3, db, sizeof db);
        CHECK(da[0] != '\0' && db[0] != '\0' && strcmp(da, db) != 0,
              "codeimage: t0 (add) and t1 (sub) decode differently at one address");
    }

    asmtest_codeimage_free(img);
    munmap(p, (size_t)ps);
#else
    printf("# SKIP codeimage temporal: not Linux\n");
#endif
}

/* Phase C: the eBPF emission detector observes the PROT_EXEC publish edge. Self-skips
 * unless built with libbpf and run with CAP_BPF (the docker-hwtrace-codeimage lane). */
static void test_codeimage_bpf(void) {
#if defined(__linux__)
    if (!asmtest_codeimage_bpf_available()) {
        char why[200];
        asmtest_codeimage_bpf_skip_reason(why, sizeof why);
        printf("# SKIP codeimage bpf: %s\n", why);
        return;
    }
    asmtest_codeimage_t *img = asmtest_codeimage_new(getpid());
    CHECK(img != NULL, "codeimage bpf: new()");
    CHECK(asmtest_codeimage_watch_bpf(img) == ASMTEST_CI_OK,
          "codeimage bpf: load + attach the CO-RE program");

    long ps = sysconf(_SC_PAGESIZE);
    void *p = mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED, "codeimage bpf: mmap a writable page");
    memcpy(p, BLOB_A, sizeof BLOB_A);
    CHECK(mprotect(p, (size_t)ps, PROT_READ | PROT_EXEC) == 0,
          "codeimage bpf: mprotect PROT_EXEC (the emission edge)");
    __builtin___clear_cache((char *)p, (char *)p + ps);

    asmtest_codeimage_event_t ev;
    memset(&ev, 0, sizeof ev);
    int got = 0;
    for (int i = 0; i < 50 && !got; i++) {
        if (asmtest_codeimage_poll_bpf(img, 20) > 0)
            got = (asmtest_codeimage_next(img, &ev) == 1);
    }
    CHECK(got, "codeimage bpf: observed an emission event");
    if (got) {
        CHECK(ev.kind == ASMTEST_CI_KIND_MPROTECT,
              "codeimage bpf: kind is MPROTECT");
        CHECK((void *)(uintptr_t)ev.addr == p,
              "codeimage bpf: addr matches the freshly-published page");
        CHECK(ev.pid == (uint32_t)getpid(),
              "codeimage bpf: filtered to the target pid");
    }
    munmap(p, (size_t)ps);
    asmtest_codeimage_free(img);
#else
    printf("# SKIP codeimage bpf: not Linux\n");
#endif
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_codeimage_temporal();
    test_codeimage_bpf();
    if (checks == 0)
        printf("1..0 # skipped\n");
    else
        printf("1..%d\n", checks);
    return failures ? 1 : 0;
}
