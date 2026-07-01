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
-- The shared library is resolved in the same order as asmtest.lua's core loader:
--   ASMTEST_HWTRACE_LIB  an explicit path wins (dev / custom build)
--   native/<os>-<arch>/  the rock-bundled slot next to this file (published rock)
--   <repo>/build/...     the in-tree build artifact (below the bundled path)
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
int  asmtest_ptrace_run_to(int pid, const void* addr);
int  asmtest_proc_region_by_addr(int pid, const void* addr, void** base_out, size_t* len_out);
int  asmtest_proc_perfmap_symbol(int pid, const char* name, void** base_out, size_t* len_out);
typedef struct { uint64_t code_addr; uint64_t code_size; uint64_t timestamp; uint64_t code_index; } asmtest_jitdump_entry_t;
int  asmtest_jitdump_find(const char* path, int pid, const char* name, asmtest_jitdump_entry_t* out, uint8_t* bytes_out, size_t bytes_cap, size_t* bytes_len);
/* Trace an attached target reading versioned code-image bytes (asmtest_ptrace.h). */
int  asmtest_ptrace_trace_attached_versioned(int pid, const void* base, size_t len, void* img, uint64_t when, long* result, void* trace);
/* asmtest_codeimage.h — time-aware code-image recorder (a userspace PERF_RECORD_TEXT_POKE). */
struct asmtest_codeimage_event { uint64_t addr; uint64_t len; uint64_t timestamp; uint32_t pid; uint32_t tid; uint32_t kind; int32_t fd; };
int   asmtest_codeimage_available(void);
void  asmtest_codeimage_skip_reason(char* buf, size_t buflen);
void* asmtest_codeimage_new(int pid);
void  asmtest_codeimage_free(void* img);
int   asmtest_codeimage_track(void* img, const void* base, size_t len);
int   asmtest_codeimage_refresh(void* img);
uint64_t asmtest_codeimage_now(const void* img);
int   asmtest_codeimage_bytes_at(const void* img, const void* addr, uint64_t when, const uint8_t** out, size_t* out_len);
int   asmtest_codeimage_bpf_available(void);
void  asmtest_codeimage_bpf_skip_reason(char* buf, size_t buflen);
int   asmtest_codeimage_watch_bpf(void* img);
int   asmtest_codeimage_poll_bpf(void* img, int timeout_ms);
int   asmtest_codeimage_next(void* img, struct asmtest_codeimage_event* out);
]])

local ASMTEST_HW_OK = 0
local ASMTEST_HW_EUNAVAIL = -3  -- no hardware-trace backend available on this host

-- asmtest_ptrace.h — out-of-process / foreign-process tracing status codes.
local ASMTEST_PTRACE_OK = 0
local ASMTEST_PTRACE_ENOENT = -7  -- region / symbol / method not found

-- asmtest_codeimage.h — time-aware code-image recorder status codes / event kinds.
local ASMTEST_CI_OK = 0
local ASMTEST_CI_ENOENT = -7  -- addr never tracked / no version at-or-before when
local ASMTEST_CI_KIND_MPROTECT = 1  -- mprotect(...PROT_EXEC...) — the common JIT edge
local ASMTEST_CI_KIND_MMAP = 2      -- mmap(...PROT_EXEC...); addr is the real base
local ASMTEST_CI_KIND_MEMFD = 3     -- memfd_create — staging hint; correlate via fd

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

-- This module's own directory, so the rock-bundled slot + repo build/ resolve
-- relative to the file (mirrors asmtest.lua). This file lives at bindings/lua/,
-- so the repo root is two directories up.
local MODULE_DIR = (debug.getinfo(1, "S").source:sub(2):match("(.*/)")) or "./"

-- The absolute path libasmtest_hwtrace actually resolved to (set by
-- hwtrace_path), for M.library_path below.
local resolved_path = nil

-- Resolve libasmtest_hwtrace. Order: an explicit ASMTEST_HWTRACE_LIB wins (dev /
-- custom build); else the rock-bundled slot native/<os>-<arch>/ next to this file
-- (how the rock ships it); else <repo>/build/ (the in-tree build artifact). The
-- build/ fallback stays BELOW the bundled path so an installed rock never prefers
-- a leaked checkout.
local function hwtrace_path()
  local p = os.getenv("ASMTEST_HWTRACE_LIB")
  if p and p ~= "" then resolved_path = p; return p end
  local os_name = ffi.os == "OSX" and "darwin" or "linux"
  local arch = ffi.arch == "arm64" and "arm64" or "x86_64" -- LuaJIT 'x64' -> x86_64
  local ext = ffi.os == "OSX" and "dylib" or "so"
  local bundled = MODULE_DIR .. "native/" .. os_name .. "-" .. arch .. "/libasmtest_hwtrace." .. ext
  local f = io.open(bundled, "r")
  if f then f:close(); resolved_path = bundled; return bundled end
  local repo = MODULE_DIR .. "../../build/libasmtest_hwtrace." .. ext
  resolved_path = repo
  return repo
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
M.ASMTEST_CI_KIND_MPROTECT = ASMTEST_CI_KIND_MPROTECT
M.ASMTEST_CI_KIND_MMAP = ASMTEST_CI_KIND_MMAP
M.ASMTEST_CI_KIND_MEMFD = ASMTEST_CI_KIND_MEMFD

