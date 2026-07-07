# conformance.rb — asm-test Ruby binding (Track C): the conformance runner.
#
# A thin consumer of the reusable library module (./asmtest): it replays the
# cross-language conformance corpus through the Asmtest::Regs / Emu / assert_*
# API and never touches Fiddle itself. Exits nonzero on any mismatch.
#
#   ASMTEST_LIB         libasmtest_emu.{so,dylib}
#   ASMTEST_CORPUS_LIB  libasmtest_corpus.{so,dylib}
#   ASMTEST_HWTRACE_LIB libasmtest_hwtrace.{so,dylib} (only for the ptrace_descent
#                       tier below; absent/off-host => that tier self-skips)
require_relative "asmtest"
require_relative "hwtrace"
require "json"

def routine(name)
  Asmtest.corpus_routine(name)
end

$fail = 0
$total = 0
def check(name, ok)
  $total += 1
  if ok
    puts "ok - #{name}"
  else
    $fail += 1
    puts "not ok - #{name}"
  end
end

def with_regs
  r = Asmtest::Regs.new
  begin
    yield r
  ensure
    r.free
  end
end

# --- Tier 1: corpus replay (capture trampoline) ----------------------------
with_regs do |r|
  r.capture6(routine("add_signed"), 40, 2)
  check("add_signed.basic", r.ret == 42 && r.abi_preserved?)
end
with_regs do |r|
  r.capture6(routine("sum_via_rbx"), 20, 22)
  check("sum_via_rbx.abi_preserved", r.ret == 42 && r.abi_preserved?)
end
with_regs do |r|
  r.capture6(routine("clobbers_rbx"), 1, 2)
  check("clobbers_rbx.abi_violation_detected", !r.abi_preserved?)
end
with_regs do |r|
  r.capture6(routine("set_carry"))
  check("set_carry.cf_set", r.flag_set?("CF"))
end
with_regs do |r|
  r.capture6(routine("clear_carry"))
  check("clear_carry.cf_clear", !r.flag_set?("CF"))
end
with_regs do |r|
  r.capture_fp2(routine("fp_add"), 1.5, 2.25)
  check("fp_add.basic", r.fret == 3.75)
end
with_regs do |r|
  r.capture_vec_f32(routine("vec_add4f"), [[1, 2, 3, 4], [10, 20, 30, 40]])
  check("vec_add4f.basic", r.vec_f32(0) == [11, 22, 33, 44])
end
with_regs do |r|
  # 8 integer args: the first 6 in registers, args 7-8 on the stack (x86-64).
  r.capture_args(routine("sum8"), [1, 2, 3, 4, 5, 6, 7, 8])
  check("sum8.wide_arity", r.ret == 36 && r.abi_preserved?)
end
with_regs do |r|
  # mix_scale(n, x) = (double)n * x reads BOTH argument register files.
  r.capture_mix(routine("mix_scale"), [3], [2.5])
  check("mix_scale.mixed_int_fp", r.fret == 7.5)
end
with_regs do |r|
  # make_big returns a 24-byte struct{long a,b,c} via the hidden pointer.
  big = r.capture_sret(routine("make_big"), 24, [7, 8, 9])
  check("make_big.struct_return_sret",
        big.unpack("q3") == [7, 8, 9] && r.ret != 0)
end

# --- Tier 1: corpus replay (emulator, x86-64 guest) ------------------------
e = Asmtest::Emu.new
res = e.call2(routine("add_signed"), 40, 2)
check("emu.add_signed", !res.faulted? && res.reg("rax") == 42)
res.free

# read_fault dereferences an unmapped address: the fault is data — where
# (fault_addr) and why (fault_kind) — not a crash.
fres = e.call2(routine("read_fault"), 0x00DEAD00, 0)
check("emu.read_fault",
      fres.faulted? && fres.fault_addr == 0x00DEAD00 &&
        fres.fault_kind == Asmtest::FaultKind::READ)
fres.free

# int_to_double lands (double)42 in xmm0 (the XMM file, beyond the GP regs);
# a clean run also keeps rflags live (x86 holds bit 1 set).
xres = e.call2(routine("int_to_double"), 42, 0)
check("emu.int_to_double",
      !xres.faulted? && xres.xmm_f64(0, 0) == 42.0 && (xres.reg("rflags") & 0x2) != 0)
