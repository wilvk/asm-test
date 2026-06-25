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
void  asmtest_capture_vec_f32(void *out, void *fn, float *lanes, int nvec);
unsigned long asmtest_regs_ret(void *r);
double asmtest_regs_fret(void *r);
float asmtest_regs_vec_f32(void *r, int index, int lane);
int   asmtest_regs_flag_set(void *r, const char *name);
int   asmtest_check_abi(void *r, char *msg, size_t n);
void *emu_open(void);
void  emu_close(void *e);
void *asmtest_emu_result_new(void);
void  asmtest_emu_result_free(void *r);
int   asmtest_emu_call2(void *e, void *fn, long a0, long a1, void *out);
int   asmtest_emu_call_asm6(void *e, const char *src, int syntax, long a0, long a1, long a2, long a3, long a4, long a5, int nargs, uint64_t max_insns, void *out);
int   asmtest_asm_bytes(int arch, int syntax, const char *src, uint64_t addr, uint8_t *buf, int cap);
const char *asmtest_asm_last_error(void);
int   asmtest_emu_result_faulted(void *r);
unsigned long long asmtest_emu_result_fault_addr(void *r);
int   asmtest_emu_result_fault_kind(void *r);
unsigned long long asmtest_emu_x86_reg(void *r, const char *name);
double asmtest_emu_x86_xmm_f64(void *r, int index, int lane);
float  asmtest_emu_x86_xmm_f32(void *r, int index, int lane);
]])

local emu_path = assert(os.getenv("ASMTEST_LIB"), "set ASMTEST_LIB to libasmtest_emu.{so,dylib}")
local corpus_path = os.getenv("ASMTEST_CORPUS_LIB")
local L = ffi.load(emu_path)
local C = corpus_path and ffi.load(corpus_path) or nil

-- The in-line assembler (Keystone) is present only in the emu+asm lib; probe for
-- its symbol so the binding degrades cleanly against the plain libasmtest_emu.
local HAS_ASM = pcall(function() return L.asmtest_emu_call_asm6 end)

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
-- Call fn with up to eight 128-bit vector args, capturing the vector register
-- file. `vectors` is a table of four-float32-lane tables; the vector return is
-- read back with :vec_f32(0).
function Regs:capture_vec_f32(fn, vectors)
  local n = #vectors
  local lanes = ffi.new("float[?]", n * 4)
  for i = 1, n do
    for l = 1, 4 do lanes[(i - 1) * 4 + (l - 1)] = vectors[i][l] or 0 end
  end
  L.asmtest_capture_vec_f32(self.h, fn, lanes, n)
end
function Regs:ret() return tonumber(L.asmtest_regs_ret(self.h)) end       -- rax / x0
function Regs:fret() return L.asmtest_regs_fret(self.h) end               -- xmm0 / d0
-- The four float32 lanes of vector register `index` (0 = the vector return).
function Regs:vec_f32(index)
  index = index or 0
  local out = {}
  for lane = 0, 3 do out[lane + 1] = L.asmtest_regs_vec_f32(self.h, index, lane) end
  return out
end
function Regs:flag_set(name) return L.asmtest_regs_flag_set(self.h, name) == 1 end
function Regs:abi_preserved() return L.asmtest_check_abi(self.h, nil, 0) == 0 end
function Regs:free() if self.h then L.asmtest_regs_free(self.h); self.h = nil end end

-- An emulator run's outcome — faults surfaced as data, not a crash.
local EmuResult = {}
EmuResult.__index = EmuResult
local function new_result() return setmetatable({ h = L.asmtest_emu_result_new() }, EmuResult) end
function EmuResult:faulted() return L.asmtest_emu_result_faulted(self.h) ~= 0 end
-- Faulting guest address; only meaningful when :faulted().
function EmuResult:fault_addr() return tonumber(L.asmtest_emu_result_fault_addr(self.h)) end
-- Why the access was invalid (an M.FaultKind value); only meaningful when :faulted().
function EmuResult:fault_kind() return L.asmtest_emu_result_fault_kind(self.h) end
function EmuResult:reg(name) return tonumber(L.asmtest_emu_x86_reg(self.h, name)) end  -- GP reg + rip/rflags
-- Lane (0..1) of guest XMM register index as a double (scalar return = :xmm_f64(0, 0)).
function EmuResult:xmm_f64(index, lane) return tonumber(L.asmtest_emu_x86_xmm_f64(self.h, index or 0, lane or 0)) end
-- Lane (0..3) of guest XMM register index as a float32.
function EmuResult:xmm_f32(index, lane) return tonumber(L.asmtest_emu_x86_xmm_f32(self.h, index or 0, lane or 0)) end
function EmuResult:free() if self.h then L.asmtest_emu_result_free(self.h); self.h = nil end end

