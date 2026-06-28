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
size_t emu_disas(int arch, const uint8_t *code, size_t code_len, uint64_t base_addr, uint64_t off, char *buf, size_t buflen);
bool   emu_disas_available(void);
int   asmtest_emu_result_faulted(void *r);
unsigned long long asmtest_emu_result_fault_addr(void *r);
int   asmtest_emu_result_fault_kind(void *r);
unsigned long long asmtest_emu_x86_reg(void *r, const char *name);
double asmtest_emu_x86_xmm_f64(void *r, int index, int lane);
float  asmtest_emu_x86_xmm_f32(void *r, int index, int lane);

/* Extended x86 emulator calls (array form: explicit code + length). */
int emu_call(void *e, const void *code, size_t len, const long *args, int nargs, uint64_t mi, void *out);
int emu_call_fp(void *e, const void *code, size_t len, const long *ia, int ni, const double *fa, int nf, uint64_t mi, void *out);
int emu_call_vec(void *e, const void *code, size_t len, const long *ia, int ni, const void *va, int nv, uint64_t mi, void *out);
int emu_call_win64(void *e, const void *code, size_t len, const long *args, int nargs, uint64_t mi, void *out);
int emu_call_traced(void *e, const void *code, size_t len, const long *args, int nargs, uint64_t mi, void *out, void *trace);

/* Opaque execution-trace handle. */
void *asmtest_emu_trace_new(size_t ic, size_t bc);
void  asmtest_emu_trace_free(void *t);
int   asmtest_emu_trace_covered(void *t, uint64_t off);

/* Cross-arch guests (raw bytes, any host) + per-arch result accessors. */
void *emu_arm64_open(void); void emu_arm64_close(void *e);
int   emu_arm64_call(void *e, const void *code, size_t len, const long *args, int nargs, uint64_t mi, void *out);
int   emu_arm64_call_traced(void *e, const void *code, size_t len, const long *args, int nargs, uint64_t mi, void *out, void *trace);
void *asmtest_emu_arm64_result_new(void); void asmtest_emu_arm64_result_free(void *r);
unsigned long long asmtest_emu_arm64_reg(void *r, const char *name);
void *emu_riscv_open(void); void emu_riscv_close(void *e);
int   emu_riscv_call(void *e, const void *code, size_t len, const long *args, int nargs, uint64_t mi, void *out);
void *asmtest_emu_riscv_result_new(void); void asmtest_emu_riscv_result_free(void *r);
unsigned long long asmtest_emu_riscv_reg(void *r, const char *name);
void *emu_arm_open(void); void emu_arm_close(void *e);
int   emu_arm_call(void *e, const void *code, size_t len, const long *args, int nargs, uint64_t mi, void *out);
void *asmtest_emu_arm_result_new(void); void asmtest_emu_arm_result_free(void *r);
unsigned long long asmtest_emu_arm_reg(void *r, const char *name);

// Mid-execution guards (Track F)
int  emu_map(void *e, uint64_t addr, size_t size);
void emu_watch_writes(void *e, uint64_t addr, size_t size, int mode, void *w);
void emu_watch_clear(void *e);
int  emu_guard_reg(void *e, const char *name, uint64_t want, void *g);
void emu_guard_reg_clear(void *e);
void *asmtest_emu_watch_new(void); void asmtest_emu_watch_free(void *w);
int asmtest_emu_watch_violated(void *w);
unsigned long long asmtest_emu_watch_addr(void *w);
unsigned long long asmtest_emu_watch_rip_off(void *w);
void *asmtest_emu_reg_guard_new(void); void asmtest_emu_reg_guard_free(void *g);
int asmtest_emu_reg_guard_violated(void *g);
unsigned long long asmtest_emu_reg_guard_got(void *g);
// Coverage-guided fuzzing + mutation testing (Track E)
int emu_fuzz_cover1(void *e, const void *code, size_t len, long lo, long hi, uint64_t iters, uint64_t seed, void *uni, void *st);
size_t emu_mutation_test1(void *e, const void *code, size_t len, const long *in, size_t n, uint64_t maxm, uint64_t seed, void *st);
void *asmtest_emu_fuzz_stat_new(void); void asmtest_emu_fuzz_stat_free(void *s);
unsigned long long asmtest_emu_fuzz_blocks_reached(void *s);
unsigned long long asmtest_emu_fuzz_corpus_len(void *s);
void *asmtest_emu_mutation_stat_new(void); void asmtest_emu_mutation_stat_free(void *s);
unsigned long long asmtest_emu_mutation_killed(void *s);
unsigned long long asmtest_emu_mutation_survived(void *s);
// AVX2 256-bit capture (Track D)
void asm_call_capture_vec256(void *vec, void *fn, const long *iargs, const void *vargs);
int  asmtest_cpu_has_avx2(void);
]])

