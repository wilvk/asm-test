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
  FLT   = Fiddle::TYPE_FLOAT
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
    capture_vec_f32: func(L, "asmtest_capture_vec_f32", [VOIDP, VOIDP, VOIDP, INT], VOID),
    regs_ret:       func(L, "asmtest_regs_ret", [VOIDP], LONG),
    regs_fret:      func(L, "asmtest_regs_fret", [VOIDP], DBL),
    regs_vec_f32:   func(L, "asmtest_regs_vec_f32", [VOIDP, INT, INT], FLT),
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
    # Optional (emu+full lib only): the Capstone disassembler — decode one
    # instruction at +off+ into a text buffer; the availability probe self-skips.
    emu_disas:       func_opt(L, "emu_disas", [INT, VOIDP, LL, LL, LL, VOIDP, LL], LL),
    emu_disas_avail: func_opt(L, "emu_disas_available", [], INT),
    emu_faulted:    func(L, "asmtest_emu_result_faulted", [VOIDP], INT),
    emu_fault_addr: func(L, "asmtest_emu_result_fault_addr", [VOIDP], LL),
    emu_fault_kind: func(L, "asmtest_emu_result_fault_kind", [VOIDP], INT),
    emu_reg:        func(L, "asmtest_emu_x86_reg", [VOIDP, VOIDP], LL),
    emu_xmm_f64:    func(L, "asmtest_emu_x86_xmm_f64", [VOIDP, INT, INT], DBL),
    emu_xmm_f32:    func(L, "asmtest_emu_x86_xmm_f32", [VOIDP, INT, INT], FLT),
    # Extended x86 emulator calls (array form: explicit code + length, so raw
    # machine-code bytes run directly). Returns are unused (read from the result).
    emu_call:        func(L, "emu_call", [VOIDP, VOIDP, SZ, VOIDP, INT, LL, VOIDP], INT),
    emu_call_fp:     func(L, "emu_call_fp", [VOIDP, VOIDP, SZ, VOIDP, INT, VOIDP, INT, LL, VOIDP], INT),
    emu_call_vec:    func(L, "emu_call_vec", [VOIDP, VOIDP, SZ, VOIDP, INT, VOIDP, INT, LL, VOIDP], INT),
    emu_call_win64:  func(L, "emu_call_win64", [VOIDP, VOIDP, SZ, VOIDP, INT, LL, VOIDP], INT),
    emu_call_traced: func(L, "emu_call_traced", [VOIDP, VOIDP, SZ, VOIDP, INT, LL, VOIDP, VOIDP], INT),
    # Opaque execution-trace handle.
    trace_new:       func(L, "asmtest_emu_trace_new", [SZ, SZ], VOIDP),
    trace_free:      func(L, "asmtest_emu_trace_free", [VOIDP], VOID),
    trace_covered:   func(L, "asmtest_emu_trace_covered", [VOIDP, LL], INT),
    # Cross-arch guests (raw bytes, any host): open/close/call + per-arch result.
    emu_arm64_open:  func(L, "emu_arm64_open", [], VOIDP),
    emu_arm64_close: func(L, "emu_arm64_close", [VOIDP], VOID),
    emu_arm64_call:  func(L, "emu_arm64_call", [VOIDP, VOIDP, SZ, VOIDP, INT, LL, VOIDP], INT),
    emu_arm64_call_traced: func(L, "emu_arm64_call_traced", [VOIDP, VOIDP, SZ, VOIDP, INT, LL, VOIDP, VOIDP], INT),
    arm64_res_new:   func(L, "asmtest_emu_arm64_result_new", [], VOIDP),
    arm64_res_free:  func(L, "asmtest_emu_arm64_result_free", [VOIDP], VOID),
    arm64_reg:       func(L, "asmtest_emu_arm64_reg", [VOIDP, VOIDP], LL),
    emu_riscv_open:  func(L, "emu_riscv_open", [], VOIDP),
    emu_riscv_close: func(L, "emu_riscv_close", [VOIDP], VOID),
    emu_riscv_call:  func(L, "emu_riscv_call", [VOIDP, VOIDP, SZ, VOIDP, INT, LL, VOIDP], INT),
    riscv_res_new:   func(L, "asmtest_emu_riscv_result_new", [], VOIDP),
    riscv_res_free:  func(L, "asmtest_emu_riscv_result_free", [VOIDP], VOID),
    riscv_reg:       func(L, "asmtest_emu_riscv_reg", [VOIDP, VOIDP], LL),
    emu_arm_open:    func(L, "emu_arm_open", [], VOIDP),
    emu_arm_close:   func(L, "emu_arm_close", [VOIDP], VOID),
    emu_arm_call:    func(L, "emu_arm_call", [VOIDP, VOIDP, SZ, VOIDP, INT, LL, VOIDP], INT),
    arm_res_new:     func(L, "asmtest_emu_arm_result_new", [], VOIDP),
    arm_res_free:    func(L, "asmtest_emu_arm_result_free", [VOIDP], VOID),
    arm_reg:         func(L, "asmtest_emu_arm_reg", [VOIDP, VOIDP], LL),
    # Mid-execution guards (Track F): arm directly; results via opaque handles.
    emu_map:         func(L, "emu_map", [VOIDP, LL, SZ], INT),
    watch_writes:    func(L, "emu_watch_writes", [VOIDP, LL, SZ, INT, VOIDP], VOID),
    watch_clear:     func(L, "emu_watch_clear", [VOIDP], VOID),
    guard_reg:       func(L, "emu_guard_reg", [VOIDP, VOIDP, LL, VOIDP], INT),
    guard_reg_clear: func(L, "emu_guard_reg_clear", [VOIDP], VOID),
    watch_new:       func(L, "asmtest_emu_watch_new", [], VOIDP),
    watch_free:      func(L, "asmtest_emu_watch_free", [VOIDP], VOID),
    watch_violated:  func(L, "asmtest_emu_watch_violated", [VOIDP], INT),
    watch_addr:      func(L, "asmtest_emu_watch_addr", [VOIDP], LL),
    watch_rip:       func(L, "asmtest_emu_watch_rip_off", [VOIDP], LL),
    rguard_new:      func(L, "asmtest_emu_reg_guard_new", [], VOIDP),
    rguard_free:     func(L, "asmtest_emu_reg_guard_free", [VOIDP], VOID),
    rguard_violated: func(L, "asmtest_emu_reg_guard_violated", [VOIDP], INT),
    rguard_got:      func(L, "asmtest_emu_reg_guard_got", [VOIDP], LL),
    # Coverage-guided fuzzing + mutation testing (Track E).
    fuzz_cover1:     func(L, "emu_fuzz_cover1", [VOIDP, VOIDP, SZ, LONG, LONG, LL, LL, VOIDP, VOIDP], INT),
    mutation_test1:  func(L, "emu_mutation_test1", [VOIDP, VOIDP, SZ, VOIDP, SZ, LL, LL, VOIDP], SZ),
    fuzz_new:        func(L, "asmtest_emu_fuzz_stat_new", [], VOIDP),
    fuzz_free:       func(L, "asmtest_emu_fuzz_stat_free", [VOIDP], VOID),
    fuzz_blocks:     func(L, "asmtest_emu_fuzz_blocks_reached", [VOIDP], LL),
    fuzz_corpus:     func(L, "asmtest_emu_fuzz_corpus_len", [VOIDP], LL),
    mut_new:         func(L, "asmtest_emu_mutation_stat_new", [], VOIDP),
    mut_free:        func(L, "asmtest_emu_mutation_stat_free", [VOIDP], VOID),
    mut_killed:      func(L, "asmtest_emu_mutation_killed", [VOIDP], LL),
    mut_survived:    func(L, "asmtest_emu_mutation_survived", [VOIDP], LL),
    # AVX2 256-bit capture (Track D) + the CPUID probe that gates it.
    capture_vec256:  func(L, "asm_call_capture_vec256", [VOIDP, VOIDP, VOIDP, VOIDP], VOID),
    cpu_avx2:        func(L, "asmtest_cpu_has_avx2", [], INT),
  }.freeze

  # Invalid-access kind reported by EmuResult#fault_kind (mirrors emu_fault_kind_t).
  module FaultKind
    NONE = 0
    READ = 1
    WRITE = 2
    FETCH = 3
  end

  # Architecture / syntax codes for Asmtest.assemble (mirror the C enums).
  ARCH   = { x86_64: 0, arm64: 1, riscv64: 2, arm32: 3 }.freeze
  SYNTAX = { intel: 0, att: 1, nasm: 2, masm: 3, gas: 4 }.freeze

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

  # Whether the loaded native lib carries the disassembler (Capstone). True only
  # for libasmtest_emu_full; the lean libasmtest_emu / _emu_asm return false.
  def self.disas_available?
    !FN[:emu_disas].nil? && FN[:emu_disas_avail].call != 0
  end

  # Disassemble the one instruction at byte +off+ of +code+ (a binary String) for
  # +arch+ (0=x86-64, 1=arm64, 2=riscv64, 3=arm32; mirrors emu_arch_t). +base+ is
  # the address the bytes run at (EMU_CODE_BASE) so PC-relative operands resolve.
  # Returns "mnemonic operands", or "" with no disassembler / on a decode miss.
  def self.disas(code, off = 0, arch: 0, base: 0x00100000)
    return "" unless disas_available?
    buf = ("\x00".b * 160)
    n = FN[:emu_disas].call(arch, code, code.bytesize, base, off, buf, 160)
    n.zero? ? "" : buf.unpack1("Z*")
  end

  # Resolve a canonical corpus routine (e.g. "add_signed") to its address.
  def self.corpus_routine(name)
    raise "set ASMTEST_CORPUS_LIB to use corpus_routine" unless FN[:corpus_routine]
    FN[:corpus_routine].call(name)
  end

  # Pack integer args into a native long array (a binary String Fiddle passes as a
  # pointer), or nil when there are none.
  def self.pack_longs(args)
    args.empty? ? nil : args.pack("q*")
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

    # Call fn with up to eight 128-bit vector args, capturing the vector register
    # file. +vectors+ is an array of four-float32-lane arrays; the vector return
    # is read back with vec_f32(0).
    def capture_vec_f32(fn, vectors)
      lanes = vectors.flat_map { |v| (0..3).map { |i| (v[i] || 0).to_f } }
      FN[:capture_vec_f32].call(@h, fn, lanes.pack("f*"), vectors.length)
    end

    def ret
      FN[:regs_ret].call(@h) # integer return (rax / x0)
    end

    def fret
      FN[:regs_fret].call(@h) # scalar FP return (xmm0 / d0)
    end

    # The four float32 lanes of vector register +index+ (0 = the vector return).
    def vec_f32(index = 0)
      (0..3).map { |lane| FN[:regs_vec_f32].call(@h, index, lane) }
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

    # Faulting guest address; only meaningful when #faulted?.
    def fault_addr
      FN[:emu_fault_addr].call(@h)
    end

    # Why the access was invalid (a FaultKind value); only meaningful when #faulted?.
    def fault_kind
      FN[:emu_fault_kind].call(@h)
    end

    def reg(name)
      FN[:emu_reg].call(@h, name) # GP register plus "rip" / "rflags" by name
    end

    # Lane (0..1) of guest XMM register +index+ as a double (scalar return = xmm_f64(0, 0)).
    def xmm_f64(index = 0, lane = 0)
      FN[:emu_xmm_f64].call(@h, index, lane)
    end

    # Lane (0..3) of guest XMM register +index+ as a float32.
    def xmm_f32(index = 0, lane = 0)
      FN[:emu_xmm_f32].call(@h, index, lane)
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

    # Run raw x86-64 machine-code bytes (a binary String) with up to six integer
    # args; returns an EmuResult.
    def call_bytes(code, args = [])
      res = EmuResult.new
      FN[:emu_call].call(@h, code, code.bytesize, Asmtest.pack_longs(args), args.length, 0, res.h)
      res
    end

    # Run raw bytes marshalling doubles into the FP arg registers (scalar return =
    # res.xmm_f64(0, 0)).
    def call_fp(code, iargs: [], fargs: [])
      res = EmuResult.new
      fa = fargs.empty? ? nil : fargs.map(&:to_f).pack("d*")
      FN[:emu_call_fp].call(@h, code, code.bytesize, Asmtest.pack_longs(iargs), iargs.length,
                            fa, fargs.length, 0, res.h)
      res
    end

    # Run raw bytes marshalling 128-bit vectors (arrays of four float32 lanes) into
    # xmm0..7.
    def call_vec(code, iargs: [], vargs: [])
      res = EmuResult.new
      lanes = vargs.flat_map { |v| (0..3).map { |i| (v[i] || 0).to_f } }
      va = vargs.empty? ? nil : lanes.pack("f*")
      FN[:emu_call_vec].call(@h, code, code.bytesize, Asmtest.pack_longs(iargs), iargs.length,
                             va, vargs.length, 0, res.h)
      res
    end

    # Run raw bytes under the Microsoft x64 (Win64) convention (args in rcx, rdx,
    # r8, r9), so a Win64 routine can be tested on a System V host.
    def call_win64(code, args = [])
      res = EmuResult.new
      FN[:emu_call_win64].call(@h, code, code.bytesize, Asmtest.pack_longs(args), args.length, 0, res.h)
      res
    end

    # Like call_bytes, but record an execution trace / coverage into +trace+.
    def call_traced(code, args, trace)
      res = EmuResult.new
      FN[:emu_call_traced].call(@h, code, code.bytesize, Asmtest.pack_longs(args), args.length, 0, res.h, trace.h)
      res
    end

    # Whether the loaded native lib has the in-line assembler (Keystone).
    def asm_available?
      !FN[:emu_call_asm6].nil?
    end

    # Assemble x86-64 +src+ in +syntax+ (0=Intel, 1=AT&T, 2=NASM, 3=MASM, 4=GAS;
    # see SYNTAX) via Keystone and run it
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

    # --- Mid-execution guards (Track F) ---

    # Map a guest RW region [addr, addr+size) the routine can use.
    def map(addr, size)
      FN[:emu_map].call(@h, addr, size) != 0
    end

    # Arm a memory-write watchpoint over [addr, addr+size): +:only+ flags a write
    # that escapes it, +:never+ one that touches it. Returns a Watch.
    def watch_writes(addr, size, mode)
      w = Watch.new
      FN[:watch_writes].call(@h, addr, size, mode == :never ? 0 : 1, w.h)
      w
    end

    def watch_clear
      FN[:watch_clear].call(@h)
    end

    # Arm a register invariant: GP register +name+ must hold +want+ at every
    # basic-block entry. Returns a RegGuard; raises for an unknown register name.
    def guard_reg(name, want)
      g = RegGuard.new
      if FN[:guard_reg].call(@h, name, want, g.h) == 0
        g.free
        raise Error, "unknown register: #{name}"
      end
      g
    end

    def guard_reg_clear
      FN[:guard_reg_clear].call(@h)
    end

    # --- Coverage-guided fuzzing + mutation testing (Track E) ---

    # Coverage-guided input search over one-int-arg +code+; [blocks_reached, corpus_len].
    def fuzz_cover(code, lo, hi, iters, seed: 0xC0FFEE)
      uni = Trace.new(0, 256)
      st = FN[:fuzz_new].call
      FN[:fuzz_cover1].call(@h, code, code.bytesize, lo, hi, iters, seed, uni.h, st)
      out = [FN[:fuzz_blocks].call(st), FN[:fuzz_corpus].call(st)]
      FN[:fuzz_free].call(st)
      uni.free
      out
    end

    # Bit-flip mutation testing of +code+ against +inputs+; [killed, survived].
    def mutation_test(code, inputs, max_mutants: 0, seed: 0xABCD)
      st = FN[:mut_new].call
      FN[:mutation_test1].call(@h, code, code.bytesize, Asmtest.pack_longs(inputs),
                               inputs.length, max_mutants, seed, st)
      out = [FN[:mut_killed].call(st), FN[:mut_survived].call(st)]
      FN[:mut_free].call(st)
      out
    end

    def close
      FN[:emu_close].call(@h) if @h
      @h = nil
    end
  end

  # A memory-write watchpoint result (Track F).
  class Watch
    attr_reader :h
    def initialize; @h = FN[:watch_new].call; end
    def violated?; FN[:watch_violated].call(@h) != 0; end
    def addr; FN[:watch_addr].call(@h); end
    def rip_off; FN[:watch_rip].call(@h); end
    def free; FN[:watch_free].call(@h) if @h; @h = nil; end
  end

  # A register-invariant guard result (Track F).
  class RegGuard
    attr_reader :h
    def initialize; @h = FN[:rguard_new].call; end
    def violated?; FN[:rguard_violated].call(@h) != 0; end
    def got; FN[:rguard_got].call(@h); end
    def free; FN[:rguard_free].call(@h) if @h; @h = nil; end
  end

  # True if the host CPU + OS support AVX2 (gate .capture_vec256).
  def self.cpu_has_avx2?
    FN[:cpu_avx2].call != 0
  end

  # AVX2 256-bit capture (Track D): +vargs+ is an array of [4 doubles]; returns
  # the 16 ymm registers as 32-byte binary Strings (out[0] = the vector return).
  def self.capture_vec256(fn, vargs)
    out = ("\0".b * (16 * 32))
    va = ("\0".b * (8 * 32))
    vargs.first(8).each_with_index do |v, i|
      va[i * 32, 32] = v.map(&:to_f).pack("d4")
    end
    ia = ("\0".b * (6 * 8)) # the trampoline reads six integer-arg slots
    FN[:capture_vec256].call(out, fn, ia, va)
    (0...16).map { |i| out[i * 32, 32] }
  end

  # An opaque execution-trace / basic-block coverage recorder.
  class Trace
    attr_reader :h

    def initialize(insns_cap = 4096, blocks_cap = 4096)
      @h = FN[:trace_new].call(insns_cap, blocks_cap)
    end

    # True if the basic block at byte-offset +off+ (from the routine entry) was entered.
    def covered?(off)
      FN[:trace_covered].call(@h, off) != 0
    end

    def free
      FN[:trace_free].call(@h) if @h
      @h = nil
    end
  end

  # A cross-arch run's outcome; registers are read by name. Call #free when done.
  class GuestResult
    attr_reader :h

    def initialize(arch)
      @arch = arch
      @h = FN[:"#{arch}_res_new"].call
    end

    def faulted?
      FN[:emu_faulted].call(@h) != 0
    end

    # Guest register by name (e.g. "x0"/"sp", "a0"/"x10", "r0").
    def reg(name)
      FN[:"#{@arch}_reg"].call(@h, name)
    end

    def free
      FN[:"#{@arch}_res_free"].call(@h) if @h
      @h = nil
    end
  end

  # A cross-arch Unicorn guest ("arm64"/"riscv"/"arm") running raw machine-code
  # bytes — emulated regardless of host arch. Call #close when done.
  class Guest
    def initialize(arch)
      @arch = arch
      @h = FN[:"emu_#{arch}_open"].call
    end

    # Run raw bytes (a binary String) with integer args in the guest ABI registers.
    def call(code, args = [])
      res = GuestResult.new(@arch)
      FN[:"emu_#{@arch}_call"].call(@h, code, code.bytesize, Asmtest.pack_longs(args), args.length, 0, res.h)
      res
    end

    # Like #call, but record an execution trace / coverage into +trace+ (arm64).
    def call_traced(code, args, trace)
      raise Error, "traced guest run only wired for arm64" unless @arch == "arm64"
      res = GuestResult.new(@arch)
      FN[:emu_arm64_call_traced].call(@h, code, code.bytesize, Asmtest.pack_longs(args), args.length, 0, res.h, trace.h)
      res
    end

    def close
      FN[:"emu_#{@arch}_close"].call(@h) if @h
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

  def self.assert_vec_f32(r, index, want)
    got = r.vec_f32(index)
    want.each_with_index do |w, i|
      raise Error, "vec[#{index}] lane #{i}: got #{got[i]}, want #{w}" unless got[i] == w
    end
  end

  def self.assert_no_fault(res)
    raise Error, "unexpected fault" if res.faulted?
  end

  def self.assert_fault(res)
    raise Error, "expected a fault, but the run completed cleanly" unless res.faulted?
  end

  def self.assert_emu_reg(res, name, want)
    got = res.reg(name)
    raise Error, "emu #{name}: got #{got}, want #{want}" unless got == want
  end
end
