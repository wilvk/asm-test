# asm-test — code-level review (2026-07-02)

*Status: review / findings. This is the code-level pass the
[2026-07-02 repo review](../archive/reviews/2026-07-02-repo-review.md) could not run
(its "incomplete code re-scan" caveat): core runtime, language bindings, and
the trace/emu tiers, extended to the Win64 port, test-suite quality, build/CI,
the eBPF surface, and doc accuracy. It should be read alongside that review and
the [2026-07-01 review](../archive/reviews/2026-07-01-repo-review.md); their findings
(1–15, B1–B7, S1–S3) were excluded from this pass and are not repeated here.*

**Method:** twelve parallel single-dimension reviewers read their slice of the
repo in full (~45k lines total). Every candidate finding was then attacked by
two independent adversarial verifiers — one checking the claim against the code
as written, one attacking its real-world impact (several built live
reproducers). Only findings that survived are listed. Near the end of the run
18 verifier agents plus a final completeness critic were lost to an API spend
limit; the six findings that lost both verifiers were re-verified by hand
(marked **[verified by hand]**, with the verification evidence inline), five
more kept a single confirming pass (marked **[verified, single pass]**), and
everything else is **[verified ×2]**.

Paths are repo-relative; `file:line` points at the exact site.

---

## Remediation status

**Complete (2026-07-02): all 54 findings addressed** — 51 fully fixed, #11
decode-side fixed (its Intel-PT address-filter half needs Intel hardware), #19
resolved by correcting the header claim, #36 mitigated (Win64 kernel-wait; a full
fix needs a Windows host). Fixes landed in batches; each has an implementation note
under `docs/summaries/`, and the working tree was validated on
x86-64 (core + sanitizer + emu + hwtrace) and linux/arm64 (via qemu). Items whose
full validation needs hardware this AMD Zen 5 dev box lacks (Intel PT, AArch64
CoreSight, Zen 3 BRS), a non-AMD OS (macOS/Windows runtime), or a privileged/manual
action (`perf_event_paranoid`, package-publish credentials) are called out inline
and mapped in roadmap-assessment.

| Finding | Status | Note |
|---|---|---|
| #1 AArch64 d8–d15 ABI check | ✅ Fixed | batch-a |
| #2 AArch64 x1 return (doc) | ✅ Fixed | batch-a |
| #3 struct{long;double} arg (doc) | ✅ Fixed | batch-a |
| #4 pst_mixed AArch64 body | ✅ Fixed | batch-a |
| #5 vm.s x23 + frame | ✅ Fixed | batch-a |
| #6 ASSERT_ABI_PRESERVED sp claim (doc) | ✅ Fixed | batch-a |
| #11 Intel PT empty trace as complete | ⚠️ Partial | batch-c — decode-side truncated fix done; PT address-filter needs Intel HW |
| #12 AMD LBR ring-full undetectable | ✅ Fixed | batch-c |
| #13 single-step block partition | ✅ Fixed | batch-c (live-validated vs Unicorn) |
| #14 available(SINGLESTEP) non-Linux stub | ✅ Fixed | batch-c |
| #15 TF-clear stops capture silently | ✅ Fixed | batch-c |
| #16 AMD replay not-taken block | ✅ Fixed | batch-c |
| #17 init during active capture | ✅ Fixed | batch-c |
| #18 trace_call non-SIGTRAP kill | ✅ Fixed | batch-d |
| #19 call-out not call-depth aware | ✅ Fixed (doc) | batch-d — header corrected; SP-aware step-over a documented follow-up |
| #20 jitdump truncation OK+garbage | ✅ Fixed | batch-d |
| #21 unreaped tracee on return path | ✅ Fixed | batch-d |
| #26 Rust call_traced heap overflow | ✅ Fixed | batch-f |
| #27 Python reg() KeyError | ✅ Fixed | batch-f |
| #28 Node 32-bit address truncation | ✅ Fixed | batch-f |
| #29 Lua 64-bit read-back corruption | ✅ Fixed | batch-f |
| #30 C++ null fn-pointer crash | ✅ Fixed | batch-f |
| #31 Java unbounded arena | ✅ Fixed | batch-f |
| #32 Python hwtrace OSError | ✅ Fixed | batch-f |
| #7 SKIP in SETUP/TEARDOWN → FAIL | ✅ Fixed | batch-b |
| #8 vec_f32 unbounded index (OOB) | ✅ Fixed | batch-b |
| #9 JUnit not XML-safe | ✅ Fixed | batch-b |
| #10 test stdout precedes JUnit XML | ✅ Fixed | batch-b |
| #22 codeimage track re-arm wipe | ✅ Fixed | batch-e |
| #23 emu SysV stack args dropped | ✅ Fixed | batch-e |
| #24 emu AArch64 x0..x7 | ✅ Fixed | batch-e |
| #25 emulator.md preload address (doc) | ✅ Fixed | batch-e |
| #33 Win64 teardown skipped | ✅ Fixed | batch-g |
| #34 Win64 DF not cleared on recovery | ✅ Fixed | batch-g |
| #35 Win64 watchdog double-delete | ✅ Fixed | batch-g |
| #36 Win64 --no-fork kernel-wait hang | ⚠️ Mitigated | batch-g (bounded hard-exit + doc; full fix needs a Windows host) |
| #40 release.yml corresponding-source | ✅ Fixed | batch-i |
| #41 lua rockspec hardcoded version | ✅ Fixed | batch-i |
| #42 asmtest.o/test header prereqs | ✅ Fixed | batch-i |
| #43 PIC/ptrace header prereqs | ✅ Fixed | batch-i |
| #44 check-version not run in CI | ✅ Fixed | batch-i |
| #45 apt-get failures swallowed | ✅ Fixed | batch-i |
| #46 install omits asmtest_assemble.h | ✅ Fixed | batch-i |
| #47 release verify without llvm | ✅ Fixed | batch-i |
| #48 vmlinux_min.h can't compile bpf | ✅ Fixed | batch-i |
| #37 expect.sh negative timeout | ✅ Fixed | batch-h |
| #38 expect.sh shuffle tautology | ✅ Fixed | batch-h |
| #39 drtrace coverage assertion | ✅ Fixed | batch-h |
| #49 quickstart auto-discovery (doc) | ✅ Fixed | batch-j |
| #50 emulator.md AArch64 args (doc) | ✅ Fixed | via #24 (batch-e) |
| #51 ASMTEST_SEED / --shuffle | ✅ Fixed | batch-j |
| #52 CHANGELOG stale emu_full (doc) | ✅ Fixed | batch-j |
| #53 bindings.md C++ tiers (doc) | ✅ Fixed | batch-j |
| #54 ci.md arm64 jobs (doc) | ✅ Fixed | batch-j |

---

## Overall

**54 confirmed findings (10 high), 9 plausible, 2 refuted.** The dominant theme
is the hardware-trace tier's central contract — *a trace is either complete or
flagged `truncated`* — which is broken in essentially every backend: Intel PT
decode dies before reaching the region, AMD LBR overflow is undetectable by
construction, single-step silently stops on a TF-clearing `popfq` and mis-blocks
not-taken branches, and a non-Linux x86-64 build reports the backend available
while the whole capture lifecycle is compiled out. A second theme is AArch64
fidelity: the capture layer misses the callee-saved SIMD registers entirely,
and two shipped examples (and one header comment) teach or embody real AAPCS64
violations.

### High-severity summary

| # | Finding | Area |
|---|---------|------|
| 1 | AArch64 callee-saved SIMD registers v8–v15 neither seeded, checked, nor restored (`src/capture.s:100`) | ABI capture & shipped examples |
| 5 | vm.s AArch64 example clobbers callee-saved x23 and under-allocates its frame (`examples/vm.s:109`) | ABI capture & shipped examples |
| 11 | Intel PT decode dies at `-pte_nomap` before reaching the region; empty trace reported complete (`src/pt_backend.c:104`) | Hardware-trace tier |
| 12 | AMD LBR ring-full sample loss is undetectable; over-ring runs reported complete (`src/hwtrace.c:503`) | Hardware-trace tier |
| 13 | Single-step block partition diverges from PT/DynamoRIO/Unicorn (`src/ss_backend.c:160`) | Hardware-trace tier |
| 22 | codeimage `track()` re-arms clear_refs globally, wiping pending soft-dirty state (`src/codeimage.c:367`) | Emulator & code-image |
| 26 | Rust `Guest::call_traced` always calls the arm64 entry — heap overflow from a safe API (`bindings/rust/src/lib.rs:1013`) | Language bindings |
| 27 | Python `EmuResult.reg()` raises KeyError for 15 of 18 documented x86 registers (`bindings/python/asmtest/core.py:269`) | Language bindings |
| 40 | release.yml corresponding-source attach step fails on any real tag (`.github/workflows/release.yml:105`) | Build / CI / packaging |
| 49 | Quickstart's suite auto-discovery claim is false — a new user's tests silently never run (`docs/quickstart.md:58`) | Documentation accuracy |

---

## ABI capture & shipped examples

### 1. AArch64 callee-saved SIMD registers v8-v15 are neither seeded, checked, nor restored - ABI violations pass ASSERT_ABI_PRESERVED and corrupt the harness — High **[verified ×2]**

`src/capture.s:100`

AAPCS64 6.1.2 requires 'Registers v8-v15 must be preserved by a callee across subroutine calls ... only the bottom 64 bits of each value stored in v8-v15 need to be preserved.' Every AArch64 trampoline in capture.s seeds and verifies only x19-x29; d8-d15 get no sentinels, asmtest_check_abi (src/asmtest.c:218-228) checks only x19..x29 on aarch64, and the trampoline does not save/restore d8-d15 around blr either. Two consequences: (1) a routine that clobbers d8-d15 without saving them (a classic NEON bug, since v0-v7/v16-v31 are free) passes ASSERT_ABI_PRESERVED, contradicting docs/abi-capture.md:106-107 ('verifies the routine restored every callee-saved register'); (2) unlike x19-x28, which the trampoline restores after capture to contain the violation, the d8-d15 clobber leaks straight through asm_call_capture into the C test function, whose compiler is entitled to keep FP locals live in d8-d15 across the call. The Win64 tier shows the project considers vector callee-saved checking in scope (it seeds xmm6-15 and provides asmtest_check_abi_vec); the AAPCS64 equivalent is simply missing.

**Evidence:** src/capture.s:100-111: '/* Seed callee-saved registers (x19-x29) with sentinels. */ ldr x19, =0x1111... ... ldr x29, =0xBBBB...' - no d8-d15 seeding or save/restore in any aarch64 body. src/asmtest.c:217-228: aarch64 chk[] lists only {"x19"..."x29"}. docs/abi-capture.md:106-107: '`ASSERT_ABI_PRESERVED(&r)` verifies the routine restored every callee-saved register'.

**Failure scenario:** AArch64 NEON routine uses v8 as scratch without stp d8/d9 saves (real AAPCS64 violation). Test does ASM_FCALL1(&r, fn, x); ASSERT_ABI_PRESERVED(&r) -> passes (false negative). Worse: the C test kept 'double expected' in d8 across the call (legal register allocation), so a following ASSERT_FP_EQ compares against a corrupted value -> spurious pass/fail on unrelated assertions.

**Suggested fix:** In the aarch64 trampolines, stp d8-d15 into the frame, seed them with sentinels, capture their post-call values into regs_t (new fields or documented vec[] slots on the _vec path), restore before returning; extend asmtest_check_abi and correct docs/abi-capture.md.

### 2. AArch64 never captures the second return register x1, but the header documents reading small (9-16 byte) struct returns 'from regs_t' — Medium (impact assessed Low) **[verified ×2]**

`include/asmtest.h:349`

Per AAPCS64, a non-HFA composite of 9-16 bytes is returned in x0 and x1. On x86-64 the framework captures the second return register (regs_t.rdx at offset 8, stored by capture.s:58), but the AArch64 regs_t has no x1 field and no aarch64 trampoline stores x1 (capture.s:118 stores only x0 then jumps to x19). The comment above asm_call_capture_sret claims small structs 'need no special call: read them from regs_t (rax/rdx, or vec[0]/vec[1] for SSE)' - this is impossible on AArch64 for the high eightbyte of an integer-pair return, so the documented pattern silently cannot verify half of the return value on one of the two first-class targets.

**Evidence:** include/asmtest.h:347-349: 'Small (<=16-byte) structs are returned in registers instead and need no special call: read them from regs_t (rax/rdx, or vec[0]/vec[1] for SSE).' src/capture.s:118-119 (aarch64): 'str x0, [x11, #0] /* ret */  str x19, [x11, #8]' - no x1 store; include/asmtest.h:223-239 aarch64 regs_t has no x1 field.

**Failure scenario:** On AArch64, test a routine 'struct pair {long a, b;} make_pair(long, long)' (returned in x0/x1). Following the header's documented pattern, the test can assert only r.ret (x0); b (x1) is unobservable through regs_t, so a routine returning a wrong second eightbyte passes every capture-based assertion.

