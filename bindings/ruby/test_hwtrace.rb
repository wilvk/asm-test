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
CodeImage    = Asmtest::HwTrace::CodeImage
SINGLESTEP   = Asmtest::HwTrace::SINGLESTEP
AMD_LBR      = Asmtest::HwTrace::AMD_LBR
BEST         = Asmtest::HwTrace::BEST
CEILING_FREE = Asmtest::HwTrace::CEILING_FREE
EUNAVAIL     = Asmtest::HwTrace::EUNAVAIL

# Cross-tier orchestrator (asmtest_trace_auto.h).
TIER_HWTRACE       = Asmtest::HwTrace::TIER_HWTRACE
TIER_EMULATOR      = Asmtest::HwTrace::TIER_EMULATOR
FIDELITY_NATIVE    = Asmtest::HwTrace::FIDELITY_NATIVE
FIDELITY_VIRTUAL   = Asmtest::HwTrace::FIDELITY_VIRTUAL
TRACE_BEST         = Asmtest::HwTrace::TRACE_BEST
TRACE_CEILING_FREE = Asmtest::HwTrace::TRACE_CEILING_FREE
TRACE_NATIVE_ONLY  = Asmtest::HwTrace::TRACE_NATIVE_ONLY

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

# ---- cross-tier orchestrator: structural invariants hold on every host (resolve
# over hwtrace + DynamoRIO + emulator) — resolve/auto need no init. ----
ct_best = HwTrace.resolve_tiers(TRACE_BEST)
ct_nat  = HwTrace.resolve_tiers(TRACE_NATIVE_ONLY)
ct_cf   = HwTrace.resolve_tiers(TRACE_CEILING_FREE)

# Every HW choice satisfies the hardware-tier probe; NATIVE choices precede the
# single VIRTUAL emulator floor, which is the last entry under BEST.
ct_field_ok = ct_best.all? do |c|
  hw_ok = c.tier != TIER_HWTRACE || HwTrace.available?(c.backend)
  want = c.tier == TIER_EMULATOR ? FIDELITY_VIRTUAL : FIDELITY_NATIVE
  hw_ok && c.fidelity == want
end
ok(ct_field_ok, "cross-tier BEST: HW choices available, fidelity matches tier")
ok(!ct_best.empty? && ct_best.last.tier == TIER_EMULATOR,
   "cross-tier BEST ends at the emulator floor")
ok(ct_best.count { |c| c.tier == TIER_EMULATOR } == 1,
   "cross-tier BEST has exactly one emulator floor entry")

# NATIVE_ONLY forbids the native->emulator crossing: it is BEST minus the floor.
ok(ct_nat.all? { |c| c.tier != TIER_EMULATOR },
   "cross-tier NATIVE_ONLY drops the emulator floor")
ok(ct_nat.length == ct_best.length - 1,
   "cross-tier NATIVE_ONLY is BEST minus the floor")

# CEILING_FREE drops AMD LBR.
ok(ct_cf.all? { |c| !(c.tier == TIER_HWTRACE && c.backend == AMD_LBR) },
   "cross-tier CEILING_FREE never selects AMD LBR")

# auto_tier(policy) is the head of resolve_tiers(policy).
ct_one = HwTrace.auto_tier(TRACE_BEST)
ok(!ct_one.nil? &&
   ct_one.tier == ct_best.first.tier && ct_one.backend == ct_best.first.backend,
   "cross-tier auto_tier(BEST) is the head of the resolved cascade")

# On any x86-64 Linux host the single-step backend is a native floor, so even
# NATIVE_ONLY resolves (the cascade never collapses to nothing here). Inline-guard
# rather than the process-exiting skip(): the rest of the suite still runs.
if HwTrace.available?(SINGLESTEP)
  pick = HwTrace.auto_tier(TRACE_NATIVE_ONLY)
  ok(!ct_nat.empty? && !pick.nil? && pick.fidelity == FIDELITY_NATIVE,
     "cross-tier NATIVE_ONLY resolves a native pick on x86-64 Linux")
  ok(ct_nat.any? { |c| c.tier == TIER_HWTRACE && c.backend == SINGLESTEP },
     "cross-tier NATIVE_ONLY includes the single-step native floor")
