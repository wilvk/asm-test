-- hwtrace.lua — in-process native runtime tracing for LuaJIT via the real CPU.
--
-- This is the Lua mirror of the Python `asmtest.hwtrace` surface (see
-- bindings/python/asmtest/hwtrace.py and include/asmtest_hwtrace.h). Like the
-- DynamoRIO tier (drtrace.lua) it records host-native code as it runs **inside
-- this LuaJIT process** — initialize the tier once, materialize host-native
-- machine code, mark a region, call into it, and read back basic-block coverage
-- / the ordered instruction stream — but by observing the **real CPU** rather
-- than a runtime instrumentor, so unlike drtrace.lua it needs no DynamoRIO.
--
-- Four backends share one API, selected by enum. SINGLESTEP (EFLAGS.TF #DB ->
-- SIGTRAP) is the portable default: exact and complete on ANY x86-64 Linux
-- (Intel, any-Zen AMD, VM, CI, container) with no PMU, perf_event, privilege, or
-- decoder library. INTEL_PT / CORESIGHT / AMD_LBR self-skip off the specific
-- bare-metal hardware they need. HwTrace.available(backend) reports whether the
-- chosen backend can run so callers self-skip cleanly; the load itself is wrapped
-- in pcall, so a missing libasmtest_hwtrace self-skips too.
--
-- The shared library is taken from the environment, matching the Makefile:
--   ASMTEST_HWTRACE_LIB  libasmtest_hwtrace.{so,dylib}  (else <repo>/build/...)
local ffi = require("ffi")

ffi.cdef([[
typedef struct { int backend; size_t aux_size; size_t data_size; int snapshot; const char* object_hint; } asmtest_hwtrace_options_t;
int  asmtest_hwtrace_available(int backend);
void asmtest_hwtrace_skip_reason(int backend, char* buf, size_t buflen);
size_t asmtest_hwtrace_resolve(int policy, int* out, size_t cap);
int  asmtest_hwtrace_auto(int policy);
typedef struct { int tier; int backend; int fidelity; } asmtest_trace_choice_t;
size_t asmtest_trace_resolve(unsigned policy, asmtest_trace_choice_t* out, size_t cap);
int    asmtest_trace_auto(unsigned policy, asmtest_trace_choice_t* out);
int  asmtest_hwtrace_init(const asmtest_hwtrace_options_t* opts);
int  asmtest_hwtrace_register_region(const char* name, void* base, size_t len, void* trace);
void asmtest_hwtrace_begin(const char* name);
void asmtest_hwtrace_end(const char* name);
void asmtest_hwtrace_shutdown(void);
int  asmtest_hwtrace_exec_alloc(const void* bytes, size_t len, void** base_out, size_t* len_out);
void asmtest_hwtrace_exec_free(void* base, size_t len);
void* asmtest_trace_new(size_t insns_cap, size_t blocks_cap);
void  asmtest_trace_free(void*);
int   asmtest_trace_covered(void* trace, uint64_t off);
uint64_t asmtest_emu_trace_blocks_len(void* trace);
uint64_t asmtest_emu_trace_insns_total(void* trace);
uint64_t asmtest_emu_trace_insns_len(void* trace);
int      asmtest_emu_trace_truncated(void* trace);
uint64_t asmtest_emu_trace_block_at(void* trace, size_t i);
uint64_t asmtest_emu_trace_insn_at(void* trace, size_t i);
/* asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit. */
int  asmtest_ptrace_available(void);
void asmtest_ptrace_skip_reason(char* buf, size_t buflen);
int  asmtest_ptrace_trace_call(const void* code, size_t len, const long* args, int nargs, long* result, void* trace);
int  asmtest_ptrace_trace_attached(int pid, const void* base, size_t len, long* result, void* trace);
int  asmtest_proc_region_by_addr(int pid, const void* addr, void** base_out, size_t* len_out);
int  asmtest_proc_perfmap_symbol(int pid, const char* name, void** base_out, size_t* len_out);
typedef struct { uint64_t code_addr; uint64_t code_size; uint64_t timestamp; uint64_t code_index; } asmtest_jitdump_entry_t;
int  asmtest_jitdump_find(const char* path, int pid, const char* name, asmtest_jitdump_entry_t* out, uint8_t* bytes_out, size_t bytes_cap, size_t* bytes_len);
]])

local ASMTEST_HW_OK = 0
local ASMTEST_HW_EUNAVAIL = -3  -- no hardware-trace backend available on this host

-- asmtest_ptrace.h — out-of-process / foreign-process tracing status codes.
local ASMTEST_PTRACE_OK = 0
local ASMTEST_PTRACE_ENOENT = -7  -- region / symbol / method not found

