-- drtrace.lua — in-process native runtime tracing for LuaJIT, backed by DynamoRIO.
--
-- This is the Lua mirror of the Python `asmtest.drtrace` surface (see
-- bindings/python/asmtest/drtrace.py and include/asmtest_drtrace.h). Where the
-- emulator tier (asmtest.lua's Trace) traces isolated guest bytes, `NativeTrace`
-- traces host-native code as it runs **inside this LuaJIT process**: initialize
-- DynamoRIO once, materialize host-native machine code, mark a region, call into
-- it, and read back basic-block coverage / the instruction stream.
--
-- It loads libasmtest_drapp and drives the C API; libdynamorio is dlopen()ed
-- lazily by the C side after the client is configured, so nothing here links DR.
-- The tier is advanced, Linux-x86-64-only, and opt-in: NativeTrace.available()
-- reports whether it can run so callers self-skip cleanly. The load itself is
-- wrapped in pcall, so a missing libasmtest_drapp (no DynamoRIO) self-skips too.
--
-- The shared library is resolved in the same order as asmtest.lua's core loader:
--   ASMTEST_DRAPP_LIB   an explicit path wins (dev / custom build)
--   native/<os>-<arch>/ the rock-bundled slot next to this file (published rock)
--   <repo>/build/...     the in-tree build artifact (below the bundled path)
local ffi = require("ffi")

ffi.cdef([[
typedef struct { const char* dynamorio_home; const char* client_path; const char* client_options; int mode; } asmtest_drtrace_options_t;
typedef struct { void* base; size_t len; } asmtest_exec_code_t;
int  asmtest_dr_available(void);
int  asmtest_dr_init(const asmtest_drtrace_options_t*);
int  asmtest_dr_start(void);
int  asmtest_dr_stop(void);
void asmtest_dr_shutdown(void);
int  asmtest_dr_register_region(const char* name, void* base, size_t len, void* trace);
int  asmtest_dr_register_symbol(const char* symbol, size_t max_len, void* trace);
int  asmtest_dr_unregister_region(const char* name);
long asmtest_symbol_demo(long a, long b);
void asmtest_trace_begin(const char* name);
void asmtest_trace_end(const char* name);
int  asmtest_dr_marker_error(void);
int  asmtest_exec_alloc(const uint8_t* bytes, size_t len, asmtest_exec_code_t* out);
void asmtest_exec_free(asmtest_exec_code_t*);
void* asmtest_trace_new(size_t insns_cap, size_t blocks_cap);
void  asmtest_trace_free(void*);
int   asmtest_trace_covered(void* trace, uint64_t off);
uint64_t asmtest_emu_trace_blocks_len(void* trace);
uint64_t asmtest_emu_trace_insns_total(void* trace);
uint64_t asmtest_emu_trace_insns_len(void* trace);
uint64_t asmtest_emu_trace_block_at(void* trace, size_t i);
uint64_t asmtest_emu_trace_insn_at(void* trace, size_t i);
]])

local ASMTEST_DR_OK = 0

-- This module's own directory, so the rock-bundled slot + repo build/ resolve
-- relative to the file (mirrors asmtest.lua). This file lives at bindings/lua/,
-- so the repo root is two directories up.
local MODULE_DIR = (debug.getinfo(1, "S").source:sub(2):match("(.*/)")) or "./"

-- The rock-bundled native slot (os, arch, ext), mirroring asmtest.lua's core
-- loader: native/<os>-<arch>/lib*.<ext> next to this file (how the rock ships it).
local function bundled_slot()
  local os_name = ffi.os == "OSX" and "darwin" or "linux"
  local arch = ffi.arch == "arm64" and "arm64" or "x86_64" -- LuaJIT 'x64' -> x86_64
  local ext = ffi.os == "OSX" and "dylib" or "so"
  return os_name, arch, ext
end

-- The absolute path libasmtest_drapp actually resolved to (set by drapp_path),
-- for M.library_path below.
local resolved_path = nil

-- Resolve libasmtest_drapp. Order: an explicit ASMTEST_DRAPP_LIB wins (dev /
-- custom build); else the rock-bundled slot native/<os>-<arch>/ next to this file
-- (how the rock ships it); else <repo>/build/ (the in-tree build artifact). The
-- build/ fallback stays BELOW the bundled path so an installed rock never prefers
-- a leaked checkout.
local function drapp_path()
  local p = os.getenv("ASMTEST_DRAPP_LIB")
  if p and p ~= "" then resolved_path = p; return p end
  local os_name, arch, ext = bundled_slot()
  local bundled = MODULE_DIR .. "native/" .. os_name .. "-" .. arch .. "/libasmtest_drapp." .. ext
  local f = io.open(bundled, "r")
  if f then f:close(); resolved_path = bundled; return bundled end
  local repo = MODULE_DIR .. "../../build/libasmtest_drapp." .. ext
  resolved_path = repo
  return repo
end

-- Load the lib defensively: requires DynamoRIO and may be absent, so a failed
-- load is not an error — it just makes available() return false (self-skip).
local L = nil
local ok, lib = pcall(ffi.load, drapp_path())
if ok then L = lib end

local M = {}

-- The absolute path of the libasmtest_drapp this module resolved (env override,
-- else the rock-bundled slot, else the repo build/ tree). Lets a clean-room test
-- assert the bundled tier — not a leaked build/ tree — satisfied the load.
function M.library_path() return resolved_path end

-- The rock-bundled DR client shipped alongside libasmtest_drapp, if present:
-- $ASMTEST_DRCLIENT wins, else native/<os>-<arch>/libasmtest_drclient.<ext> next
-- to this file, else the <repo>/build tree; nil when none exists (the C side then
-- takes its own $ASMTEST_DRCLIENT / repo-build fallback). Mirrors drtrace.py's
-- _default_client.
local function default_client()
  local env = os.getenv("ASMTEST_DRCLIENT")
  if env and env ~= "" then return env end
  local os_name, arch, ext = bundled_slot()
  local cands = {
    MODULE_DIR .. "native/" .. os_name .. "-" .. arch .. "/libasmtest_drclient." .. ext,
    MODULE_DIR .. "../../build/libasmtest_drclient." .. ext,
  }
  for _, c in ipairs(cands) do
    local f = io.open(c, "r")
    if f then f:close(); return c end
  end
  return nil
end

-- ---- Host-native machine code in real executable (W^X) memory ----
local NativeCode = {}
NativeCode.__index = NativeCode
M.NativeCode = NativeCode

-- Materialize a Lua string of host-native machine code into executable memory.
function NativeCode.from_bytes(bytes)
  assert(L, "libasmtest_drapp not loaded")
  local ec = ffi.new("asmtest_exec_code_t[1]")
  local rc = L.asmtest_exec_alloc(ffi.cast("const uint8_t*", bytes), #bytes, ec)
  if rc ~= ASMTEST_DR_OK then
    error("asmtest_exec_alloc failed: " .. tonumber(rc))
  end
  return setmetatable({ ec = ec, freed = false }, NativeCode)
end

-- The executable mapping's base address (offset 0 = entry) as a cdata pointer.
function NativeCode:base() return self.ec[0].base end
-- The number of code bytes.
function NativeCode:length() return tonumber(self.ec[0].len) end
-- Invoke the code through a function pointer, passing two C longs and reading the
-- result back as a long (the SysV integer ABI). Returns a Lua number.
function NativeCode:call(a, b)
  local fn = ffi.cast("long(*)(long,long)", self.ec[0].base)
  return tonumber(fn(a or 0, b or 0))
end
function NativeCode:free()
  if not self.freed then
    L.asmtest_exec_free(self.ec)
    self.freed = true
  end
end

-- ---- An app-owned coverage recorder for a registered native region ----
local NativeTrace = {}
NativeTrace.__index = NativeTrace
M.NativeTrace = NativeTrace

-- ---- process-wide lifecycle ----

-- True if the tier can run (built + libdynamorio resolvable). False on a failed
-- library load OR when asmtest_dr_available() reports 0.
function NativeTrace.available()
  if not L then return false end
  return L.asmtest_dr_available() ~= 0
end

-- Bring DynamoRIO up in-process and take over. `opts` is an optional table:
--   client          path to libasmtest_drclient.so (else $ASMTEST_DRCLIENT)
--   dynamorio_home  DR runtime root (else $ASMTEST_DR_LIB / rpath)
--   client_options  extra client options
--   mode            process-init default recording mode (0 = blocks)
-- error()s on a nonzero return from init or start.
function NativeTrace.initialize(opts)
  assert(L, "libasmtest_drapp not loaded")
  opts = opts or {}
  local o = ffi.new("asmtest_drtrace_options_t")
  -- Default the client to the rock-bundled libasmtest_drclient (honoring
  -- $ASMTEST_DRCLIENT first) when none is passed; a still-nil client_path stays
  -- NULL so the C side takes its own env / repo-build fallback.
  o.client_path = opts.client or default_client()
  o.dynamorio_home = opts.dynamorio_home
  o.client_options = opts.client_options
  o.mode = opts.mode or 0
  local rc = L.asmtest_dr_init(o)
  if rc ~= ASMTEST_DR_OK then error("asmtest_dr_init failed: " .. tonumber(rc)) end
  rc = L.asmtest_dr_start()
  if rc ~= ASMTEST_DR_OK then error("asmtest_dr_start failed: " .. tonumber(rc)) end
end

function NativeTrace.shutdown()
  if L then L.asmtest_dr_shutdown() end
end

-- The exported fixture (a*2+b) the symbol-mode test traces by name. Returns a
-- Lua number.
function M.symbol_demo(a, b)
  assert(L, "libasmtest_drapp not loaded")
  return tonumber(L.asmtest_symbol_demo(a, b))
end

-- Count of illegal marker operations since init; 0 means every marker balanced.
function NativeTrace.marker_error()
  return tonumber(L.asmtest_dr_marker_error())
end

-- ---- per-trace ----

-- Allocate an app-owned trace. `blocks` is the basic-block capacity (default 64);
-- `instructions` the ordered-instruction-stream capacity (default 0 = blocks
-- only). NOTE: the C constructor takes insns FIRST, blocks SECOND.
function NativeTrace.new(blocks, instructions)
  assert(L, "libasmtest_drapp not loaded")
  blocks = blocks or 64
  instructions = instructions or 0
  local h = L.asmtest_trace_new(instructions, blocks)
  if h == nil then error("asmtest_trace_new failed") end
  return setmetatable({ h = h }, NativeTrace)
end

-- Register a non-overlapping native code range under `name`, recording coverage
-- into this trace. error()s on failure.
function NativeTrace:register(name, code)
  local rc = L.asmtest_dr_register_region(name, code:base(), code:length(), self.h)
  if rc ~= ASMTEST_DR_OK then
    error(string.format("register_region(%q) failed: %d", name, tonumber(rc)))
  end
  return self
end

-- Symbol mode: trace a named exported function by NAME, with no begin/end
-- markers — always-on recording for [entry, entry+max_len). error()s on failure.
function NativeTrace:register_symbol(symbol, max_len)
  max_len = max_len or 256
  local rc = L.asmtest_dr_register_symbol(symbol, max_len, self.h)
  if rc ~= ASMTEST_DR_OK then
    error(string.format("register_symbol(%q) failed: %d", symbol, tonumber(rc)))
  end
  return self
end

function NativeTrace:unregister(name)
  L.asmtest_dr_unregister_region(name)
end

-- Run `fn` inside a balanced begin/end marker pair for the named region. The end
-- marker always fires (pcall), even if `fn` errors, so markers stay balanced;
-- the error is then re-raised.
function NativeTrace:region(name, fn)
  L.asmtest_trace_begin(name)
  local ran_ok, err = pcall(fn)
  L.asmtest_trace_end(name)
  if not ran_ok then error(err) end
end

-- True if the basic block at byte-offset `off` (from the region entry) was entered.
function NativeTrace:covered(off)
  return L.asmtest_trace_covered(self.h, off) ~= 0
end

-- Number of distinct basic blocks recorded so far.
function NativeTrace:blocks_len()
  return tonumber(L.asmtest_emu_trace_blocks_len(self.h))
end

-- Total instructions recorded so far (only when allocated with instructions > 0).
function NativeTrace:insns_total()
  return tonumber(L.asmtest_emu_trace_insns_total(self.h))
end

-- The distinct basic-block start offsets recorded, in first-seen order, as a
-- 1-based Lua array of numbers.
function NativeTrace:block_offsets()
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
function NativeTrace:insn_offsets()
  local n = tonumber(L.asmtest_emu_trace_insns_len(self.h))
  local t = {}
  for i = 0, n - 1 do
    t[i + 1] = tonumber(L.asmtest_emu_trace_insn_at(self.h, i))
  end
  return t
end

function NativeTrace:free()
  if self.h ~= nil then
    L.asmtest_trace_free(self.h)
    self.h = nil
  end
end

return M
