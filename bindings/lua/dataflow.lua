-- asmtest/dataflow (Lua, LuaJIT FFI) — binding for the data-flow tier (Phase 6 + F7).
--
-- Wraps libasmtest_dataflow (built with `make shared-dataflow`), mirroring the
-- Python/C++/Node/Ruby bindings: the pure GC-move canonicalizer, the tiered-re-JIT
-- method resolver, and — F7 — LIVE-ATTACH capture over a running pid (ValueTrace
-- attach_pid / attach_pid_tid / attach_jit). LuaJIT FFI (like ctypes) gives
-- high-level struct arrays.
local ffi = require("ffi")

ffi.cdef([[
typedef struct { uint64_t old_base; uint64_t new_base; uint64_t len; uint32_t step; } asmtest_gcmove_t;
typedef struct { uint64_t addr; uint64_t size; const char *name; uint64_t version; } asmtest_method_t;
uint64_t asmtest_gcmove_canon(const asmtest_gcmove_t *moves, size_t nmoves, uint32_t step, uint64_t phys);
int asmtest_method_resolve_pc(const asmtest_method_t *methods, size_t nmethods, uint64_t pc);

/* The L0 sink handle, opaque here — the bindings pass it around, never inspect it. */
typedef struct asmtest_valtrace asmtest_valtrace_t;
asmtest_valtrace_t *asmtest_valtrace_new(size_t steps_cap, size_t recs_cap, size_t wide_cap);
void asmtest_valtrace_free(asmtest_valtrace_t *v);
size_t asmtest_valtrace_steps(const asmtest_valtrace_t *v);
size_t asmtest_valtrace_recs(const asmtest_valtrace_t *v);

/* F7 — the LIVE-ATTACH producer entry points (src/dataflow_ptrace.c). The producer
 * ships NO header on purpose (a value-trace PRODUCER is a tier, not part of the
 * shared sink API), so — exactly as its own C suite does — this binding re-declares
 * them. Keep in step with that file. No struct crosses by value. */
typedef struct asmtest_codeimage asmtest_codeimage_t;
int asmtest_dataflow_ptrace_attach_pid(int pid, uint64_t base, size_t code_len,
                                       uint64_t max_insns, long *result,
                                       asmtest_valtrace_t *vt);
int asmtest_dataflow_ptrace_attach_pid_tid(int pid, int only_tid, uint64_t base,
                                           size_t code_len, uint64_t max_insns,
                                           long *result, asmtest_valtrace_t *vt);
int asmtest_dataflow_ptrace_attach_jit(int pid, int only_tid, uint64_t base,
                                       size_t code_len, asmtest_codeimage_t *img,
                                       uint64_t when, uint64_t max_insns, long *result,
                                       int *survived, asmtest_valtrace_t *vt);
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

-- --- F7: live-attach data-flow capture ------------------------------------
-- The scoped ptrace producer's return codes (src/dataflow_ptrace.c), re-declared
-- for the same reason the prototypes above are.
M.PTRACE_OK = 0      -- a complete scoped trace
M.PTRACE_FAULT = 1   -- the routine faulted; a partial trace is filled
M.PTRACE_EINVAL = -1 -- bad arguments
M.PTRACE_ENOSYS = -3 -- off Linux x86-64 / no Capstone: the tier is absent
M.PTRACE_ETRACE = -4 -- ptrace/wait failure (seccomp/yama)

-- True iff this build's live-attach tier is real (Linux x86-64 + Capstone) rather
-- than the off-platform ENOSYS stub. PROBED, not guessed: an argument-rejecting
-- call returns EINVAL from the real producer and ENOSYS from the stub, which is the
-- only way to tell them apart — the symbol resolves either way. Attaches to nothing.
function M.live_attach_available()
  local v = C.asmtest_valtrace_new(1, 1, 0)
  local out = ffi.new("long[1]")
  local rc = C.asmtest_dataflow_ptrace_attach_pid(0, 0, 0, 0, out, v)
  C.asmtest_valtrace_free(v)
  return rc ~= M.PTRACE_ENOSYS
end

-- An L0 value trace handle (opaque) filled by the LIVE-ATTACH producer — the same
-- asmtest_valtrace_t the pure analysis layers consume. This binding exposes the
-- attach + the step/record counts; the def-use + slice surface is the
-- Python/C++/Node ValueTrace's and is not wrapped here yet.
local ValueTrace = {}
ValueTrace.__index = ValueTrace

function M.ValueTrace(steps_cap, recs_cap)
  local v = C.asmtest_valtrace_new(steps_cap or 256, recs_cap or 2048, 0)
  assert(v ~= nil, "asmtest_valtrace_new failed")
  return setmetatable({ _v = v }, ValueTrace)
end

-- Attach to LIVE `pid`, single-step [base, base+code_len), then DETACH leaving the
-- target running. Steps the thread-group LEADER. Returns rc, result.
function ValueTrace:attach_pid(pid, base, code_len, max_insns)
  local out = ffi.new("long[1]")
  local rc = C.asmtest_dataflow_ptrace_attach_pid(pid, base, code_len, max_insns or 0,
                                                  out, self._v)
  return tonumber(rc), tonumber(out[0])
end

-- As attach_pid, but SEIZE every thread and step whichever one first ENTERS the
-- region (by its own tid, never assumed to be the leader); only_tid 0 = any. The
-- entry managed methods need — they run on workers. Returns rc, result.
function ValueTrace:attach_pid_tid(pid, only_tid, base, code_len, max_insns)
  local out = ffi.new("long[1]")
  local rc = C.asmtest_dataflow_ptrace_attach_pid_tid(pid, only_tid, base, code_len,
                                                      max_insns or 0, out, self._v)
  return tonumber(rc), tonumber(out[0])
end

-- The JIT-aware live attach: worker-targeting plus an explicit survival report.
-- Returns rc, result, survived (1 = the detach left the target alive). The
-- versioned-decode code-image (img/when) is NULL — operands decode from a live snapshot.
function ValueTrace:attach_jit(pid, only_tid, base, code_len, max_insns, when)
  local out = ffi.new("long[1]")
  local survived = ffi.new("int[1]")
  local rc = C.asmtest_dataflow_ptrace_attach_jit(pid, only_tid, base, code_len, nil,
                                                  when or 0, max_insns or 0, out,
                                                  survived, self._v)
  return tonumber(rc), tonumber(out[0]), tonumber(survived[0])
end

function ValueTrace:steps() return tonumber(C.asmtest_valtrace_steps(self._v)) end
function ValueTrace:recs() return tonumber(C.asmtest_valtrace_recs(self._v)) end

function ValueTrace:free()
  if self._v ~= nil then
    C.asmtest_valtrace_free(self._v)
    self._v = nil
  end
end

return M
