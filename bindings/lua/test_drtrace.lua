-- test_drtrace.lua — standalone test for the LuaJIT DynamoRIO native-trace
-- wrapper (drtrace.lua), mirroring bindings/python/tests/test_drtrace.py.
--
-- Run from this directory (the module resolves the lib relative to itself):
--   ASMTEST_DRAPP_LIB=../../build/libasmtest_drapp.so luajit test_drtrace.lua
--
-- Self-skips (prints "SKIP: ..." and exits 0) unless the tier is built AND
-- DynamoRIO is resolvable — i.e. unless ASMTEST_DRAPP_LIB / ASMTEST_DRCLIENT
-- (and ASMTEST_DR_LIB or DYNAMORIO_HOME) point at a built libasmtest_drapp + a
-- client on a DynamoRIO-capable Linux x86-64 host. The `make docker-drtrace`
-- lane sets these up in a container.
package.path = (debug.getinfo(1, "S").source:sub(2):match("(.*/)") or "./")
  .. "?.lua;" .. package.path
local drtrace = require("drtrace")
local NativeTrace = drtrace.NativeTrace
local NativeCode = drtrace.NativeCode

-- mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
local ROUTINE = string.char(0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D,
                            0x64, 0x00, 0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF,
                            0xC8, 0xC3)

-- Self-skip cleanly when the tier can't run, mirroring the pytest importorskip /
-- _skip_if_unavailable() pattern.
if not NativeTrace.available() then
  print("SKIP: DynamoRIO native-trace tier unavailable (self-skip)")
  os.exit(0)
end
if not (os.getenv("ASMTEST_DRCLIENT") and os.getenv("ASMTEST_DRCLIENT") ~= "") then
  print("SKIP: ASMTEST_DRCLIENT not set (build the DR client)")
  os.exit(0)
end

local ok, err = pcall(NativeTrace.initialize)
if not ok then
  print("SKIP: dr_init/start failed: " .. tostring(err))
  os.exit(0)
end

-- test_block_coverage_and_accumulation
local code = NativeCode.from_bytes(ROUTINE)
local tr = NativeTrace.new(64)  -- blocks only (instructions defaults to 0)
tr:register("add2", code)

local r
tr:region("add2", function() r = code:call(20, 22) end)
assert(r == 42, "expected 42, got " .. tostring(r))
assert(tr:covered(0), "entry block should be covered")

local before = tr:blocks_len()
local r2
tr:region("add2", function() r2 = code:call(60, 60) end)  -- 120>100 -> dec -> 119
assert(r2 == 119, "expected 119, got " .. tostring(r2))
assert(tr:blocks_len() >= before, "blocks_len should not decrease")
assert(NativeTrace.marker_error() == 0, "markers should be balanced")

tr:unregister("add2")
code:free()
tr:free()

-- test_instruction_mode
local code2 = NativeCode.from_bytes(ROUTINE)
local tr2 = NativeTrace.new(64, 64)  -- blocks + ordered instruction stream
tr2:register("add2i", code2)
local r3
tr2:region("add2i", function() r3 = code2:call(1, 2) end)
assert(r3 == 3, "expected 3, got " .. tostring(r3))
assert(tr2:insns_total() >= 4, "expected >=4 instructions recorded")

-- The ordered instruction-offset stream is the exact, first-block instruction
-- sequence of ROUTINE: mov(0x0), add(0x3), cmp(0x6), jle(0xc), ret(0x11).
local insns = tr2:insn_offsets()
local expected = { 0x0, 0x3, 0x6, 0xc, 0x11 }
assert(#insns == #expected,
       "expected " .. #expected .. " insn offsets, got " .. #insns)
for i = 1, #expected do
  assert(insns[i] == expected[i],
         string.format("insn_offsets[%d]: expected 0x%x, got 0x%x",
                       i, expected[i], insns[i]))
end
-- The distinct basic-block set includes the region entry (offset 0).
local blocks = tr2:block_offsets()
local has_zero = false
for _, off in ipairs(blocks) do
  if off == 0 then has_zero = true break end
end
assert(has_zero, "block_offsets should contain 0 (entry block)")

tr2:unregister("add2i")
code2:free()
tr2:free()

NativeTrace.shutdown()
print("PASS")