**Suggested fix:** Capture x1 into a regs_t field on AArch64 (mirroring x86's rdx 'second return reg') and store it in each aarch64 trampoline; or correct the header/doc to state the limitation and route such returns through asm_call_capture_sret.

### 3. Header advice to pass struct{long;double} via asm_call_capture_fp is wrong under AAPCS64 (mixed <=16-byte composites go in x0/x1, not x0/d0) — Low **[verified ×2]**

`include/asmtest.h:358`

The comment above the cross-arch asm_call_capture_bigstruct says small structs need no special call: 'pass their eightbytes as ordinary integer/double args (e.g. ... struct{long;double} via asm_call_capture_fp)'. That is SysV eightbyte classification (INTEGER->rdi, SSE->xmm0) and is correct on x86-64 only. Under AAPCS64 stage C, a <=16-byte composite that is not an HFA is assigned to consecutive general registers: struct{long;double} arrives in x0 and x1, with the double's bit pattern in x1 and nothing in d0. Following the header's recipe on AArch64 places the double in d0 and leaves x1 = 0, so the callee reads a zero for its second member.

**Evidence:** include/asmtest.h:356-358: 'Small (<=16-byte) structs need no special call — pass their eightbytes as ordinary integer/double args (e.g. struct{long,long} via ASM_CALL2, or struct{long;double} via asm_call_capture_fp).' capture.s aarch64 fp path (lines 234-248) marshals fargs into d0-d7 and iargs into x0-x5 only.

**Failure scenario:** On AArch64, user tests 'long f(struct {long a; double b;} s)' per the header comment: asm_call_capture_fp(&r, f, (long[6]){s.a}, (double[8]){s.b}). The AAPCS64-conformant callee reads s.b from x1, which the trampoline left as 0 -> routine computes on garbage, test result is meaningless (false failure, or false pass for b==0.0).

**Suggested fix:** Qualify the comment as x86-64/SysV-only and document that on AArch64 mixed <=16-byte composites must be passed as two integer eightbytes (e.g. ASM_CALL2 with the double bit-cast to long).

### 4. pst_mixed AArch64 example does not implement its documented C prototype — AAPCS64 passes {long,double} in x0/x1, not x0+d0, so the test is tautological — Medium **[verified by hand]**

`examples/structparam.s:30`

structparam.s documents `long pst_mixed(struct mixed m)` ({long; double}, 16 bytes) as "INTEGER + SSE eightbyte -> rdi + xmm0 / x0 + d0" and the AArch64 body reads the double from d0. The rdi+xmm0 half is correct per SysV AMD64 psABI 3.2.3 (two eightbytes classified INTEGER and SSE), but the "x0 + d0" half is wrong per AAPCS64: a composite type of <=16 bytes that is not an HFA/HVA is passed in consecutive general-purpose registers (stage C rule C.12; only HFAs reach SIMD registers via C.2), so a real C caller puts the double's bit pattern in x1 and d0 holds garbage. test_structparam.c's `mixed_struct_int_plus_sse` passes on AArch64 only because asm_call_capture_fp marshals (long,double) the same non-AAPCS64 way — it asserts the framework's own convention, not the platform ABI the comments claim ("struct{long; double} by value == one integer arg + one double arg").

**Evidence:** examples/structparam.s:7 `* -> rdi + xmm0 / x0 + d0`; :29-31 AArch64 body `fcvtzs x1, d0 / add x0, x0, x1`. Empirical proof (arm64 docker, gcc:14): a genuine C caller `struct mixed m = {40, 2.0}; pst_mixed(m)` compiled against structparam.s prints `pst_mixed({40,2.0}) = 40 (want 42)` — gcc passed 2.0's bits in x1 per AAPCS64; the routine read d0 (0.0). Meanwhile `make test` (test_structparam) passes on the same arm64 host.

**Failure scenario:** An AArch64 user copies this example to test a routine taking struct{long;double} from real C code: following the example they read d0, their unit test passes (the harness marshals x0+d0), but every production C call site passes the double in x1 -> silently wrong results despite a green asm-test suite.

**Suggested fix:** In the AArch64 body take the second eightbyte from x1 (`fmov d0, x1; fcvtzs x1, d0; add x0, x0, x1`), have the driver marshal both eightbytes as integer args on AArch64 (bit-cast the double), and correct the structparam.s/test_structparam.c comments to state the AAPCS64 rule (non-HFA composite <=16B -> x0,x1).

**Hand verification:** under AAPCS64 rule C.12 a non-HFA composite of 16 bytes goes in x0+x1, never d0 (only HFAs reach SIMD registers, rule C.2); the AArch64 body reads d0 (`examples/structparam.s:29-32`) and the header comment claims `x0 + d0` (`:6-7`). The finder additionally proved it empirically in an arm64 container: a genuine gcc C caller returns 40 (double's bits in x1, d0 garbage) while `make test` passes.

### 5. vm.s AArch64 body clobbers callee-saved x23 and under-allocates its frame — `make usecases` fails on AArch64 — High **[verified, single pass]**

`examples/vm.s:109`

The AArch64 vm_eval uses x23 as the operand-stack base but never saves or restores it, violating AAPCS64 5.1.1/6.1.1 (r19-r29 must be preserved). The framework's own capture seeds x23 with ASMTEST_SENTINEL_X23 (src/capture.s:105) and asmtest_check_abi checks it (src/asmtest.c), so test_vm.c's `vm.preserves_callee_saved_registers` (ASSERT_ABI_PRESERVED) fails on AArch64 — the shipped example breaks the exact discipline it claims to demonstrate. Additionally the frame is 32 bytes too small: the file documents a 256-byte/32-slot operand stack, but `stp x29,x30,[sp,#-272]!` plus saves at sp+16..47 leaves only 224 bytes (28 slots) between the base (sp+48) and the frame end (sp+272); pushing 29-32 literals (legal per the documented encoding) writes past the frame into the caller's stack. The x86-64 body reserves the full 256 bytes.

**Evidence:** examples/vm.s:105 `stp x29, x30, [sp, #-272]! /* 16 (fp/lr) + 256 operand stack */`; :109 `add x23, sp, #48 /* operand-stack base */`; epilogue :174-176 restores only x21/x22, x19/x20, x29/x30 — no x23. Empirically confirmed in the repo's linux/arm64 docker lane (gcc:14, `make usecases`): `not ok 9 - vm.preserves_callee_saved_registers / at: examples/test_vm.c:77 / msg: ASSERT_ABI_PRESERVED: x23 not restored (got 0x78aec0bfe990, expected 0x5555555555555555)` -> `make: *** [Makefile:300: usecases] Error 1`.

**Failure scenario:** On any AArch64 host, `make usecases` exits nonzero: vm.preserves_callee_saved_registers fails with "x23 not restored". Independently, any RPN program pushing more than 28 operands (e.g. 30 literals then ADDs) stores at [sp+272..] — the caller's frame — corrupting caller state on AArch64 while returning normally.

**Suggested fix:** Grow the frame to 304 bytes (`stp x29, x30, [sp, #-304]!`, restore with `#304`) and save/restore x23 alongside x21/x22 (e.g. `stp x21, x22, [sp, #32]` + `str x23, [sp, #48]`, base at sp+56 rounded to sp+48... simplest: save x23/x24 at [sp,#48], operand base `add x23, sp, #64`); update the '16 (fp/lr) + 256' comment.

### 6. Docs claim ASSERT_ABI_PRESERVED verifies the stack pointer; nothing captures or checks sp/rsp anywhere — Low **[verified ×2]**

`docs/abi-capture.md:107`

docs/abi-capture.md states ASSERT_ABI_PRESERVED 'verifies the routine restored every callee-saved register (and the stack pointer)'. regs_t has no sp/rsp field, no trampoline in capture.s/capture.asm stores the post-call stack pointer, and asmtest_check_abi (src/asmtest.c:193-244) compares only the named GP fields. A stack-pointer violation is never 'verified'; at best it manifests as a wild load/crash inside the trampoline (reported as a crash by the forked runner), and on the frame-pointer paths (fp_n/vec_n/args/sret) the epilogue 'leaq -40(%rbp), %rsp' / 'mov sp, x29' would silently repair an rsp/sp discrepancy rather than detect it.

**Evidence:** docs/abi-capture.md:106-107: '`ASSERT_ABI_PRESERVED(&r)` verifies the routine restored every callee-saved register (and the stack pointer).' src/asmtest.c:193-244 (asmtest_check_abi): chk[] contains only rbx/rbp/r12-r15 (SysV) or x19-x29 (aarch64); grep for sp/rsp in regs_t and asmtest.c finds no stack-pointer field or check.

**Failure scenario:** User reads the doc and relies on ASSERT_ABI_PRESERVED to diagnose a stack-imbalance bug; the assertion never reports 'stack pointer not restored' - the test instead dies with an unexplained SIGSEGV crash report (or, on the _n paths, passes cleanly because rbp-based unwinding masks the imbalance).

**Suggested fix:** Delete '(and the stack pointer)' from the doc, or actually capture the post-call sp into regs_t and compare it against the pre-call value in asmtest_check_abi.

## Core runner

### 7. SKIP() inside a SETUP or TEARDOWN hook is reported as a test FAILURE, not a skip — Medium **[verified ×2]**

`src/asmtest.c:781`

run_one's setup recovery point treats any nonzero sigsetjmp return as ST_FAIL, so a JMP_SKIP (=2) raised by asmtest_skip() in a SETUP hook marks the test failed with the skip reason as the failure message. The teardown recovery does the same (`else if (outcome != ST_FAIL) outcome = ST_FAIL;` at line 801), converting a passing test whose teardown calls SKIP into a failure. Skipping a whole suite from its fixture (the GTest GTEST_SKIP-in-SetUp idiom) is a natural use of the public SKIP macro; here it silently inverts skip into red CI.

**Evidence:** asmtest.c:781-784: `if (sigsetjmp(asmtest_jmp, 1) != 0) { alarm(0); asmtest_in_test = 0; return ST_FAIL; /* setup failed/crashed */ }` — JMP_SKIP (enum at line 23) is not distinguished; asmtest.c:799-802 teardown path likewise promotes JMP_SKIP to ST_FAIL.

**Failure scenario:** SETUP(avx) { if (!asmtest_cpu_has_avx2()) SKIP("no AVX2"); } — on a non-AVX2 host every test in suite `avx` prints `not ok ... msg: no AVX2` and the runner exits 1, instead of the tests counting as skipped.

**Suggested fix:** Capture the sigsetjmp return code in setup/teardown and return/propagate ST_SKIP when it is JMP_SKIP.

### 8. asmtest_regs_vec_f32 never checks the upper bound of `index`, giving dynamic-language bindings an out-of-bounds heap read — Medium (impact assessed Low) **[verified ×2]**

`src/ffi.c:41`

The accessor validates `lane` (0..3) and rejects negative `index`, but has no upper bound, while regs_t.vec has only 16 entries on x86-64 (32 on AArch64). Every other accessor in this file bounds both parameters (e.g. asmtest_emu_x86_xmm_f32 checks `index > 15` at ffi.c:162). This function is exported to all dynamic bindings, and at least the Lua binding forwards a caller-supplied index with no clamp (bindings/lua/asmtest.lua:179-183 `Regs:vec_f32(index)`), so a script-level call reads past the 336-byte calloc'd regs_t.

**Evidence:** ffi.c:40-44: `float asmtest_regs_vec_f32(const regs_t *r, int index, int lane) { if (index < 0 || lane < 0 || lane > 3) return 0.0f; return r->vec[index].f32[lane]; }` — vs asmtest.h:192 `vec128_t vec[16];` (x86-64) and ffi.c:161-165 which does check `index > 15`.

**Failure scenario:** From Lua/Ruby/Node: `regs:vec_f32(16)` returns 4 floats of adjacent heap garbage (flaky test verdicts); `regs:vec_f32(10000000)` reads ~160 MB past the allocation and typically SIGSEGVs the host interpreter.

**Suggested fix:** Clamp against the arch's array length: `if (index < 0 || (size_t)index >= sizeof r->vec / sizeof r->vec[0] || lane < 0 || lane > 3) return 0.0f;`

### 9. JUnit output is not XML-safe: control characters in messages pass through unescaped and r->file is emitted raw inside <failure> — Low **[verified ×2]**

`src/asmtest.c:1371`

xml_print_escaped handles & < > " and \n but passes all other bytes through, including C0 control characters (0x01-0x08, 0x0B-0x1F), which are illegal in XML 1.0 even when escaped as entities. Failure messages routinely embed raw test data (asmtest_assert_streq prints both strings verbatim with %s, asmtest.c:142), so one control byte in a compared string makes the entire report unparseable. Additionally the failure element body prints `at %s:%d` with r->file unescaped (line 1422), so a source path containing `&` or `<` also yields malformed XML.

**Evidence:** asmtest.c:1363-1374: `default: fputc((unsigned char)*s, stdout);` with no control-char filtering; asmtest.c:1422: `printf("\">at %s:%d&#10;", r->file, r->line);` — r->file bypasses xml_print_escaped.

**Failure scenario:** ASSERT_STREQ(buf, "expected") where the asm routine wrote a stray 0x1B/0x01 byte into buf → `--format=junit` produces XML containing a literal control byte → Jenkins/GitLab JUnit ingestion rejects the whole report, hiding all results of the run.

**Suggested fix:** In xml_print_escaped, replace bytes < 0x20 (other than \t, \n, \r) with a placeholder such as \\xNN, and route r->file through xml_print_escaped.

### 10. In --format=junit, test-emitted stdout is deliberately preserved ahead of the XML document, producing an invalid report — Low **[verified ×2]**

`src/asmtest.c:1129`

Forked children flush inherited stdout so anything the test printed survives ("preserve anything the test itself printed"), and the JUnit document is rendered to the same stdout only after all tests ran (render_junit at line 1838). Any printf from a test body therefore lands before the `<?xml ...?>` declaration, which must be the first bytes of the document. The runner is careful to suppress its own non-XML output in junit mode (e.g. the shuffle-seed line at 1764-1765 is TAP-only), but test output has no capture/suppression path.

**Evidence:** asmtest.c:1127-1131 (child): `write_full(fds[1], &w, sizeof w); fflush(stdout); /* preserve anything the test itself printed */` and asmtest.c:1837-1838: `if (opt.format_junit) { render_junit(results, n); }` — both on the same stdout.

**Failure scenario:** TEST(x, y) { printf("debug\n"); ... } with `./tests --format=junit > report.xml` → report.xml begins with `debug` before `<?xml version=...` → any XML parser rejects it ("Start tag expected") and CI shows no results.

**Suggested fix:** In junit mode, redirect the child's stdout to /dev/null or to a captured buffer (emit it as <system-out>), or write the XML to a separate stream.

## Hardware-trace tier

### 11. Intel PT decode cannot reconstruct the region: image serves only [base,base+len) while the trace covers all userspace execution, so pt_insn_next dies with -pte_nomap before ever reaching in-region code — empty trace returned as complete — High **[verified ×2]**

`src/pt_backend.c:104`

The capture (hwtrace.c) opens the intel_pt event with a zeroed attr and never programs an address filter (no PERF_EVENT_IOC_SET_FILTER anywhere in the repo), so the AUX stream traces every userspace instruction from IOC_ENABLE (inside libc/ioctl glue) to IOC_DISABLE. The decoder's image callback read_region returns -pte_nomap for any IP outside the registered region (pt_backend.c:46-47), but libipt's instruction-flow decoder must fetch and decode the bytes at EVERY IP to follow the flow — it cannot skip unmapped code. So after pt_insn_sync_forward lands on the initial PSB (whose FUP IP is in the perf/libc glue), the very first pt_insn_next returns -pte_nomap, the inner loop breaks, and the next sync_forward hits end-of-stream (a short capture rarely contains a second PSB at the default psb_period). The else-branch at :114-117 that resets prev_was_branch for out-of-region instructions is unreachable in practice, because out-of-region instructions error out instead of streaming through. asmtest_pt_decode then returns ASMTEST_HW_OK with nothing appended, and hwtrace.c:749 sets truncated only on (overflow || rc != OK) — so the PT tier's flagship flow yields an empty trace claimed complete. The file header admits live capture is 'exercised only on capable hardware'; the live assertions in examples/test_hwtrace.c:1533-1535 (block 0 covered, insns_total >= 4) would fail on a real PT host.

**Evidence:** pt_backend.c:46-47 'if (ip < c->base_ip || ip >= c->base_ip + c->len) return -pte_nomap;'; pt_backend.c:104-106 'status = pt_insn_next(dec, &insn, sizeof insn); if (status < 0) break;'; hwtrace.c:630-637 zeroed attr, no address filter, then 'long fd = perf_open(&attr, 0, -1, -1, 0);'; hwtrace.c:749 'if (r->trace != NULL && (overflow || rc != ASMTEST_HW_OK)) r->trace->truncated = true;'

**Failure scenario:** Bare-metal GenuineIntel host with perf_event_paranoid lowered: asmtest_hwtrace_available(INTEL_PT)=1, begin/call/end run; decode syncs to the enable-time PSB whose IP is in the ioctl return glue, pt_insn_next -> -pte_nomap, loop exits; trace has insns_total=0, blocks empty, truncated=false — reported as a complete empty trace, and any coverage/differential assertion against it gives wrong results.

**Suggested fix:** Program a PT address filter for the region (ioctl PERF_EVENT_IOC_SET_FILTER "filter <base>/<len>") so tracing starts/stops at region boundaries (sync IPs are then in-region), and/or treat a decode that appends zero in-region instructions from a non-empty AUX stream as a failure (set truncated / return EDECODE).

### 12. AMD LBR capture: ring-full sample loss is undetectable — PERF_RECORD_LOST can never appear in a ring that is never drained, so an over-ring run decodes gaplessly and is reported complete while missing its tail — High (impact assessed Medium) **[verified ×2]**

`src/hwtrace.c:503`

hwtrace_end_amd relies on finding PERF_RECORD_LOST/THROTTLE records to set `lost` (:503-505), per the comment 'on overflow the kernel drops the NEWEST samples and emits PERF_RECORD_LOST'. But the kernel (perf_output_begin) only writes the pending LOST record on the NEXT SUCCESSFUL reservation; capture here never advances data_tail until end() (:595), so once the ring fills every subsequent reservation fails, rb->lost only accumulates, and no LOST record is ever written before IOC_DISABLE. The drop is at the run's tail, so the surviving sample_period=1 windows stitch contiguously: asmtest_amd_stitch returns gap=0, decode_stitched is called with (gap||lost)==0, and the trace is not flagged truncated despite missing every branch after the ring filled. The live test only observed truncated because its 64-entry insns buffer overflowed (trace_append_insn cap), masking this hole.

**Evidence:** hwtrace.c:503-505 'else if (h->type == PERF_RECORD_LOST || h->type == PERF_RECORD_THROTTLE) lost = 1;' with comment :494-496 '...the kernel drops the NEWEST samples and emits PERF_RECORD_LOST — the precise "the run did not fit" signal'; :595 'mp->data_tail = head; /* consume */' is the only tail advance; :575-580 'asmtest_amd_decode_stitched(out, st, r->base, r->len, r->trace, gap || lost);'

**Failure scenario:** Zen 4/5 host with perf permitted, default data_size (64 KiB ring, ~163 samples at ~400 B each): trace a loop with ~200-500 taken branches into an asmtest_trace_new(1024,64) sink. The last ~40-340 branch samples are dropped with no LOST/THROTTLE record reachable; stitching is gapless; the trace omits the final loop iterations and the ret, insns_total is short, yet truncated=false — an incomplete trace claimed complete, defeating the documented 'never emit partial as complete' contract and the truncated-based fallback re-resolve.

**Suggested fix:** Treat a (nearly) full ring as loss: e.g. flag lost when dsz - span is smaller than one max-size sample record, or drain/advance data_tail during capture, or cross-check sample count against the event count read from the fd.

### 13. Single-step block normalization diverges from the PT/DynamoRIO/Unicorn partition: no block boundary at the fall-through of a not-taken conditional branch or at a call-return re-entry — High **[verified, single pass]**

`src/ss_backend.c:160`

ss_normalize derives blocks purely from address discontinuities: a new block only when `off != expected_next`. A conditional branch that is NOT taken falls through (off == expected_next), so no block starts there; likewise a return from an in-region call lands at the call's fall-through and creates no block. But pt_backend.c:113 sets prev_was_branch = is_branch(insn.iclass) for every branch-class instruction regardless of direction, DynamoRIO ends a bb at every CTI, and Unicorn's UC_HOOK_BLOCK fires at the new TB that starts at the fall-through — all three record a block there. The docs (docs/native-tracing.md:339-340) claim single-step is 'byte-for-byte the Unicorn/DynamoRIO instruction and block streams'. The shared test fixture only exercises the taken-jle path, so this never trips in CI. Additionally, REP-prefixed string instructions trap after every iteration under TF, recording the same offset N times and (from the second iteration) a spurious block at the REP offset, where PT/DR record it once.

**Evidence:** ss_backend.c:160-161 'if (!have_prev || off != expected_next) trace_append_block(t, off);' vs pt_backend.c:111-113 'if (prev_was_branch) trace_append_block(trace, off); prev_was_branch = is_branch(insn.iclass);' (is_branch includes ptic_cond_jump whether or not taken); src/emu.c:105 appends a block per UC_HOOK_BLOCK TB entry.

**Failure scenario:** The repo's own fixture ROUTINE called as fn(60,60) (120 > 100, jle at 0xc NOT taken): executed stream {0,3,6,c,e,11}. Unicorn/DR/PT block set = {0, 0xe}; single-step block set = {0} only. asmtest_trace_covered(tr, 0xe) returns 0 even though the block executed — coverage falsely reports the fall-through block uncovered, blocks_len comparisons across backends disagree, and 'byte-for-byte parity' assertions fail for any routine with an untaken jcc or an in-region call.

**Suggested fix:** In ss_normalize, classify each decoded instruction (Capstone is already invoked per insn) and start a new block after every branch-class instruction — same rule as pt_backend's is_branch — instead of relying solely on fall-through discontinuities; collapse consecutive identical offsets from REP iterations.

### 14. asmtest_hwtrace_available(SINGLESTEP) returns 1 on non-Linux x86-64 builds where the backend is a no-op stub — the 'Linux x86-64 only' self-skip is unreachable and captures silently produce empty complete traces — Medium **[verified by hand]**

`src/hwtrace.c:157`

cpu_matches(SINGLESTEP) is gated only on __x86_64__ (no __linux__), and asmtest_hwtrace_available returns 1 immediately for SINGLESTEP once decoder+cpu pass (:231-234). But the whole capture lifecycle is compiled out off Linux: asmtest_hwtrace_begin/end are (void)name no-ops (:609/:671, :714/:759) and asmtest_ss_begin is the ENOSYS stub (ss_backend.c:201-209). So on a non-Linux x86-64 build with Capstone (e.g. an Intel-Mac source build — the release workflow already builds and packages libasmtest_hwtrace on macOS runners), available()=1, skip_reason reports 'available', init() succeeds, and begin/call/end yields an untouched trace with truncated=false. The skip string at :260 ('single-step backend is Linux x86-64 only (Windows/macOS planned)') exists precisely for this case but can never be produced on an x86-64 non-Linux host, because cpu_matches returns 1 there.

**Evidence:** hwtrace.c:154-160 'case ASMTEST_HWTRACE_SINGLESTEP: /* TF/#DB single-step is baseline x86-64...*/ #if defined(__x86_64__) return 1;'; :231-234 'if (backend == ASMTEST_HWTRACE_SINGLESTEP) ... return 1;'; :608-609/:671-673 begin body entirely inside '#if defined(__linux__)' with '#else (void)name;'. docs/native-tracing.md:384-386 promises the self-skip reason on non-Linux.

**Failure scenario:** Build the hwtrace library on macOS x86-64 (or any non-Linux x86-64) with Capstone: HwTrace.available(SINGLESTEP) == true, init/register/begin/end all 'succeed', the routine runs untraced, and the caller receives insns_total=0, blocks empty, truncated=false — a false-available that breaks the documented detect-and-skip chain (condition (b)/(d) of the header contract).

**Suggested fix:** Gate the SINGLESTEP arm of cpu_matches on defined(__linux__) && defined(__x86_64__) so available() self-skips and skip_reason emits the already-written 'Linux x86-64 only' message.

**Hand verification:** `cpu_matches` gates SINGLESTEP on `__x86_64__` only (`src/hwtrace.c:154-160`); `available()` returns 1 for SINGLESTEP with no platform check (`:231-234`); `init()` gates only on `available()` (`:369-379`) and `register_region` has no gate; the bodies of `begin`/`end` are `#if defined(__linux__)` with no-op else-arms (`:608-609`, `:713-714`); the documented "single-step backend is Linux x86-64 only" skip string exists at `:260` but is unreachable on a non-Linux x86-64 build.

### 15. A routine that clears EFLAGS.TF (e.g. popfq of synthetically built flags) silently stops the single-step capture; the partial trace is NOT flagged truncated, contradicting the documented contract — Medium **[verified by hand]**

`src/ss_backend.c:185`

docs/native-tracing.md:385-387 states in-routine POPF/IRET 'break naive stepping and are flagged truncated rather than emitted as complete'. No such detection exists: the SIGTRAP handler only re-asserts TF in the interrupted context when a trap arrives (:106); if the routine itself executes popfq with an image whose TF bit is 0 (flags built from an immediate or captured before begin — plausible in a flags-testing framework), no further #DB fires, the rest of the routine runs unrecorded, and asmtest_ss_end -> ss_normalize sets truncated only on an undecodable byte (:171) or buffer overflow (:178-179). The result is a prefix of the execution presented as a complete trace.

**Evidence:** ss_backend.c:184-185 'g_armed = 0; ss_disarm_tf();' — end() never checks whether TF was still armed; ss_normalize's only truncation sources are :170-173 'if (l == 0) { t->truncated = true; return; }' and :178-179 'if (g_overflow) t->truncated = true;'. docs/native-tracing.md:385-387: 'no in-routine POPF/IRET... which break naive stepping and are flagged truncated rather than emitted as complete'.

**Failure scenario:** Routine: push an immediate flags image (TF=0), popfq, then 10 more instructions, ret. Trace via SINGLESTEP: offsets recorded up to and including the popfq; the remaining 10 instructions and the ret are absent; truncated=false. Assertions on insns_total/coverage silently operate on a partial trace claimed complete.

**Suggested fix:** In asmtest_ss_end, before disarming, read the current EFLAGS (pushfq) and if TF is no longer set — or if g_armed traps stopped before the disarm — set trace->truncated; alternatively detect in ss_normalize that the last recorded in-region instruction is not a region exit.

**Hand verification:** the SIGTRAP handler only re-asserts TF when a trap arrives (`src/ss_backend.c:106`); `ss_normalize`'s only truncation sources are an undecodable byte (`:170-173`) and buffer overflow (`:178-179`); `docs/native-tracing.md:383-387` explicitly promises in-routine POPF is "flagged truncated rather than emitted as complete". The finder's reproduction scenario is consistent with the code as read.

### 16. AMD replay creates blocks only at taken-branch targets, so the fall-through of a not-taken conditional branch never starts a block — diverging from the PT/DR/Unicorn partition it claims to match — Medium **[verified, single pass]**

`src/amd_backend.c:99`

amd_replay appends a block at the region entry and at each taken-branch target that lands in-region. A not-taken conditional branch produces no perf branch record, and the straight-line Capstone walk between records (:74-93) decodes through the jcc without opening a new block at its fall-through. PT (block after every branch-class instruction), DynamoRIO (bb ends at every CTI), and Unicorn (new TB at the fall-through) all record a block there, so the AMD backend's block set disagrees with the partition the file header claims to reproduce ('the same asmtest_trace_t offset stream the Intel PT backend produces... cross-checked against the DynamoRIO/PT block+instruction partition'). The synthetic test only covers the taken-jle path, so the divergence is untested.

**Evidence:** amd_backend.c:97-100 '/* Follow the branch. A target inside the region begins a new block...*/ ip = to; if (to >= base_ip && to < end_ip) trace_append_block(trace, to - base_ip);' — the only block appends besides the entry block at :62 'trace_append_block(trace, 0);'.

**Failure scenario:** Live AMD LBR capture (or synthetic decode) of ROUTINE with fn(60,60): jle at 0xc not taken, records contain only the ret. amd_replay yields insns {0,3,6,c,e,11} but blocks {0}; PT/DR/Unicorn yield blocks {0, 0xe}. Block-coverage of 0xe is falsely reported missing and cross-backend block comparisons fail.

**Suggested fix:** During the straight-line walk, classify each decoded instruction; when it is a branch-class instruction that execution falls through (o != from_off path continues past it — i.e., any branch insn before the recorded one), start a new block at its fall-through, matching pt_backend's normalization.

### 17. asmtest_hwtrace_init during an active capture corrupts the capture state machine: switching backend then calling end() leaks the perf fd + mappings and permanently wedges the tier — Low **[verified, single pass]**

`src/hwtrace.c:369`

init() unconditionally overwrites g_opts and zeroes g_regions without checking g_fd/g_active (:369-379), while end() dispatches on the CURRENT g_opts.backend (:719-727). If a caller re-inits with a different backend while a capture is active (a harness that forgot end() between cases), the subsequent end() runs the wrong teardown: e.g. AMD capture active, re-init to SINGLESTEP, end() calls asmtest_ss_end() and clears g_active — the AMD perf fd, base mmap, and enabled event are never disabled/unmapped/closed. Because g_fd stays >= 0, every later begin() returns immediately (:610) and shutdown()'s end(NULL) also returns at the g_active==NULL check (:716-717) without clearing g_fd, so the hardware tier is disabled for the rest of the process and the event keeps sampling.

**Evidence:** hwtrace.c:369-379 init() sets g_opts/g_regions/g_inited with no g_fd/g_active guard; :610 'if (!g_inited || g_fd >= 0 || g_active != NULL) return;'; :716-717 'if (g_active == NULL) return;' before the g_fd teardown; :768-769 shutdown relies on end(NULL) to release fd state.

**Failure scenario:** init(AMD_LBR); register; begin("r"); [missing end]; init(SINGLESTEP): now end()/shutdown() take the single-step arm, g_active is cleared, but the AMD fd stays open and enabled and g_base_map stays mapped; all subsequent hwtrace begins are silently ignored for the process lifetime.

**Suggested fix:** In asmtest_hwtrace_init, return ASMTEST_HW_ESTATE (or force-run the old-backend end path) when g_fd >= 0 || g_active != NULL; also make shutdown clear/close g_fd even when g_active is NULL.

## ptrace / DynamoRIO tier

### 18. trace_call treats any non-SIGTRAP signal as a fault: kills the tracee yet returns OK with an unset *result and an empty, un-truncated trace when the signal lands before region entry — Medium (impact assessed Low) **[verified ×2]**

`src/ptrace_backend.c:696`

In the asmtest_ptrace_trace_call stepping loop, any non-SIGTRAP signal-delivery stop kills the child and breaks out. Under ptrace, signal-delivery-stop happens BEFORE disposition is applied, so even ignored process-group signals (SIGWINCH on a terminal resize, SIGTSTP from Ctrl-Z — the forked tracee is in the same process group) trigger this path. run_until() forwards unrelated signals (lines 547-549), but this loop does not, even though PTRACE_SINGLESTEP takes the same sig argument. If the signal arrives before the region is entered, entered==0 so overflow stays 0: normalize() produces an EMPTY trace with truncated==false, rc is ASMTEST_PTRACE_OK, and *result is never written — violating the header's contracts "On success *result receives the routine's return value" and "never emitting a partial trace as complete" (include/asmtest_ptrace.h:85-89). The WIFEXITED break at line 692 has the same OK-with-unset-result hole. (When the signal lands after entry, truncated is correctly set — only the result contract is broken there.)

**Evidence:** src/ptrace_backend.c:696-707: `if (WSTOPSIG(status) != SIGTRAP) { ... if (entered) overflow = 1; /* incomplete in-region capture */ kill(pid, SIGKILL); waitpid(pid, &status, 0); break; }` then :755-756 `if (rc == ASMTEST_PTRACE_OK) normalize(trace, ..., n, overflow);` — result only assigned on the EXIT_RETURNED path at :746-747. Contrast run_until :547-549: `if (WSTOPSIG(status) != SIGTRAP) { sig = WSTOPSIG(status); /* forward an unrelated signal and keep running */ continue; }`. Header contract: include/asmtest_ptrace.h:85-89.

**Failure scenario:** A suite runs asmtest_ptrace_trace_call in a terminal; the user resizes the window while the child is still single-stepping through raise()/call glue (pre-entry). SIGWINCH is delivered to the process group, the tracee stops, the loop SIGKILLs it and returns ASMTEST_PTRACE_OK with insns_total==0, truncated==false, and *result holding the caller's uninitialized stack garbage — a coverage/result assertion then compares against garbage or an empty-but-"complete" trace.

**Suggested fix:** Forward non-fatal signals like run_until does (ptrace(PTRACE_SINGLESTEP, pid, NULL, (void*)(uintptr_t)WSTOPSIG(status)) for signals that are not SIGSEGV/SIGBUS/SIGILL/SIGFPE), and on every abort/exit path either return a non-OK code or set trace->truncated when returned==0.

### 19. Call-out step-over is not call-depth aware although the header explicitly claims "(call-depth aware)": a helper that re-enters the region makes the tracer resume in the wrong invocation and report a wrong, "complete" trace and result — Medium **[verified ×2]**

`src/ptrace_backend.c:568`

run_until() stops at the FIRST arrival at the call's return address with no stack-depth tracking (no SP comparison, no depth counter). If the stepped-over out-of-region helper calls back into the registered region (callback pattern, tiering/OSR stub re-invoking the method — exactly the managed-runtime scenarios this API is marketed for), the NESTED invocation executes the same call site and its return lands on the planted breakpoint one stack frame deeper. The tracer then resumes single-stepping inside the nested invocation: when the nested routine returns, classify_region_exit sees a non-call exit, records the NESTED return value into *result, and emits the trace as complete (truncated==false) while the outer invocation's remaining instructions were never traced. include/asmtest_ptrace.h:107 states call-outs are "stepped over at native speed and not recorded (call-depth aware)" — the implementation is not.

**Evidence:** src/ptrace_backend.c:560-569: `uint64_t hit; if (hw) hit = pc; else hit = pc - PTRACE_BP_LEN; ... if (hit != target) continue;` — the only match criterion is the PC; no RSP/depth check. include/asmtest_ptrace.h:106-108: "call-outs are stepped over at native speed and not recorded (call-depth aware), so a real JIT method that calls runtime helpers traces correctly".

**Failure scenario:** Region routine R at offset X calls out-of-region helper H; H invokes R once as a callback. Breakpoint is planted at base+X+call_len; the nested R reaches X, calls H, H returns to X+call_len -> int3 fires at depth 2. run_until reports the call-out returned; single-stepping resumes mid-nested-invocation; the nested ret is classified EXIT_RETURNED, *result gets the nested call's return value, and the trace (missing the entire outer post-call tail) is returned OK and not flagged truncated.

**Suggested fix:** Record the tracee's stack pointer when the call-out is detected and, in run_until, only accept a breakpoint hit whose SP is >= (x86: equal to pre-call SP + 8) the expected depth; otherwise re-plant/continue. Alternatively, correct the header to drop the "(call-depth aware)" claim and document the re-entrancy limitation.

### 20. asmtest_jitdump_find returns OK with *bytes_len unset and garbage bytes when the matching JIT_CODE_LOAD record's code bytes are truncated — Medium (impact assessed Low) **[verified ×2]**

`src/ptrace_backend.c:223`

When a name match is found, rc is set to ASMTEST_PTRACE_OK and *out is filled BEFORE the code bytes are read. If the subsequent fread of the code bytes comes up short (file truncated mid-record), the loop breaks with rc still OK, but *bytes_len is never written and bytes_out holds partial/garbage data. The header promises "copies up to bytes_cap of the recorded code into it and sets *bytes_len. Returns ASMTEST_PTRACE_OK" (include/asmtest_ptrace.h:191-194), so a caller returning OK will read the uninitialized *bytes_len and index bytes_out with it. A live jitdump is appended incrementally by the running JIT, so reading it while the last (most recent — exactly the record this function prefers) record is partially flushed is a realistic state, and the whole point of this API is resolving the latest body.

**Evidence:** src/ptrace_backend.c:213-226: `if (match) { rc = ASMTEST_PTRACE_OK; if (out) { out->code_addr = code_addr; ... } if (bytes_out != NULL && bytes_cap > 0) { size_t cpy = ...; if (fread(bytes_out, 1, cpy, f) != cpy) break; if (bytes_len) *bytes_len = cpy; ... }` — the `break` on short fread exits the for(;;) and :237-238 `fclose(f); return rc;` returns the already-set ASMTEST_PTRACE_OK without touching *bytes_len.

**Failure scenario:** A JIT is still running and appending /tmp/jit-<pid>.dump; asmtest_jitdump_find(path, pid, "Fib::compute", &e, buf, cap, &blen) hits the newest matching record whose code bytes are only partially flushed. fread is short, the function returns ASMTEST_PTRACE_OK, blen is uninitialized stack memory (e.g. a huge value), and the caller reads blen bytes of buf — out-of-bounds read / garbage code bytes handed to the disassembler.

**Suggested fix:** On a short fread of the code bytes, set rc = ASMTEST_PTRACE_EINVAL (or roll back to the previous complete match) before breaking, and/or initialize *bytes_len = 0 at function entry.

### 21. trace_call EXIT_RETURNED path ignores PTRACE_CONT/waitpid results: a signal stop between the routine's return and _exit leaves the tracee stopped and unreaped until the tracer exits — Low **[verified ×2]**

`src/ptrace_backend.c:749`

On the normal-return path the child is resumed with PTRACE_CONT and awaited once, both return values discarded. If that waitpid reports a stop (any signal delivered to the child between the routine's return and _exit(0) — e.g. a process-group SIGINT/SIGWINCH, plausible since this window includes the child's remaining C glue), the loop breaks with the child still alive in signal-delivery-stop: it is neither killed nor reaped, surviving until the tracer process exits (PTRACE_O_EXITKILL fires only then). Every other abnormal path in this function was hardened with kill+waitpid (the fault-path leak fix); this is the one remaining path where the child can outlive the call.

**Evidence:** src/ptrace_backend.c:749-751: `ptrace(PTRACE_CONT, pid, NULL, NULL); waitpid(pid, &status, 0); break;` — the status is never checked for WIFSTOPPED, unlike the non-SIGTRAP path at :703-705 which does `kill(pid, SIGKILL); waitpid(pid, &status, 0);`.

**Failure scenario:** A long-lived test runner traces many routines; during one trace a SIGWINCH/SIGINT reaches the process group in the tiny window after the routine returned. waitpid at :750 returns a stop status, the function returns OK, and the stopped child persists for the runner's lifetime — repeated across a large suite this accumulates stopped PIDs (the same exhaustion the :696 comment warns about).

**Suggested fix:** Loop on waitpid until WIFEXITED/WIFSIGNALED, resuming stops with PTRACE_CONT(sig), or simply kill(pid, SIGKILL)+waitpid after harvesting the return value (the child's only remaining work is _exit).

## Emulator & code-image

### 22. asmtest_codeimage_track() re-arms clear_refs globally, silently wiping pending soft-dirty state of previously tracked regions — bytes_at() then returns stale code bytes forever — High (impact assessed Medium) **[verified ×2]**

`src/codeimage.c:367`

ci_arm() writes "4" to /proc/<pid>/clear_refs, which clears the soft-dirty bit on EVERY page of the target process. refresh() is carefully written to scan ALL regions before re-arming (the comment at src/codeimage.c:377-378 explicitly acknowledges the global-wipe hazard), but track() has no such protection: it snapshots only the NEW region and then unconditionally calls ci_arm(). Any write that landed in an already-tracked region between the previous arm and this track() call has its dirty bit erased without ever being snapshotted, so the next refresh() sees the region as clean and records no new version. The timeline is then permanently wrong: asmtest_codeimage_bytes_at() returns the pre-write bytes as the 'current' version even though the target's code changed. This is precisely the JIT flow the module advertises (the eBPF detector exists to discover NEW regions mid-trace, which the caller then track()s while the JIT keeps patching existing tracked methods), and the header invites it: "May be called for several disjoint regions." A smaller instance of the same wipe exists inside refresh() itself (a write between ci_region_dirty() at :387 and ci_arm() at :403 is lost), but track()'s window spans arbitrary caller time. Fix: before arming in track(), run the same detect+snapshot pass refresh() performs over existing regions (e.g., factor refresh's loop out and call it when nreg > 0).

**Evidence:** src/codeimage.c:96-97: "Clear soft-dirty for the whole target so future writes become detectable. Global by design (clear_refs is process-wide); callers arm once after recording, see refresh()." — src/codeimage.c:366-367 (end of track()): "/* Arm: any write after this point is detectable on the next refresh. */ return ci_arm(img);" with no scan of existing regions, versus src/codeimage.c:377-378 in refresh(): "/* Detect + snapshot ALL regions BEFORE re-arming (clear_refs is global, so re-arming would wipe pending soft-dirty state for regions not yet scanned). */"

**Failure scenario:** track(A) at seq 1; the JIT patches a byte inside A (soft-dirty set on A's page); the eBPF detector reports a new method and the caller does track(B) — ci_arm() clears A's pending soft-dirty bit; refresh() finds A clean and records nothing; asmtest_codeimage_bytes_at(A, 0) returns the seq-1 (pre-patch) bytes while the live code differs — silently wrong bytes, unrecoverable until A is written again after the last arm.

**Suggested fix:** In asmtest_codeimage_track(), when img->nreg > 0, run the refresh() detection+snapshot loop over the existing regions before calling ci_arm() (or have track() delegate to a shared helper that scans-then-arms once).

### 23. emu_call/emu_call_fp silently drop SysV integer args beyond the 6th (no stack-arg marshaling), while the header promises args[0..nargs) and the Win64 path does marshal stack args — Medium (impact assessed Low) **[verified ×2]**

`src/emu.c:257`

emu_x86_setup_sysv() loads at most 6 integer args (loop bound `i < niargs && i < 6`) and never spills the rest to the guest stack, yet SysV AMD64 psABI §3.2.3 places INTEGER-class arguments beyond the sixth eightbyte on the stack starting at [rsp+8] at function entry. The public contract (include/asmtest_emu.h:105-107: "run them with the System V integer args (args[0..nargs))" and docs/emulator.md:70) states no 6-arg cap — only the FFI wrapper asmtest_emu_call6 documents clamping. The sibling Win64 path (emu_call_win64_traced, src/emu.c:463-473) DOES write stack args above the shadow space, and the native tier explicitly supports the same case (include/asmtest.h:336-338: asm_call_capture_args passes "the rest ... on the stack per the ABI"), so a differential test that passes natively silently mis-runs under the emulator. Worse than silent zeros: the setup plants the return address at the very top of the stack mapping (sp = EMU_STACK_BASE + EMU_STACK_SIZE - 8), so the slot where the routine looks for arg 7, [rsp+8] = 0x210000, is one byte past the mapped stack — the routine takes a spurious EMU_FAULT_READ instead of receiving its argument. emu_call_fp similarly drops double args past the 8th (psABI puts them on the stack), though that is at least consistent with the 8-register SSE budget.

**Evidence:** src/emu.c:253-259: "uint64_t sp = EMU_STACK_BASE + EMU_STACK_SIZE - 8; ... for (int i = 0; i < niargs && i < 6; i++) { ... uc_reg_write(uc, arg_regs[i], &v); }" — no stack-arg writes, versus the Win64 path src/emu.c:470-473: "for (int i = 0; i < stack_args; i++) { uint64_t v = (uint64_t)args[4 + i]; uc_mem_write(uc, sp + 40 + 8 * (uint64_t)i, &v, sizeof v); }". Header claim include/asmtest_emu.h:105-106: "run them with the System V integer args (args[0..nargs))".

**Failure scenario:** emu_call(e, fn, len, args, 7, 0, &r) for a 7-int-arg SysV routine: arg 7 is never written; the routine executes `mov rax,[rsp+8]` and reads 0x210000 (unmapped, one past the stack mapping) -> r.faulted = true, EMU_FAULT_READ at 0x210000 — or, if it only uses the value, computes with garbage — while the identical ASM_CALL-style native run passes.

**Suggested fix:** Marshal args[6..nargs) onto the guest stack below the return address per the psABI (allocate 8*(nargs-6) bytes, keep rsp ≡ 8 mod 16 at entry), or reject nargs > 6 explicitly and document the cap in emu_call/emu_call_fp.

### 24. AArch64 emulator marshals only x0..x5, but AAPCS64 assigns the first 8 integer args to x0..x7 (and the native tier passes 8 in registers) — args 7 and 8 are silently dropped — Low **[verified ×2]**

`src/emu.c:638`

emu_arm64_setup's arg_regs table stops at x5 and the loop clamps at 6, while AAPCS64 stage C (rule C.9) allocates the first eight integer/pointer arguments to r0..r7 (x0..x7). The framework's own native tier honors this (include/asmtest.h:336-337: "the first 6 (x86-64) / 8 (AArch64) go in registers"), and the RISC-V guest in the same file correctly fills all 8 arg registers a0..a7 (src/emu.c:806-808), so the 6-register cutoff on arm64 is an arbitrary parity break rather than a design rule. The header self-consistently documents "integer args in x0..x5" (include/asmtest_emu.h:239), but emu_arm64_call accepts any nargs and gives no error: a 7- or 8-arg AAPCS64 routine reads whatever stale/zero value is in x6/x7 and returns a wrong result with ok == true.

**Evidence:** src/emu.c:638-640: "static const int arg_regs[6] = {UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4, UC_ARM64_REG_X5};" and src/emu.c:647: "for (int i = 0; i < niargs && i < 6; i++)" — contrast src/emu.c:806-808 (RISC-V, 8 regs) and include/asmtest.h:336-337 (native AArch64: 8 register args).

**Failure scenario:** emu_arm64_call(e, code, len, args, 8, 0, &out) on an 8-arg AAPCS64 routine that sums its arguments: x6/x7 are never written (0 on a fresh handle, stale from a prior call on a reused one), so out.regs.x[0] is wrong while out.ok is true; the same routine passes under asm_call_capture_args on real hardware.

**Suggested fix:** Extend arg_regs to UC_ARM64_REG_X7 and the loop bound to 8 (x6/x7 are consecutive-adjacent enum values only via explicit entries — mirror the RISC-V table).

### 25. docs/emulator.md "Preloading memory" example maps guest memory at 0x100000, which is EMU_CODE_BASE — the map fails and the written value is overwritten by the routine's own code — Low **[verified ×2]**

`docs/emulator.md:109`

emu_open() pre-maps the code region at EMU_CODE_BASE = 0x00100000 (include/asmtest_emu.h:47, src/emu.c:189), and the header explicitly requires emu_map addresses "distinct from the internal code/stack regions" (include/asmtest_emu.h:98-99). The doc example nevertheless uses 0x100000: emu_map(E, 0x100000, 0x1000) returns false (Unicorn rejects overlapping maps) — ignored by the example — and emu_write(E, 0x100000, &value, ...) then succeeds into the CODE region, where the next emu_call's load_code() copies the routine bytes over it (src/emu.c:167 writes at EMU_CODE_BASE). The routine's load from 0x100000 therefore reads its own first instruction bytes, not `value`. The project's own tests use non-colliding addresses (0x300000 in examples/test_emu.c:106 and test_emu_usecases.c:31, 0x400000 for the watch tests), confirming the doc drifted.

**Evidence:** docs/emulator.md:109-112: "emu_map(E, 0x100000, 0x1000); emu_write(E, 0x100000, &value, sizeof value); /* call a routine that loads from 0x100000 ... */ emu_read(E, 0x100000, &out, sizeof out);" versus include/asmtest_emu.h:47: "#define EMU_CODE_BASE 0x00100000UL" and src/emu.c:189: "uc_mem_map(e->uc, EMU_CODE_BASE, EMU_CODE_SIZE, UC_PROT_ALL)".

**Failure scenario:** A user copies the doc example verbatim: emu_map silently fails, emu_call's load_code overwrites the preloaded value at 0x100000 with the routine's code bytes, and the routine returns its own opcode bytes reinterpreted as data — a baffling wrong result with no fault and no error.

**Suggested fix:** Change the example address to one outside the internal regions (e.g., 0x300000, matching the tests), and note that EMU_CODE_BASE/stack are reserved.

## Language bindings

### 26. Rust Guest::call_traced always calls emu_arm64_call_traced, overflowing the smaller Arm/Riscv result buffer (heap corruption from a safe API) — High **[verified ×2]**

`bindings/rust/src/lib.rs:1013`

Guest::call_traced does not dispatch on self.arch: it unconditionally invokes emu_arm64_call_traced, while `out` is allocated per-arch by new_result(). For GuestArch::Arm the buffer is calloc(1, sizeof(emu_arm_result_t)) = 360 bytes (asmtest_emu.h pins emu_arm_regs_t at 328 + a 32-byte header); for Riscv it is 808 bytes. emu_arm64_call_traced immediately does memset(out, 0, sizeof *out) with sizeof(emu_arm64_result_t) = 816 bytes and later writes the full arm64 register file through it, so a plain safe-Rust call writes 456 (Arm) or 8 (Riscv) bytes past the heap allocation. It also passes an emu_arm_t*/emu_riscv_t* engine handle to the arm64 entry point, so even absent a crash the run and register readback are garbage. Go, Java, and .NET all guard this exact call to the arm64 guest (Java: `throw new AsmtestException("traced guest run only wired for arm64")`); Rust has no guard, only a parenthetical "(AArch64)" in the doc comment, and the crate's own test only exercises Arm64 so the bug is never hit in CI.

**Evidence:** bindings/rust/src/lib.rs:1008-1017:
    pub fn call_traced(&self, code: &[u8], args: &[i64], trace: &Trace) -> GuestResult {
        ...
        let out = self.new_result();   // per-arch: asmtest_emu_arm_result_new() for Arm
        unsafe {
            emu_arm64_call_traced(self.h, code.as_ptr() as *const c_void, code.len(), ptr,
                                  args.len() as c_int, 0, out, trace.h);
        }
src/emu.c:682-686:
    bool emu_arm64_call_traced(emu_arm64_t *e, ..., emu_arm64_result_t *out, ...) {
        uc_engine *uc = e->uc;
        memset(out, 0, sizeof *out);   // sizeof(emu_arm64_result_t) == 816
src/ffi.c:324-326:
    emu_arm_result_t *asmtest_emu_arm_result_new(void) {
        return (emu_arm_result_t *)calloc(1, sizeof(emu_arm_result_t));  // 360 bytes
include/asmtest_emu.h:341: ASMTEST_STATIC_ASSERT(sizeof(emu_arm_regs_t) == 328, ...); :219 sizeof(emu_arm64_regs_t) == 784

**Failure scenario:** let g = Guest::new(GuestArch::Arm).unwrap(); let tr = Trace::new(); g.call_traced(&arm32_bytes, &[0], &tr); -> memset zeroes 816 bytes into a 360-byte calloc block, corrupting adjacent heap metadata/objects -> malloc abort, crash, or silent data corruption later; on Riscv an 8-byte overflow plus arm64 register writes on a RISC-V Unicorn engine yielding bogus results.

**Suggested fix:** Match on self.arch as Guest::call does, calling emu_riscv_call_traced / emu_arm_call_traced (declaring those externs), or return an error/panic for non-Arm64 arches as the Java/.NET bindings do.

### 27. Python EmuResult.reg() raises KeyError for 15 of the 18 documented x86 register names (manifest only publishes rax/rip/rflags/xmm) — High (impact assessed Medium) **[verified ×2]**

`bindings/python/asmtest/core.py:269`

EmuResult.reg() resolves the field offset via load().offset("emu_x86_regs_t", name), and Context.offset raises KeyError for any field the manifest does not list. The manifest generator emits only rax, rip, rflags, and xmm for emu_x86_regs_t (scripts/gen-manifest.c:101-106), so every other guest GP register — rbx, rcx, rdx, rsi, rdi, rbp, rsp, r8..r15 — raises KeyError even though the method's own docstring promises "the GP file plus rip/rflags". The register bytes are present in the result buffer (emu_x86_regs_t is a contiguous 18-u64 file, asmtest_emu.h:62-67); only the name lookup is missing. Ruby/Node/Lua use the C accessor asmtest_emu_x86_reg (src/ffi.c:139-150) which supports all 18 names, so Python is the only binding where this documented read fails. The conformance corpus only ever asserts rax/rflags, so no test catches it.

**Evidence:** core.py:266-272: `def reg(self, name): """Read an x86-64 guest register from the result — the GP file plus rip/rflags...""" base = load().offset("emu_result_t", "regs") + load().offset("emu_x86_regs_t", name)`; core.py:47-51: `def offset(...): ... raise KeyError(f"{struct_name}.{field_name} not in manifest")`; scripts/gen-manifest.c:101-106: `BEGIN(emu_x86_regs_t); FIELD(emu_x86_regs_t, rax); FIELD(emu_x86_regs_t, rip); FIELD(emu_x86_regs_t, rflags); FIELD(emu_x86_regs_t, xmm);`

**Failure scenario:** res = Emulator().call(code, [40, 2]); res.reg("rbx") -> KeyError: 'emu_x86_regs_t.rbx not in manifest'. Any pytest suite inspecting a callee-saved or argument register per the documented API errors out instead of reading the value.

**Suggested fix:** Add the remaining 14 GP fields to gen-manifest.c's emu_x86_regs_t block, or resolve names in core.py by index off the rax offset (the file is a contiguous uint64[18], same trick as src/ffi.c:139-150).

### 28. Node hwtrace: numeric addresses passed as void* are truncated to 32 bits by koffi (jitdumpFind -> runTo/traceAttached pipeline breaks) — Medium **[verified ×2]**

`bindings/node/hwtrace.js:530`

jitdumpFind() returns codeAddr as a JS Number (Number(entry.readBigUInt64LE(0))), and the module documents passing numeric addresses to the `const void*` parameters of Ptrace.runTo/traceAttached/procRegionByAddr and CodeImage.track/bytesAt ("`base` may be a NativeCode's external pointer or a numeric/BigInt address", hwtrace.js:586). But koffi converts a JS Number pointer argument with Int32Value(): call.cc `case napi_number: intptr_t ptr = (intptr_t)number.Int32Value();` (verified in koffi 2.16.x, the version ^2.8.0 resolves to). Any real 64-bit address (JIT code lives at ~0x7f...., i.e. >= 2^31) is silently truncated modulo 2^32, so the binding's own resolve-then-trace pipeline hands the C tier a wrong address. Only BigInt goes through the lossless Int64Value path. Python (c_void_p), Ruby (Fiddle::Pointer), and Lua (ffi.cast) pass the full address.

**Evidence:** hwtrace.js:530: `codeAddr: Number(entry.readBigUInt64LE(0)),` returned as "the base to trace"; hwtrace.js:492-494: `static runTo(pid, addr) { return _fn.ptraceRunTo(pid, addr); }` with declaration `'int asmtest_ptrace_run_to(int, const void*)'` (hwtrace.js:196); koffi src/call.cc:1040-1046: `case napi_number: { ... intptr_t ptr = (intptr_t)number.Int32Value(); *out_ptr = (void *)ptr; ... }`

**Failure scenario:** const m = Ptrace.jitdumpFind(null, 'method', pid); Ptrace.runTo(pid, m.codeAddr) with codeAddr = 0x7f2030001000 -> koffi passes 0x30001000 to asmtest_ptrace_run_to, which PTRACE_POKETEXTs an int3 at the wrong target address (corrupting foreign memory if mapped) and the breakpoint at the real method never fires; traceAttached with the same numeric base traces the wrong region.

**Suggested fix:** Return codeAddr/codeSize as BigInt from jitdumpFind (or keep the External pointer), and coerce any numeric address argument with BigInt(addr) before passing it to a pointer parameter (koffi's BigInt path uses lossless Int64Value).

### 29. Lua binding funnels all 64-bit read-backs through tonumber(), silently corrupting values >= 2^53 — Medium (impact assessed Low) **[verified ×2]**

`bindings/lua/asmtest.lua:175`

Every 64-bit result accessor converts the uint64_t/long cdata to a Lua double with tonumber(): Regs:ret (asmtest.lua:175), EmuResult:fault_addr (:194), EmuResult:reg (:197), Watch:addr/rip_off (:212-213), RegGuard:got (:219), and in hwtrace.lua NativeCode:call (:203), ptrace results (:440,:455,:473), proc_region_by_addr/proc_perfmap_symbol base+len (:496,:507), and jitdump code_addr/size/timestamp/index (:527-530). A double holds only 53 bits of integer precision, so any register value, return value, or address >= 2^53 is silently rounded, and two distinct 64-bit values can compare equal in M.assert_ret/assert_emu_reg. This is the Lua counterpart of the already-known Node Number() issue (#6) but was not previously reported; the other known Lua item (#4) is the unrelated dropped-args bug. Ruby (Fiddle bignum) and Python (int) read back exactly.

**Evidence:** asmtest.lua:175: `function Regs:ret() return tonumber(L.asmtest_regs_ret(self.h)) end` where asmtest_regs_ret returns `unsigned long` (src/ffi.c:29-31); asmtest.lua:197: `function EmuResult:reg(name) return tonumber(L.asmtest_emu_x86_reg(self.h, name)) end`; asmtest.lua:455-457: `M.assert_ret` compares the rounded double against want.

**Failure scenario:** A hash routine returns 0x1234567890ABCDF1 in rax; Regs:ret() returns the double 1311768467294899200 (low bits lost), so assert_ret against the true value fails — and conversely a mutated routine returning 0x1234567890ABCDF0 false-passes because both round to the same double.

**Suggested fix:** Return the boxed cdata unchanged (LuaJIT compares/prints uint64_t cdata exactly) or add *_u64 accessors returning cdata, converting via tonumber only for values known to fit 53 bits.

### 30. C++ hwtrace/drtrace wrappers call null function pointers when the optional lib is absent, contradicting their own no-crash claim — Medium **[verified ×2]**

`bindings/cpp/asmtest_hwtrace.hpp:529`

When dlopen of libasmtest_hwtrace/libasmtest_drapp fails, detail::api() returns a table whose function pointers are all nullptr (only available()/skip_reason()/resolve*/auto* check loaded()). Every other entry point invokes the pointer unguarded: HwTrace::init/shutdown/create/register_region, NativeCode::from_bytes, RegionScope's begin/end, Ptrace::traceCall/traceAttached/runTo/regionByAddr/perfmapSymbol/jitdumpFind, and in asmtest_drtrace.hpp NativeTrace::initialize/shutdown/marker_error/symbol_demo/create and NativeCode::from_bytes. Calling through nullptr is an immediate SIGSEGV, yet both headers document "nothing here crashes or throws on a missing lib" and init/initialize document throwing std::runtime_error on failure. Every other binding (Go, Rust, Java, Zig, .NET) converts the missing-lib case into an error/exception on these same entry points.

**Evidence:** bindings/cpp/asmtest_hwtrace.hpp:30-32: "When the library is unavailable, HwTrace::available() returns false and callers self-skip; nothing here crashes or throws on a missing lib."
bindings/cpp/asmtest_hwtrace.hpp:526-533:
    static void init(int backend = SINGLESTEP) {
        ...
        int rc = detail::api().init(&opts);   // init == nullptr when dlopen failed
bindings/cpp/asmtest_hwtrace.hpp:535: static void shutdown() { detail::api().shutdown(); }
bindings/cpp/asmtest_drtrace.hpp:297: int rc = a.dr_init(&opts);  (no a.loaded() check)
bindings/cpp/asmtest_hwtrace.hpp:241-245: dlopen failure returns table with handle==nullptr and every slot nullptr.

**Failure scenario:** Host without the optional tier built (no build/libasmtest_hwtrace.so, no ASMTEST_HWTRACE_LIB): test harness calls HwTrace::init() expecting the documented std::runtime_error, or calls HwTrace::shutdown() in an unconditional teardown path after a self-skip -> jump to address 0 -> SIGSEGV instead of a catchable error/self-skip.

**Suggested fix:** Guard each wrapper with detail::api().loaded() and throw std::runtime_error("libasmtest_hwtrace not found...") (matching skip_reason's message), or fix the header comment and document that all non-available() calls require a prior available() gate.

### 31. Java binding allocates all per-call native buffers from a never-closed shared Arena, so native memory grows unboundedly with call count — Medium (impact assessed Low) **[verified ×2]**

`bindings/java/Asmtest.java:63`

Asmtest.java (and HwTrace.java/DrTrace.java, same pattern) uses one static `Arena.ofShared()` for every transient native allocation: bytesSeg/longsSeg/doublesSeg/vecsSeg copies of code and argument arrays, every str(name) C-string (including each Regs.flagSet / EmuResult.reg register-name lookup), the 160-byte disas/skip-reason buffers, and captureVec256/512 scratch. A shared Arena only releases memory at close(), which is never called, so every call site leaks its allocation for the JVM's lifetime. For a testing library whose intended usage is high-iteration loops (differential/property testing, fuzz sweeps, per-instruction trace readback), native RSS grows monotonically with the number of API calls and cannot be reclaimed.

**Evidence:** bindings/java/Asmtest.java:63: private static final Arena ARENA = Arena.ofShared();  // never closed
bindings/java/Asmtest.java:292-295:
    private static MemorySegment bytesSeg(byte[] b) {
        MemorySegment seg = ARENA.allocate(Math.max(b.length, 1));  // per callBytes/callTraced/... call
bindings/java/Asmtest.java:333: private static MemorySegment str(String s) { return ARENA.allocateUtf8String(s); }  // per reg()/flagSet() call
bindings/java/HwTrace.java:99: private static final Arena ARENA = Arena.ofShared(); (same pattern, e.g. :774 argSeg per ptraceTraceCall)

**Failure scenario:** A JUnit property test loops 1,000,000 times over emu.callBytes(code, x) and res.reg("rax"): each iteration permanently allocates the code copy, the args segment, and the "rax" C-string from the global arena -> hundreds of MB of native memory that is never freed until the JVM exits, eventually OOM-killing long-running CI workers.

**Suggested fix:** Allocate per-call temporaries from a confined arena per call (try (Arena a = Arena.ofConfined()) { ... }) and keep the shared ARENA only for the library lookup and truly static strings.

### 32. Python hwtrace available()/skip probes raise OSError instead of self-skipping when libasmtest_hwtrace is absent — Low **[verified ×2]**

`bindings/python/asmtest/hwtrace.py:388`

HwTrace.available(), Ptrace.available(), and CodeImage.available() all call _get() -> _load(), which raises OSError when no candidate library loads (hwtrace.py:208-211). The module docstring promises "HwTrace.available(backend) reports whether the chosen backend can run so callers self-skip cleanly" (hwtrace.py:17-18), and the Node (hwtrace.js:272-275), Ruby (hwtrace.rb:317-319), and Lua (hwtrace.lua:222-224) bindings all fold a load failure into a clean false. Python is the only binding where the documented gate itself throws.

**Evidence:** hwtrace.py:386-388: `def available(backend=SINGLESTEP) -> bool: ... return bool(_get().asmtest_hwtrace_available(backend))` with hwtrace.py:208-211: `raise OSError("libasmtest_hwtrace not found; build it with `make shared-hwtrace` ...")`; contrast hwtrace.js:272-275: `static available(backend = SINGLESTEP) { if (!_lib) return false; ... }`

**Failure scenario:** On a checkout (or install) where the hwtrace tier was never built, a test following the documented idiom `if HwTrace.available(SINGLESTEP):` errors with OSError instead of skipping, while the identical Node/Ruby/Lua tests skip cleanly.

**Suggested fix:** Catch the load OSError in available()/skip_reason() (returning False / a 'load failed: ...' string, as the other three bindings do), keeping the raise for calls that genuinely need the library (init, NativeCode.from_bytes).

## Windows port

### 33. Win64 run_one skips TEARDOWN hooks whenever the test body fails, skips, crashes, or times out (diverges from the POSIX runner) — Medium **[verified ×2]**

`src/asmtest.c:819`

The POSIX run_one uses three separate sigsetjmp scopes and always runs the teardown hooks once setup succeeded, even after a body failure or SKIP (lines 798-802: "teardown — runs because setup succeeded; may itself fail"). The Win64 run_one collapses setup, body, and teardown into ONE __builtin_setjmp scope, so any asmtest_fail/SKIP/crash/timeout longjmps straight past run_hooks(tc->suite, 1) and teardown never executes. This affects both the default re-exec-isolation path (the child runs run_one, so external teardown effects like temp-file cleanup are lost for every failing test) and --no-fork, where a suite's TEARDOWN that resets global state is skipped, contaminating every subsequent test in the same process. tests/win64/suite_win64.c registers no SETUP/TEARDOWN, so win64-runner-test never exercises this.

**Evidence:** src/asmtest.c:819-823 (Win64): "if (__builtin_setjmp(asmtest_win32_test_recover) == 0) { run_hooks(tc->suite, 0); /* setup */ tc->fn(); /* body */ run_hooks(tc->suite, 1); /* teardown */ outcome = ST_PASS; } else { ... }" — vs POSIX src/asmtest.c:790-802 where the body has its own sigsetjmp and "if (sigsetjmp(asmtest_jmp, 1) == 0) run_hooks(tc->suite, 1);" runs unconditionally afterwards.

**Failure scenario:** Win64 build, --no-fork: a suite defines TEARDOWN(s) that frees a guarded allocation / resets a global; test A fails an ASSERT (asmtest_fail -> asmtest_jump -> longjmp). Teardown is skipped, so test B of the same suite runs against the dirty state and reports a wrong verdict; on POSIX the identical suite runs teardown and passes.

**Suggested fix:** Mirror the POSIX structure: after the recovery branch, re-arm a second __builtin_setjmp scope and run run_hooks(tc->suite, 1) whenever setup succeeded, folding a teardown failure into the outcome.

### 34. Crash/timeout recovery resumes the runner with the faulting routine's EFLAGS (DF not cleared), violating the ABI the recovered C code relies on — Medium **[verified, single pass]**

`src/platform_win32.c:326`

Both vectored handlers (asmtest_win32_veh, rt_veh_cb) and the watchdog (rt_timer_cb) redirect only Rsp/Rip in the captured CONTEXT and resume via EXCEPTION_CONTINUE_EXECUTION / SetThreadContext; __builtin_longjmp then restores only fp/sp/pc. The interrupted routine's RFLAGS — in particular the direction flag — survives into the runner. The Microsoft x64 ABI requires "the direction flag must be cleared on function entry and return"; compilers and the CRT assume DF=0 and emit rep movs/stos accordingly. Backward string routines (std-based copies) are exactly the kind of assembly this framework tests, and the guarded_alloc overrun trap makes an AV with DF=1 a designed-for event. On POSIX this cannot happen: the kernel clears DF when entering a signal handler, so the siglongjmp recovery resumes with DF=0. The same gap leaves MxCsr/x87 state unrestored (a real Win64 setjmp saves xmm6-15 and MxCsr; the 5-word __builtin_setjmp buffer does not).

**Evidence:** src/platform_win32.c:323-327: "DWORD64 sp = info->ContextRecord->Rsp; sp = (sp & ~(DWORD64)15) - 8; info->ContextRecord->Rsp = sp; info->ContextRecord->Rip = (DWORD64)(ULONG_PTR)asmtest_win32_landing; return EXCEPTION_CONTINUE_EXECUTION;" — ContextRecord->EFlags is never touched; identical pattern at 379-383 (rt_veh_cb) and 400-404 (rt_timer_cb).

**Failure scenario:** A routine under test executes std for a descending copy, overruns an asmtest_guarded_alloc buffer, and faults on the guard page. rt_veh_cb redirects to rt_landing and run_one continues with DF=1; the next inlined memset/memcpy (capture_wire's memset of the ~1.3KB wire_result_t, or vsnprintf internals emitting rep stos) writes descending, corrupting the stack — the re-exec child crashes after having contained the fault (parent reports a raw crash instead of the clean failure), and under --no-fork the surviving runner corrupts state for all later tests.

**Suggested fix:** In both VEH callbacks clear DF (info->ContextRecord->EFlags &= ~0x400) — and ideally reset MxCsr to 0x1F80 — before resuming; in rt_timer_cb do the same on the ctx written by SetThreadContext.

### 35. Watchdog vs asmtest_win32_test_end race: hijacking the runner inside DeleteTimerQueueTimer causes a second delete of the same timer and flips a passing test to TIMEOUT — Low **[verified ×2]**

`src/platform_win32.c:432`

If the deadline fires just as the test completes, rt_timer_cb wins the InterlockedExchange on rt_armed before the main thread reaches test_end's disarm. The main thread then enters test_end and blocks in DeleteTimerQueueTimer(..., INVALID_HANDLE_VALUE) waiting for that very callback, which proceeds to SuspendThread/SetThreadContext the main thread and longjmp it back into run_one — abandoning the in-progress DeleteTimerQueueTimer mid-call. run_one then reports TIMEOUT for a test that had already passed and calls asmtest_win32_test_end() a second time (rt_timer was never cleared), issuing a second DeleteTimerQueueTimer on a timer whose deletion was already initiated — per MSDN an invalid double-delete that can free the timer object twice (use-after-free inside the timer queue).

**Evidence:** src/platform_win32.c:391-406: "if (!InterlockedExchange(&rt_armed, 0)) return; ... SuspendThread(rt_test_thread); ... ctx.Rip = (DWORD64)(ULONG_PTR)rt_landing; SetThreadContext(...); ResumeThread(...)" — the hijack targets the main thread wherever it is, including inside src/platform_win32.c:430-434's "DeleteTimerQueueTimer(rt_timer_queue, rt_timer, INVALID_HANDLE_VALUE); rt_timer = NULL;" which run_one (src/asmtest.c:846) re-enters after the longjmp because rt_timer was never NULLed.

**Failure scenario:** --no-fork --timeout N where a test finishes within a scheduler quantum of its deadline: the passing test is reported "timed out after N s", and the doubled DeleteTimerQueueTimer on the freed timer object can crash or corrupt the timer queue, taking down the rest of the run.

**Suggested fix:** Have rt_timer_cb capture and clear rt_timer/rt_test_thread state itself (or have test_end detect that the callback won the disarm race and skip/serialize the delete, e.g. via a second interlocked flag), and never hijack a thread that has already entered test_end.

### 36. The --no-fork watchdog cannot recover a test blocked in a kernel wait: SetThreadContext only takes effect on return to user mode, so the runner hangs despite --timeout — Low **[verified ×2]**

`src/platform_win32.c:402`

rt_timer_cb 'unhangs' a test by SuspendThread + SetThreadContext(Rip=rt_landing). On Windows, a thread blocked in a kernel wait (Sleep(INFINITE), WaitForSingleObject on a never-signaled handle, a deadlocked critical section) stays in the wait; the rewritten trap-frame Rip only applies when the thread next returns to user mode, which never happens. The POSIX analogue (alarm + SIGALRM + siglongjmp) does interrupt blocking syscalls, so this is a silent capability regression of the port. The only hang exercised by the tests (suite_win64.c:72-75) is a pure spin loop, which is exactly the one case the mechanism handles; the header's claim that begin arms "a watchdog (a hung test)" (platform_win32.h:58-59) over-promises for blocked-wait hangs.

**Evidence:** src/platform_win32.c:396-406: "SuspendThread(rt_test_thread); CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL; if (GetThreadContext(rt_test_thread, &ctx)) { ... ctx.Rip = (DWORD64)(ULONG_PTR)rt_landing; SetThreadContext(rt_test_thread, &ctx); } ResumeThread(rt_test_thread);" — no mechanism (e.g. QueueUserAPC on an alertable wait, or CancelSynchronousIo/TerminateThread fallback) forces the blocked thread back to user mode.

**Failure scenario:** Win64 native runner, --no-fork --timeout 3, a test deadlocks on WaitForSingleObject(event, INFINITE): the timer fires, sets reason=TIMEOUT, rewrites the context — and the runner then waits forever; the whole suite wedges even though a timeout was configured (the default forked mode would have contained it).

**Suggested fix:** Document the limitation, or add a fallback: after SetThreadContext, wait briefly for the landing to be reached and otherwise fail hard (e.g. report the timeout from the watchdog thread and ExitProcess with a distinctive code) instead of wedging.

## Test-suite quality

### 37. expect.sh runs the negative suite unfiltered without --timeout: 20s of guaranteed spinning per `make check`, and an infinite hang if ASMTEST_TIMEOUT=0 is set — Medium **[verified by hand]**

`tests/expect.sh:173`

negative.c's header states the contract: "the harness never runs it unfiltered (the timeout case would stall)" (tests/negative.c:8-9). But the -jN checks at expect.sh:173 and :175 run `$NEG` with no --filter and no --timeout, so neg.timeout (an infinite volatile loop) spins until the runner's default 10-second alarm in each of the two invocations. Every `make check` burns ~20s of CPU (measured: expect.sh total 22.1s wall, 21.0s user, vs ~2s for everything else). Worse, the runner honors the documented ASMTEST_TIMEOUT env var before the default (src/asmtest.c:1661-1668); with ASMTEST_TIMEOUT=0 (the documented way to disable timeouts) exported in the environment, alarm() is never armed and both unfiltered runs never terminate — `make check` hangs forever.

**Evidence:** tests/expect.sh:173 `expect_fail_msg "-j4 still reports failures" "ASSERT_EQ" "$NEG" --jobs=4` and :175 `expect_fail_re "-j4 contains a crash" "$CRASH_RE" "$NEG" -j4` — no --filter/--timeout. tests/negative.c:8-9: "the harness never runs it unfiltered (the timeout case would stall)". Measured: `time sh tests/expect.sh` -> 21.03s user / 22.115s total, 35/35 ok. Hang confirmed: `timeout 8 env ASMTEST_TIMEOUT=0 ./build/tests_negative -j4` -> exit 124 (killed by timeout, never finished).

**Failure scenario:** A developer (or CI job) with ASMTEST_TIMEOUT=0 in the environment runs `make check`: the -j4 checks block forever on neg.timeout's spin loop and the job hangs until externally killed. Even without that env var, every `make check` wastes ~20 CPU-seconds spinning.

**Suggested fix:** Pass an explicit short timeout on both unfiltered runs (`"$NEG" --jobs=4 --timeout=1` and `"$NEG" -j4 --timeout=1`) — --timeout overrides ASMTEST_TIMEOUT — or exclude the timeout case via a filter, and update the negative.c comment to match.

**Hand verification:** `tests/expect.sh:173,175` run `$NEG` with no `--filter`/`--timeout`, which `tests/negative.c:8-9` says the harness must never do; the runner honours `ASMTEST_TIMEOUT` before the default (`src/asmtest.c:1661-1668`). The finder measured ~21 s of CPU per `make check` and reproduced the hang: `timeout 8 env ASMTEST_TIMEOUT=0 ./build/tests_negative -j4` exits 124.

### 38. expect.sh's --shuffle coverage is tautological: it passes if --shuffle is a complete no-op — Low **[verified by hand]**

`tests/expect.sh:163`

The only shuffle assertion runs the identical command twice (`--shuffle --seed=123`) and checks the outputs are equal. If the Fisher-Yates loop (src/asmtest.c:1755-1763) were deleted, or --shuffle/--seed parsing regressed so opt.shuffle never gets set, both runs produce the registration order and the check still passes — determinism holds trivially for the identity permutation. Nothing asserts that a shuffled order ever differs from the serial order or that different seeds produce different orders, so the file's claim of "proving ... --shuffle ... behave as documented" (expect.sh:8-10) is not met: only same-seed reproducibility is tested, not shuffling.

**Evidence:** tests/expect.sh:162-165: `# --shuffle --seed is deterministic: same seed -> same order.` / `ord1=$("$POS" --shuffle --seed=123 2>/dev/null | grep '^ok')` / `ord2=$("$POS" --shuffle --seed=123 2>/dev/null | grep '^ok')` / `if [ "$ord1" = "$ord2" ]; then ok "--shuffle --seed deterministic" ...` — the sole --shuffle assertion in the file.

**Failure scenario:** A regression makes `--shuffle` silently ignored (e.g. the strcmp branch at src/asmtest.c:1597 is broken or the permutation loop is dropped): tests always run in registration order, hiding order-dependence bugs the flag exists to expose, yet `make check` still reports 35/35 ok.

**Suggested fix:** Add a positive shuffle assertion: pick a seed whose permutation of the current suite demonstrably differs from registration order and assert `ord1 != serial` (or assert orders for two different seeds differ), alongside the existing same-seed determinism check.

**Hand verification:** `tests/expect.sh:162-165` is the sole `--shuffle` assertion in the harness and runs the identical command twice, so the identity permutation (shuffle a no-op) passes it.

### 39. test_drtrace.c 'coverage accumulates' check can never fail (>= against a monotonic counter); the second run's new block is never asserted — Low **[verified by hand]**

`examples/test_drtrace.c:109`

After a second traced call chosen specifically to take the dec branch (fn(60,60) -> two blocks), the harness asserts `asmtest_emu_trace_blocks_len(tr) >= blocks_before`. blocks_len only grows, so equality holds even if the second traced call records nothing at all — the assertion is a tautology and the labeled property ("re-running the region accumulates coverage") is untested. The discriminating fact — the dec-path block at offset 0xe becomes covered — is never asserted, even though the file's own encoding comment identifies that offset.

**Evidence:** examples/test_drtrace.c:104-110: `unsigned long long blocks_before = asmtest_emu_trace_blocks_len(tr); ... long r2 = fn(60, 60); /* 120 > 100 -> dec -> 119 */ ... CHECK(asmtest_emu_trace_blocks_len(tr) >= blocks_before, "re-running the region accumulates coverage");` — `>=` on a counter that never decreases. The dec block offset is documented at :43-44 (`e: 48 ff c8 dec rax`), but no `asmtest_trace_covered(tr, 0xe)` check exists.

**Failure scenario:** A DynamoRIO client regression stops recording blocks after the first begin/end window (e.g. the region stays marked inactive on re-entry): the second call records zero new blocks, real accumulation is broken, but the smoke test still prints `ok ... re-running the region accumulates coverage` and the lane stays green.

**Suggested fix:** Assert the new block directly: `CHECK(asmtest_trace_covered(tr, 0xe), ...)` (and/or use a strict `>` since the dec path is guaranteed new after the first 20+22 run).

**Hand verification:** `examples/test_drtrace.c:104-110` asserts `>=` on a counter that never decreases; the discriminating dec-branch block (offset 0xe, documented at `:43-44`) is never asserted covered.

## Build / CI / packaging

### 40. release.yml corresponding-source attach step fails on any real tag: token is contents:read and gh release upload cannot create a missing release — High (impact assessed Medium) **[verified ×2]**

`.github/workflows/release.yml:105`

The workflow sets a global `permissions: contents: read` (lines 14-15) and no job overrides it. The corresponding-source job's tag-gated step runs `gh release upload` with `GH_TOKEN: ${{ github.token }}`; uploading release assets requires contents:write, so the API returns 403 (Resource not accessible by integration). Independently, `gh release upload` does not create a release — pushing a tag alone creates no release object, so even with write permission the step fails with 'release not found' unless a release was manually created first. Either way, the very job that exists to satisfy the GPL corresponding-source obligation fails the release workflow on the first genuine tag, for reasons unrelated to the known 'pipeline never fired' finding.

**Evidence:** release.yml:14-15 `permissions:\n  contents: read`; release.yml:101-105 `- name: Attach to the GitHub release (on a tag)\n  if: ${{ startsWith(github.ref, 'refs/tags/') }}\n  env:\n    GH_TOKEN: ${{ github.token }}\n  run: gh release upload "${GITHUB_REF_NAME}" corresponding-source/*.tar.gz corresponding-source/SOURCES.txt --clobber`

**Failure scenario:** Maintainer pushes tag v1.0.0 -> release workflow runs -> corresponding-source job builds the archive, then `gh release upload v1.0.0 ...` fails (no release exists; and with one created manually, HTTP 403 from the read-only token) -> job and workflow fail, GPL source never attached.

**Suggested fix:** Add `permissions: contents: write` to the corresponding-source job and use `gh release create "$GITHUB_REF_NAME" --verify-tag ... || true` before upload (or `gh release upload --clobber` after ensuring the release exists).

### 41. lua-package hardcodes the rockspec filename asmtest-1.0.0-1.rockspec, which sync-version renames on the first version bump — Medium **[verified ×2]**

`mk/bindings.mk:386`

The lua packer copies a literally-named rockspec instead of using $(ASMTEST_VERSION). scripts/sync-version.sh:109-112 explicitly renames the rockspec file to asmtest-$VERSION-1.rockspec when VERSION changes ('the filename encodes name-version-revision, so rename it'). So the first release after a VERSION bump (with manifests correctly synced) breaks `make lua-package`, which is run by release.yml's dlopen-package matrix (lang: lua, both ubuntu and macos) and by `make packages`.

**Evidence:** mk/bindings.mk:386 `cp -f bindings/lua/asmtest-1.0.0-1.rockspec $(PKG_DIST)/lua/`; scripts/sync-version.sh:111-112 `oldrock=$(ls "$LUA_DIR"/asmtest-*.rockspec ...)` / `newrock="$LUA_DIR/asmtest-$VERSION-1.rockspec"`; VERSION currently `1.0.0`

**Failure scenario:** Bump VERSION to 1.0.1, run `make sync-version` (renames the rockspec to asmtest-1.0.1-1.rockspec), tag -> release lua jobs run `make lua-package` -> `cp: cannot stat 'bindings/lua/asmtest-1.0.0-1.rockspec'` -> both lua release jobs fail.

**Suggested fix:** Use `cp -f bindings/lua/asmtest-$(ASMTEST_VERSION)-1.rockspec $(PKG_DIST)/lua/` (or glob bindings/lua/asmtest-*.rockspec).

### 42. Runner and test-suite objects miss real header prerequisites: src/platform.h is not a prereq of asmtest.o, and the examples/tests pattern rules depend only on asmtest.h — Medium **[verified ×2]**

`Makefile:202`

There is no auto-dependency generation (no -MMD/-MD anywhere), so the hand-listed prerequisites are the only rebuild edges. src/asmtest.c includes "platform.h" (which in turn includes glob_match.h and platform_win32.h), but none of the three rules compiling it — $(BUILD)/asmtest.o (Makefile:202), $(BUILD)/pic/asmtest.o (Makefile:342), $(BUILD)/asmtest_nomain.o (mk/bindings.mk:17) — lists src/platform.h. Likewise the generic pattern rules for test objects (Makefile:205, 209) list only include/asmtest.h, yet examples/test_emu.c includes asmtest_emu.h and examples/test_asm.c includes asmtest_assemble.h, so editing a tier header rebuilds the tier object (whose explicit rule lists it) but not the test TU that shares its structs.

**Evidence:** Makefile:202 `$(BUILD)/asmtest.o: src/asmtest.c include/asmtest.h | $(BUILD)`; src/asmtest.c:6 `#include "platform.h"`; src/platform.h:20-21 `#include "glob_match.h"` / `#include "platform_win32.h"`; Makefile:205 `$(BUILD)/%.o: examples/%.c include/asmtest.h | $(BUILD)`; examples/test_emu.c:9 `#include "asmtest_emu.h"`

**Failure scenario:** Edit include/asmtest_emu.h (e.g. add a field to the emu result struct) and run `make emu-test` in an existing tree: build/emu.o rebuilds (its rule lists asmtest_emu.h) but build/test_emu.o does not -> test_emu links two layouts of the same struct -> garbage register values or a crash; similarly editing src/platform.h leaves every suite's runner stale under `make test`.

**Suggested fix:** Add src/platform.h (plus src/glob_match.h, src/platform_win32.h) to the asmtest.o rules, and either list the tier headers on the affected test objects or switch to compiler-generated deps (-MMD -MP + include $(wildcard $(BUILD)/*.d)).

### 43. PIC object rules omit include/asmtest_trace.h (and ptrace_backend.o omits asmtest_codeimage.h) while their non-PIC twins list it, so a header edit produces a shared library with mixed struct layouts — Medium (impact assessed Low) **[verified ×2]**

`Makefile:354`

$(BUILD)/pic/emu.o (Makefile:354), $(BUILD)/pic/ffi.o (Makefile:345) and $(BUILD)/pic/fuzz.o (Makefile:359) do not list include/asmtest_trace.h, although each TU includes it transitively (asmtest_emu.h:27 includes asmtest_trace.h) — and the corresponding non-PIC rules (Makefile:636, 653, 646) DO list it, proving the edge is known to be needed. In the same class, both ptrace_backend.o rules (mk/native-trace.mk:208 and :592) omit include/asmtest_codeimage.h even though src/ptrace_backend.c:31 includes it, while codeimage.o's rules list it. After editing one of these headers, some objects inside libasmtest_emu / libasmtest_hwtrace rebuild and others do not, and the freshly-relinked .so mixes two ABI layouts of the shared trace/codeimage structs.

**Evidence:** Makefile:354 `$(BUILD)/pic/emu.o: src/emu.c include/asmtest_emu.h | $(BUILD)/pic` vs Makefile:636 `$(BUILD)/emu.o: src/emu.c include/asmtest_emu.h include/asmtest_trace.h | $(BUILD)`; include/asmtest_emu.h:27 `#include "asmtest_trace.h"`; mk/native-trace.mk:208-209 `$(BUILD)/ptrace_backend.o: src/ptrace_backend.c include/asmtest_ptrace.h include/asmtest_trace.h | $(BUILD)` vs src/ptrace_backend.c:31 `#include "asmtest_codeimage.h"`

**Failure scenario:** Edit include/asmtest_trace.h (add a field to the trace block struct) then `make python-test`: pic/trace.o rebuilds, pic/emu.o and pic/ffi.o are reused stale -> libasmtest_emu.so links objects compiled against two different layouts -> binding tests read shifted fields (silent wrong data) or segfault.

**Suggested fix:** Mirror the non-PIC prerequisite lists onto the pic/ rules (add include/asmtest_trace.h to pic/emu.o, pic/ffi.o, pic/fuzz.o; add include/asmtest_codeimage.h to both ptrace_backend.o rules), or adopt -MMD dep files.

### 44. check-version is documented as a CI gate but no workflow runs it, so VERSION/manifest drift is only discovered at release time — Medium **[verified ×2]**

`Makefile:109`

The Makefile claims the gate exists twice — help text 'check-version   verify every manifest matches VERSION (CI)' (line 109) and the comment '`make check-version` verifies they match (run in CI)' (lines 158-159) — but grep of .github/workflows/ shows neither ci.yml nor release.yml invokes check-version (or sync-version) anywhere. With the gate absent, a VERSION bump without `make sync-version` sails through every CI job and only explodes inside the release workflow: `make ruby-package` does `mv bindings/ruby/asmtest-$(ASMTEST_VERSION).gem` while `gem build` produced the old-version filename, and the dotnet smoke installs `--version "$ver"` (from VERSION) against a nupkg packed with the stale csproj version; npm would publish the stale version silently.

**Evidence:** Makefile:109 `@echo '  check-version   verify every manifest matches VERSION (CI)'`; Makefile:158-159 `...verifies they match (run in CI)`; `grep -rn 'check-version\|sync-version' .github/workflows/` returns nothing; mk/bindings.mk:347 `mv bindings/ruby/asmtest-$(ASMTEST_VERSION).gem $(PKG_DIST)/ruby/`

**Failure scenario:** Edit VERSION to 1.0.1, forget sync-version, push -> all CI jobs green -> tag v1.0.1 -> release ruby job fails at the mv (gem built as asmtest-1.0.0.gem), dotnet smoke fails to resolve AsmTest 1.0.1, node would publish 1.0.0 again.

**Suggested fix:** Add a step (e.g. in the bindings-parity job or a small version job) running `make check-version` in ci.yml.

### 45. Payload jobs swallow real apt-get failures with `[ Linux ] && apt-get install ... || true`, so a release payload can silently ship hwtrace without PT/CoreSight decoders — Low **[verified ×2]**

`.github/workflows/ci.yml:455`

In ci.yml package-libs (lines 455-456), release.yml native (38-39) and release.yml python (137-138), the Linux-only install of patchelf/cmake/libipt-dev/libopencsd-dev is written as `[ "$RUNNER_OS" = "Linux" ] && sudo apt-get install -y ... || true`. The `|| true` exists for the macOS branch but also masks a genuine apt failure on Linux. patchelf/cmake absence is caught downstream (package-native.sh / the drclient build fail), but libipt-dev/libopencsd-dev absence is not: pt_backend.o/cs_backend.o compile decoder-free by design and package-libs-verify checks only lib presence and rpaths, not decoder linkage — so the published wheel/payload loses the Intel PT and CoreSight decode capability with every job green.

**Evidence:** ci.yml:455-456 `[ "$RUNNER_OS" = "Linux" ] && sudo apt-get install -y \\\n            patchelf cmake libipt-dev libopencsd-dev || true`; mk/native-trace.mk:139-144 falls back to decoder-free when the intel-pt.h probe fails

**Failure scenario:** Transient apt mirror error during a tag build -> libipt-dev not installed -> hwtrace built without -DASMTEST_HAVE_LIBIPT -> release publishes a wheel whose PT backend permanently self-skips; no job fails, nothing flags the degraded artifact.

**Suggested fix:** Restructure as `if [ "$RUNNER_OS" = "Linux" ]; then sudo apt-get install -y ...; fi` so Linux install failures still fail the step.

### 46. make install never installs include/asmtest_assemble.h although the installed asmtest-emu.pc advertises the Keystone assembler tier — Low **[verified ×2]**

`Makefile:446`

install copies only asmtest.h, asmtest_emu.h, asmtest_trace.h, asm.h and asm_nasm.inc into $(incdir). install-shared-emu installs libasmtest_emu plus asmtest-emu.pc, whose Description calls it 'the full superset' including 'the Keystone assembler' and whose Cflags point at that includedir — but the assembler API's header (include/asmtest_assemble.h, which asm_available()/asmtest_assemble consumers must include, and which bindings/cpp/asmtest.hpp includes under ASMTEST_ENABLE_ASM) is never installed. A pkg-config consumer of the advertised superset cannot compile against the assemble entry points from an installed prefix.

**Evidence:** Makefile:446-447 `cp include/asmtest.h include/asmtest_emu.h include/asmtest_trace.h \\\n\t   include/asm.h include/asm_nasm.inc $(incdir)/`; asmtest-emu.pc.in:7 `Description: asm-test emulator tier (Unicorn) plus the Keystone assembler and Capstone disassembler — the full superset shared library`

**Failure scenario:** User runs `make install install-shared-emu PREFIX=/opt/asmtest`, then compiles a C file with `pkg-config --cflags asmtest-emu` and `#include <asmtest_assemble.h>` -> 'fatal error: asmtest_assemble.h: No such file or directory' despite the .pc claiming the assembler tier.

**Suggested fix:** Add include/asmtest_assemble.h to the install cp list (and to uninstall's rm list).

### 47. release.yml native-all runs package-libs-verify without installing llvm, so the darwin Mach-O assertions silently self-skip on the release path — Low **[verified ×2]**

`.github/workflows/release.yml:62`

package-libs-verify folds in package-libs-verify-macho, and scripts/verify-macho.sh self-skips with exit 0 when llvm-otool/llvm-lipo are absent (line 45: 'SKIP ... install llvm to run Mach-O checks'). ci.yml's collect job deliberately installs llvm first (ci.yml:522-524), but release.yml's native-all job — the one whose output actually feeds the published packages — has no llvm install step, so every darwin slot's arch/install-name/min-OS assertions are skipped exactly where they matter most. A darwin dylib with a leaked /opt/homebrew install-name would verify green on the tag build (the per-push CI run catches it only if the breakage predates the tag).

**Evidence:** release.yml:62-66 `- name: Merge into one build/dist/native/ tree\n  run: |\n    mkdir -p build/dist/native\n    cp -R payloads/*/* build/dist/native/\n    make package-libs-verify` (no llvm install step in the job); scripts/verify-macho.sh:45 `echo "  SKIP $slot — llvm-otool/llvm-lipo not found (install llvm to run Mach-O checks)"`; ci.yml:523-524 installs llvm before the same target

**Failure scenario:** A Keystone build change introduces an absolute /opt/homebrew dependency in the darwin payload in the same PR as the tag -> release native-all verifies 'ok' (Mach-O checks skipped) -> packages ship dylibs that fail to load on user machines.

**Suggested fix:** Add the same `sudo apt-get install -y --no-install-recommends llvm` step to release.yml's native-all job before `make package-libs-verify`.

### 48. eBPF fallback header vmlinux_min.h cannot compile codeimage.bpf.c — the documented BTF-less fallback build fails hard — Medium (impact assessed Low) **[verified ×2]**

`bpf/vmlinux_min.h:6`

vmlinux_min.h is the checked-in fallback used when /sys/kernel/btf/vmlinux is unavailable (mk/native-trace.mk:178-179 copies it to build/vmlinux.h). Its header comment claims it "defines exactly the base types and the two syscall-tracepoint context structs codeimage.bpf.c touches", but codeimage.bpf.c also needs kernel definitions that only the full BTF dump supplies: enum bpf_map_type (BPF_MAP_TYPE_ARRAY/RINGBUF/HASH), enum values (BPF_ANY), struct bpf_pidns_info, and the __be16/__be32/__wsum types that bpf_helper_defs.h references. None are in vmlinux_min.h, so on any kernel lacking BTF the clang -target bpf compile (native-trace.mk:181) fails and `make codeimage-test` aborts. The fallback that exists precisely for BTF-less kernels can never produce a working object; the header's "defines exactly ... the structs codeimage.bpf.c touches" claim is factually wrong.

**Evidence:** vmlinux_min.h defines only __u8..__s64/u8..u64, struct trace_entry, trace_event_raw_sys_enter/exit. Compiling codeimage.bpf.c against it (in asmtest-hwtrace-codeimage, fallback path) yields 13 errors: "use of undeclared identifier 'BPF_MAP_TYPE_ARRAY'" (codeimage.bpf.c:42), "'BPF_MAP_TYPE_RINGBUF'" (:50), "'BPF_MAP_TYPE_HASH'" (:60,:68), "variable has incomplete type 'struct bpf_pidns_info'" (:79), "use of undeclared identifier 'BPF_ANY'" (:127,:154), plus "unknown type name '__be16'/'__be32'/'__wsum'" from bpf_helper_defs.h. The real-BTF path in the same container compiles clean (PRIMARY-PATH-OK).

**Failure scenario:** Build the codeimage tier on a kernel without CONFIG_DEBUG_INFO_BTF (no /sys/kernel/btf/vmlinux) -> bpftool btf dump fails -> vmlinux_min.h is copied to vmlinux.h -> clang -target bpf compile of codeimage.bpf.c fails with 13 errors -> the codeimage build/test target fails rather than degrading.

**Suggested fix:** Either add the missing definitions to vmlinux_min.h (enum bpf_map_type, the BPF_ANY/update-flag enum, struct bpf_pidns_info, and __be16/__be32/__wsum typedefs) so the minimal header is self-sufficient, or drop the fallback and gate CODEIMAGE_SKEL on real BTF being present; correct the header comment either way.

## Documentation accuracy

### 49. Quickstart claims the Makefile auto-discovers examples/test_*.c suites; SUITES is a hardcoded list, so a new suite is silently never built or run — High (impact assessed Medium) **[verified ×2]**

`docs/quickstart.md:58`

quickstart.md steps 2-4 tell a new user to create examples/square.s + examples/test_square.c and state the Makefile "discovers examples/test_*.c, links each with the matching routine ... so make test picks up your new pair automatically". The Makefile has no wildcard discovery: SUITES is a hardcoded 14-entry list (Makefile:50-55) and each suite has an explicit link rule (e.g. Makefile:236). No included mk/*.mk extends SUITES. A user following the quickstart gets a green `make test` that never compiled or ran their new tests, and step 4's `./build/test_square` fails with 'No such file or directory'.

**Evidence:** docs/quickstart.md:58-60: "By convention a suite is the pair foo.s + test_foo.c. The Makefile discovers examples/test_*.c, links each with the matching routine, and builds a suite binary — so make test picks up your new pair automatically." vs Makefile:50-55: "SUITES := $(BUILD)/test_arith $(BUILD)/test_mem $(BUILD)/test_capture ... $(BUILD)/test_qadd" (fixed list; `grep 'SUITES +=' Makefile mk/*.mk` finds nothing, and each suite binary has a hand-written rule like Makefile:236 "$(BUILD)/test_arith: $(FRAMEWORK_OBJS) $(BUILD)/add.o $(BUILD)/test_arith.o")

**Failure scenario:** New user creates examples/square.s + examples/test_square.c exactly per the quickstart, runs `make test`: all pre-existing suites pass, exit 0, their own tests were never compiled or executed (a failing ASSERT in test_square.c goes undetected); `./build/test_square` (quickstart step 4) errors because the binary was never built.

**Suggested fix:** Either add wildcard-based suite discovery to the Makefile (SUITES := $(patsubst examples/test_%.c,$(BUILD)/test_%,$(wildcard examples/test_*.c)) plus a generic pairing rule), or rewrite quickstart step 3-4 to say the new suite must be added to SUITES and given a link rule.

### 50. emulator.md documents the AArch64 guest as taking args in x0–x7, but emu.c marshals only x0..x5 — the 7th/8th args are silently dropped — Medium **[verified ×2]**

`docs/emulator.md:150`

The "Other guests" table says the AArch64 guest passes integer args in x0–x7 (which is also what AAPCS64 would suggest). The implementation writes at most 6 argument registers: emu_arm64_setup uses a static arg_regs[6] = {X0..X5} and clamps the loop with `i < 6`, with no stack spill. The header comment agrees with the code ("integer args in x0..x5", asmtest_emu.h:239-240). So args[6] and args[7] are never delivered to the guest; x6/x7 hold whatever state the engine has.

**Evidence:** docs/emulator.md:150: "| **AArch64** | `emu_arm64_open` | `x0`–`x7` | `x0` | ..." vs src/emu.c:638-650: "static const int arg_regs[6] = {UC_ARM64_REG_X0, ... UC_ARM64_REG_X5}; ... for (int i = 0; i < niargs && i < 6; i++) { ... uc_reg_write(uc, arg_regs[i], &v); }" and include/asmtest_emu.h:239: "Run raw AArch64 machine code with integer args in x0..x5."

**Failure scenario:** A user writes an 8-arg AArch64 routine (reads x6/x7 per the doc's x0–x7 claim), calls emu_arm64_call(e, code, len, args, 8, 0, &out): the guest computes with stale/zero x6/x7 instead of args[6]/args[7], producing a wrong result that the test then asserts against — a silent wrong-answer, not an error.

**Suggested fix:** Correct the table to x0–x5 (and note the 6-arg limit), or extend emu_arm64_setup to marshal 8 registers to match AAPCS64.

### 51. api-reference.md says ASMTEST_SEED seeds --shuffle; the shuffle never reads it, so the documented reproducibility silently does not work — Medium **[verified ×2]**

`docs/api-reference.md:137`

The environment-variable table claims ASMTEST_SEED is the "seed for property-test RNG and --shuffle". In src/asmtest.c, ASMTEST_SEED is read only by asmtest_seed() (line 616), which is used exclusively by the asmtest_match_ref* property-test engines. The shuffle path derives its seed from --seed=N or time^pid (lines 1750-1753) and never consults ASMTEST_SEED. The header comment (asmtest.h:407-409) also describes ASMTEST_SEED as the property-test seed only.

**Evidence:** docs/api-reference.md:137: "| `ASMTEST_SEED` | seed for property-test RNG and `--shuffle` (decimal or `0x`-hex) |" vs src/asmtest.c:1750-1753: "if (opt.shuffle) { uint64_t seed = opt.has_seed ? opt.seed : ((uint64_t)time(NULL) ^ ((uint64_t)ASMTEST_GETPID() << 32));" — the only getenv("ASMTEST_SEED") is src/asmtest.c:616 inside asmtest_seed(), used only by asmtest_match_ref1/2/3.

**Failure scenario:** CI exports ASMTEST_SEED=0x1234 and runs `./build/test_arith --shuffle` expecting a reproducible order per the API reference; the order still varies run to run (time/pid seed), so an order-dependent failure cannot be replayed by setting the documented variable.

**Suggested fix:** Change the table entry to "seed for the property-test RNG" (shuffle is seeded by --seed=N only), or make the runner fall back to ASMTEST_SEED when --seed is absent.

### 52. CHANGELOG [Unreleased] describes a `libasmtest_emu_full` / `make shared-emu-full` split and a "Keystone-free libasmtest_emu" that no longer exist — Low **[verified ×2]**

`CHANGELOG.md:453`

The "Track C disassembly reaches all ten bindings (via a single 'full' lib)" entry says one `libasmtest_emu_full` carries Keystone+Capstone (`make shared-emu-full`) while "the lean `libasmtest_emu` stays Unicorn-only" and a binding "points ASMTEST_LIB at the full lib"; the in-line-assembler entry likewise says the tier is "kept separate from libasmtest_emu" and bindings "self-skip against the Keystone-free libasmtest_emu" (lines 637, 649). In the current tree there is no shared-emu-full target and no libasmtest_emu_full anywhere in the Makefile or mk/*.mk; `shared-emu` links assemble.o + disasm.o + $(KEYSTONE_LIBS) + $(CAPSTONE_LIBS) into libasmtest_emu itself (the superset), exactly as api-reference.md:163-173 and bindings.md:17-19 state. The [Unreleased] section therefore misdescribes the code it will be released with, and the command it gives fails.

**Evidence:** CHANGELOG.md:451-456: "Instead one `libasmtest_emu_full` carries *both* optional native tiers ... (`make shared-emu-full`); the lean `libasmtest_emu` stays Unicorn-only, and a binding points `ASMTEST_LIB` at the full lib" vs Makefile:392-406: "# Emulator shared lib: the SUPERSET — emulator (emu.o, -lunicorn) plus BOTH optional native tiers ... shared-emu: $(call shlib_dev,libasmtest_emu) ... $(BUILD)/pic/assemble.o $(BUILD)/pic/disasm.o ... $(UNICORN_LIBS) $(KEYSTONE_LIBS) $(CAPSTONE_LIBS)"; `grep -rn shared-emu-full Makefile mk/` matches nothing.

**Failure scenario:** A user reading the [Unreleased] changelog to enable disassembly runs `make shared-emu-full` — make errors "No rule to make target 'shared-emu-full'" — or hunts for a libasmtest_emu_full to point ASMTEST_LIB at and concludes the disassembler tier is missing, when plain `make shared-emu` already provides it.

**Suggested fix:** Rewrite the superseded [Unreleased] bullets (lines ~449-467 and ~637-649) to describe the final state: libasmtest_emu is the single superset built by `make shared-emu`; drop shared-emu-full/libasmtest_emu_full references.

### 53. bindings.md claims C++ compiles the asm/disas tiers in by default with -DASMTEST_ENABLE_ASM/DISAS as opt-outs; they are opt-in #ifdef gates and default OFF — Low **[verified ×2]**

`docs/bindings.md:130`

bindings.md says "The statically compiled bindings (C++ and Zig) compile the tiers in by default; C++ still honours the -DASMTEST_ENABLE_ASM / -DASMTEST_ENABLE_DISAS opt-outs." In bindings/cpp/asmtest.hpp every assembler/disassembler (and emulator) surface is guarded by `#ifdef ASMTEST_ENABLE_ASM` / `#ifdef ASMTEST_ENABLE_DISAS` / `#ifdef ASMTEST_ENABLE_EMU` — i.e. the macros are opt-INs that must be defined to get the API at all; without them the tier code does not compile in. Both the "by default" claim and the "opt-outs" characterization are inverted for C++.

**Evidence:** docs/bindings.md:130-132: "The statically compiled bindings (C++ and Zig) compile the tiers in by default; C++ still honours the `-DASMTEST_ENABLE_ASM` / `-DASMTEST_ENABLE_DISAS` opt-outs." vs bindings/cpp/asmtest.hpp:22-27: "#ifdef ASMTEST_ENABLE_EMU\n#include \"asmtest_emu.h\"\n#endif\n#ifdef ASMTEST_ENABLE_ASM\n#include \"asmtest_assemble.h\"\n#endif" and asmtest.hpp:536: "#ifdef ASMTEST_ENABLE_DISAS" (the disas wrapper exists only when defined); asmtest.hpp:15-16: "Define ASMTEST_ENABLE_EMU ... before including to get the asmtest::Emu wrapper."

**Failure scenario:** A C++ user compiles a test including asmtest.hpp with no extra defines, expecting the disassembler "on by default" per bindings.md, and calls the asmtest disas wrapper: compilation fails (symbol not declared) because ASMTEST_ENABLE_DISAS was never defined.

**Suggested fix:** Reword to: the C++ binding gates the emu/asm/disas tiers behind opt-in -DASMTEST_ENABLE_EMU/-DASMTEST_ENABLE_ASM/-DASMTEST_ENABLE_DISAS defines (Zig compiles them in by default).

### 54. ci.md states that on arm64 CI runs only the test and emu jobs; the asm (Keystone) and package-libs jobs also run on ubuntu-24.04-arm — Low **[verified ×2]**

`docs/ci.md:42`

ci.md's arm64 section says "On arm64, CI only runs the `test` and `emu` jobs". In .github/workflows/ci.yml the `asm` job's matrix is os: [ubuntu-latest, ubuntu-24.04-arm] (line 186) and the `package-libs` payloads job also includes ubuntu-24.04-arm (line 446). So at least two more jobs run on the arm64 runner, and a contributor emulating the arm64 lane per this doc would not reproduce the asm lane that can actually fail there.

**Evidence:** docs/ci.md:42-43: "On arm64, CI only runs the `test` and `emu` jobs:" vs .github/workflows/ci.yml:179-186 (asm job): "asm:\n  name: asm (${{ matrix.os }})\n  ...\n  matrix:\n    os: [ubuntu-latest, ubuntu-24.04-arm]" and ci.yml:439-446 (package-libs): "os: [ubuntu-latest, ubuntu-24.04-arm, macos-latest]".

**Failure scenario:** A contributor changes the Keystone in-line-assembler tier, follows ci.md and runs only `make docker-test DOCKER_PLATFORM=linux/arm64` and `make docker-emu DOCKER_PLATFORM=linux/arm64` believing arm64 CI runs nothing else; the push then fails in the real `asm (ubuntu-24.04-arm)` CI job they never reproduced.

**Suggested fix:** Update the sentence to list test, emu, asm (and the package-libs payload staging) as the arm64 jobs, and mention `make docker-asm DOCKER_PLATFORM=linux/arm64`.

---

## Plausible (one verifier upheld, one dissented)

These survived one adversarial pass but not both — read the code before acting
on them.

- **`src/asmtest.c:1242`** — run_parallel's non-EINTR poll() failure path reports never-run tests as PASSED and prints NULL suite/name. If poll() fails with anything other than EINTR (e.g. ENOMEM under memory pressure), run_parallel `break`s out with tests still unspawned and children still in flight. main() pre-zeroed results[] (lines 1792-1793) and ST_PASS is enum value 0 (line 26), so every unfinished test is counted as passed (false green), and its suite/name pointers are NULL — print_tap_result then does printf("%s.%s", NULL, NULL), which is undefined behavior.
- **`src/glob_match.c:25`** — glob_match diverges from fnmatch(flags=0) on unterminated '[' classes and backslashes, so Win64 --filter selects different tests than POSIX. match_class parses to end-of-string when a class has no closing ']', treating the remainder as class members, whereas POSIX/glibc fnmatch treats an unterminated '[' as a literal character. It also treats '\' inside a class as an ordinary member (fnmatch without FNM_NOESCAPE honors escaping there, e.g. "[\\]]" matches "]") and matches a lone trailing '\' literally (POSIX fnmatch shall return non-match).
- **`src/asmtest.c:482`** — asmtest_guarded_alloc size arithmetic overflows for n within a page of SIZE_MAX, returning a pointer into the guard page instead of NULL. round_up_page computes `(n + pg - 1) / pg * pg` without an overflow check; for n > SIZE_MAX - pg + 1 the addition wraps, `usable` collapses to pg (via the ==0 branch), the 2-page mmap succeeds, and `base + (usable - n)` wraps to a pointer inside the PROT_NONE guard page.
- **`src/fuzz.c:54`** — Corpus 'nudge' mutation in emu_fuzz_cover1 does signed long addition that overflows at the range extremes (UB, trips the UBSan lane). `in = base + asmtest_rng_range(&rng, -4, 4)` adds a signed offset to a corpus member that may equal hi or lo. When the caller fuzzes a boundary range (hi = LONG_MAX or lo = LONG_MIN), base can sit within 4 of the limit and the addition is signed overflow — undefined behavior.
- **`src/drtrace_app.c:161`** — asmtest_dr_available() is not in lock-step with dr_lib_path(): the bare-soname (rpath/LD_LIBRARY_PATH) fallback is reported unavailable, and the in-code comment claims the opposite. dr_lib_path() (whose comment says it is "Kept in lock-step with asmtest_dr_available(), which advertises the same cascade") falls back to the bare soname "libdynamorio.so", which dlopen resolves via RUNPATH/LD_LIBRARY_PATH/ldconfig.
- **`src/drtrace_client.c:209`** — DR client begin/end stack desynchronizes past MAX_DEPTH: on_begin does not count overflowed begins but on_end always pops, silently ending recording for still-active regions. In the client, a begin at depth >= MAX_DEPTH is dropped entirely (no push, no increment), but every end decrements. One over-deep begin/end pair therefore pops a real, still-active region off the recording stack, and all outer regions lose their remaining coverage one end too early — silently (no marker error, no truncated flag).
- **`bindings/zig/src/conformance.zig:387`** — Zig conformance test passes 2-element vector-arg arrays to asm_call_capture_vec256/512, which unconditionally read 8 slots (192/384-byte stack over-read). asmtest.h documents that asm_call_capture_vec256/vec512 take `vargs has 8` slots, and the trampoline unconditionally loads ymm0..7 from vargs[0..256) (vmovdqu 0..224(%rcx)) and zmm0..7 from vargs[0..512). The Zig tests build `var varr = [_]c.vec256_t{ a, b }` (64 bytes) and `[_]c.vec512_t{ a, b }` (128 bytes) and pass &varr, so the trampoline reads 192 (vec256) / 384 (vec512) bytes past the end of a stack array.
- **`src/platform_win32.c:369`** — rt_veh_cb's arming is process-global, so a fault on any non-test thread is longjmp'd onto the main thread's stack. The guard() facility correctly scopes its recovery with __thread tls_armed/tls_recover (platform_win32.c:284-286), so only the faulting thread's own guard triggers.
- **`docs/emulator.md:38`** — emulator.md says `make deps DEPS_ARGS=--emu` installs "libunicorn (and only that)"; --emu actually installs unicorn + capstone + pkg-config. emulator.md annotates the deps command with "install libunicorn (and only that)", and installation.md:45 similarly says "only what make emu-test needs (libunicorn)". scripts/install-deps.sh maps --emu to unicorn + capstone + pkg-config (its own usage text says "unicorn + capstone + pkg-config"), and the CHANGELOG itself records that --emu "now installs libcapstone-dev".

## Refuted during verification

- **`scripts/clean-room-test.sh:209`** — clean-room-test.sh eval-interpolates ASMTEST_CLEANROOM_ONLY into a shell command (command injection via a controlled variable). Refuted: the interpolation is real (the verifier's PoC fired), but `ASMTEST_CLEANROOM_ONLY` is set only via the maintainer-invoked `CLEANROOM_ONLY` make variable — the "attacker" is someone already running arbitrary make commands, so no trust boundary is crossed.
- **`SECURITY.md:11`** — SECURITY.md advertises 1.0.x as a supported released series, but no release or tag exists. Refuted: the observation is accurate but is known finding S1 (release pipeline never fired) restated; the CHANGELOG's own `[1.0.0] — 2026-06-24` entry makes the support table internally consistent.

---

## Suggested fix order

1. **The d8–d15 capture gap (#1)** — it is the framework's central promise on
   AArch64, and it also corrupts the harness itself.
2. **The Rust `call_traced` overflow (#26)** — heap corruption reachable from a
   safe API.
3. **The trace-tier truncation contract as one coordinated fix (#11–#15)** —
   PT address filter/image coverage, AMD ring draining or overflow accounting,
   single-step TF-loss detection and block-partition parity, and the
   `__linux__` gate in `cpu_matches`.
4. **The `{long; double}` example and header advice (#3, #4)** — they actively
   teach users an ABI bug that the harness's own marshaling then hides.
5. **Quickstart auto-discovery (#49) and the release-token fix (#40)** — both
   are first-contact traps: one for new users, one for the first real tag.

## Caveats

- The completeness critic (an agent auditing what this review itself failed to
  cover) never ran; no independent coverage-gap check exists for this pass.
- Five findings carry a single verifier pass instead of two (marked above).
- The two prior reviews' still-open items (notably in-process crash
  containment, the Lua arg-dropping, emulator state reset, and Node `Number()`
  read-backs) remain open and are tracked in
  [the 2026-07-02 repo review](../archive/reviews/2026-07-02-repo-review.md), not here.

