# hwtrace.rb — hardware-tier native runtime tracing for Ruby (single-step / PT / AMD).
#
# This is the Ruby counterpart to bindings/python/asmtest/hwtrace.py: the optional
# hardware-trace tier (see include/asmtest_hwtrace.h and docs/native-tracing.md). It
# records the same asmtest_trace_t offsets as the emulator and DynamoRIO tiers, but
# by observing the **real CPU** — and unlike the DynamoRIO wrapper it needs no
# DynamoRIO install.
#
# Four backends share one API, selected by enum:
#
# * SINGLESTEP — EFLAGS.TF single-step (#DB -> SIGTRAP). Exact and complete on ANY
#   x86-64 Linux (Intel, any-Zen AMD, VM, CI, container): no PMU, no perf_event, no
#   privilege. This is the portable default and what this binding's self-test
#   exercises live.
# * INTEL_PT / CORESIGHT / AMD_LBR — hardware branch-trace backends that self-skip
#   off the specific bare-metal hardware they need.
#
# Like drtrace.rb it keeps all Fiddle FFI inside and loads a *separate* shared object
# — libasmtest_hwtrace — whose path comes from env ASMTEST_HWTRACE_LIB, else
# <repo>/build/libasmtest_hwtrace.so; a load failure is caught so HwTrace.available?
# returns false rather than raising.
#
# Example:
#   require_relative "hwtrace"
#   if Asmtest::HwTrace::HwTrace.available?(Asmtest::HwTrace::SINGLESTEP)
#     Asmtest::HwTrace::HwTrace.init
#     code = Asmtest::HwTrace::NativeCode.from_bytes(
#              [0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3].pack("C*"))  # mov rax,rdi; add; ret
#     tr = Asmtest::HwTrace::HwTrace.create(blocks: 64, instructions: 64)
#     tr.register("add", code)
#     tr.region("add") { @r = code.call(20, 22) }
#     raise unless @r == 42 && tr.covered?(0)
#     Asmtest::HwTrace::HwTrace.shutdown
#   end
require "fiddle"