-- asmtest_trace_backend_t. SINGLESTEP is the portable default that runs on any
-- x86-64 Linux; the rest self-skip off their specific bare-metal hardware.
local INTEL_PT = 0
local CORESIGHT = 1
local AMD_LBR = 2
local SINGLESTEP = 3

-- asmtest_hwtrace_policy_t — backend auto-selection policy.
local BEST = 0          -- the most faithful available backend
local CEILING_FREE = 1  -- the same, but skipping the one fixed-window backend
                        -- (AMD LBR); re-resolve under this after a truncated trace

-- asmtest_trace_auto.h — the CROSS-TIER orchestrator over all three trace tiers
-- (hardware + DynamoRIO + emulator), not just the hardware backends above.
-- asmtest_trace_tier_t.
local TIER_HWTRACE = 0    -- HW branch trace / single-step (real CPU)
local TIER_DYNAMORIO = 1  -- in-process software DBI (real CPU)
local TIER_EMULATOR = 2   -- Unicorn virtual CPU (isolated guest)
-- asmtest_trace_fidelity_t.
local FIDELITY_NATIVE = 0   -- runs the real bytes on the real CPU in-process
local FIDELITY_VIRTUAL = 1  -- isolated guest on an emulated CPU
-- cross-tier policy bitmask.
local TRACE_BEST = 0x0          -- most-faithful available; emulator floor allowed
local TRACE_CEILING_FREE = 0x1  -- drop the fixed-window backend (AMD LBR)
local TRACE_NATIVE_ONLY = 0x2   -- forbid the native->emulator fidelity crossing

-- Resolve libasmtest_hwtrace: an explicit ASMTEST_HWTRACE_LIB wins (dev / custom
-- build); otherwise fall back to <repo>/build/ next to this binding. This file
-- lives at bindings/lua/, so the repo root is two directories up.
local function hwtrace_path()
  local p = os.getenv("ASMTEST_HWTRACE_LIB")
  if p and p ~= "" then return p end
  local ext = ffi.os == "OSX" and "dylib" or "so"
  local dir = (debug.getinfo(1, "S").source:sub(2):match("(.*/)")) or "./"
  return dir .. "../../build/libasmtest_hwtrace." .. ext
end

-- Load the lib defensively: the tier may be absent (not built / wrong host), so a
-- failed load is not an error — it just makes available() return false (self-skip).
local L = nil
local ok, lib = pcall(ffi.load, hwtrace_path())
if ok then L = lib end

local M = {}
M.INTEL_PT = INTEL_PT
M.CORESIGHT = CORESIGHT
M.AMD_LBR = AMD_LBR
M.SINGLESTEP = SINGLESTEP
M.BEST = BEST
M.CEILING_FREE = CEILING_FREE
M.TIER_HWTRACE = TIER_HWTRACE
M.TIER_DYNAMORIO = TIER_DYNAMORIO
M.TIER_EMULATOR = TIER_EMULATOR
M.FIDELITY_NATIVE = FIDELITY_NATIVE
M.FIDELITY_VIRTUAL = FIDELITY_VIRTUAL
M.TRACE_BEST = TRACE_BEST
M.TRACE_CEILING_FREE = TRACE_CEILING_FREE
M.TRACE_NATIVE_ONLY = TRACE_NATIVE_ONLY
M.ASMTEST_HW_EUNAVAIL = ASMTEST_HW_EUNAVAIL

-- ---- Host-native machine code in real executable (W^X) memory ----
local NativeCode = {}
NativeCode.__index = NativeCode
M.NativeCode = NativeCode

-- Materialize host-native machine code into executable memory. Accepts a Lua
-- string of bytes, or a 1-based array (table) of byte values.
function NativeCode.from_bytes(bytes)
  assert(L, "libasmtest_hwtrace not loaded")
  if type(bytes) == "table" then bytes = string.char(unpack(bytes)) end
  local base = ffi.new("void*[1]")
  local len = ffi.new("size_t[1]")
  local rc = L.asmtest_hwtrace_exec_alloc(ffi.cast("const void*", bytes), #bytes,
                                          base, len)
  if rc ~= ASMTEST_HW_OK then
    error("asmtest_hwtrace_exec_alloc failed: " .. tonumber(rc))
  end
  return setmetatable({ base = base[0], len = tonumber(len[0]), freed = false },
                      NativeCode)
end

-- Invoke the code through a function pointer, passing two C longs and reading the
-- result back as a long (the SysV integer ABI). Returns a Lua number.
function NativeCode:call(a, b)
  local fn = ffi.cast("long(*)(long,long)", self.base)
  return tonumber(fn(a or 0, b or 0))
