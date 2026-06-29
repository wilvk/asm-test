# test_hwtrace.rb — standalone smoke test for the Ruby single-step hardware-trace
# wrapper (hwtrace.rb). Mirrors bindings/python/tests/test_hwtrace.py.
#
# Run it directly:
#   cd bindings/ruby
#   ASMTEST_HWTRACE_LIB=$PWD/../../build/libasmtest_hwtrace.so ruby test_hwtrace.rb
#
# Unlike the DynamoRIO wrapper (which needs a DynamoRIO install) and the PT/AMD
# backends (which need specific bare-metal hardware), the SINGLESTEP backend runs on
# ANY x86-64 Linux — so this asserts a real, live trace. It self-skips only when the
# tier is unavailable (off x86-64 Linux / lib absent): prints "# SKIP ..." and exits
# 0 so an incapable host passes cleanly. On failure it exits nonzero.
require_relative "hwtrace"

HwTrace      = Asmtest::HwTrace::HwTrace
NativeCode   = Asmtest::HwTrace::NativeCode
SINGLESTEP   = Asmtest::HwTrace::SINGLESTEP
AMD_LBR      = Asmtest::HwTrace::AMD_LBR
BEST         = Asmtest::HwTrace::BEST
CEILING_FREE = Asmtest::HwTrace::CEILING_FREE
EUNAVAIL     = Asmtest::HwTrace::EUNAVAIL

# mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
ROUTINE = [0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
           0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3].pack("C*")

# mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  (19 back-edges > LBR's 16)
LOOP = [0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x01, 0xF8, 0x48, 0xFF, 0xCE, 0x75, 0xF8, 0xC3].pack("C*")

$n = 0
$failed = false

def ok(cond, desc)
  $n += 1
  if cond
    puts "ok #{$n} - #{desc}"
  else
    puts "not ok #{$n} - #{desc}"
    $failed = true
  end
end

def skip(msg)
  puts "# SKIP #{msg}"
  exit 0
end

# ---- auto-select: selection invariants hold on every host (even where all
# backends self-skip and the cascade is empty) — resolve/auto need no init. ----
best = HwTrace.resolve(BEST)
cf   = HwTrace.resolve(CEILING_FREE)

# Every resolved backend is actually available, ordered by descending fidelity
# (ascending enum), with no duplicates.
ok(best.all? { |b| HwTrace.available?(b) }, "auto BEST returns only available backends")
ok(best == best.uniq.sort, "auto BEST is ordered by descending fidelity, no dups")

# CEILING_FREE drops the one fixed-window backend (AMD LBR) and is otherwise a
# subset of BEST.
ok(!cf.include?(AMD_LBR), "auto CEILING_FREE never selects AMD LBR")
ok((cf - best).empty?, "auto CEILING_FREE is a subset of BEST")

# auto(policy) is the head of resolve(policy), or EUNAVAIL when empty.
ab = HwTrace.auto(BEST)
ok(ab == (best.empty? ? EUNAVAIL : best.first),
   "auto(BEST) is the head of the resolved cascade")

# Self-skip cleanly when the single-step tier can't run (lib absent / off x86-64).
unless HwTrace.available?(SINGLESTEP)
  skip("single-step backend unavailable: #{HwTrace.skip_reason(SINGLESTEP)}")
end

HwTrace.init(SINGLESTEP)

begin
  # ---- routine: two blocks, jle taken so dec is skipped ----
  code = NativeCode.from_bytes(ROUTINE)
  trace = HwTrace.create(blocks: 64, instructions: 64)
  trace.register("add2", code)

  result = nil
  trace.region("add2") { result = code.call(20, 22) } # 42 <= 100 -> jle taken

  ok(result == 42, "routine call(20,22) == 42 (got #{result})")
  ok(trace.insn_offsets == [0x0, 0x3, 0x6, 0xC, 0x11],
     "routine insn_offsets == [0,3,6,12,17] (got #{trace.insn_offsets.inspect})")
  ok(trace.insns_total == 5, "routine insns_total == 5 (got #{trace.insns_total})")
  ok(trace.covered?(0) && trace.covered?(0x11),
     "routine covers entry(0) and ret(17)")
  ok(trace.blocks_len == 2, "routine blocks_len == 2 (got #{trace.blocks_len})")
  ok(!trace.truncated?, "routine not truncated")

  trace.free
  code.free

  # ---- loop: 19 back-edges, exact + complete under single-step ----
  loop_code = NativeCode.from_bytes(LOOP)
  loop_trace = HwTrace.create(blocks: 64, instructions: 256)
  loop_trace.register("loop", loop_code)

  loop_result = nil
  loop_trace.region("loop") { loop_result = loop_code.call(1, 20) }

  ok(loop_result == 20, "loop call(1,20) == 20 (got #{loop_result})")
  ok(loop_trace.insns_total == 62,
     "loop insns_total == 62 (got #{loop_trace.insns_total})")
  ok(loop_trace.covered?(0) && loop_trace.covered?(0x7),
     "loop covers entry(0) and body(7)")
  ok(loop_trace.blocks_len == 2, "loop blocks_len == 2 (got #{loop_trace.blocks_len})")
  ok(!loop_trace.truncated?, "loop not truncated")

  loop_trace.free
  loop_code.free
ensure
  HwTrace.shutdown
end

# ---- auto-select live: on any x86-64 Linux the cascade is non-empty (single-step
# floor), so auto() resolves a usable backend; trace the shared fixture through it.
# Own init/shutdown — the C tier is a single global lifecycle (the block above has
# already shut down). ----
best = HwTrace.resolve(BEST)
pick = HwTrace.auto(BEST)
ok(!best.empty? && pick >= 0, "auto resolves a backend here (single-step floor)")

HwTrace.init(pick)
begin
  auto_code = NativeCode.from_bytes(ROUTINE)
  auto_trace = HwTrace.create(blocks: 64, instructions: 64)
  auto_trace.register("auto", auto_code)

  auto_result = nil
  auto_trace.region("auto") { auto_result = auto_code.call(20, 22) }

  ok(auto_result == 42, "auto call(20,22) == 42 (got #{auto_result})")
  ok(auto_trace.covered?(0), "auto-selected backend covers block offset 0")
  if pick == SINGLESTEP # the pick off PT/AMD hosts: byte-exact parity
    ok(auto_trace.insn_offsets == [0x0, 0x3, 0x6, 0xC, 0x11],
       "auto pick (single-step) insn_offsets == [0,3,6,12,17] (got #{auto_trace.insn_offsets.inspect})")
  end

  auto_trace.free
  auto_code.free
ensure
  HwTrace.shutdown
end

exit($failed ? 1 : 0)
