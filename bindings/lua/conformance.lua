-- conformance.lua — asm-test Lua binding (Track C): the conformance runner.
--
-- A thin consumer of the reusable library module (asmtest.lua): it replays the
-- cross-language conformance corpus through the Regs / Emu / assert_* API and
-- never touches `ffi` itself. Exits nonzero on a mismatch.
--
--   ASMTEST_LIB         libasmtest_emu.{so,dylib}
--   ASMTEST_CORPUS_LIB  libasmtest_corpus.{so,dylib}

-- Find asmtest.lua next to this script, regardless of the current directory.
local here = (arg and arg[0] and arg[0]:match("^(.*)[/\\][^/\\]*$")) or "."
package.path = here .. "/?.lua;" .. package.path
local asmtest = require("asmtest")

local function routine(name) return asmtest.corpus_routine(name) end

local fails = 0
local total = 0
local function check(name, ok)
  total = total + 1
  if ok then
    print("ok - " .. name)
  else
    fails = fails + 1
    print("not ok - " .. name)
  end
end

local function withRegs(f)
  local r = asmtest.Regs()
  local ok, err = pcall(f, r)
  r:free()
  if not ok then error(err) end
end

-- --- Tier 1: corpus replay (capture trampoline) ----------------------------
withRegs(function(r)
  r:capture6(routine("add_signed"), 40, 2)
  check("add_signed.basic", r:ret() == 42 and r:abi_preserved())
end)
withRegs(function(r)
  r:capture6(routine("sum_via_rbx"), 20, 22)
  check("sum_via_rbx.abi_preserved", r:ret() == 42 and r:abi_preserved())
end)
withRegs(function(r)
  r:capture6(routine("clobbers_rbx"), 1, 2)
  check("clobbers_rbx.abi_violation_detected", not r:abi_preserved())
end)
withRegs(function(r)
  r:capture6(routine("set_carry"))
  check("set_carry.cf_set", r:flag_set("CF"))
end)
withRegs(function(r)
  r:capture6(routine("clear_carry"))
  check("clear_carry.cf_clear", not r:flag_set("CF"))
end)
withRegs(function(r)
  r:capture_fp2(routine("fp_add"), 1.5, 2.25)
  check("fp_add.basic", r:fret() == 3.75)
end)
withRegs(function(r)
  r:capture_vec_f32(routine("vec_add4f"), {{1, 2, 3, 4}, {10, 20, 30, 40}})
  local v = r:vec_f32(0)
  check("vec_add4f.basic", v[1] == 11 and v[2] == 22 and v[3] == 33 and v[4] == 44)
end)

-- --- Tier 1: corpus replay (emulator, x86-64 guest) ------------------------
do
  local e = asmtest.Emu()
  local res = e:call2(routine("add_signed"), 40, 2)
  check("emu.add_signed", not res:faulted() and res:reg("rax") == 42)
  res:free()

  -- read_fault dereferences an unmapped address: the fault is data — where
  -- (fault_addr) and why (fault_kind) — not a crash.
  local fres = e:call2(routine("read_fault"), 0x00DEAD00, 0)
  check("emu.read_fault",
    fres:faulted() and fres:fault_addr() == 0x00DEAD00 and
    fres:fault_kind() == asmtest.FaultKind.READ)
  fres:free()

  -- int_to_double lands (double)42 in xmm0 (the XMM file, beyond the GP regs);
  -- a clean run also keeps rflags live (x86 holds bit 1 set).
  local xres = e:call2(routine("int_to_double"), 42, 0)
  check("emu.int_to_double",
    not xres:faulted() and xres:xmm_f64(0, 0) == 42.0 and
    math.floor(xres:reg("rflags") / 2) % 2 == 1) -- rflags reserved bit 1 always set
  xres:free()

  -- in-line assembly (Keystone) replays add_signed, only if the lib has it
  if e:asm_available() then
    local ares = e:call_asm("mov rax, rdi; add rax, rsi; ret", {40, 2})
    check("asm.add_signed", not ares:faulted() and ares:reg("rax") == 42)
    ares:free()

    -- Widened shim: AT&T syntax + a third arg (rdi+rsi+rdx).
    local att = e:call_asm("mov %rdi, %rax; add %rsi, %rax; add %rdx, %rax; ret",
      {10, 20, 12}, { syntax = asmtest.Syntax.ATT })
    check("asm.att_3arg", not att:faulted() and att:reg("rax") == 42)
    att:free()

    -- Failure path: a bad string error()s with the Keystone diagnostic.
    local threw = not pcall(function() e:call_asm("mov rax, nonsense_token"):free() end)
    check("asm.bad_source_throws", threw)

    -- Multi-arch assemble-to-bytes: AArch64 `ret` is C0 03 5F D6.
    local a64 = asmtest.assemble("ret", asmtest.Arch.ARM64)
    check("asm.arm64_bytes", #a64 == 4 and a64:byte(1) == 0xC0 and a64:byte(4) == 0xD6)
  end
  e:close()
end

-- --- Tier 2: idiomatic assertions pass on good input -----------------------
local t2pass = pcall(function()
  withRegs(function(r)
    r:capture6(routine("add_signed"), 40, 2)
    asmtest.assert_ret(r, 42)
    asmtest.assert_abi_preserved(r)
  end)
  withRegs(function(r)
    r:capture_fp2(routine("fp_add"), 1.5, 2.25)
    asmtest.assert_fp(r, 3.75)
  end)
  withRegs(function(r)
    r:capture_vec_f32(routine("vec_add4f"), {{1, 2, 3, 4}, {10, 20, 30, 40}})
    asmtest.assert_vec_f32(r, 0, {11, 22, 33, 44})
  end)
  local fe = asmtest.Emu()
  local ff = fe:call2(routine("read_fault"), 0x00DEAD00, 0)
  asmtest.assert_fault(ff)
  ff:free()
  fe:close()
end)
check("tier2.assertions_pass", t2pass)

-- --- Tier 2: the assertions actually fail when they should -----------------
local t2teeth = not pcall(function()
  withRegs(function(r)
    r:capture6(routine("add_signed"), 40, 2)
    asmtest.assert_ret(r, 99) -- wrong on purpose
  end)
end)
check("tier2.assertions_have_teeth", t2teeth)

print(string.format("# %d passed, %d failed, %d total", total - fails, fails, total))
os.exit(fails == 0 and 0 or 1)