end

function NativeCode:free()
  if not self.freed then
    L.asmtest_hwtrace_exec_free(self.base, self.len)
    self.freed = true
  end
end

-- ---- A coverage recorder for a registered native region, via the hardware tier ----
local HwTrace = {}
HwTrace.__index = HwTrace
M.HwTrace = HwTrace

-- ---- process-wide lifecycle ----

-- True if the chosen backend can run on this host (self-skip otherwise). False on
-- a failed library load OR when asmtest_hwtrace_available() reports 0.
function HwTrace.available(backend)
  if not L then return false end
  return L.asmtest_hwtrace_available(backend or SINGLESTEP) ~= 0
end

-- Human-readable reason available() is false (or "available") for the self-skip
-- message. Returns the empty string if the lib failed to load.
function HwTrace.skip_reason(backend)
  if not L then return "libasmtest_hwtrace not loaded" end
  local buf = ffi.new("char[?]", 160)
  L.asmtest_hwtrace_skip_reason(backend or SINGLESTEP, buf, 160)
  return ffi.string(buf)
end

-- This host's hardware-trace fallback cascade: the available backends, most-
-- faithful first (INTEL_PT > AMD_LBR > SINGLESTEP > CORESIGHT), honoring `policy`
-- (default BEST), as a 1-based Lua array of backend ints. Empty only off x86-64
-- Linux (single-step is the floor there). CEILING_FREE drops the depth-bounded
-- backend (AMD LBR).
function HwTrace.resolve(policy)
  assert(L, "libasmtest_hwtrace not loaded")
  local out = ffi.new("int[4]")
  local n = tonumber(L.asmtest_hwtrace_resolve(policy or BEST, out, 4))
  local t = {}
  for i = 0, n - 1 do
    t[i + 1] = out[i]
  end
  return t
end

-- The single most-preferred available backend under `policy` (default BEST), as a
-- backend enum >= 0 ready to init, or ASMTEST_HW_EUNAVAIL (< 0) when no hardware-
-- trace backend is available on this host.
function HwTrace.auto(policy)
  assert(L, "libasmtest_hwtrace not loaded")
  return tonumber(L.asmtest_hwtrace_auto(policy or BEST))
end

-- This host's full CROSS-TIER fallback cascade (asmtest_trace_resolve), most-
-- faithful first: Intel PT -> AMD LBR -> DynamoRIO -> single-step -> CoreSight ->
-- emulator, each included only if its tier is available. Honors `policy` (default
-- TRACE_BEST), returned as a 1-based Lua array of { tier=, backend=, fidelity= }
-- tables. TRACE_NATIVE_ONLY drops the emulator floor (no native->emulator fidelity
-- crossing); TRACE_CEILING_FREE drops the depth-bounded backend (AMD LBR).
function HwTrace.resolve_tiers(policy)
  assert(L, "libasmtest_hwtrace not loaded")
  local cap = 8
  local out = ffi.new("asmtest_trace_choice_t[?]", cap)
  local n = tonumber(L.asmtest_trace_resolve(policy or TRACE_BEST, out, cap))
  local t = {}
  for i = 0, n - 1 do
    t[i + 1] = { tier = out[i].tier, backend = out[i].backend,
                 fidelity = out[i].fidelity }
  end
  return t
end

-- The single most-preferred available cross-tier choice under `policy` (default
-- TRACE_BEST) as a { tier=, backend=, fidelity= } table, or nil when the cascade is
-- empty (only off a native host under TRACE_NATIVE_ONLY).
function HwTrace.auto_tier(policy)
  assert(L, "libasmtest_hwtrace not loaded")
  local out = ffi.new("asmtest_trace_choice_t[1]")
  local rc = L.asmtest_trace_auto(policy or TRACE_BEST, out)
  if rc ~= ASMTEST_HW_OK then return nil end
  return { tier = out[0].tier, backend = out[0].backend,
           fidelity = out[0].fidelity }
end

-- Select a backend and initialize the tier. SINGLESTEP is the portable default
-- that runs on any x86-64 Linux. error()s on a nonzero return.
function HwTrace.init(backend)
  assert(L, "libasmtest_hwtrace not loaded")
  local o = ffi.new("asmtest_hwtrace_options_t")
  o.backend = backend or SINGLESTEP
  -- Leave aux_size/data_size/snapshot zero and object_hint NULL so the C side
  -- uses its defaults.
  local rc = L.asmtest_hwtrace_init(o)
  if rc ~= ASMTEST_HW_OK then
    error("asmtest_hwtrace_init failed: " .. tonumber(rc))
  end