-- Resolve libasmtest_emu: an explicit ASMTEST_LIB wins (dev / custom build);
-- otherwise fall back to the native payload bundled in the rock at
-- native/<os>-<arch>/ next to this module (how the rock ships it).
local function resolve_emu_lib()
  local p = os.getenv("ASMTEST_LIB")
  if p and p ~= "" then return p end
  local os_name = ffi.os == "OSX" and "darwin" or "linux"
  local arch = ffi.arch == "arm64" and "arm64" or "x86_64" -- LuaJIT 'x64' -> x86_64
  local ext = ffi.os == "OSX" and "dylib" or "so"
  local dir = (debug.getinfo(1, "S").source:sub(2):match("(.*/)")) or "./"
  local bundled = dir .. "native/" .. os_name .. "-" .. arch .. "/libasmtest_emu." .. ext
  local f = io.open(bundled, "r")
  if f then f:close(); return bundled end
  error("set ASMTEST_LIB to libasmtest_emu." .. ext ..
        " (no bundled native/" .. os_name .. "-" .. arch .. "/ in this rock)")
end

local emu_path = resolve_emu_lib()
local corpus_path = os.getenv("ASMTEST_CORPUS_LIB")
local L = ffi.load(emu_path)
local C = corpus_path and ffi.load(corpus_path) or nil

-- The in-line assembler (Keystone) is carried by libasmtest_emu; still probe for
-- its symbol so the binding degrades cleanly against an older/leaner lib.
local HAS_ASM = pcall(function() return L.asmtest_emu_call_asm6 end)

-- The disassembler (Capstone) is carried by libasmtest_emu; still probe its
-- symbol so the binding degrades cleanly against an older/leaner lib.
local HAS_DISAS = pcall(function() return L.emu_disas end)

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
-- Mid-execution guard result handles (Track F).
local Watch = {}
Watch.__index = Watch
function Watch:violated() return L.asmtest_emu_watch_violated(self.h) ~= 0 end
function Watch:addr() return tonumber(L.asmtest_emu_watch_addr(self.h)) end
function Watch:rip_off() return tonumber(L.asmtest_emu_watch_rip_off(self.h)) end
function Watch:free() if self.h then L.asmtest_emu_watch_free(self.h); self.h = nil end end

local RegGuard = {}
RegGuard.__index = RegGuard
function RegGuard:violated() return L.asmtest_emu_reg_guard_violated(self.h) ~= 0 end
function RegGuard:got() return tonumber(L.asmtest_emu_reg_guard_got(self.h)) end
function RegGuard:free() if self.h then L.asmtest_emu_reg_guard_free(self.h); self.h = nil end end

local Emu = {}
Emu.__index = Emu
function M.Emu() return setmetatable({ h = L.emu_open() }, Emu) end
-- Run fn in the emulator with two integer args; returns an EmuResult.
function Emu:call2(fn, a0, a1)
  local res = new_result()
  L.asmtest_emu_call2(self.h, fn, a0, a1, res.h)
  return res
end
-- Marshal a Lua array of integers into a native long[] (nil, 0 when empty).
local function longs(args)
  local n = #args
  if n == 0 then return nil, 0 end
  return ffi.new("long[?]", n, args), n
