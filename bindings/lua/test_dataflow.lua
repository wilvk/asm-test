-- Lua data-flow binding smoke (Phase 6): GC-move canonicalizer + method resolver,
-- mirroring the Python/C++/Node/Ruby suites. Run under LuaJIT.
local df = require("dataflow")

local n = 0
local failed = false
local function check(cond, desc)
  n = n + 1
  print((cond and "ok " or "not ok ") .. n .. " - " .. desc)
  if not cond then failed = true end
end

-- GC-move canonicalizer
check(df.gcmove_canon({}, 0, 0x1234) == 0x1234, "gcmove: empty move set is identity")
local mv = { { 0x1000, 0x2000, 0x100, 5 } }
check(df.gcmove_canon(mv, 3, 0x1010) == 0x2010, "gcmove: pre-move addr forwards to final")
check(df.gcmove_canon(mv, 3, 0x1000) == 0x2000, "gcmove: object base forwards")
check(df.gcmove_canon(mv, 3, 0x10FF) == 0x20FF, "gcmove: last byte of half-open window forwards")
check(df.gcmove_canon(mv, 3, 0x1100) == 0x1100, "gcmove: one past the window not forwarded")
check(df.gcmove_canon(mv, 5, 0x1010) == 0x1010, "gcmove: at-move-step observation not forwarded")
check(df.gcmove_canon(mv, 3, 0x3000) == 0x3000, "gcmove: out-of-range addr unchanged")
local mv2 = { { 0x1000, 0x2000, 0x100, 3 }, { 0x2000, 0x3000, 0x100, 6 } }
check(df.gcmove_canon(mv2, 1, 0x1010) == 0x3010, "gcmove: two compactions compose to final")

-- method resolver
local ms = { { 0x1000, 0x40, "Foo", 3 }, { 0x2000, 0x20, "Bar", 1 }, { 0x3000, 0, "Baz", 2 } }
check(df.method_resolve_pc(ms, 0x1000) == 0, "method: Foo range start")
check(df.method_resolve_pc(ms, 0x103F) == 0, "method: Foo last byte (half-open)")
check(df.method_resolve_pc(ms, 0x1040) == -1, "method: one past Foo -> none")
check(df.method_resolve_pc(ms, 0x2010) == 1, "method: Bar range")
check(df.method_resolve_pc(ms, 0x3000) == 2, "method: Baz point match")
check(df.method_resolve_pc(ms, 0x3001) == -1, "method: Baz is point-only")
local rj = { { 0x1000, 0x40, "Foo", 1 }, { 0x1000, 0x40, "Foo", 5 } }
check(df.method_resolve_pc(rj, 0x1010) == 1, "method: tiered re-JIT newest version wins")
check(df.method_resolve_pc({}, 0x1000) == -1, "method: empty map -> -1")

print("1.." .. n)
os.exit(failed and 1 or 0)