else
  puts "# SKIP cross-tier NATIVE_ONLY (single-step unavailable): #{HwTrace.skip_reason(SINGLESTEP)}"
end

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

# ---- Out-of-process / foreign-process toolkit (asmtest_ptrace.h) ----
# The same libasmtest_hwtrace this binding already loaded exposes the ptrace tier:
# single-step a forked/attached target out of band, and resolve the (base,len) to
# trace from the OS. Inline-guard on ptrace_available? rather than the process-
# exiting skip() — the harness is one linear script and earlier tests have run.
require "tmpdir"

if HwTrace.ptrace_available?
  # Fork a tracee, single-step it out of process, get the same offsets as the
  # in-process stepper for the shared ROUTINE fixture.
  pt_code = NativeCode.from_bytes(ROUTINE)
  pt_trace = HwTrace.create(blocks: 64, instructions: 64)
  pt_result = HwTrace.ptrace_trace_call(pt_code.base, pt_code.length, [20, 22], pt_trace)
  ok(pt_result == 42, "ptrace trace_call(20,22) == 42 (got #{pt_result})")
  ok(pt_trace.insn_offsets == [0x0, 0x3, 0x6, 0xC, 0x11],
     "ptrace trace_call insn_offsets == [0,3,6,12,17] (got #{pt_trace.insn_offsets.inspect})")
  ok(!pt_trace.truncated?, "ptrace trace_call not truncated")
  pt_trace.free

  # run_to drives an attached target to a resolved method (software breakpoint). A live
  # foreign attach is covered by the C suite (forking + ptrace of a foreign process is
  # impractical here, same as ptrace_trace_attached); exercise the FFI round-trip safely
  # — a NULL target address is rejected (EINVAL, non-zero) before any ptrace call.
  ok(HwTrace.ptrace_run_to(Process.pid, 0) != Asmtest::HwTrace::PTRACE_OK,
     "ptrace run_to(NULL addr) rejected (EINVAL) via the FFI round-trip")

  # Discover an executable region's extent from /proc/<pid>/maps by an interior
  # address (this process); addr 1 maps nothing.
  region = HwTrace.proc_region_by_addr(Process.pid, pt_code.base + 4)
  ok(!region.nil?, "region_by_addr finds the mapping for an interior address")
  if region
    rbase, rlen = region
    ok(rbase == pt_code.base && rlen >= ROUTINE.bytesize,
       "region_by_addr base == code base and len >= 18 (got #{rbase}/#{rlen})")
  else
    ok(false, "region_by_addr base == code base and len >= 18 (no region)")
  end
  ok(HwTrace.proc_region_by_addr(Process.pid, 0x1).nil?,
     "region_by_addr(addr 1) is nil (nothing maps addr 1)")
  pt_code.free

  # Parse a JIT perf-map (/tmp/perf-<pid>.map) and resolve a method by name.
  pid = Process.pid
  perf_path = "/tmp/perf-#{pid}.map"
  File.write(perf_path, "400000 1a void demo(long, long)\n500000 8 other\n")
  begin
    sym = HwTrace.proc_perfmap_symbol(pid, "void demo(long, long)")
    ok(sym == [0x400000, 0x1A],
       "perfmap_symbol resolves [0x400000, 0x1a] (got #{sym.inspect})")
    ok(HwTrace.proc_perfmap_symbol(pid, "missing").nil?,
       "perfmap_symbol(missing) is nil")
  ensure
    File.delete(perf_path) if File.exist?(perf_path)
  end

  # Read a binary jitdump (little-endian: V for u32, Q< for u64) and resolve a
  # method to (addr,size,index,timestamp) + recorded code bytes; missing -> nil.
  Dir.mktmpdir do |dir|
    jit_path = File.join(dir, "jit.dump")
    name = "void demo(long, long)"
    File.open(jit_path, "wb") do |f|
      # header: magic, version, total_size=40, elf_mach, pad1, pid, timestamp, flags
      f.write([0x4A695444, 1, 40, 62, 0, 0].pack("V6") + [0, 0].pack("Q<2"))
      total = 16 + 40 + (name.bytesize + 1) + ROUTINE.bytesize
      # JIT_CODE_LOAD record header: id, total_size, timestamp
      f.write([0, total].pack("V2") + [5].pack("Q<"))
      # body: pid, tid, vma, code_addr, code_size, code_index
      f.write([0, 0].pack("V2") + [0x2000, 0x2000, ROUTINE.bytesize, 9].pack("Q<4"))
      f.write(name + "\x00")
      f.write(ROUTINE)
    end
    m = HwTrace.jitdump_find(jit_path, "void demo(long, long)", want_bytes: 64)
    ok(!m.nil?, "jitdump_find resolves the method")
    if m
      ok([m.code_addr, m.code_size, m.code_index, m.timestamp] ==
           [0x2000, ROUTINE.bytesize, 9, 5],
         "jitdump_find (addr,size,index,ts) == (0x2000,18,9,5) (got #{[m.code_addr, m.code_size, m.code_index, m.timestamp].inspect})")
      ok(m.code == ROUTINE, "jitdump_find code bytes == ROUTINE")
    else
      ok(false, "jitdump_find (addr,size,index,ts) (no method)")
      ok(false, "jitdump_find code bytes == ROUTINE (no method)")
    end
    ok(HwTrace.jitdump_find(jit_path, "missing").nil?, "jitdump_find(missing) is nil")
  end
