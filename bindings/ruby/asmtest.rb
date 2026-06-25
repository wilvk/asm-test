# asmtest.rb — asm-test Ruby binding (Track C): the reusable library module.
#
# This is the module a Ruby project requires; it keeps all Fiddle FFI inside, so
# calling code never declares a native entry point. It drives the opaque-handle
# FFI layer (src/ffi.c), so no C struct layout is mirrored: Asmtest::Regs with
# capture6 / capture_fp2 + accessors, Asmtest::Emu / EmuResult for the emulator
# (faults as data), and Asmtest.assert_* helpers that raise Asmtest::Error.
#
# The shared libraries are taken from the environment, matching how the
# framework's Makefile invokes the bindings:
#   ASMTEST_LIB         libasmtest_emu.{so,dylib}    (capture + emulator + accessors)
#   ASMTEST_CORPUS_LIB  libasmtest_corpus.{so,dylib} (the canonical fixtures)
require "fiddle"

module Asmtest
  # Raised by the assert_* helpers on a failed check.
  class Error < StandardError; end

  emu_path = ENV["ASMTEST_LIB"] or raise "set ASMTEST_LIB to libasmtest_emu.{so,dylib}"
  corpus_path = ENV["ASMTEST_CORPUS_LIB"]

  L = Fiddle.dlopen(emu_path)
  C = corpus_path ? Fiddle.dlopen(corpus_path) : nil

  VOIDP = Fiddle::TYPE_VOIDP
  LONG  = Fiddle::TYPE_LONG
  INT   = Fiddle::TYPE_INT
  DBL   = Fiddle::TYPE_DOUBLE
  LL    = Fiddle::TYPE_LONG_LONG
  SZ    = Fiddle::TYPE_SIZE_T
  VOID  = Fiddle::TYPE_VOID

  def self.func(lib, name, args, ret)
    Fiddle::Function.new(lib[name], args, ret)
  end

  # Like func, but returns nil if the symbol is absent — for optional entry
  # points such as the in-line assembler (present only in the emu+asm lib).
  def self.func_opt(lib, name, args, ret)
    func(lib, name, args, ret)
  rescue Fiddle::DLError
    nil
  end

  # All native entry points, bound once and kept private to the module.
  FN = {
    corpus_routine: (C && func(C, "asmtest_corpus_routine", [VOIDP], VOIDP)),
    regs_new:       func(L, "asmtest_regs_new", [], VOIDP),
    regs_free:      func(L, "asmtest_regs_free", [VOIDP], VOID),
    capture6:       func(L, "asmtest_capture6", [VOIDP, VOIDP, LONG, LONG, LONG, LONG, LONG, LONG], VOID),
    capture_fp2:    func(L, "asmtest_capture_fp2", [VOIDP, VOIDP, DBL, DBL], VOID),
    regs_ret:       func(L, "asmtest_regs_ret", [VOIDP], LONG),
    regs_fret:      func(L, "asmtest_regs_fret", [VOIDP], DBL),
    regs_flag_set:  func(L, "asmtest_regs_flag_set", [VOIDP, VOIDP], INT),
    check_abi:      func(L, "asmtest_check_abi", [VOIDP, VOIDP, SZ], INT),
    emu_open:       func(L, "emu_open", [], VOIDP),
    emu_close:      func(L, "emu_close", [VOIDP], VOID),
    emu_res_new:    func(L, "asmtest_emu_result_new", [], VOIDP),
    emu_res_free:   func(L, "asmtest_emu_result_free", [VOIDP], VOID),
    emu_call2:      func(L, "asmtest_emu_call2", [VOIDP, VOIDP, LONG, LONG, VOIDP], INT),
    # Optional (emu+asm lib only): widened run shim (six scalars + syntax + cap),
    # multi-arch text->bytes, and the thread-local diagnostic.
    emu_call_asm6:  func_opt(L, "asmtest_emu_call_asm6", [VOIDP, VOIDP, INT, LONG, LONG, LONG, LONG, LONG, LONG, INT, LL, VOIDP], INT),
    asm_bytes:      func_opt(L, "asmtest_asm_bytes", [INT, INT, VOIDP, LL, VOIDP, INT], INT),
    asm_last_error: func_opt(L, "asmtest_asm_last_error", [], VOIDP),
    emu_faulted:    func(L, "asmtest_emu_result_faulted", [VOIDP], INT),
    emu_reg:        func(L, "asmtest_emu_x86_reg", [VOIDP, VOIDP], LL),
  }.freeze

  # Architecture / syntax codes for Asmtest.assemble (mirror the C enums).
  ARCH   = { x86_64: 0, arm64: 1, riscv64: 2, arm32: 3 }.freeze
  SYNTAX = { intel: 0, att: 1 }.freeze

  # The Keystone diagnostic from the most recent assemble ("" on success).
  def self.asm_error
    FN[:asm_last_error] ? FN[:asm_last_error].call.to_s : ""
  end

  # Assemble +src+ for +arch+/+syntax+ at load address +addr+ and return the
  # machine-code bytes (a binary String). Multi-arch (unlike Emu#call_asm, which
  # runs on the x86-64 guest). Raises Error with the Keystone diagnostic on failure.
  def self.assemble(src, arch: 0, syntax: 0, addr: 0x00100000)
    raise Error, "in-line assembler not in this build" unless FN[:asm_bytes]
    cap = 256
    buf = ("\x00".b * cap)
    n = FN[:asm_bytes].call(arch, syntax, src, addr, buf, cap)
    raise Error, "assemble failed: #{asm_error}" if n.zero?
    if n > cap
      buf = ("\x00".b * n)
      n = FN[:asm_bytes].call(arch, syntax, src, addr, buf, n)
    end
    buf[0, n]
  end

  # Resolve a canonical corpus routine (e.g. "add_signed") to its address.
  def self.corpus_routine(name)
    raise "set ASMTEST_CORPUS_LIB to use corpus_routine" unless FN[:corpus_routine]
    FN[:corpus_routine].call(name)
  end

  # A captured register/flags snapshot. Call #free when done.
  class Regs
    def initialize
      @h = FN[:regs_new].call
    end

    # Call fn through the real ABI with up to six integer args.
    def capture6(fn, a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0)
      FN[:capture6].call(@h, fn, a0, a1, a2, a3, a4, a5)
    end

    # Call fn with two double args, capturing the FP return.
    def capture_fp2(fn, f0, f1)
      FN[:capture_fp2].call(@h, fn, f0, f1)
    end

    def ret
      FN[:regs_ret].call(@h) # integer return (rax / x0)
    end

    def fret
      FN[:regs_fret].call(@h) # scalar FP return (xmm0 / d0)
    end

    def flag_set?(name)
      FN[:regs_flag_set].call(@h, name) == 1
    end

    def abi_preserved?
      FN[:check_abi].call(@h, nil, 0) == 0
    end

    def free
      FN[:regs_free].call(@h) if @h
      @h = nil
    end
  end

  # An emulator run's outcome — faults surfaced as data, not a crash.
  class EmuResult
    attr_reader :h

    def initialize
      @h = FN[:emu_res_new].call
    end

    def faulted?
      FN[:emu_faulted].call(@h) != 0
    end

    def reg(name)
      FN[:emu_reg].call(@h, name) # x86-64 guest register by name
    end

    def free
      FN[:emu_res_free].call(@h) if @h
      @h = nil
    end
  end

  # An open emulator (x86-64 Unicorn guest). Call #close when done.
  class Emu
    def initialize
      @h = FN[:emu_open].call
    end

    # Run fn in the emulator with two integer args; returns an EmuResult.
    def call2(fn, a0, a1)
      res = EmuResult.new
      FN[:emu_call2].call(@h, fn, a0, a1, res.h)
      res
    end

    # Whether the loaded native lib has the in-line assembler (Keystone).
    def asm_available?
      !FN[:emu_call_asm6].nil?
    end

    # Assemble x86-64 +src+ in +syntax+ (0=Intel, 1=AT&T) via Keystone and run it
    # with the integer +args+ (up to six), stopping after +max_insns+ instructions
    # (0 = run to +ret+). Returns the EmuResult; raises Error carrying the Keystone
    # diagnostic if it fails to assemble. Only when #asm_available? — needs the
    # emu+asm native lib.
    def call_asm(src, args = [], syntax: 0, max_insns: 0)
      raise Error, "in-line assembler not in this build" unless asm_available?
      a = Array.new(6, 0)
      nargs = [args.length, 6].min
      nargs.times { |i| a[i] = args[i] }
      res = EmuResult.new
      ok = FN[:emu_call_asm6].call(@h, src, syntax, a[0], a[1], a[2], a[3], a[4], a[5],
                                   nargs, max_insns, res.h) != 0
      unless ok
        res.free
        raise Error, "in-line assembly failed: #{Asmtest.asm_error}"
      end
      res
    end

    def close
      FN[:emu_close].call(@h) if @h
      @h = nil
    end
  end

  # ---- Tier-2 idiomatic assertions: raise Error with a message on failure ----
  def self.assert_ret(r, want)
    got = r.ret
    raise Error, "ret: got #{got}, want #{want}" unless got == want
  end

  def self.assert_abi_preserved(r)
    raise Error, "ABI not preserved" unless r.abi_preserved?
  end

  def self.assert_flag(r, name, set = true)
    raise Error, "flag #{name}: want #{set}" unless r.flag_set?(name) == set
  end

  def self.assert_fp(r, want)
    got = r.fret
    raise Error, "fp: got #{got}, want #{want}" unless got == want
  end

  def self.assert_no_fault(res)
    raise Error, "unexpected fault" if res.faulted?
  end

  def self.assert_emu_reg(res, name, want)
    got = res.reg(name)
    raise Error, "emu #{name}: got #{got}, want #{want}" unless got == want
  end
end