end

function HwTrace.shutdown()
  if L then L.asmtest_hwtrace_shutdown() end
end

-- ---- per-trace ----

-- Allocate a trace. `blocks` is the basic-block capacity (default 64);
-- `instructions` the ordered-instruction-stream capacity (default 0 = blocks
-- only). NOTE: the C constructor takes insns FIRST, blocks SECOND.
function HwTrace.create(blocks, instructions)
  assert(L, "libasmtest_hwtrace not loaded")
  blocks = blocks or 64
  instructions = instructions or 0
  local h = L.asmtest_trace_new(instructions, blocks)
  if h == nil then error("asmtest_trace_new failed") end
  return setmetatable({ h = h }, HwTrace)
end

-- Register a non-overlapping native code range under `name`, recording coverage
-- into this trace. error()s on failure.
function HwTrace:register(name, code)
  local rc = L.asmtest_hwtrace_register_region(name, code.base, code.len, self.h)
  if rc ~= ASMTEST_HW_OK then
    error(string.format("register_region(%q) failed: %d", name, tonumber(rc)))
  end
  return self
end

-- Run `fn` inside a balanced begin/end marker pair for the named region. The end
-- marker always fires (pcall), even if `fn` errors, so capture state stays
-- balanced; the error is then re-raised.
function HwTrace:region(name, fn)
  L.asmtest_hwtrace_begin(name)
  local ran_ok, err = pcall(fn)
  L.asmtest_hwtrace_end(name)
  if not ran_ok then error(err) end
end

-- True if the basic block at byte-offset `off` (from the region entry) was entered.
function HwTrace:covered(off)
  return L.asmtest_trace_covered(self.h, off) ~= 0
end

-- Number of distinct basic blocks recorded so far.
function HwTrace:blocks_len()
  return tonumber(L.asmtest_emu_trace_blocks_len(self.h))
end

-- Total instructions executed in the region (may exceed the stored insns_len when
-- the instruction-stream capacity is smaller than the run).
function HwTrace:insns_total()
  return tonumber(L.asmtest_emu_trace_insns_total(self.h))
end