xres.free

# Pack bytes (0..255) into a binary machine-code String.
def code(*bytes)
  bytes.pack("C*")
end

# --- Tier 1: cross-arch emulator guests (raw bytes, any host) --------------
{
  "arm64" => [code(0x00, 0x00, 0x01, 0x8B, 0xC0, 0x03, 0x5F, 0xD6), "x0"],
  "riscv" => [code(0x33, 0x05, 0xB5, 0x00, 0x67, 0x80, 0x00, 0x00), "a0"],
  "arm"   => [code(0x01, 0x00, 0x80, 0xE0, 0x1E, 0xFF, 0x2F, 0xE1), "r0"],
}.each do |arch, (bytes, regname)|
  g = Asmtest::Guest.new(arch)
  res = g.call(bytes, [40, 2])
  check("emu_#{arch}.add", !res.faulted? && res.reg(regname) == 42)
  res.free
  g.close
end

# --- Tier 1: extended x86-64 emulator calls (raw bytes) --------------------
ex = Asmtest::Emu.new
wide = ex.call_bytes(code(0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x01, 0xD0, 0xC3), [10, 20, 12])
check("emu.wide_int", !wide.faulted? && wide.reg("rax") == 42)
wide.free

fp = ex.call_fp(code(0xF2, 0x0F, 0x58, 0xC1, 0xC3), fargs: [1.5, 2.25])
check("emu.fp_add", !fp.faulted? && fp.xmm_f64(0, 0) == 3.75)
fp.free

vec = ex.call_vec(code(0x0F, 0x58, 0xC1, 0xC3), vargs: [[1, 2, 3, 4], [10, 20, 30, 40]])
check("emu.vec_add4f", !vec.faulted? && vec.xmm_f32(0, 0) == 11 && vec.xmm_f32(0, 3) == 44)
vec.free

win = ex.call_win64(code(0x48, 0x89, 0xC8, 0x48, 0x01, 0xD0, 0xC3), [40, 2])
check("emu.win64_add", !win.faulted? && win.reg("rax") == 42)
win.free
ex.close

# --- Tier 1: execution trace / coverage (cross-arch arm64) -----------------
gt = Asmtest::Guest.new("arm64")
tr = Asmtest::Trace.new
sel = code(0x60, 0x00, 0x00, 0xB4, 0x60, 0x0C, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6,
           0x40, 0x05, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6)
tres = gt.call_traced(sel, [0], tr)
check("emu_arm64.trace_sel",
      !tres.faulted? && tres.reg("x0") == 42 &&
        tr.covered?(0) && tr.covered?(12) && !tr.covered?(4))
tres.free
tr.free
gt.close

# --- Tier 1: in-line assembly (Keystone) replays add_signed ----------------
# libasmtest_emu carries the assembler, so this runs by default; the probe stays
# a defensive guard, false only against an older/leaner lib.
if e.asm_available?
  ares = e.call_asm("mov rax, rdi; add rax, rsi; ret", [40, 2])
  check("asm.add_signed", !ares.faulted? && ares.reg("rax") == 42)
  ares.free

  # Widened shim: AT&T syntax + a third arg (rdi+rsi+rdx).
  att = e.call_asm("mov %rdi, %rax; add %rsi, %rax; add %rdx, %rax; ret",
                   [10, 20, 12], syntax: Asmtest::SYNTAX[:att])
  check("asm.att_3arg", !att.faulted? && att.reg("rax") == 42)
  att.free

  # Failure path: a bad string raises with the Keystone diagnostic.
  threw = begin
    e.call_asm("mov rax, nonsense_token").free
    false
  rescue Asmtest::Error => ex
    ex.message.length > "in-line assembly failed: ".length
  end
  check("asm.bad_source_throws", threw)

  # Multi-arch assemble-to-bytes: AArch64 `ret` is C0 03 5F D6.
  a64 = Asmtest.assemble("ret", arch: Asmtest::ARCH[:arm64])
  check("asm.arm64_bytes", a64.bytesize == 4 && a64.bytes[0] == 0xC0 && a64.bytes[3] == 0xD6)
