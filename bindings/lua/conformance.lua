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
withRegs(function(r)
  -- 8 integer args: the first 6 in registers, args 7-8 on the stack (x86-64).
  r:capture_args(routine("sum8"), {1, 2, 3, 4, 5, 6, 7, 8})
  check("sum8.wide_arity", r:ret() == 36 and r:abi_preserved())
end)
withRegs(function(r)
  -- mix_scale(n, x) = (double)n * x reads BOTH argument register files.
  r:capture_mix(routine("mix_scale"), {3}, {2.5})
  check("mix_scale.mixed_int_fp", r:fret() == 7.5)
end)
withRegs(function(r)
  -- make_big returns a 24-byte struct{long a,b,c} via the hidden pointer.
  local big = r:capture_sret(routine("make_big"), 24, {7, 8, 9})
  local f = asmtest.read_longs(big, 3)
  check("make_big.struct_return_sret", f[1] == 7 and f[2] == 8 and f[3] == 9)
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

  -- cross-arch emulator guests (raw bytes, emulated on any host)
  for _, t in ipairs({
    { "arm64", string.char(0x00, 0x00, 0x01, 0x8B, 0xC0, 0x03, 0x5F, 0xD6), "x0" },
    { "riscv", string.char(0x33, 0x05, 0xB5, 0x00, 0x67, 0x80, 0x00, 0x00), "a0" },
    { "arm",   string.char(0x01, 0x00, 0x80, 0xE0, 0x1E, 0xFF, 0x2F, 0xE1), "r0" },
  }) do
    local g = asmtest.Guest(t[1])
    local gres = g:call(t[2], { 40, 2 })
    check("emu_" .. t[1] .. ".add", not gres:faulted() and gres:reg(t[3]) == 42)
    gres:free()
    g:close()
  end

  -- extended x86-64 emulator calls (raw bytes): wide int, FP, vector, Win64
  local wide = e:call_bytes(string.char(0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x01, 0xD0, 0xC3), { 10, 20, 12 })
  check("emu.wide_int", not wide:faulted() and wide:reg("rax") == 42)
  wide:free()
  local fpr = e:call_fp(string.char(0xF2, 0x0F, 0x58, 0xC1, 0xC3), { fargs = { 1.5, 2.25 } })
  check("emu.fp_add", not fpr:faulted() and fpr:xmm_f64(0, 0) == 3.75)
  fpr:free()
  local vecr = e:call_vec(string.char(0x0F, 0x58, 0xC1, 0xC3), { vargs = { { 1, 2, 3, 4 }, { 10, 20, 30, 40 } } })
  check("emu.vec_add4f", not vecr:faulted() and vecr:xmm_f32(0, 0) == 11 and vecr:xmm_f32(0, 3) == 44)
  vecr:free()
  local winr = e:call_win64(string.char(0x48, 0x89, 0xC8, 0x48, 0x01, 0xD0, 0xC3), { 40, 2 })
  check("emu.win64_add", not winr:faulted() and winr:reg("rax") == 42)
  winr:free()

  -- execution trace / coverage (cross-arch arm64 two-block select)
  local gt = asmtest.Guest("arm64")
  local tr = asmtest.Trace()
  local sel = string.char(0x60, 0x00, 0x00, 0xB4, 0x60, 0x0C, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6,
    0x40, 0x05, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6)
  local tres = gt:call_traced(sel, { 0 }, tr)
  check("emu_arm64.trace_sel",
    not tres:faulted() and tres:reg("x0") == 42 and
    tr:covered(0) and tr:covered(12) and not tr:covered(4))
  tres:free()
  tr:free()
  gt:close()

  -- in-line assembly (Keystone) replays add_signed; carried by libasmtest_emu,
  -- the probe is a defensive guard against an older/leaner lib
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

-- --- Tier 1: disassembly (Capstone) decodes known bytes to text ------------
-- libasmtest_emu carries Capstone, so this runs by default; the probe stays a
-- defensive guard -- only an older/leaner lib reports disas_available() false.
if asmtest.disas_available() then
  local code = string.char(0x48, 0x31, 0xC0, 0xC3) -- xor rax, rax ; ret
  check("disas.xor_rax", asmtest.disas(code, 0) == "xor rax, rax")
  check("disas.ret", asmtest.disas(code, 3) == "ret")
  check("disas.nop", asmtest.disas(string.char(0x90)) == "nop")
else
  print("ok - disas.xor_rax # SKIP no disassembler (older/leaner lib)")
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

-- Track F: mid-execution guards (byte-literal routines).
do
  local e = asmtest.Emu()
  local two_writes = string.char(0x48, 0x89, 0x07, 0x48, 0x89, 0x87, 0x00, 0x08, 0x00, 0x00, 0xC3)
  e:map(0x400000, 0x1000)
  local w = e:watch_writes(0x400000, 8, "only")
  e:call_bytes(two_writes, { 0x400000 })
  e:watch_clear()
  check("guard.watch_escape", w:violated() and w:addr() == 0x400800 and w:rip_off() == 3)
  w:free()
  local clobber = string.char(0x48, 0xC7, 0xC3, 0x99, 0x00, 0x00, 0x00, 0xEB, 0x00, 0xC3)
  local g = e:guard_reg("rbx", 0)
  e:call_bytes(clobber, {})
  e:guard_reg_clear()
  check("guard.reg_invariant", g:violated() and g:got() == 0x99)
  g:free()
end

-- Track E: coverage-guided fuzzing + mutation testing over classify3.
do
  local e = asmtest.Emu()
  local classify3 = string.char(0x31, 0xC0, 0x48, 0x85, 0xFF, 0x78, 0x0B, 0x48, 0x85, 0xFF, 0x74, 0x05,
    0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3)
  local fixed = e:fuzz_cover(classify3, 5, 5, 1)
  local guided = e:fuzz_cover(classify3, -50, 50, 2000)
  check("fuzz.coverage_beats_fixed", guided > fixed)
  local _, weak_s = e:mutation_test(classify3, { 5 })
  local _, strong_s = e:mutation_test(classify3, { -7, 0, 9 })
  check("mutation.strong_kills_more", weak_s > 0 and strong_s < weak_s)
end

-- Track D: AVX2 256-bit capture (self-skips off-AVX2).
if asmtest.cpu_has_avx2() then
  local lanes = asmtest.capture_vec256(routine("vec_add4d"), { { 1, 2, 3, 4 }, { 10, 20, 30, 40 } })
  check("vec256.add4d", lanes[1] == 11 and lanes[2] == 22 and lanes[3] == 33 and lanes[4] == 44)
else
  print("ok - vec256.add4d # SKIP no AVX2")
end

-- Track D: AVX-512 512-bit capture (self-skips off-AVX-512F).
if asmtest.cpu_has_avx512f() then
  local lanes = asmtest.capture_vec512(routine("vec_add8d"),
    { { 1, 2, 3, 4, 5, 6, 7, 8 }, { 10, 20, 30, 40, 50, 60, 70, 80 } })
  check("vec512.add8d", lanes[1] == 11 and lanes[2] == 22 and lanes[3] == 33 and lanes[4] == 44
    and lanes[5] == 55 and lanes[6] == 66 and lanes[7] == 77 and lanes[8] == 88)
else
  print("ok - vec512.add8d # SKIP no AVX512F")
end

-- --- Tier: call descent (asmtest_ptrace.h) — fork + single-step host-native code,
-- replaying the recorded frame-0 body, edges (L1), and descended frames (L2). This is
-- the ptrace/native tier (libasmtest_hwtrace, a separate lib from the emulator above),
-- so it replays only when the corpus `arch` matches the host AND the out-of-process
-- stepper (PTRACE_SINGLESTEP + Capstone) is available; otherwise it SKIPs (never
-- fails), mirroring the C reference / Python `_run_ptrace_descent`.
do
  local hwtrace = require("hwtrace")
  local HwTrace = hwtrace.HwTrace
  local Descent = hwtrace.Descent
  local NativeCode = hwtrace.NativeCode
  local ffi = require("ffi")

  -- The two ptrace_descent cases from bindings/conformance/corpus.json, inline (the
  -- Lua runner replays through the binding API rather than parsing the JSON corpus).
  -- R@0: mov rax,rdi; call S(+4); add rax,rsi; ret.  S@0xc: inc rax; ret.
  local DESCENT_CODE = { 0x48, 0x89, 0xF8, 0xE8, 0x04, 0x00, 0x00, 0x00,
                         0x48, 0x01, 0xF0, 0xC3, 0x48, 0xFF, 0xC0, 0xC3 }
  local cases = {
    { name = "ptrace_descent.calls_leaf.edges", arch = "x86_64", level = 1,
      code = DESCENT_CODE, region = 12, args = { 20, 22 },
      expect = { result = 43, frame0 = { 0, 3, 8, 11 },
                 edges = { { site = 3, target_off = 12 } }, frames = {} } },
    { name = "ptrace_descent.calls_leaf.descend", arch = "x86_64", level = 2,
      code = DESCENT_CODE, region = 12, allow_off = 12, allow_len = 4, args = { 20, 22 },
      expect = { result = 43, frame0 = { 0, 3, 8, 11 }, edges = {},
                 frames = { { base_off = 12, depth = 1, insns = { 0, 3 } } } } },
  }

  -- LuaJIT reports the host arch as 'x64' on x86-64 / 'arm64' on aarch64.
  local host = (ffi.arch == "x64") and "x86_64"
            or (ffi.arch == "arm64") and "aarch64" or ffi.arch

  -- cdata-uint64 list compare: frame_insns are boxed uint64_t (never tonumber()'d);
  -- LuaJIT compares a uint64_t cdata to a Lua number numerically.
  local function u64_list_eq(got, want)
    if #got ~= #want then return false end
    for i = 1, #want do if got[i] ~= want[i] then return false end end
    return true
  end

  local function replay(case)
    local code = NativeCode.from_bytes(case.code)
    local d = Descent.new(case.level)
    if case.level >= 2 and case.allow_off then
      d:allow_region(ffi.cast("uint64_t", code.base) + case.allow_off, case.allow_len)
    end
    local tr = HwTrace.create(64, 64)
    local ran, result = pcall(HwTrace.ptrace_trace_call_ex, code.base, code.len,
                              case.args, tr, d, case.region)
    local pass = ran
    if ran then
      local exp = case.expect
      if result ~= exp.result then pass = false end
      if not u64_list_eq(d:frame_insns(0), exp.frame0) then pass = false end
      local edges = d:edges()
      if #edges ~= #exp.edges then
        pass = false
      else
        for i = 1, #exp.edges do
          local w = exp.edges[i]
          if tonumber(edges[i].site) ~= w.site then pass = false end
          if edges[i].target ~= ffi.cast("uint64_t", code.base) + w.target_off then
            pass = false
          end
        end
      end
      -- Descended frames are 1..N; frame 0 is the root region.
      for _, fs in ipairs(exp.frames) do
        local want_base = ffi.cast("uint64_t", code.base) + fs.base_off
        local idx
        for i = 1, d:frames_len() - 1 do
          if d:frame_base(i) == want_base then idx = i; break end
        end
        if not idx then
          pass = false
        else
          if d:frame_depth(idx) ~= fs.depth then pass = false end
          if not u64_list_eq(d:frame_insns(idx), fs.insns) then pass = false end
        end
      end
    end
    d:free(); tr:free(); code:free()
    return pass
  end

  for _, case in ipairs(cases) do
    if not HwTrace.ptrace_available() then
      print("ok - " .. case.name .. " # SKIP no ptrace single-step: "
            .. HwTrace.ptrace_skip_reason())
    elseif case.arch ~= host then
      print("ok - " .. case.name .. " # SKIP ptrace_descent corpus arch "
            .. case.arch .. " != host " .. tostring(host))
    else
      check(case.name, replay(case))
    end
  end
end

print(string.format("# %d passed, %d failed, %d total", total - fails, fails, total))
os.exit(fails == 0 and 0 or 1)
