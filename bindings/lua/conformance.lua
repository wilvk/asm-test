-- conformance.lua — asm-test Lua binding (Track C), via LuaJIT's `ffi`.
--
-- LuaJIT consumes the binding ABI almost verbatim: `ffi.cdef` declares the
-- opaque-handle FFI helpers, `ffi.load` opens the shared libraries, and calls
-- go straight through. Replays the conformance corpus; exits nonzero on a
-- mismatch.
--
--   ASMTEST_LIB         libasmtest_emu.{so,dylib}
--   ASMTEST_CORPUS_LIB  libasmtest_corpus.{so,dylib}
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
int   asmtest_emu_result_faulted(void *r);
unsigned long long asmtest_emu_x86_reg(void *r, const char *name);
]])

local emu_path = assert(os.getenv("ASMTEST_LIB"), "set ASMTEST_LIB")
local corpus_path = assert(os.getenv("ASMTEST_CORPUS_LIB"), "set ASMTEST_CORPUS_LIB")
local L = ffi.load(emu_path)
local C = ffi.load(corpus_path)

local function routine(name) return C.asmtest_corpus_routine(name) end

local fails = 0
local total = 0
local function check(name, ok)
  total = total + 1
  if ok then
    print("ok - " .. name)
  else
    fails = fails + 1
    print("not ok - " .. name)
  end
end

local function withRegs(f)
  local r = L.asmtest_regs_new()
  local ok, err = pcall(f, r)
  L.asmtest_regs_free(r)
  if not ok then error(err) end
end

withRegs(function(r)
  L.asmtest_capture6(r, routine("add_signed"), 40, 2, 0, 0, 0, 0)
  check("add_signed.basic", tonumber(L.asmtest_regs_ret(r)) == 42 and L.asmtest_check_abi(r, nil, 0) == 0)
end)

withRegs(function(r)
  L.asmtest_capture6(r, routine("sum_via_rbx"), 20, 22, 0, 0, 0, 0)
  check("sum_via_rbx.abi_preserved", tonumber(L.asmtest_regs_ret(r)) == 42 and L.asmtest_check_abi(r, nil, 0) == 0)
end)

withRegs(function(r)
  L.asmtest_capture6(r, routine("clobbers_rbx"), 1, 2, 0, 0, 0, 0)
  check("clobbers_rbx.abi_violation_detected", L.asmtest_check_abi(r, nil, 0) ~= 0)
end)

withRegs(function(r)
  L.asmtest_capture6(r, routine("set_carry"), 0, 0, 0, 0, 0, 0)
  check("set_carry.cf_set", L.asmtest_regs_flag_set(r, "CF") == 1)
end)

withRegs(function(r)
  L.asmtest_capture6(r, routine("clear_carry"), 0, 0, 0, 0, 0, 0)
  check("clear_carry.cf_clear", L.asmtest_regs_flag_set(r, "CF") == 0)
end)

withRegs(function(r)
  L.asmtest_capture_fp2(r, routine("fp_add"), 1.5, 2.25)
  check("fp_add.basic", L.asmtest_regs_fret(r) == 3.75)
end)

do
  local e = L.emu_open()
  local res = L.asmtest_emu_result_new()
  L.asmtest_emu_call2(e, routine("add_signed"), 40, 2, res)
  check("emu.add_signed", L.asmtest_emu_result_faulted(res) == 0 and tonumber(L.asmtest_emu_x86_reg(res, "rax")) == 42)
  L.asmtest_emu_result_free(res)
  L.emu_close(e)
end

print(string.format("# %d passed, %d failed, %d total", total - fails, fails, total))
os.exit(fails == 0 and 0 or 1)