end
e.close

# --- Tier 1: disassembly (Capstone) decodes known bytes to text ------------
# libasmtest_emu carries Capstone, so this runs by default; the probe stays a
# defensive guard — only an older/leaner lib reports disas_available? == false.
if Asmtest.disas_available?
  code = [0x48, 0x31, 0xC0, 0xC3].pack("C*") # xor rax, rax ; ret
  check("disas.xor_rax", Asmtest.disas(code, 0) == "xor rax, rax")
  check("disas.ret", Asmtest.disas(code, 3) == "ret")
  check("disas.nop", Asmtest.disas([0x90].pack("C*")) == "nop")
else
  puts "ok - disas.xor_rax # SKIP no disassembler (older/leaner lib)"
end

# --- Tier 2: idiomatic assertions pass on good input -----------------------
tier2_pass = begin
  with_regs do |r|
    r.capture6(routine("add_signed"), 40, 2)
    Asmtest.assert_ret(r, 42)
    Asmtest.assert_abi_preserved(r)
  end
  with_regs do |r|
    r.capture_fp2(routine("fp_add"), 1.5, 2.25)
    Asmtest.assert_fp(r, 3.75)
  end
  with_regs do |r|
    r.capture_vec_f32(routine("vec_add4f"), [[1, 2, 3, 4], [10, 20, 30, 40]])
    Asmtest.assert_vec_f32(r, 0, [11, 22, 33, 44])
  end
  fe = Asmtest::Emu.new
  ff = fe.call2(routine("read_fault"), 0x00DEAD00, 0)
  Asmtest.assert_fault(ff)
  ff.free
  fe.close
  true
rescue Asmtest::Error
  false
end
check("tier2.assertions_pass", tier2_pass)

# --- Tier 2: the assertions actually fail when they should -----------------
tier2_teeth = begin
  with_regs do |r|
    r.capture6(routine("add_signed"), 40, 2)
    Asmtest.assert_ret(r, 99) # wrong on purpose
  end
  false
rescue Asmtest::Error
  true
end
check("tier2.assertions_have_teeth", tier2_teeth)

# --- Track F: mid-execution guards (byte-literal routines) -----------------
ge = Asmtest::Emu.new
two_writes = code(0x48, 0x89, 0x07, 0x48, 0x89, 0x87, 0x00, 0x08, 0x00, 0x00, 0xC3)
ge.map(0x400000, 0x1000)
w = ge.watch_writes(0x400000, 8, :only)
ge.call_bytes(two_writes, [0x400000])
ge.watch_clear
check("guard.watch_escape", w.violated? && w.addr == 0x400800 && w.rip_off == 3)
w.free
clobber = code(0x48, 0xC7, 0xC3, 0x99, 0x00, 0x00, 0x00, 0xEB, 0x00, 0xC3)
g = ge.guard_reg("rbx", 0)
ge.call_bytes(clobber, [])
ge.guard_reg_clear
check("guard.reg_invariant", g.violated? && g.got == 0x99)
g.free
ge.close

# --- Track E: coverage-guided fuzzing + mutation testing -------------------
fze = Asmtest::Emu.new
classify3 = code(0x31, 0xC0, 0x48, 0x85, 0xFF, 0x78, 0x0B, 0x48, 0x85, 0xFF, 0x74, 0x05,
                 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3)
fixed_blocks, = fze.fuzz_cover(classify3, 5, 5, 1)
guided_blocks, = fze.fuzz_cover(classify3, -50, 50, 2000)
check("fuzz.coverage_beats_fixed", guided_blocks > fixed_blocks)
_, weak_s = fze.mutation_test(classify3, [5])
_, strong_s = fze.mutation_test(classify3, [-7, 0, 9])
check("mutation.strong_kills_more", weak_s.positive? && strong_s < weak_s)
fze.close

# --- Track D: AVX2 256-bit capture (self-skips off-AVX2) --------------------
if Asmtest.cpu_has_avx2?
  out = Asmtest.capture_vec256(routine("vec_add4d"),
                               [[1.0, 2.0, 3.0, 4.0], [10.0, 20.0, 30.0, 40.0]])
  check("vec256.add4d", out[0].unpack("d4") == [11.0, 22.0, 33.0, 44.0])
