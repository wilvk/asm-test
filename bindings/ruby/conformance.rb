# conformance.rb — asm-test Ruby binding (Track C), via the stdlib Fiddle FFI.
#
# Replays the conformance corpus using the opaque-handle FFI layer (no struct
# layout needed): asmtest_corpus_routine for addresses, asmtest_capture6 /
# _fp2 + accessors for the capture tier, and asmtest_emu_call2 + accessors for
# the emulator. Exits nonzero on any mismatch.
#
#   ASMTEST_LIB         libasmtest_emu.{so,dylib}
#   ASMTEST_CORPUS_LIB  libasmtest_corpus.{so,dylib}
require "fiddle"

emu_path = ENV["ASMTEST_LIB"] or abort "set ASMTEST_LIB"
corpus_path = ENV["ASMTEST_CORPUS_LIB"] or abort "set ASMTEST_CORPUS_LIB"

L = Fiddle.dlopen(emu_path)
C = Fiddle.dlopen(corpus_path)

VOIDP = Fiddle::TYPE_VOIDP
LONG  = Fiddle::TYPE_LONG
INT   = Fiddle::TYPE_INT
DBL   = Fiddle::TYPE_DOUBLE
LL    = Fiddle::TYPE_LONG_LONG
SZ    = Fiddle::TYPE_SIZE_T
VOID  = Fiddle::TYPE_VOID

def func(lib, name, args, ret)
  Fiddle::Function.new(lib[name], args, ret)
end

CorpusRoutine = func(C, "asmtest_corpus_routine", [VOIDP], VOIDP)
RegsNew       = func(L, "asmtest_regs_new", [], VOIDP)
RegsFree      = func(L, "asmtest_regs_free", [VOIDP], VOID)
Capture6      = func(L, "asmtest_capture6", [VOIDP, VOIDP, LONG, LONG, LONG, LONG, LONG, LONG], VOID)
CaptureFp2    = func(L, "asmtest_capture_fp2", [VOIDP, VOIDP, DBL, DBL], VOID)
RegsRet       = func(L, "asmtest_regs_ret", [VOIDP], LONG)
RegsFret      = func(L, "asmtest_regs_fret", [VOIDP], DBL)
RegsFlagSet   = func(L, "asmtest_regs_flag_set", [VOIDP, VOIDP], INT)
CheckAbi      = func(L, "asmtest_check_abi", [VOIDP, VOIDP, SZ], INT)
EmuOpen       = func(L, "emu_open", [], VOIDP)
EmuClose      = func(L, "emu_close", [VOIDP], VOID)
EmuResNew     = func(L, "asmtest_emu_result_new", [], VOIDP)
EmuResFree    = func(L, "asmtest_emu_result_free", [VOIDP], VOID)
EmuCall2      = func(L, "asmtest_emu_call2", [VOIDP, VOIDP, LONG, LONG, VOIDP], INT)
EmuFaulted    = func(L, "asmtest_emu_result_faulted", [VOIDP], INT)
EmuReg        = func(L, "asmtest_emu_x86_reg", [VOIDP, VOIDP], LL)

def routine(name) = CorpusRoutine.call(name)

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

# Tier-2 idiomatic assertions: raise with a clear message on failure.
class AsmtestError < StandardError; end
def assert_ret(r, e)
  got = RegsRet.call(r)
  raise AsmtestError, "ret: got #{got}, want #{e}" unless got == e
end
def assert_abi_preserved(r)
  raise AsmtestError, "ABI not preserved" unless CheckAbi.call(r, nil, 0) == 0
end
def assert_flag(r, name, set = true)
  raise AsmtestError, "flag #{name}" unless (RegsFlagSet.call(r, name) == 1) == set
end
def assert_fp(r, e)
  got = RegsFret.call(r)
  raise AsmtestError, "fp: got #{got}, want #{e}" unless got == e
end

def with_regs
  r = RegsNew.call
  begin
    yield r
  ensure
    RegsFree.call(r)
  end
end

with_regs do |r|
  Capture6.call(r, routine("add_signed"), 40, 2, 0, 0, 0, 0)
  check("add_signed.basic", RegsRet.call(r) == 42 && CheckAbi.call(r, nil, 0) == 0)
end

with_regs do |r|
  Capture6.call(r, routine("sum_via_rbx"), 20, 22, 0, 0, 0, 0)
  check("sum_via_rbx.abi_preserved", RegsRet.call(r) == 42 && CheckAbi.call(r, nil, 0) == 0)
end

with_regs do |r|
  Capture6.call(r, routine("clobbers_rbx"), 1, 2, 0, 0, 0, 0)
  check("clobbers_rbx.abi_violation_detected", CheckAbi.call(r, nil, 0) != 0)
end

with_regs do |r|
  Capture6.call(r, routine("set_carry"), 0, 0, 0, 0, 0, 0)
  check("set_carry.cf_set", RegsFlagSet.call(r, "CF") == 1)
end

with_regs do |r|
  Capture6.call(r, routine("clear_carry"), 0, 0, 0, 0, 0, 0)
  check("clear_carry.cf_clear", RegsFlagSet.call(r, "CF") == 0)
end

with_regs do |r|
  CaptureFp2.call(r, routine("fp_add"), 1.5, 2.25)
  check("fp_add.basic", RegsFret.call(r) == 3.75)
end

# Emulator: faults as data via the opaque handle.
e = EmuOpen.call
res = EmuResNew.call
EmuCall2.call(e, routine("add_signed"), 40, 2, res)
check("emu.add_signed", EmuFaulted.call(res) == 0 && EmuReg.call(res, "rax") == 42)
EmuResFree.call(res)
EmuClose.call(e)

# ---- Tier-2 idiomatic assertions: pass paths succeed, failure paths bite ----
tier2_pass = begin
  with_regs do |r|
    Capture6.call(r, routine("add_signed"), 40, 2, 0, 0, 0, 0)
    assert_ret(r, 42)
    assert_abi_preserved(r)
  end
  with_regs do |r|
    CaptureFp2.call(r, routine("fp_add"), 1.5, 2.25)
    assert_fp(r, 3.75)
  end
  true
rescue AsmtestError
  false
end
check("tier2.assertions_pass", tier2_pass)

tier2_teeth = begin
  with_regs do |r|
    Capture6.call(r, routine("add_signed"), 40, 2, 0, 0, 0, 0)
    assert_ret(r, 99) # wrong on purpose
  end
  false
rescue AsmtestError
  true
end
check("tier2.assertions_have_teeth", tier2_teeth)

puts "# #{$total - $fail} passed, #{$fail} failed, #{$total} total"
exit($fail == 0 ? 0 : 1)
