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

/* One operand read/write record (include/asmtest_valtrace.h). LuaJIT ffi gives a
 * typed struct matching the C ABI exactly (verified via offsetof on this build:
 * kind 0, reg/base/index 4/8/12, scale 16, disp 24, addr 32, size 40,
 * is_write/value_valid/wide 42/43/44, wide_off 48, value 56, step 64 — 72 bytes). */
typedef struct at_val_rec {
  int32_t kind; /* at_loc_kind_t */
  uint32_t reg;
  uint32_t base;
  uint32_t index;
  int32_t scale;
  int64_t disp;
  uint64_t addr;
  uint16_t size;
  bool is_write;
  bool value_valid;
  bool wide;
  uint32_t wide_off;
  uint64_t value;
  uint32_t step;
} at_val_rec_t;

/* Append one executed step at instruction offset `off`, copying its `n` operand
 * records and stamping each with the new step index. */
void asmtest_valtrace_append(asmtest_valtrace_t *v, uint64_t off,
                             const at_val_rec_t *recs, size_t n);

/* L1 def-use graph + L2 slice (analysis pipeline, src/dataflow.c). The seed
 * crosses BY POINTER (asmtest_slice_forward_seed/_backward_seed); a by-value
 * at_val_rec_t call is NYI on LuaJIT's FFI (aggregates by value are never JIT-
 * compiled), so the by-pointer seed keeps this call on the compiled path. */
typedef struct asmtest_defuse asmtest_defuse_t;
typedef struct asmtest_slice asmtest_slice_t;
asmtest_defuse_t *asmtest_defuse_build(const asmtest_valtrace_t *v);
void asmtest_defuse_free(asmtest_defuse_t *g);
asmtest_slice_t *asmtest_slice_forward_seed(const asmtest_defuse_t *g,
                                            const at_val_rec_t *seed);
asmtest_slice_t *asmtest_slice_backward_seed(const asmtest_defuse_t *g,
                                             const at_val_rec_t *seed);
void asmtest_slice_free(asmtest_slice_t *s);
int asmtest_slice_contains(const asmtest_slice_t *s, uint32_t step);

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

-- at_loc_kind_t (include/asmtest_valtrace.h): the location space of an operand.
M.LOC_REG = 0     -- a register (key = Capstone reg id)
M.LOC_MEM_ABS = 1 -- memory at an absolute effective address (key = addr)
M.LOC_MEM_OFF = 2 -- memory at a routine offset

-- An L0 value trace handle (opaque) filled by the LIVE-ATTACH producer, or built
-- by hand via :step(). The same asmtest_valtrace_t the pure analysis layers
-- consume, so either path feeds the same L1 def-use graph (:defuse()) and L2
-- slicer (:forward_slice() / :backward_slice()).
local ValueTrace = {}
ValueTrace.__index = ValueTrace

function M.ValueTrace(steps_cap, recs_cap)
  local v = C.asmtest_valtrace_new(steps_cap or 256, recs_cap or 2048, 0)
  assert(v ~= nil, "asmtest_valtrace_new failed")
  return setmetatable({ _v = v, _g = nil, _n_steps = 0 }, ValueTrace)
end

-- Append one executed instruction at offset `off` reading `reads` and writing
-- `writes` (each an array of {kind, key} locations — kind is LOC_REG (key = reg
-- id) or LOC_MEM_ABS (key = address)). Read-set before write-set.
function ValueTrace:step(off, reads, writes)
  reads = reads or {}
  writes = writes or {}
  local n = #reads + #writes
  local arr = n > 0 and ffi.new("at_val_rec_t[?]", n) or nil
  local i = 0
  for _, loc in ipairs(reads) do
    arr[i].kind = loc[1]
    if loc[1] == M.LOC_REG then arr[i].reg = loc[2] else arr[i].addr = loc[2] end
    arr[i].is_write = false
    i = i + 1
  end
  for _, loc in ipairs(writes) do
    arr[i].kind = loc[1]
    if loc[1] == M.LOC_REG then arr[i].reg = loc[2] else arr[i].addr = loc[2] end
    arr[i].is_write = true
    i = i + 1
  end
  C.asmtest_valtrace_append(self._v, off, arr, n)
  self._n_steps = self._n_steps + 1
  self:_invalidate_defuse()
  return self
end

-- Attach to LIVE `pid`, single-step [base, base+code_len), then DETACH leaving the
-- target running. Steps the thread-group LEADER. Returns rc, result.
function ValueTrace:attach_pid(pid, base, code_len, max_insns)
  local out = ffi.new("long[1]")
  local rc = C.asmtest_dataflow_ptrace_attach_pid(pid, base, code_len, max_insns or 0,
                                                  out, self._v)
  self:_post_attach()
  return tonumber(rc), tonumber(out[0])
end

-- As attach_pid, but SEIZE every thread and step whichever one first ENTERS the
-- region (by its own tid, never assumed to be the leader); only_tid 0 = any. The
-- entry managed methods need — they run on workers. Returns rc, result.
function ValueTrace:attach_pid_tid(pid, only_tid, base, code_len, max_insns)
  local out = ffi.new("long[1]")
  local rc = C.asmtest_dataflow_ptrace_attach_pid_tid(pid, only_tid, base, code_len,
                                                      max_insns or 0, out, self._v)
  self:_post_attach()
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
  self:_post_attach()
  return tonumber(rc), tonumber(out[0]), tonumber(survived[0])
end

function ValueTrace:steps() return tonumber(C.asmtest_valtrace_steps(self._v)) end
function ValueTrace:recs() return tonumber(C.asmtest_valtrace_recs(self._v)) end

-- The L1 last-writer def-use graph over this trace, built once and cached until
-- the next :step() / attach invalidates it.
function ValueTrace:defuse()
  if self._g == nil then
    local g = C.asmtest_defuse_build(self._v)
    assert(g ~= nil, "asmtest_defuse_build failed")
    self._g = g
  end
  return self._g
end

-- A live producer appends behind our back (unlike :step(), which counts as it
-- goes), so resync the step count and drop any stale def-use graph.
function ValueTrace:_post_attach()
  self._n_steps = tonumber(C.asmtest_valtrace_steps(self._v))
  self:_invalidate_defuse()
end

function ValueTrace:_invalidate_defuse()
  if self._g ~= nil then
    C.asmtest_defuse_free(self._g)
    self._g = nil
  end
end

function ValueTrace:_slice(origin, forward)
  local g = self:defuse()
  local seed = ffi.new("at_val_rec_t[1]")
  seed[0].step = origin
  local fn = forward and C.asmtest_slice_forward_seed or C.asmtest_slice_backward_seed
  local s = fn(g, seed)
  local out = {}
  for i = 0, self._n_steps - 1 do
    if C.asmtest_slice_contains(s, i) ~= 0 then out[#out + 1] = i end
  end
  C.asmtest_slice_free(s)
  return out
end

-- Steps influenced by the value defined at step `origin` (origin included).
function ValueTrace:forward_slice(origin) return self:_slice(origin, true) end

-- Steps that produced the value used at step `sink` (sink included).
function ValueTrace:backward_slice(sink) return self:_slice(sink, false) end

function ValueTrace:free()
  if self._v ~= nil then
    self:_invalidate_defuse()
    C.asmtest_valtrace_free(self._v)
    self._v = nil
  end
end

return M