-- Invalid-access kind reported by EmuResult:fault_kind() (mirrors emu_fault_kind_t).
M.FaultKind = { NONE = 0, READ = 1, WRITE = 2, FETCH = 3 }

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
-- Whether the loaded native lib carries the in-line assembler.
function Emu:asm_available() return HAS_ASM end
-- The Keystone diagnostic from the most recent assemble ("" on success).
function M.asm_error() return HAS_ASM and ffi.string(L.asmtest_asm_last_error()) or "" end
-- Assemble x86-64 `src` in `opts.syntax` (0=Intel, 1=AT&T) via Keystone and run
-- it with the integer `args` (a table of up to six), stopping after
-- `opts.max_insns` instructions (0 = run to `ret`). Returns the EmuResult;
-- error()s with the Keystone diagnostic if it fails to assemble. Only when
-- :asm_available() — needs the emu+asm lib.
function Emu:call_asm(src, args, opts)
  assert(HAS_ASM, "in-line assembler not in this build")
  args, opts = args or {}, opts or {}
  local a = {}
  for i = 1, 6 do a[i] = args[i] or 0 end
  local res = new_result()
  local ok = L.asmtest_emu_call_asm6(self.h, src, opts.syntax or 0,
    a[1], a[2], a[3], a[4], a[5], a[6], math.min(#args, 6), opts.max_insns or 0, res.h) ~= 0
  if not ok then res:free(); error("in-line assembly failed: " .. M.asm_error()) end
  return res
end
function Emu:close() if self.h then L.emu_close(self.h); self.h = nil end end

-- Architecture / syntax codes for M.assemble (mirror asm_arch_t / asm_syntax_t).
M.Arch = { X86_64 = 0, ARM64 = 1, RISCV64 = 2, ARM32 = 3 }
M.Syntax = { INTEL = 0, ATT = 1 }
-- Assemble `src` for `arch`/`syntax` at load address `addr` and return the
-- machine-code bytes (a Lua string). Multi-arch (unlike :call_asm, which runs on
-- the x86-64 guest). error()s with the Keystone diagnostic on failure.
function M.assemble(src, arch, syntax, addr)
  assert(HAS_ASM, "in-line assembler not in this build")
  arch, syntax, addr = arch or 0, syntax or 0, addr or 0x00100000
  local cap = 256
  local buf = ffi.new("uint8_t[?]", cap)
  local n = L.asmtest_asm_bytes(arch, syntax, src, addr, buf, cap)
  if n == 0 then error("assemble failed: " .. M.asm_error()) end
  if n > cap then buf = ffi.new("uint8_t[?]", n); n = L.asmtest_asm_bytes(arch, syntax, src, addr, buf, n) end
  return ffi.string(buf, n)
end

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
function M.assert_vec_f32(r, index, want)
  local got = r:vec_f32(index)
  for i = 1, #want do
    if got[i] ~= want[i] then
      error(string.format("vec[%d] lane %d: got %s, want %s", index, i - 1, tostring(got[i]), tostring(want[i])))
    end
  end
end
function M.assert_no_fault(res)
  if res:faulted() then error("unexpected fault") end
end
function M.assert_fault(res)
  if not res:faulted() then error("expected a fault, but the run completed cleanly") end
end
function M.assert_emu_reg(res, name, want)
  local got = res:reg(name)
  if got ~= want then error(string.format("emu %s: got %d, want %d", name, got, want)) end
end

return M
