-- test_hwtrace.lua — standalone test for the LuaJIT single-step hardware-trace
-- wrapper (hwtrace.lua), mirroring bindings/python/tests/test_hwtrace.py.
--
-- Run from this directory (the module resolves the lib relative to itself):
--   ASMTEST_HWTRACE_LIB=../../build/libasmtest_hwtrace.so luajit test_hwtrace.lua
--
-- Unlike the DynamoRIO wrapper (which needs a DynamoRIO install) and the PT/AMD
-- backends (which need specific bare-metal hardware), the SINGLESTEP backend runs
-- on ANY x86-64 Linux — so this asserts a real, live trace here and in
-- CI/containers, self-skipping (prints "# SKIP ..." and exits 0) only off x86-64
-- Linux, without Capstone, or when libasmtest_hwtrace is absent.
package.path = (debug.getinfo(1, "S").source:sub(2):match("(.*/)") or "./")
  .. "?.lua;" .. package.path
local hwtrace = require("hwtrace")
local HwTrace = hwtrace.HwTrace
local NativeCode = hwtrace.NativeCode
local SINGLESTEP = hwtrace.SINGLESTEP
local AMD_LBR = hwtrace.AMD_LBR
local BEST = hwtrace.BEST
local CEILING_FREE = hwtrace.CEILING_FREE
local ASMTEST_HW_EUNAVAIL = hwtrace.ASMTEST_HW_EUNAVAIL
local TIER_HWTRACE = hwtrace.TIER_HWTRACE
local TIER_EMULATOR = hwtrace.TIER_EMULATOR
local FIDELITY_NATIVE = hwtrace.FIDELITY_NATIVE
local FIDELITY_VIRTUAL = hwtrace.FIDELITY_VIRTUAL
local TRACE_BEST = hwtrace.TRACE_BEST
local TRACE_CEILING_FREE = hwtrace.TRACE_CEILING_FREE
local TRACE_NATIVE_ONLY = hwtrace.TRACE_NATIVE_ONLY

-- mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
local ROUTINE = { 0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D,
                  0x64, 0x00, 0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF,
                  0xC8, 0xC3 }

-- mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  (19 back-edges > LBR's 16 deep)
local LOOP = { 0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00,
               0x48, 0x01, 0xF8, 0x48, 0xFF, 0xCE, 0x75, 0xF8, 0xC3 }

-- ---- TAP-ish harness: "ok N - ..." / "not ok N - ..."; nonzero exit on failure ----
local count = 0
local failed = 0
local function ok(cond, desc)
  count = count + 1
  if cond then
    print(string.format("ok %d - %s", count, desc))
  else
    failed = failed + 1
    print(string.format("not ok %d - %s", count, desc))
  end
end
local function eq(got, want, desc)
  ok(got == want, string.format("%s (got %s, want %s)",
                                desc, tostring(got), tostring(want)))
end
local function list_eq(got, want, desc)
  if #got ~= #want then
    ok(false, string.format("%s (len %d != %d)", desc, #got, #want))
    return
  end
  for i = 1, #want do
    if got[i] ~= want[i] then
      ok(false, string.format("%s ([%d]: got 0x%x, want 0x%x)",
                              desc, i, got[i], want[i]))
      return
    end
  end
  ok(true, desc)
end

-- Self-skip cleanly when the single-step backend can't run, mirroring the pytest
-- skip pattern.
if not HwTrace.available(SINGLESTEP) then
  print("# SKIP single-step hardware-trace tier unavailable: "
        .. HwTrace.skip_reason(SINGLESTEP))
  os.exit(0)
end

-- test_auto_resolve_selection_invariants — the orchestrator's selection invariants
-- hold on every host (here, every x86-64 Linux host past the skip guard); they need
-- no initialized tier, so assert them before the global single-step lifecycle.
do
  local best = HwTrace.resolve(BEST)
  local cf = HwTrace.resolve(CEILING_FREE)

  -- Every resolved backend is actually available, in descending-fidelity
  -- (ascending-enum) order, with no duplicates.
  local avail, order = true, true
  for i = 1, #best do
    if not HwTrace.available(best[i]) then avail = false end
    if i > 1 and best[i] <= best[i - 1] then order = false end
  end
  ok(avail, "auto BEST returns only available backends")
  ok(order, "auto BEST is ordered by descending fidelity, no dups")

  -- CEILING_FREE drops the one fixed-window backend (AMD LBR) and is otherwise a
  -- subset of BEST.
  local no_amd, subset = true, true
  for i = 1, #cf do
    if cf[i] == AMD_LBR then no_amd = false end
    local in_best = false
    for j = 1, #best do
      if best[j] == cf[i] then in_best = true end
    end
    if not in_best then subset = false end
  end
  ok(no_amd, "auto CEILING_FREE never selects AMD LBR (16-branch window)")
  ok(subset, "auto CEILING_FREE is a subset of BEST")

  -- auto(policy) is the head of resolve(policy), or EUNAVAIL when empty.
  local ab = HwTrace.auto(BEST)
  eq(ab, #best > 0 and best[1] or ASMTEST_HW_EUNAVAIL,
     "auto(BEST) is the head of the resolved cascade")
end

-- test_cross_tier_resolve_invariants — the cross-tier orchestrator (resolve over
-- hwtrace + DynamoRIO + emulator) holds its structural invariants on every host.
-- Needs no initialized tier, like the hardware-backend invariants above.
do
  local best = HwTrace.resolve_tiers(TRACE_BEST)
  local nat = HwTrace.resolve_tiers(TRACE_NATIVE_ONLY)
  local cf = HwTrace.resolve_tiers(TRACE_CEILING_FREE)

  -- Every HW choice satisfies the hardware-tier probe; every choice's fidelity
  -- matches its tier (VIRTUAL for the emulator, NATIVE otherwise).
  local hw_avail, fidelity_ok = true, true
  for i = 1, #best do
    local c = best[i]
    if c.tier == TIER_HWTRACE and not HwTrace.available(c.backend) then
      hw_avail = false
    end
    local want = (c.tier == TIER_EMULATOR) and FIDELITY_VIRTUAL or FIDELITY_NATIVE
    if c.fidelity ~= want then fidelity_ok = false end
  end
  ok(hw_avail, "cross-tier BEST: every HW choice is available")
  ok(fidelity_ok, "cross-tier BEST: fidelity matches tier")

  -- The single VIRTUAL emulator floor is the last entry under BEST, exactly once.
  local emu_count = 0
  for i = 1, #best do
    if best[i].tier == TIER_EMULATOR then emu_count = emu_count + 1 end
  end
  ok(#best > 0 and best[#best].tier == TIER_EMULATOR,
     "cross-tier BEST: emulator floor is the last entry")
  eq(emu_count, 1, "cross-tier BEST: exactly one emulator floor")

  -- NATIVE_ONLY forbids the native->emulator crossing: it is BEST minus the floor.
  local no_emu = true
  for i = 1, #nat do
    if nat[i].tier == TIER_EMULATOR then no_emu = false end
  end
  ok(no_emu, "cross-tier NATIVE_ONLY: no emulator tier")
  eq(#nat, #best - 1, "cross-tier NATIVE_ONLY: BEST minus the floor")

  -- CEILING_FREE drops AMD LBR.
  local no_amd = true
  for i = 1, #cf do
    if cf[i].tier == TIER_HWTRACE and cf[i].backend == AMD_LBR then
      no_amd = false
    end
  end
  ok(no_amd, "cross-tier CEILING_FREE: never selects AMD LBR")

  -- auto(policy) is the head of resolve(policy).
  local one = HwTrace.auto_tier(TRACE_BEST)
  ok(one ~= nil, "cross-tier auto(BEST) is non-nil")
  if one then
    ok(one.tier == best[1].tier and one.backend == best[1].backend,
       "cross-tier auto(BEST) is the head of the resolved cascade")
  end
end

-- test_cross_tier_native_only_resolves_on_linux_x86_64 — on any x86-64 Linux host
-- the single-step backend is a native floor, so even NATIVE_ONLY resolves (the
-- cascade never collapses to nothing here). Past the SINGLESTEP skip guard above.
do
  local nat = HwTrace.resolve_tiers(TRACE_NATIVE_ONLY)
  local pick = HwTrace.auto_tier(TRACE_NATIVE_ONLY)
  ok(#nat > 0 and pick ~= nil and pick.fidelity == FIDELITY_NATIVE,
     "cross-tier NATIVE_ONLY resolves a native pick on x86-64 Linux")
  local has_singlestep = false
  for i = 1, #nat do
    if nat[i].tier == TIER_HWTRACE and nat[i].backend == SINGLESTEP then
      has_singlestep = true
    end
  end
  ok(has_singlestep, "cross-tier NATIVE_ONLY: single-step is in the cascade")
end

local init_ok, err = pcall(HwTrace.init, SINGLESTEP)
if not init_ok then
  print("# SKIP hwtrace init failed: " .. tostring(err))
  os.exit(0)
end

-- test_singlestep_live_trace — ROUTINE, call(20, 22): 42 <= 100 -> jle taken,
-- dec skipped. Byte-for-byte the Unicorn/DynamoRIO/PT/AMD result for this fixture.
do
  local code = NativeCode.from_bytes(ROUTINE)
  local tr = HwTrace.create(64, 64)  -- blocks + ordered instruction stream
  tr:register("add2", code)

  local result
  tr:region("add2", function() result = code:call(20, 22) end)

  eq(result, 42, "ROUTINE: call(20,22) == 42")
  list_eq(tr:insn_offsets(), { 0x0, 0x3, 0x6, 0xC, 0x11 },
          "ROUTINE: insn_offsets == {0,3,6,12,17}")
  eq(tr:insns_total(), 5, "ROUTINE: insns_total == 5")
  ok(tr:covered(0) and tr:covered(0x11), "ROUTINE: covered(0) and covered(17)")
  eq(tr:blocks_len(), 2, "ROUTINE: blocks_len == 2")
  ok(not tr:truncated(), "ROUTINE: not truncated")

  tr:free()
  code:free()
end

-- test_singlestep_loop_no_depth_ceiling — LOOP, call(1, 20): 19 back-edges, all
-- captured (no LBR-style 16-deep ceiling). Use instructions=256 for the loop.
do
  local code = NativeCode.from_bytes(LOOP)
  local tr = HwTrace.create(64, 256)
  tr:register("loop", code)

  local result
  tr:region("loop", function() result = code:call(1, 20) end)

  eq(result, 20, "LOOP: call(1,20) == 20")
  eq(tr:insns_total(), 62, "LOOP: insns_total == 62")  -- 1 + 20*3 + 1
  ok(tr:covered(0) and tr:covered(0x7), "LOOP: covered(0) and covered(7)")
  eq(tr:blocks_len(), 2, "LOOP: blocks_len == 2")
  ok(not tr:truncated(), "LOOP: not truncated")

  tr:free()
  code:free()
end

HwTrace.shutdown()

-- test_auto_resolve_traces_live — on any x86-64 Linux host the cascade is non-empty
-- (single-step floor), so auto() resolves a usable backend; trace the shared ROUTINE
-- fixture through whatever it picked. Owns its own init/shutdown (one tier at a time)
-- now that the global single-step lifecycle above is done.
do
  local best = HwTrace.resolve(BEST)
  local ab = HwTrace.auto(BEST)
  ok(#best > 0 and ab >= 0, "auto resolves a backend (single-step floor)")

  HwTrace.init(ab)
  local code = NativeCode.from_bytes(ROUTINE)
  local tr = HwTrace.create(64, 64)
  tr:register("auto", code)

  local result
  tr:region("auto", function() result = code:call(20, 22) end)

  eq(result, 42, "auto: call(20,22) == 42")
  ok(tr:covered(0), "auto: covered(0)")
  if ab == SINGLESTEP then  -- the pick off PT/AMD hosts: byte-exact parity
    list_eq(tr:insn_offsets(), { 0x0, 0x3, 0x6, 0xC, 0x11 },
            "auto (single-step): insn_offsets == {0,3,6,12,17}")
  end

  tr:free()
  code:free()
  HwTrace.shutdown()
end

print(string.format("# %d tests, %d failed", count, failed))
os.exit(failed == 0 and 0 or 1)