module Asmtest
  # The optional hardware-trace tier. Namespaced apart from the core Asmtest module
  # so requiring it is explicit (it loads a different .so).
  module HwTrace
    # Status code shared by the lifecycle + registration calls (ASMTEST_HW_OK).
    OK = 0

    # asmtest_trace_backend_t. SINGLESTEP is the portable default that runs on any
    # x86-64 Linux; the others self-skip off the hardware they need.
    INTEL_PT   = 0
    CORESIGHT  = 1
    AMD_LBR    = 2
    SINGLESTEP = 3

    VOIDP = Fiddle::TYPE_VOIDP
    LONG  = Fiddle::TYPE_LONG
    INT   = Fiddle::TYPE_INT
    SZ    = Fiddle::TYPE_SIZE_T
    LL    = Fiddle::TYPE_LONG_LONG
    VOID  = Fiddle::TYPE_VOID

    # Resolve libasmtest_hwtrace: an explicit ASMTEST_HWTRACE_LIB wins; otherwise
    # fall back to <repo>/build/libasmtest_hwtrace.so (the in-tree build artifact).
    def self.resolve_hwtrace_lib
      env = ENV["ASMTEST_HWTRACE_LIB"].to_s
      return env unless env.empty?
      File.expand_path("../../build/libasmtest_hwtrace.so", __dir__)
    end

    def self.func(lib, name, args, ret)
      Fiddle::Function.new(lib[name], args, ret)
    end

    # Load the lib and bind every entry point, OR leave LIB/FN nil if it can't load
    # (the tier is built separately and may be absent). HwTrace.available? folds both
    # the load failure and a runtime asmtest_hwtrace_available()==0 into a clean false.
    begin
      LIB = Fiddle.dlopen(resolve_hwtrace_lib)
      FN = {
        # ---- process-wide lifecycle ----
        available:    func(LIB, "asmtest_hwtrace_available", [INT], INT),
        skip_reason:  func(LIB, "asmtest_hwtrace_skip_reason", [INT, VOIDP, SZ], VOID),
        init:         func(LIB, "asmtest_hwtrace_init", [VOIDP], INT),
        shutdown:     func(LIB, "asmtest_hwtrace_shutdown", [], VOID),
        # ---- region registration + markers ----
        register:     func(LIB, "asmtest_hwtrace_register_region", [VOIDP, VOIDP, SZ, VOIDP], INT),
        trace_begin:  func(LIB, "asmtest_hwtrace_begin", [VOIDP], VOID),
        trace_end:    func(LIB, "asmtest_hwtrace_end", [VOIDP], VOID),
        # ---- host-native executable code ----
        exec_alloc:   func(LIB, "asmtest_hwtrace_exec_alloc", [VOIDP, SZ, VOIDP, VOIDP], INT),
        exec_free:    func(LIB, "asmtest_hwtrace_exec_free", [VOIDP, SZ], VOID),
        # ---- the app-owned trace handle + accessors (from the shared trace.o) ----
        trace_new:    func(LIB, "asmtest_trace_new", [SZ, SZ], VOIDP),
        trace_free:   func(LIB, "asmtest_trace_free", [VOIDP], VOID),
        trace_covered: func(LIB, "asmtest_trace_covered", [VOIDP, LL], INT),
        blocks_len:   func(LIB, "asmtest_emu_trace_blocks_len", [VOIDP], LL),
        insns_total:  func(LIB, "asmtest_emu_trace_insns_total", [VOIDP], LL),
        insns_len:    func(LIB, "asmtest_emu_trace_insns_len", [VOIDP], LL),
        truncated:    func(LIB, "asmtest_emu_trace_truncated", [VOIDP], INT),
        block_at:     func(LIB, "asmtest_emu_trace_block_at", [VOIDP, SZ], LL),
        insn_at:      func(LIB, "asmtest_emu_trace_insn_at", [VOIDP, SZ], LL),
      }.freeze
    rescue Fiddle::DLError
      LIB = nil
      FN = nil
    end

    # Build the asmtest_hwtrace_options_t the C side reads:
    #   { int backend; size_t aux_size; size_t data_size; int snapshot; const char* object_hint; }
    # On x86-64 the layout is backend@0 (int, padded to 8), aux_size@8, data_size@16,
    # snapshot@24 (int, padded to 8), object_hint@32 (ptr) = 40 bytes. Zeroed first so
    # aux_size/data_size take their C defaults and object_hint is NULL. The struct
    # buffer is returned as a Fiddle::Pointer kept alive by the caller's scope.
    def self.build_options(backend)
      opts = Fiddle::Pointer.new(Fiddle.malloc(40), 40)
      opts[0, 40] = "\x00".b * 40
      opts[0, 4]  = [backend].pack("l") # asmtest_trace_backend_t backend (int)
      opts                              # aux_size/data_size/snapshot=0, object_hint=NULL
    end

    # Host-native machine code in real executable (W^X) memory. The single-step
    # backend's asmtest_hwtrace_exec_alloc returns the base and length via two
    # separate out-params (not the DynamoRIO tier's 16-byte struct).
    class NativeCode
      # Map executable memory and copy +bytes+ (a binary String) into it, returning
      # a NativeCode whose #base is the live entry address. exec_alloc writes the
      # executable address into *base_out and its length into *len_out.
      def self.from_bytes(bytes)
        base_p = Fiddle::Pointer.new(Fiddle.malloc(8), 8)
        len_p  = Fiddle::Pointer.new(Fiddle.malloc(8), 8)
        base_p[0, 8] = "\x00".b * 8
        len_p[0, 8]  = "\x00".b * 8
        rc = Asmtest::HwTrace::FN[:exec_alloc].call(bytes, bytes.bytesize, base_p, len_p)
        if rc != Asmtest::HwTrace::OK
          Fiddle.free(base_p.to_i)
          Fiddle.free(len_p.to_i)
          raise "asmtest_hwtrace_exec_alloc failed: #{rc}"
        end
        base = base_p[0, 8].unpack1("Q")
        len  = len_p[0, 8].unpack1("Q")
        Fiddle.free(base_p.to_i)
        Fiddle.free(len_p.to_i)
        new(base, len)
      end

      def initialize(base, len)
        @base = base
        @len = len
        @freed = false
      end

      # Entry address of the executable mapping (offset 0 = entry).
      def base
        @base
      end

      # Number of code bytes copied.
      def length
        @len
      end

      # Invoke the code through a function pointer, passing each argument as a C long
      # and reading the result back as a long (the SysV integer ABI). Fiddle::Function
      # accepts the raw entry address as its first argument. Mirrors drtrace.rb.
      def call(*args)
        sig = [LONG] * args.length
        Fiddle::Function.new(@base, sig, LONG).call(*args)
      end

      # Unmap the executable memory.
      def free
        return if @freed
        Asmtest::HwTrace::FN[:exec_free].call(@base, @len)
        @freed = true
      end
    end

    # An app-owned coverage recorder for a registered native region, recorded via the
    # hardware tier. Wraps the opaque asmtest_trace_t handle; instruction recording
    # when instructions > 0, block recording when blocks > 0.
    #
    # Named HwTrace (matching the Python class) and distinct from the enclosing
    # HwTrace module via fully-qualified use; lifecycle verbs are class methods.
    class HwTrace
      attr_reader :handle

      def initialize(handle)
        @handle = handle
      end

      # ---- process-wide lifecycle (class-level; one tier per process) ----

      # True if the chosen +backend+ can run on this host: the lib loaded AND the
      # full detect-and-skip chain passes. Folds a load failure (FN nil) and a
      # runtime asmtest_hwtrace_available()==0 into one clean false, so callers
      # self-skip without a rescue.
      def self.available?(backend = SINGLESTEP)
        return false if Asmtest::HwTrace::FN.nil?
        Asmtest::HwTrace::FN[:available].call(backend) != 0
      end

      # Human-readable reason available? is false (or "available"), for the self-skip
      # message. Returns "" if the lib failed to load.
      def self.skip_reason(backend = SINGLESTEP)
        return "" if Asmtest::HwTrace::FN.nil?
        buf = Fiddle::Pointer.new(Fiddle.malloc(160), 160)
        buf[0, 160] = "\x00".b * 160
        Asmtest::HwTrace::FN[:skip_reason].call(backend, buf, 160)
        s = buf.to_s # up to first NUL
        Fiddle.free(buf.to_i)
        s
      end

      # Select a backend and initialize the tier. SINGLESTEP is the portable default
      # that runs on any x86-64 Linux. Named .init (not Ruby's reserved #initialize)
      # to read as a lifecycle verb at the class level. Raises on a nonzero rc.
      def self.init(backend = SINGLESTEP)
        opts = Asmtest::HwTrace.build_options(backend)
        rc = Asmtest::HwTrace::FN[:init].call(opts)
        raise "asmtest_hwtrace_init failed: #{rc}" if rc != OK
      end

      # Tear the tier down, returning to the uninitialized state.
      def self.shutdown
        Asmtest::HwTrace::FN[:shutdown].call
      end

      # ---- per-trace ----

      # Allocate a trace handle. NOTE the C order: asmtest_trace_new(insns_cap,
      # blocks_cap) — instructions FIRST, blocks SECOND. Named .create (not .new)
      # because the handle comes from C, not a plain Ruby allocation.
      def self.create(blocks: 64, instructions: 64)
        h = Asmtest::HwTrace::FN[:trace_new].call(instructions, blocks)
        raise "asmtest_trace_new failed" if h.null?
        new(h)
      end

      # Register a non-overlapping native range under +name+, recording coverage into
      # this trace. +code+ is a NativeCode.
      def register(name, code)
        rc = Asmtest::HwTrace::FN[:register].call(name, code.base, code.length, @handle)
        raise "register_region(#{name.inspect}) failed: #{rc}" if rc != OK
        self
      end

      # Open hardware capture for +name+, run the block, then close it — capture stays
      # balanced even if the block raises (the ensure runs end). Returns the block's
      # value. Keep the begin..call..end bracket tight (the C tier allows only ONE
      # active region at a time).
      def region(name)
        Asmtest::HwTrace::FN[:trace_begin].call(name)
        begin
          yield
        ensure
          Asmtest::HwTrace::FN[:trace_end].call(name)
        end
      end

      # True if the basic block at byte-offset +off+ (from the region entry) was entered.
      def covered?(off)
        Asmtest::HwTrace::FN[:trace_covered].call(@handle, off) != 0
      end

      # Number of distinct basic blocks recorded.
      def blocks_len
        Asmtest::HwTrace::FN[:blocks_len].call(@handle)
      end

      # Total instructions executed in the region (may exceed insns_len if capped).
      def insns_total
        Asmtest::HwTrace::FN[:insns_total].call(@handle)
      end

      # Number of instruction offsets actually stored (<= the trace's insns capacity).
      def insns_len
        Asmtest::HwTrace::FN[:insns_len].call(@handle)
      end

      # True if the stored instruction stream was truncated against its capacity.
      def truncated?
        Asmtest::HwTrace::FN[:truncated].call(@handle) != 0
      end

      # The distinct basic-block start offsets recorded, in first-seen order.
      def block_offsets
        n = Asmtest::HwTrace::FN[:blocks_len].call(@handle)
        (0...n).map { |i| Asmtest::HwTrace::FN[:block_at].call(@handle, i) }
      end

      # The ordered instruction-offset stream actually stored — each executed
      # instruction's offset in execution order, up to insns_len (not the
      # possibly-larger insns_total).
      def insn_offsets
        n = Asmtest::HwTrace::FN[:insns_len].call(@handle)
        (0...n).map { |i| Asmtest::HwTrace::FN[:insn_at].call(@handle, i) }
      end

      def free
        return unless @handle
        Asmtest::HwTrace::FN[:trace_free].call(@handle)
        @handle = nil
      end
    end
  end
end