end
-- Run raw x86-64 machine-code bytes (a Lua string) with up to six integer args.
function Emu:call_bytes(code, args)
  args = args or {}
  local res = new_result()
  local a, n = longs(args)
  L.emu_call(self.h, code, #code, a, n, 0, res.h)
  return res
end
-- Run raw bytes marshalling doubles into the FP arg registers (scalar return =
-- res:xmm_f64(0, 0)). opts.fargs is a Lua array of doubles.
function Emu:call_fp(code, opts)
  opts = opts or {}
  local fargs = opts.fargs or {}
  local res = new_result()
  local fa = #fargs > 0 and ffi.new("double[?]", #fargs, fargs) or nil
  L.emu_call_fp(self.h, code, #code, nil, 0, fa, #fargs, 0, res.h)
  return res
end
-- Run raw bytes marshalling 128-bit vectors (opts.vargs = array of four-lane arrays).
function Emu:call_vec(code, opts)
  opts = opts or {}
  local vargs = opts.vargs or {}
  local nv = #vargs
  local res = new_result()
  local va = nil
  if nv > 0 then
    va = ffi.new("float[?]", nv * 4)
    for i = 1, nv do for l = 1, 4 do va[(i - 1) * 4 + (l - 1)] = vargs[i][l] or 0 end end
  end
  L.emu_call_vec(self.h, code, #code, nil, 0, va, nv, 0, res.h)
  return res
end
-- Run raw bytes under the Microsoft x64 (Win64) convention.
function Emu:call_win64(code, args)
  args = args or {}
  local res = new_result()
  local a, n = longs(args)
  L.emu_call_win64(self.h, code, #code, a, n, 0, res.h)
  return res
end
-- Like :call_bytes, but record an execution trace / coverage into `trace`.
function Emu:call_traced(code, args, trace)
  args = args or {}
  local res = new_result()
  local a, n = longs(args)
  L.emu_call_traced(self.h, code, #code, a, n, 0, res.h, trace.h)
  return res
end
-- Mid-execution guards (Track F).
function Emu:map(addr, size) return L.emu_map(self.h, addr, size) ~= 0 end
function Emu:watch_writes(addr, size, mode)
  local w = setmetatable({ h = L.asmtest_emu_watch_new() }, Watch)
  L.emu_watch_writes(self.h, addr, size, mode == "never" and 0 or 1, w.h)
  return w
end
function Emu:watch_clear() L.emu_watch_clear(self.h) end
function Emu:guard_reg(name, want)
  local g = setmetatable({ h = L.asmtest_emu_reg_guard_new() }, RegGuard)
  if L.emu_guard_reg(self.h, name, want, g.h) == 0 then
    g:free(); error("unknown register: " .. name)
  end
  return g
end
function Emu:guard_reg_clear() L.emu_guard_reg_clear(self.h) end
-- Coverage-guided fuzzing + mutation testing (Track E).
function Emu:fuzz_cover(code, lo, hi, iters, seed)
  local uni = L.asmtest_emu_trace_new(0, 256)
  local st = L.asmtest_emu_fuzz_stat_new()
  L.emu_fuzz_cover1(self.h, code, #code, lo, hi, iters, seed or 0xC0FFEE, uni, st)
  local blocks = tonumber(L.asmtest_emu_fuzz_blocks_reached(st))
  local corpus = tonumber(L.asmtest_emu_fuzz_corpus_len(st))
  L.asmtest_emu_fuzz_stat_free(st); L.asmtest_emu_trace_free(uni)
  return blocks, corpus
end
function Emu:mutation_test(code, inputs, maxm, seed)
  local a, n = longs(inputs)
  local st = L.asmtest_emu_mutation_stat_new()
  L.emu_mutation_test1(self.h, code, #code, a, n, maxm or 0, seed or 0xABCD, st)
  local killed = tonumber(L.asmtest_emu_mutation_killed(st))
  local survived = tonumber(L.asmtest_emu_mutation_survived(st))
  L.asmtest_emu_mutation_stat_free(st)
  return killed, survived
end
-- AVX2 256-bit capture (Track D): vargs = array of four-double arrays; returns
-- the 4 f64 lanes of ymm0 (the vector return). Gate on M.cpu_has_avx2().
function M.cpu_has_avx2() return L.asmtest_cpu_has_avx2() ~= 0 end
function M.capture_vec256(fn, vargs)
  local out = ffi.new("uint8_t[?]", 16 * 32)
  local va = ffi.new("uint8_t[?]", 8 * 32)
  local dv = ffi.cast("double *", va)
  for i = 1, math.min(#vargs, 8) do
    for l = 1, 4 do dv[(i - 1) * 4 + (l - 1)] = vargs[i][l] or 0 end
  end
  local ia = ffi.new("long[6]")
  L.asm_call_capture_vec256(out, fn, ia, va)
  local dout = ffi.cast("double *", out)
  return { dout[0], dout[1], dout[2], dout[3] }
end
-- Whether the loaded native lib carries the in-line assembler.
function Emu:asm_available() return HAS_ASM end
-- The Keystone diagnostic from the most recent assemble ("" on success).
function M.asm_error() return HAS_ASM and ffi.string(L.asmtest_asm_last_error()) or "" end

-- Whether the loaded native lib carries the disassembler (Capstone). True for
-- libasmtest_emu (the superset); only an older/leaner lib returns false.
function M.disas_available() return HAS_DISAS and L.emu_disas_available() end
-- Disassemble the one instruction at byte `off` of `code` (a byte string) for
-- `arch` (0=x86-64, 1=arm64, 2=riscv64, 3=arm32; mirrors emu_arch_t). `base` is
-- the address the bytes run at (EMU_CODE_BASE) so PC-relative operands resolve.
-- Returns "mnemonic operands", or "" with no disassembler / on a decode miss.
function M.disas(code, off, arch, base)
  if not M.disas_available() then return "" end
  local buf = ffi.new("char[?]", 160)
  local n = L.emu_disas(arch or 0, code, #code, base or 0x00100000, off or 0, buf, 160)
  return n ~= 0 and ffi.string(buf) or ""
end
-- Assemble x86-64 `src` in `opts.syntax` (0=Intel, 1=AT&T, 2=NASM, 3=MASM,
-- 4=GAS; see M.Syntax) via Keystone and run
-- it with the integer `args` (a table of up to six), stopping after
-- `opts.max_insns` instructions (0 = run to `ret`). Returns the EmuResult;
-- error()s with the Keystone diagnostic if it fails to assemble. libasmtest_emu
-- carries the assembler, so this works by default; guard with :asm_available()
-- for an older/leaner lib.
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

-- An opaque execution-trace / basic-block coverage recorder. Call :free() when done.
local Trace = {}
Trace.__index = Trace
function M.Trace(insns_cap, blocks_cap)
  return setmetatable({ h = L.asmtest_emu_trace_new(insns_cap or 4096, blocks_cap or 4096) }, Trace)
end
-- True if the basic block at byte-offset `off` (from the routine entry) was entered.
function Trace:covered(off) return L.asmtest_emu_trace_covered(self.h, off) ~= 0 end
function Trace:free() if self.h then L.asmtest_emu_trace_free(self.h); self.h = nil end end

-- A cross-arch run's outcome; registers are read by name. Call :free() when done.
local GuestResult = {}
GuestResult.__index = GuestResult
local function new_guest_result(arch)
  return setmetatable({ h = L["asmtest_emu_" .. arch .. "_result_new"](), arch = arch }, GuestResult)
end
function GuestResult:faulted() return L.asmtest_emu_result_faulted(self.h) ~= 0 end
-- Guest register by name (e.g. "x0"/"sp", "a0"/"x10", "r0").
function GuestResult:reg(name) return tonumber(L["asmtest_emu_" .. self.arch .. "_reg"](self.h, name)) end
function GuestResult:free()
  if self.h then L["asmtest_emu_" .. self.arch .. "_result_free"](self.h); self.h = nil end
end

-- A cross-arch Unicorn guest ("arm64"/"riscv"/"arm") running raw machine-code
-- bytes — emulated regardless of host arch. Call :close() when done.
local Guest = {}
Guest.__index = Guest
function M.Guest(arch)
  return setmetatable({ h = L["emu_" .. arch .. "_open"](), arch = arch }, Guest)
end
-- Run raw bytes (a Lua string) with integer args in the guest ABI registers.
function Guest:call(code, args)
  args = args or {}
  local res = new_guest_result(self.arch)
  local a, n = longs(args)
  L["emu_" .. self.arch .. "_call"](self.h, code, #code, a, n, 0, res.h)
  return res
end
-- Like :call, but record an execution trace / coverage into `trace` (arm64).
function Guest:call_traced(code, args, trace)
  assert(self.arch == "arm64", "traced guest run only wired for arm64")
  args = args or {}
  local res = new_guest_result(self.arch)
  local a, n = longs(args)
  L.emu_arm64_call_traced(self.h, code, #code, a, n, 0, res.h, trace.h)
  return res
end
function Guest:close() if self.h then L["emu_" .. self.arch .. "_close"](self.h); self.h = nil end end

-- Architecture / syntax codes for M.assemble (mirror asm_arch_t / asm_syntax_t).
M.Arch = { X86_64 = 0, ARM64 = 1, RISCV64 = 2, ARM32 = 3 }
M.Syntax = { INTEL = 0, ATT = 1, NASM = 2, MASM = 3, GAS = 4 }
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
function M.assert_guest_reg(res, name, want)
  local got = res:reg(name)
  if got ~= want then error(string.format("guest %s: got %d, want %d", name, got, want)) end
end
function M.assert_covered(trace, off)
  if not trace:covered(off) then error(string.format("block %d: expected covered", off)) end
end

return M
