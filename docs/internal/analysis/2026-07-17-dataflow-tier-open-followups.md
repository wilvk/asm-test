# Data-flow tier — open follow-ups after the 2026-07-17 batch (F1/F2/F6/F7 + W-1)

**Status: items 2 and 3 landed 2026-07-17 (same day, separate diff). Items 1, 4, 5 remain OPEN.**
None was a regression; each was surfaced (not introduced) while landing the batch, and each was
recorded here rather than fixed inline, because fixing them inside the diff that found them would
have been scope creep — or, in one case, would have disturbed a neighbouring oracle.

Ordered by how much they can lie to a user.

---

## 1. The scoped path's call-out step-over has the fabricated-edge shape F6 just fixed for windows

**Verified, not inferred.** [`src/dataflow_ptrace.c:65`](../../../src/dataflow_ptrace.c#L65) states the
design outright: the call-out step-over runs the helper at native speed *"(recording NOTHING over the
helper)"* and then resumes in-region single-stepping, so a non-leaf routine is traced across its
helper calls.

That is structurally the same gap F6's barrier exists to close. F6 established (with a mutation that
**passes the value assertion while fabricating the edge**) that when a producer elides a span, the
last-writer builder hands a post-gap read to the **stale in-region writer**. A caller-saved register
clobbered by a stepped-over helper is exactly that shape.

**Why it is dangerous out of proportion to its size:** the VALUE stays honest — it is read from
silicon — and only the EDGE lies. So **no value oracle and no byte-identical comparison can catch
it**, which is precisely what F1's and F2's oracles are. The tier's strongest existing checks are
blind to this class by construction.

**Why F6 did not fix it:** changing `dfp_step_loop`'s record stream would disturb F1's and F2's
byte-identical oracles, which compare block-step+replay against the single-step reference. Any fix
must land with those oracles' owners, not around them.

**Fix shape:** F6's barrier is directly transferable — snapshot the at-risk set (what the survey has
already written) at gap entry, diff at exit, append one GAP step. Registers must compare the
alias's own bit slice (`dataflow.c:219` keys raw Capstone ids with no canonicalization); memory per
byte. Note F6 measured that a *blanket* barrier deletes the exit-criterion slice — precision is
load-bearing, so this is not a one-line change.

## 2. `covered(t, 0)` is vacuous — `amd_replay` appends block 0 unconditionally — LANDED 2026-07-17

[`src/amd_backend.c:267`](../../../src/amd_backend.c#L267) does `trace_append_block(trace, 0)`
unconditionally ("Block start at the region entry"). So `covered(t, 0)` is **always true** and can
never fail.

The shipped `test_branchsnap` uses `covered0` as its entry evidence. It is carried entirely by the
`ni > 0` conjunct sitting beside it — not by `covered0`. Nothing is currently wrong, but the
assertion contributes zero discriminating power while reading as though it proves entry coverage.

Related and worth keeping together: while confirming the tail-`jmp` non-eviction property, `!truncated`
alone was found to be a **worthless** assertion for that property — it does not catch *partial*
eviction. `!covered(0x0e)` was the load-bearing discriminator. Both facts point the same way: on this
tier, "covered" and "truncated" assertions need their discriminating power checked, not assumed.

**Fixed**: `test_branchsnap.c`'s `snap_default_run` (the multi-exit test, a DIFFERENT call site from
the Phase 9 one this item was found next to) was the one shipped instance actually asserting `cov0`
as evidence, exactly as described above. It now asserts the PATH-SPECIFIC block instead —
`covered(want_off) && !covered(other_off)`, the two exits' own `mov` blocks — real evidence that the
default-on snapshot captured the exit that actually ran. `amd_backend.c:267` itself is unchanged and
correct (block 0 SHOULD be appended unconditionally); the fix was entirely at the assertion site.
Live-hardware verification blocked this session on a missing BPF toolchain (clang/libbpf-dev) and a
`sudo` password this session could not supply; verified by compilation and the ENOSYS stub path only.

## 3. `examples/test_dataflow_blockstep.c` re-declares `asmtest_blockstep_info_t` with no layout guard — LANDED 2026-07-17

The tier ships no header (deliberately — it keeps the producer off the public ABI), so its suite
**re-declares the producer's structs**: 27 references to `asmtest_blockstep_info_t`, and **0**
`sizeof`/`offsetof` guards.

This is not hypothetical. F6 hit the identical skew in its own suite and it cost 3 green checks, and
then found its *first* guard could not fail either — the mutant's added field landed in **tail
padding**, so `sizeof` was unchanged and the guard passed on an already-skewed struct. The guard that
works pins **size AND final-field offset**.

The same skew is available to the blockstep suite today. Cheap to close; the failure mode is silent.

**Fixed**: `asmtest_dataflow_blockstep_info_layout()` added to `src/dataflow_blockstep.c` (mirrors
`asmtest_dataflow_ptrace_win_info_layout`, defined unconditionally since the struct itself sits
before the platform gate), and `test_dataflow_blockstep.c` now checks its re-declared copy against
it before trusting any `info.*` field. Verified: `make dataflow-blockstep-test` natively on the
Zen 5 dev box, 119/119 checks, 0 skips.

## 4. F2 increment 2 — `rdtsc`/`cpuid` are a PRIMITIVE gap, not a decode gap

F2 landed record-and-inject for `syscall`/`int 0x80` and correctly gated
`rdtsc`/`rdtscp`/`rdrand`/`rdseed`/`cpuid`/`sysenter`. The reason is worth preserving so nobody
"fixes" it at the wrong layer: **`PTRACE_SINGLEBLOCK` (BTF) traps control transfers**, which is why the
syscall boundary is free — and these instructions are **not** control transfers, so BTF gives no
boundary to record from at all. No amount of decoding helps.

Needs a hardware execution breakpoint. Deliberately declined rather than half-landed.

## 5. Seven bindings still lack the def-use / slice half

F7 gave all ten bindings the live-attach producer (`attach_pid` / `attach_pid_tid` / `attach_jit`) on
a minimal `ValueTrace`. Only python/cpp/node carry the full valtrace→defuse→slice pipeline; the other
seven stop at the producer plus `gcmove_canon` / `method_resolve_pc`.

The blocker is specific: **the slice seed crosses the FFI boundary BY VALUE as `at_val_rec_t`**. F7's
own surface deliberately passes no struct by value — which is the only reason its signatures are safe
despite the parity gate being unable to see them at all (a producer ships no header; the gate derives
from `TIER_HEADERS`). Widening to the slice half re-opens exactly the ABI-cliff exposure that has
already bitten Ruby/Java once.

---

### The through-line

Items 1, 2 and 3 are all the same lesson in different clothes: **an assertion's discriminating power
has to be demonstrated, not assumed.** A value oracle cannot see a fabricated edge; `covered(t,0)`
cannot fail; a `sizeof` guard cannot see a field that fits in padding. Each looks like coverage and
provides none.