-- Number of instruction offsets actually stored (<= insns_total, bounded by the
-- trace's instruction capacity).
function HwTrace:insns_len()
  return tonumber(L.asmtest_emu_trace_insns_len(self.h))
end

-- True if recording hit a capacity ceiling and dropped entries.
function HwTrace:truncated()
  return L.asmtest_emu_trace_truncated(self.h) ~= 0
end

-- The distinct basic-block start offsets recorded, in first-seen order, as a
-- 1-based Lua array of numbers.
function HwTrace:block_offsets()
  local n = tonumber(L.asmtest_emu_trace_blocks_len(self.h))
  local t = {}
  for i = 0, n - 1 do
    t[i + 1] = tonumber(L.asmtest_emu_trace_block_at(self.h, i))
  end
  return t
end

-- The ordered instruction-offset stream actually stored — each executed
-- instruction's offset in execution order, up to the trace's insns capacity
-- (insns_len, not the possibly-larger insns_total) — as a 1-based Lua array.
function HwTrace:insn_offsets()
  local n = tonumber(L.asmtest_emu_trace_insns_len(self.h))
  local t = {}
  for i = 0, n - 1 do
    t[i + 1] = tonumber(L.asmtest_emu_trace_insn_at(self.h, i))
  end
  return t
end

function HwTrace:free()
  if self.h ~= nil then
    L.asmtest_trace_free(self.h)
    self.h = nil
  end
end

-- ---- Out-of-process / foreign-process tracing (asmtest_ptrace.h) ----
--
-- Single-step a forked or externally-attached target out of band, and resolve the
-- code region to trace from the OS — /proc/<pid>/maps, a JIT perf-map, or a binary
-- jitdump. The managed-runtime path (JVM/.NET/Node on AMD, where Intel PT is
-- unavailable and in-process DynamoRIO cannot seize the runtime's threads). Linux
-- x86-64. These are static methods on HwTrace mirroring the Python `Ptrace` class.

-- True if the out-of-process single-step tracer can run on this host (self-skip
-- otherwise). False on a failed library load too.
function HwTrace.ptrace_available()
  if not L then return false end
  return L.asmtest_ptrace_available() ~= 0
end

-- Human-readable reason ptrace_available() is false (or "available"). Returns a
-- fixed message when the lib failed to load.
function HwTrace.ptrace_skip_reason()
  if not L then return "libasmtest_hwtrace not loaded" end
  local buf = ffi.new("char[?]", 160)
  L.asmtest_ptrace_skip_reason(buf, 160)
  return ffi.string(buf)
end

-- Fork a tracee that calls `code` (a NativeCode, given as base + length) with up to
-- six integer `args` (a 1-based Lua array), single-step it out of process, and fill
-- `trace` (an HwTrace). Returns the routine's return value (the child's RAX at the
-- ret) as a Lua number. error()s on a nonzero return.
function HwTrace.ptrace_trace_call(code_base, code_len, args_table, trace)
  assert(L, "libasmtest_hwtrace not loaded")
  args_table = args_table or {}
  local n = #args_table
  local arr = ffi.new("long[?]", n > 0 and n or 1)
  for i = 1, n do arr[i - 1] = args_table[i] end
  local result = ffi.new("long[1]")
  local rc = L.asmtest_ptrace_trace_call(ffi.cast("const void*", code_base),
                                         code_len, arr, n, result, trace.h)
  if rc ~= ASMTEST_PTRACE_OK then
    error("asmtest_ptrace_trace_call failed: " .. tonumber(rc))
  end
  return tonumber(result[0])
end

-- Trace a region in a SEPARATE, already-ptrace-stopped process (the caller owns
-- PTRACE_ATTACH/DETACH). Reads the target's bytes via process_vm_readv. `pid` is a
-- C int, `base`/`len` the region in the target's address space, `trace` an HwTrace.
-- Returns the target's RAX at the ret as a Lua number. error()s on a nonzero return.
function HwTrace.ptrace_trace_attached(pid, base, len, trace)
  assert(L, "libasmtest_hwtrace not loaded")
  local result = ffi.new("long[1]")
  local rc = L.asmtest_ptrace_trace_attached(pid, ffi.cast("const void*", base),
                                             len, result, trace.h)
  if rc ~= ASMTEST_PTRACE_OK then
    error("asmtest_ptrace_trace_attached failed: " .. tonumber(rc))
  end
  return tonumber(result[0])
end

-- The executable mapping in /proc/<pid>/maps containing `addr`, as two return
-- values base, len (Lua numbers), or nil if no executable mapping contains it.
function HwTrace.proc_region_by_addr(pid, addr)
  assert(L, "libasmtest_hwtrace not loaded")
  local base = ffi.new("void*[1]")
  local len = ffi.new("size_t[1]")
  local rc = L.asmtest_proc_region_by_addr(pid, ffi.cast("const void*", addr),
                                           base, len)
  if rc ~= ASMTEST_PTRACE_OK then return nil end
  return tonumber(ffi.cast("uintptr_t", base[0])), tonumber(len[0])
end

-- A JIT method by `name` in /tmp/perf-<pid>.map, as two return values base, len
-- (Lua numbers), or nil if no such symbol (or no map file).
function HwTrace.proc_perfmap_symbol(pid, name)
  assert(L, "libasmtest_hwtrace not loaded")
  local base = ffi.new("void*[1]")
  local len = ffi.new("size_t[1]")
  local rc = L.asmtest_proc_perfmap_symbol(pid, name, base, len)
  if rc ~= ASMTEST_PTRACE_OK then return nil end
  return tonumber(ffi.cast("uintptr_t", base[0])), tonumber(len[0])
end

-- A JIT method by `name` from a jitdump (`path`, or /tmp/jit-<pid>.dump when path is
-- nil) as a table { code_addr=, code_size=, timestamp=, code_index=, code= } (the
-- latest re-JIT body — highest timestamp — wins), or nil if no such method / no
-- file. `want_bytes` (default 0) caps the recorded code bytes copied into `code` (a
-- Lua string); 0 means no bytes (code == ""). Numeric fields are Lua numbers.
function HwTrace.jitdump_find(path, name, pid, want_bytes)
  assert(L, "libasmtest_hwtrace not loaded")
  pid = pid or 0
  want_bytes = want_bytes or 0
  local e = ffi.new("asmtest_jitdump_entry_t")
  local buf = want_bytes > 0 and ffi.new("uint8_t[?]", want_bytes) or nil
  local blen = want_bytes > 0 and ffi.new("size_t[1]") or nil
  local rc = L.asmtest_jitdump_find(path, pid, name, e, buf, want_bytes, blen)
  if rc ~= ASMTEST_PTRACE_OK then return nil end
  local code = ""
  if want_bytes > 0 then code = ffi.string(buf, tonumber(blen[0])) end
  return {
    code_addr  = tonumber(e.code_addr),
    code_size  = tonumber(e.code_size),
    timestamp  = tonumber(e.timestamp),
    code_index = tonumber(e.code_index),
    code       = code,
  }
end

return M