-- The absolute path of the libasmtest_hwtrace this module resolved (env override,
-- else the rock-bundled slot, else the repo build/ tree). Lets a clean-room test
-- assert the bundled tier — not a leaked build/ tree — satisfied the load.
function M.library_path() return resolved_path end

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

-- Like ptrace_trace_attached, but resolve the region's code bytes from a CodeImage
-- timeline as of capture sequence `when` (when == 0 => latest) instead of a single
-- live snapshot — so a method whose address was reused mid-trace is reconstructed
-- from the bytes that were live then, not the bytes there at read time. `img` is a
-- CodeImage, `trace` an HwTrace. Returns the target's RAX at the ret as a Lua number.
-- error()s on a nonzero return. Pass img == nil to fall back to the live snapshot.
function HwTrace.ptrace_trace_attached_versioned(pid, base, len, img, when, trace)
  assert(L, "libasmtest_hwtrace not loaded")
  local result = ffi.new("long[1]")
  local rc = L.asmtest_ptrace_trace_attached_versioned(
    pid, ffi.cast("const void*", base), len,
    img and img.img or nil, ffi.cast("uint64_t", when or 0), result, trace.h)
  if rc ~= ASMTEST_PTRACE_OK then
    error("asmtest_ptrace_trace_attached_versioned failed: " .. tonumber(rc))
  end
  return tonumber(result[0])
end

-- Run an already-attached, ptrace-stopped target forward until it reaches `addr` (a
-- software breakpoint that fires when the program itself next calls in), leaving it
-- stopped there ready for ptrace_trace_attached -- the step that makes a resolved JIT
-- method traceable when you don't control call timing. Returns the status code
-- (ASMTEST_PTRACE_OK, or ASMTEST_PTRACE_ENOENT if the target exited first) as a Lua
-- number. `pid` is a C int, `addr` an integer address. Caller owns PTRACE_ATTACH/DETACH.
function HwTrace.ptrace_run_to(pid, addr)
  assert(L, "libasmtest_hwtrace not loaded")
  return tonumber(L.asmtest_ptrace_run_to(pid, ffi.cast("const void*", addr)))
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

-- ---- Time-aware code-image recorder (asmtest_codeimage.h) ----
--
-- A userspace PERF_RECORD_TEXT_POKE: a timestamped code-image timeline for one
-- target process. track() snapshots a region's bytes (version 0) and arms write-
-- protect-async; refresh() re-snapshots only the pages written since the last arm,
-- appending a new version stamped with the next monotonic sequence; bytes_at(addr,
-- when) answers "what bytes were live at addr as of sequence `when`" — the query a
-- W2 block-normalizer needs to reconstruct a JIT method whose address was reused.
-- pid 0 records THIS process. An optional eBPF emission detector (watch_bpf /
-- poll_bpf / next_event) self-skips without libbpf / CAP_BPF.
local CodeImage = {}
CodeImage.__index = CodeImage
M.CodeImage = CodeImage

-- True if the userspace recorder can detect page changes on this host (PAGEMAP_SCAN
-- or the soft-dirty fallback). False on a failed library load too (self-skip).
function CodeImage.available()
  if not L then return false end
  return L.asmtest_codeimage_available() ~= 0
end

-- Human-readable reason available() is false (or "available"). Returns a fixed
-- message when the lib failed to load.
function CodeImage.skip_reason()
  if not L then return "libasmtest_hwtrace not loaded" end
  local buf = ffi.new("char[?]", 160)
  L.asmtest_codeimage_skip_reason(buf, 160)
  return ffi.string(buf)
end

-- True if the eBPF emission detector can load and attach on this host (built with
-- libbpf, kernel BTF present, sufficient privilege). False on a failed load too.
function CodeImage.bpf_available()
  if not L then return false end
  return L.asmtest_codeimage_bpf_available() ~= 0
end

-- Human-readable reason bpf_available() is false (or "available").
function CodeImage.bpf_skip_reason()
  if not L then return "libasmtest_hwtrace not loaded" end
  local buf = ffi.new("char[?]", 160)
  L.asmtest_codeimage_bpf_skip_reason(buf, 160)
  return ffi.string(buf)
