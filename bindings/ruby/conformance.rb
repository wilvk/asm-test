# conformance.rb — asm-test Ruby binding (Track C): the conformance runner.
#
# A thin consumer of the reusable library module (./asmtest): it replays the
# cross-language conformance corpus through the Asmtest::Regs / Emu / assert_*
# API and never touches Fiddle itself. Exits nonzero on any mismatch.
#
#   ASMTEST_LIB         libasmtest_emu.{so,dylib}
#   ASMTEST_CORPUS_LIB  libasmtest_corpus.{so,dylib}
require_relative "asmtest"

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

# --- Tier 1: corpus replay (emulator, x86-64 guest) ------------------------
e = Asmtest::Emu.new
res = e.call2(routine("add_signed"), 40, 2)
check("emu.add_signed", !res.faulted? && res.reg("rax") == 42)
res.free

# --- Tier 1: in-line assembly (Keystone) replays add_signed ----------------
# Only when the loaded lib carries the assembler (libasmtest_emu_asm); against
# the plain libasmtest_emu it is simply absent.
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

puts "# #{$total - $fail} passed, #{$fail} failed, #{$total} total"
exit($fail == 0 ? 0 : 1)
