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
]])

local ASMTEST_HW_OK = 0

-- asmtest_trace_backend_t. SINGLESTEP is the portable default that runs on any
-- x86-64 Linux; the rest self-skip off their specific bare-metal hardware.
local INTEL_PT = 0
local CORESIGHT = 1
local AMD_LBR = 2
local SINGLESTEP = 3

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

return M
