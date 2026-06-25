-- asmtest.lua — asm-test Lua binding (Track C): the reusable library module.
--
-- This is the module a LuaJIT project requires; it keeps all `ffi` declarations
-- inside, so calling code never declares a native entry point. It drives the
-- opaque-handle FFI layer (src/ffi.c), so no C struct layout is mirrored: a Regs
-- object with :capture6 / :capture_fp2 + accessors, Emu / EmuResult for the
-- emulator (faults as data), and assert_* helpers that error() on failure.
--
-- The shared libraries are taken from the environment, matching how the
-- framework's Makefile invokes the bindings:
--   ASMTEST_LIB         libasmtest_emu.{so,dylib}    (capture + emulator + accessors)
--   ASMTEST_CORPUS_LIB  libasmtest_corpus.{so,dylib} (the canonical fixtures)
local ffi = require("ffi")

ffi.cdef([[
void *asmtest_corpus_routine(const char *name);
void *asmtest_regs_new(void);
void  asmtest_regs_free(void *r);
void  asmtest_capture6(void *out, void *fn, long a0, long a1, long a2, long a3, long a4, long a5);
void  asmtest_capture_fp2(void *out, void *fn, double f0, double f1);
unsigned long asmtest_regs_ret(void *r);
double asmtest_regs_fret(void *r);
int   asmtest_regs_flag_set(void *r, const char *name);
int   asmtest_check_abi(void *r, char *msg, size_t n);
void *emu_open(void);
void  emu_close(void *e);
void *asmtest_emu_result_new(void);
void  asmtest_emu_result_free(void *r);
int   asmtest_emu_call2(void *e, void *fn, long a0, long a1, void *out);
int   asmtest_emu_call_asm(void *e, const char *src, long a0, long a1, void *out);
int   asmtest_emu_result_faulted(void *r);
unsigned long long asmtest_emu_x86_reg(void *r, const char *name);
]])

local emu_path = assert(os.getenv("ASMTEST_LIB"), "set ASMTEST_LIB to libasmtest_emu.{so,dylib}")
local corpus_path = os.getenv("ASMTEST_CORPUS_LIB")
local L = ffi.load(emu_path)
local C = corpus_path and ffi.load(corpus_path) or nil

local M = {}

-- Resolve a canonical corpus routine (e.g. "add_signed") to its address.
function M.corpus_routine(name)
  assert(C, "set ASMTEST_CORPUS_LIB to use corpus_routine")
  return C.asmtest_corpus_routine(name)
end

-- A captured register/flags snapshot. Call :free() when done.
local Regs = {}
Regs.__index = Regs
function M.Regs() return setmetatable({ h = L.asmtest_regs_new() }, Regs) end
-- Call fn through the real ABI with up to six integer args.
function Regs:capture6(fn, a0, a1, a2, a3, a4, a5)
  L.asmtest_capture6(self.h, fn, a0 or 0, a1 or 0, a2 or 0, a3 or 0, a4 or 0, a5 or 0)
end
-- Call fn with two double args, capturing the FP return.
function Regs:capture_fp2(fn, f0, f1) L.asmtest_capture_fp2(self.h, fn, f0, f1) end
function Regs:ret() return tonumber(L.asmtest_regs_ret(self.h)) end       -- rax / x0
function Regs:fret() return L.asmtest_regs_fret(self.h) end               -- xmm0 / d0
function Regs:flag_set(name) return L.asmtest_regs_flag_set(self.h, name) == 1 end
function Regs:abi_preserved() return L.asmtest_check_abi(self.h, nil, 0) == 0 end
function Regs:free() if self.h then L.asmtest_regs_free(self.h); self.h = nil end end

-- An emulator run's outcome — faults surfaced as data, not a crash.
local EmuResult = {}
EmuResult.__index = EmuResult
local function new_result() return setmetatable({ h = L.asmtest_emu_result_new() }, EmuResult) end
function EmuResult:faulted() return L.asmtest_emu_result_faulted(self.h) ~= 0 end
function EmuResult:reg(name) return tonumber(L.asmtest_emu_x86_reg(self.h, name)) end  -- x86-64 guest reg
function EmuResult:free() if self.h then L.asmtest_emu_result_free(self.h); self.h = nil end end

-- An open emulator (x86-64 Unicorn guest). Call :close() when done.
local Emu = {}
Emu.__index = Emu
function M.Emu() return setmetatable({ h = L.emu_open() }, Emu) end
-- Run fn in the emulator with two integer args; returns an EmuResult.
function Emu:call2(fn, a0, a1)
  local res = new_result()
  L.asmtest_emu_call2(self.h, fn, a0, a1, res.h)
  return res
end
-- Assemble x86-64 `src` (Intel syntax) via Keystone and run it with two integer
-- args; returns res, ok (ok is false if it failed to assemble). Needs the
-- Keystone-backed native lib.
function Emu:call_asm(src, a0, a1)
  local res = new_result()
  local ok = L.asmtest_emu_call_asm(self.h, src, a0, a1, res.h) ~= 0
  return res, ok
end
function Emu:close() if self.h then L.emu_close(self.h); self.h = nil end end

-- ---- Tier-2 idiomatic assertions: error() with a clear message on failure ----
function M.assert_ret(r, want)
  local got = r:ret()
  if got ~= want then error(string.format("ret: got %d, want %d", got, want)) end
end
function M.assert_abi_preserved(r)
  if not r:abi_preserved() then error("ABI not preserved") end
end
function M.assert_flag(r, name, set)
  if set == nil then set = true end
  if r:flag_set(name) ~= set then error("flag " .. name) end
end
function M.assert_fp(r, want)
  local got = r:fret()
  if got ~= want then error("fp: got " .. tostring(got) .. ", want " .. tostring(want)) end
end
function M.assert_no_fault(res)
  if res:faulted() then error("unexpected fault") end
end
function M.assert_emu_reg(res, name, want)
  local got = res:reg(name)
  if got ~= want then error(string.format("emu %s: got %d, want %d", name, got, want)) end
end

return M