end

-- Create a timeline recording `pid`'s memory (pid == 0 => this process). The wrapper
-- holds the opaque handle and frees it via a finalizer (ffi.gc) so the timeline is
-- released even if free() is not called explicitly. error()s on allocation failure.
function CodeImage.new(pid)
  assert(L, "libasmtest_hwtrace not loaded")
  local img = L.asmtest_codeimage_new(pid or 0)
  if img == nil then error("asmtest_codeimage_new failed") end
  return setmetatable({ img = ffi.gc(img, L.asmtest_codeimage_free) }, CodeImage)
end

-- Begin tracking [base, base+len) in the target: snapshot version 0 now and arm
-- write-protect on its pages. May be called for several disjoint regions. error()s
-- on a negative status.
function CodeImage:track(base, len)
  local rc = L.asmtest_codeimage_track(self.img, ffi.cast("const void*", base), len)
  if rc ~= ASMTEST_CI_OK then
    error("asmtest_codeimage_track failed: " .. tonumber(rc))
  end
  return self
end

-- Scan the tracked ranges for changed pages, re-snapshot each as a new version, and
-- re-arm. Returns the number of new versions recorded (>= 0) as a Lua number.
-- error()s on a negative status.
function CodeImage:refresh()
  local rc = tonumber(L.asmtest_codeimage_refresh(self.img))
  if rc < 0 then
    error("asmtest_codeimage_refresh failed: " .. rc)
  end
  return rc
end

-- The current capture sequence — a monotonic logical timestamp. Advances by one for
-- every version recorded (track + each refresh change). 0 before anything is tracked.
-- Converts the uint64_t cdata result to a Lua number.
function CodeImage:now()
  return tonumber(L.asmtest_codeimage_now(self.img))
end

-- The bytes live at `addr` as of capture sequence `when` (when == 0 => latest), as a
-- Lua string, or nil when addr is not in any tracked region / there is no version
-- at-or-before `when` (ASMTEST_CI_ENOENT). error()s on any other negative status.
function CodeImage:bytes_at(addr, when)
  local out = ffi.new("const uint8_t*[1]")
  local out_len = ffi.new("size_t[1]")
  local rc = L.asmtest_codeimage_bytes_at(self.img, ffi.cast("const void*", addr),
                                          ffi.cast("uint64_t", when or 0),
                                          out, out_len)
  if rc == ASMTEST_CI_ENOENT then return nil end
  if rc ~= ASMTEST_CI_OK then
    error("asmtest_codeimage_bytes_at failed: " .. tonumber(rc))
  end
  return ffi.string(out[0], tonumber(out_len[0]))
end

-- Load and attach the eBPF emission detector, filtered to this timeline's pid.
-- Returns the status code (ASMTEST_CI_OK, or a negative status when unavailable) as
-- a Lua number, so the caller can self-skip without an error.
function CodeImage:watch_bpf()
  return tonumber(L.asmtest_codeimage_watch_bpf(self.img))
end

-- Drain ready emission events from the BPF ring buffer into the internal queue.
-- timeout_ms == 0 is a non-blocking drain; > 0 waits up to that long. Returns the
-- number of events queued (>= 0) as a Lua number. error()s on a negative status.
function CodeImage:poll_bpf(timeout_ms)
  local rc = tonumber(L.asmtest_codeimage_poll_bpf(self.img, timeout_ms or 0))
  if rc < 0 then
    error("asmtest_codeimage_poll_bpf failed: " .. rc)
  end
  return rc
end

-- Pop one queued emission event as a table { addr=, len=, timestamp=, pid=, tid=,
-- kind=, fd= } (numeric fields are Lua numbers), or nil when the queue is empty.
-- error()s on a negative status.
function CodeImage:next_event()
  local e = ffi.new("struct asmtest_codeimage_event")
  local rc = tonumber(L.asmtest_codeimage_next(self.img, e))
  if rc < 0 then
    error("asmtest_codeimage_next failed: " .. rc)
  end
  if rc == 0 then return nil end
  return {
    addr      = tonumber(e.addr),
    len       = tonumber(e.len),
    timestamp = tonumber(e.timestamp),
    pid       = tonumber(e.pid),
    tid       = tonumber(e.tid),
    kind      = tonumber(e.kind),
    fd        = tonumber(e.fd),
  }
end

-- Free the timeline and detach any eBPF watch. Idempotent; cancels the finalizer.
function CodeImage:free()
  if self.img ~= nil then
    ffi.gc(self.img, nil)  -- cancel the finalizer; we free explicitly below
    L.asmtest_codeimage_free(self.img)
    self.img = nil
  end
end

return M
