-- asmtest/dataflow (Lua, LuaJIT FFI) — binding for the data-flow analysis tier (Phase 6).
--
-- Wraps libasmtest_dataflow (built with `make shared-dataflow`): this increment mirrors
-- the Python/C++/Node/Ruby bindings' pure helpers — the GC-move canonicalizer and the
-- tiered-re-JIT method resolver. LuaJIT FFI (like ctypes) gives high-level struct arrays.
local ffi = require("ffi")

ffi.cdef([[
typedef struct { uint64_t old_base; uint64_t new_base; uint64_t len; uint32_t step; } asmtest_gcmove_t;
typedef struct { uint64_t addr; uint64_t size; const char *name; uint64_t version; } asmtest_method_t;
uint64_t asmtest_gcmove_canon(const asmtest_gcmove_t *moves, size_t nmoves, uint32_t step, uint64_t phys);
int asmtest_method_resolve_pc(const asmtest_method_t *methods, size_t nmethods, uint64_t pc);
]])

local function lib_path()
  local env = os.getenv("ASMTEST_DATAFLOW_LIB")
  if env then return env end
  local dir = (debug.getinfo(1, "S").source:match("@?(.*/)")) or "./"
  return dir .. "../../build/libasmtest_dataflow.so"
end

local C = ffi.load(lib_path())

local M = {}

-- Map heap address `phys` observed at value-trace `step` to its canonical
-- (final-resting) address. `moves` is an array of {old_base, new_base, len, step}
-- (1-indexed tuples), sorted ascending by step. Returns a Lua number.
function M.gcmove_canon(moves, step, phys)
  local n = #moves
  local arr = n > 0 and ffi.new("asmtest_gcmove_t[?]", n) or nil
  for i = 1, n do
    local m = moves[i]
    arr[i - 1].old_base = m[1]
    arr[i - 1].new_base = m[2]
    arr[i - 1].len = m[3]
    arr[i - 1].step = m[4]
  end
  return tonumber(C.asmtest_gcmove_canon(arr, n, step, phys))
end

-- Resolve `pc` to the owning method-map record index, or -1. Each method is a
-- {addr, size, name, version} tuple (size 0 = point match on addr). The name
-- strings are kept alive in `keep` across the call.
function M.method_resolve_pc(methods, pc)
  local n = #methods
  local arr = n > 0 and ffi.new("asmtest_method_t[?]", n) or nil
  local keep = {}
  for i = 1, n do
    local m = methods[i]
    local cstr = ffi.new("char[?]", #tostring(m[3]) + 1, tostring(m[3]))
    keep[i] = cstr -- prevent GC of the string buffer during the call
    arr[i - 1].addr = m[1]
    arr[i - 1].size = m[2]
    arr[i - 1].name = cstr
    arr[i - 1].version = m[4]
  end
  local r = C.asmtest_method_resolve_pc(arr, n, pc)
  return tonumber(r), keep
end

return M