else
  puts "ok - vec256.add4d # SKIP no AVX2"
end

# --- Track D: AVX-512 512-bit capture (self-skips off-AVX-512) --------------
if Asmtest.cpu_has_avx512f?
  out = Asmtest.capture_vec512(routine("vec_add8d"),
                               [[1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0],
                                [10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0]])
  check("vec512.add8d",
        out[0].unpack("d8") == [11.0, 22.0, 33.0, 44.0, 55.0, 66.0, 77.0, 88.0])
else
  puts "ok - vec512.add8d # SKIP no AVX-512"
end

# --- Tier: call descent (asmtest_ptrace.h ptrace_descent) ------------------
# Replay the corpus's ptrace_descent cases through the out-of-process stepper:
# fork host-native code, single-step it, and check the frame-0 body, the call
# EDGES (level 1), and the DESCENDED frames (level 2). Host-native, so it replays
# only when the corpus arch matches the host AND the ptrace single-step tier is
# available (Linux x86-64 + Capstone); otherwise every case SKIPs (never fails) —
# mirroring the Python _run_ptrace_descent handler and the C reference tier. The
# descent tier carries raw code bytes (not a named corpus routine), so this reads
# the cases straight from the JSON corpus the C reference emits.
hw = Asmtest::HwTrace::HwTrace
host_arch =
  case RbConfig::CONFIG["host_cpu"]
  when /x86_64|amd64/ then "x86_64"
  when /aarch64|arm64/ then "aarch64"
  end

descent_cases =
  begin
    corpus_json = ENV["ASMTEST_CORPUS_JSON"].to_s
    corpus_json = File.expand_path("../conformance/corpus.json", __dir__) if corpus_json.empty?
    if File.exist?(corpus_json)
      (JSON.parse(File.read(corpus_json)).dig("corpus", "cases") || [])
        .select { |c| c["tier"] == "ptrace_descent" }
    else
      []
    end
  rescue StandardError
    []
  end

if descent_cases.empty?
  puts "ok - ptrace_descent # SKIP no ptrace_descent cases in corpus"
elsif !hw.ptrace_available?
  descent_cases.each do |c|
    puts "ok - #{c['name']} # SKIP ptrace stepper unavailable: #{hw.ptrace_skip_reason}"
  end
else
  descent_cases.each do |c|
    if c["arch"] != host_arch
      puts "ok - #{c['name']} # SKIP corpus arch #{c['arch']} != host #{host_arch.inspect}"
      next
    end
    exp = c["expect"]
    nc = Asmtest::HwTrace::NativeCode.from_bytes(c["code"].pack("C*"))
    d  = Asmtest::HwTrace::Descent.new(c["level"])
    tr = hw.create(blocks: 64, instructions: 64)
    begin
      if c["level"] >= Asmtest::HwTrace::DESCENT_DESCEND_KNOWN && c["allow_off"]
        d.allow_region(nc.base + c["allow_off"], c["allow_len"])
      end
      result = hw.ptrace_trace_call_ex(nc.base, nc.length, c["args"] || [], tr, d,
                                       region: c["region"])
      good = result == exp["result"] && d.frame_insns(0) == exp["frame0"]
      # Call edges (level 1): site offset + absolute target (base + target_off).
      got_edges = d.edges
      want_edges = exp["edges"] || []
      good &&= got_edges.length == want_edges.length
      want_edges.each_with_index do |we, i|
        ge = got_edges[i]
        good &&= ge && ge[0] == we["site"] && ge[1] == nc.base + we["target_off"]
      end
      # Descended frames beyond frame 0 (level 2), matched by absolute base.
      (exp["frames"] || []).each do |wf|
        idx = (1...d.frames_len).find { |i| d.frame_base(i) == nc.base + wf["base_off"] }
        good &&= idx && d.frame_depth(idx) == wf["depth"] && d.frame_insns(idx) == wf["insns"]
      end
      check(c["name"], good)
    ensure
      tr.free
      d.free
      nc.free
    end
  end
end

puts "# #{$total - $fail} passed, #{$fail} failed, #{$total} total"
exit($fail == 0 ? 0 : 1)