else
  puts "# SKIP out-of-process ptrace toolkit (unavailable): #{HwTrace.ptrace_skip_reason}"
end

# ---- Time-aware code-image recorder (asmtest_codeimage.h) ----
# A userspace PERF_RECORD_TEXT_POKE: track() snapshots a region's bytes and arms
# write-protect; bytes_at(addr, when) answers "what bytes were live at addr as of
# sequence when". Round-trip a tracked region's bytes through version 0 (pid 0 =
# this process). Inline-guard on CodeImage.available? — the recorder self-skips
# where there is no PAGEMAP_SCAN / soft-dirty support.
CI_BYTES = [0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3].pack("C*") # mov rax,rdi; add rax,rsi; ret

if CodeImage.available?
  ci_code = NativeCode.from_bytes(CI_BYTES)
  img = CodeImage.new(0)
  begin
    rc = img.track(ci_code.base, CI_BYTES.bytesize)
    ok(rc == Asmtest::HwTrace::CI_OK, "codeimage track(base, 7) == CI_OK (got #{rc})")
    ok(img.now >= 1, "codeimage now >= 1 after track (got #{img.now})")
    ok(img.refresh >= 0, "codeimage refresh >= 0 (got #{img.refresh})")
    got = img.bytes_at(ci_code.base, 0)
    ok(!got.nil? && got[0, CI_BYTES.bytesize] == CI_BYTES,
       "codeimage bytes_at(base, 0) round-trips the 7 code bytes (got #{got.inspect})")
  ensure
    img.free
    ci_code.free
  end

  # eBPF emission detector (Phase C) — optional; self-skips without libbpf /
  # CAP_BPF / BTF. When available, watch_bpf loads + attaches and returns CI_OK.
  if CodeImage.bpf_available?
    bpf_img = CodeImage.new(0)
    begin
      ok(bpf_img.watch_bpf == Asmtest::HwTrace::CI_OK,
         "codeimage watch_bpf == CI_OK")
    ensure
      bpf_img.free
    end
  else
    puts "# SKIP codeimage eBPF detector (unavailable): #{CodeImage.bpf_skip_reason}"
  end
else
  puts "# SKIP code-image recorder (unavailable): #{CodeImage.skip_reason}"
end

exit($failed ? 1 : 0)
