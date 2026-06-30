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
    # No hardware-trace backend available on this host (ASMTEST_HW_EUNAVAIL): what
    # auto returns when even single-step can't run (off x86-64 Linux).
    EUNAVAIL = -3

    # asmtest_trace_backend_t. SINGLESTEP is the portable default that runs on any
    # x86-64 Linux; the others self-skip off the hardware they need.
    INTEL_PT   = 0
    CORESIGHT  = 1
    AMD_LBR    = 2
    SINGLESTEP = 3

    # asmtest_hwtrace_policy_t — the backend auto-selection policy. BEST is the most
    # faithful available backend; CEILING_FREE is the same but skips the one
    # fixed-window backend (AMD LBR), to re-resolve under after a truncated trace.
    BEST         = 0
    CEILING_FREE = 1

    # asmtest_trace_auto.h — the CROSS-TIER orchestrator over all three trace tiers
    # (hardware + DynamoRIO + emulator), not just the hardware backends above.
    # asmtest_trace_tier_t.
    TIER_HWTRACE   = 0 # HW branch trace / single-step (real CPU)
    TIER_DYNAMORIO = 1 # in-process software DBI (real CPU)
    TIER_EMULATOR  = 2 # Unicorn virtual CPU (isolated guest)
    # asmtest_trace_fidelity_t.
    FIDELITY_NATIVE  = 0 # runs the real bytes on the real CPU in-process
    FIDELITY_VIRTUAL = 1 # isolated guest on an emulated CPU
    # cross-tier policy bitmask.
    TRACE_BEST         = 0x0 # most-faithful available; emulator floor allowed
    TRACE_CEILING_FREE = 0x1 # drop the fixed-window backend (AMD LBR)
    TRACE_NATIVE_ONLY  = 0x2 # forbid the native->emulator fidelity crossing

    # A resolved cross-tier trace option: which +tier+ to use, which hardware
    # +backend+ within it (meaningful only when +tier+ == TIER_HWTRACE), and the
    # +fidelity+ class (FIDELITY_NATIVE vs FIDELITY_VIRTUAL). Mirrors
    # asmtest_trace_choice_t — three packed C ints.
    TierChoice = Struct.new(:tier, :backend, :fidelity)

    # asmtest_ptrace.h — out-of-process / foreign-process tracing status codes.
    # PTRACE_OK is the shared success spirit; PTRACE_ENOENT is "region / symbol /
    # method not found" (region_by_addr/perfmap_symbol/jitdump_find map it to nil).
    PTRACE_OK     = 0
    PTRACE_ENOENT = -7

    # asmtest_codeimage.h — time-aware code-image recorder status codes (own
    # namespace, like the ptrace ones). CI_OK is success; CI_ENOENT is "address
    # never tracked / no version at-or-before when" (bytes_at maps it to nil).
    CI_OK     = 0
    CI_ENOENT = -7

    # asmtest_codeimage_event_t.kind — how a code-emission event was observed.
    CI_KIND_MPROTECT = 1 # mprotect(...PROT_EXEC...) — the common JIT edge
    CI_KIND_MMAP     = 2 # mmap(...PROT_EXEC...); addr is the real base
    CI_KIND_MEMFD    = 3 # memfd_create — staging hint; correlate via fd

    # A code-emission event popped from the eBPF detector (asmtest_codeimage_event_t):
    # the published base +addr+ and +len+, the bpf_ktime_get_ns() +timestamp+, the
    # publishing +pid+/+tid+, the +kind+ (CI_KIND_*), and the memfd +fd+ (or -1).
    CodeImageEvent = Struct.new(:addr, :len, :timestamp, :pid, :tid, :kind, :fd)

    # A JIT method resolved from a jitdump (asmtest_jitdump_entry_t): the load
    # address (the base to trace), the code size, the JIT's record timestamp/index,
    # and (optionally) the recorded native +code+ bytes as a binary String.
    JitMethod = Struct.new(:code_addr, :code_size, :timestamp, :code_index, :code)

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
        resolve:      func(LIB, "asmtest_hwtrace_resolve", [INT, VOIDP, SZ], SZ),
        auto:         func(LIB, "asmtest_hwtrace_auto", [INT], INT),
        # ---- the cross-tier orchestrator (asmtest_trace_auto.h) ----
        trace_resolve: func(LIB, "asmtest_trace_resolve", [INT, VOIDP, SZ], SZ),
        trace_auto:    func(LIB, "asmtest_trace_auto", [INT, VOIDP], INT),
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
        # ---- out-of-process / foreign-process tracing toolkit (asmtest_ptrace.h) ----
        ptrace_available:    func(LIB, "asmtest_ptrace_available", [], INT),
        ptrace_skip_reason:  func(LIB, "asmtest_ptrace_skip_reason", [VOIDP, SZ], VOID),
        ptrace_trace_call:   func(LIB, "asmtest_ptrace_trace_call", [VOIDP, SZ, VOIDP, INT, VOIDP, VOIDP], INT),
        ptrace_trace_attached: func(LIB, "asmtest_ptrace_trace_attached", [INT, VOIDP, SZ, VOIDP, VOIDP], INT),
        ptrace_run_to:       func(LIB, "asmtest_ptrace_run_to", [INT, VOIDP], INT),
        proc_region_by_addr: func(LIB, "asmtest_proc_region_by_addr", [INT, VOIDP, VOIDP, VOIDP], INT),
        proc_perfmap_symbol: func(LIB, "asmtest_proc_perfmap_symbol", [INT, VOIDP, VOIDP, VOIDP], INT),
        jitdump_find:        func(LIB, "asmtest_jitdump_find", [VOIDP, INT, VOIDP, VOIDP, VOIDP, SZ, VOIDP], INT),
        # ---- decode a foreign region against TIME-CORRECT bytes (asmtest_ptrace.h) ----
        ptrace_trace_attached_versioned:
          func(LIB, "asmtest_ptrace_trace_attached_versioned",
               [INT, VOIDP, SZ, VOIDP, LL, VOIDP, VOIDP], INT),
        # ---- time-aware code-image recorder (asmtest_codeimage.h) ----
        ci_available:      func(LIB, "asmtest_codeimage_available", [], INT),
        ci_skip_reason:    func(LIB, "asmtest_codeimage_skip_reason", [VOIDP, SZ], VOID),
        ci_new:            func(LIB, "asmtest_codeimage_new", [INT], VOIDP),
        ci_free:           func(LIB, "asmtest_codeimage_free", [VOIDP], VOID),
        ci_track:          func(LIB, "asmtest_codeimage_track", [VOIDP, VOIDP, SZ], INT),
        ci_refresh:        func(LIB, "asmtest_codeimage_refresh", [VOIDP], INT),
        ci_now:            func(LIB, "asmtest_codeimage_now", [VOIDP], LL),
        ci_bytes_at:       func(LIB, "asmtest_codeimage_bytes_at", [VOIDP, VOIDP, LL, VOIDP, VOIDP], INT),
        ci_bpf_available:  func(LIB, "asmtest_codeimage_bpf_available", [], INT),
        ci_bpf_skip_reason: func(LIB, "asmtest_codeimage_bpf_skip_reason", [VOIDP, SZ], VOID),
        ci_watch_bpf:      func(LIB, "asmtest_codeimage_watch_bpf", [VOIDP], INT),
        ci_poll_bpf:       func(LIB, "asmtest_codeimage_poll_bpf", [VOIDP, INT], INT),
        ci_next:           func(LIB, "asmtest_codeimage_next", [VOIDP, VOIDP], INT),
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

      # This host's hardware-trace fallback cascade: the available backends, most-
      # faithful first (INTEL_PT > AMD_LBR > SINGLESTEP > CORESIGHT), honoring
      # +policy+. Empty only off x86-64 Linux (single-step is the floor there).
      # CEILING_FREE drops the depth-bounded backend (AMD LBR). Returns an Array of
      # backend ints. resolve writes up to 4 enums into the out buffer and returns
      # the count; we read back the first n int values.
      def self.resolve(policy = BEST)
        out = Fiddle::Pointer.new(Fiddle.malloc(16), 16) # 4 ints
        out[0, 16] = "\x00".b * 16
        n = Asmtest::HwTrace::FN[:resolve].call(policy, out, 4)
        backends = (0...n).map { |i| out[i * 4, 4].unpack1("l") }
        Fiddle.free(out.to_i)
        backends
      end

      # The single most-preferred available backend under +policy+ (a backend enum
      # >= 0, ready to .init), or EUNAVAIL (-3) when no hardware-trace backend is
      # available on this host. Named .auto to match the C name (not a Ruby keyword).
      def self.auto(policy = BEST)
        Asmtest::HwTrace::FN[:auto].call(policy)
      end

      # This host's full CROSS-TIER cascade (asmtest_trace_resolve), most-faithful
      # first: Intel PT -> AMD LBR -> DynamoRIO -> single-step -> CoreSight ->
      # emulator, each included only if its tier is available. Returns an Array of
      # TierChoice. TRACE_NATIVE_ONLY drops the emulator floor (no native->emulator
      # fidelity crossing); TRACE_CEILING_FREE drops AMD LBR. Each choice is three
      # packed C ints (tier, backend, fidelity); we allocate 3*cap ints and unpack
      # the first n triples the call reports it wrote.
      def self.resolve_tiers(policy = TRACE_BEST)
        cap = 8
        out = Fiddle::Pointer.new(Fiddle.malloc(12 * cap), 12 * cap) # 3 ints each
        out[0, 12 * cap] = "\x00".b * (12 * cap)
        n = Asmtest::HwTrace::FN[:trace_resolve].call(policy, out, cap)
        choices = (0...n).map do |i|
          tier, backend, fidelity = out[i * 12, 12].unpack("l3")
          TierChoice.new(tier, backend, fidelity)
        end
        Fiddle.free(out.to_i)
        choices
      end

      # The single most-preferred available cross-tier choice under +policy+ as a
      # TierChoice, or nil when the cascade is empty (only off a native host under
      # TRACE_NATIVE_ONLY). asmtest_trace_auto returns OK(0) and fills *out, or
      # EUNAVAIL(-3) when the cascade is empty.
      def self.auto_tier(policy = TRACE_BEST)
        out = Fiddle::Pointer.new(Fiddle.malloc(12), 12) # one choice = 3 ints
        out[0, 12] = "\x00".b * 12
        rc = Asmtest::HwTrace::FN[:trace_auto].call(policy, out)
        choice = nil
        if rc == OK
          tier, backend, fidelity = out[0, 12].unpack("l3")
          choice = TierChoice.new(tier, backend, fidelity)
        end
        Fiddle.free(out.to_i)
        choice
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

      # ---- out-of-process / foreign-process tracing (asmtest_ptrace.h) ----
      #
      # Single-step a forked or externally-attached target OUT OF BAND, and resolve
      # the code region to trace from the OS — /proc/<pid>/maps, a JIT perf-map, or a
      # binary jitdump. The managed-runtime path (JVM/.NET/Node on AMD, where Intel PT
      # is unavailable and in-process DynamoRIO cannot seize the runtime's threads).
      # Linux x86-64. Class methods on this same HwTrace class, mirroring the Python
      # Ptrace surface; they live in the SAME libasmtest_hwtrace this binding loads.

      # True if the out-of-process single-step tracer can run on this host (Linux
      # x86-64). Folds a load failure (FN nil) into a clean false so callers self-skip.
      def self.ptrace_available?
        return false if Asmtest::HwTrace::FN.nil?
        Asmtest::HwTrace::FN[:ptrace_available].call != 0
      end

      # Human-readable reason ptrace_available? is false (or "available"). "" if the
      # lib failed to load.
      def self.ptrace_skip_reason
        return "" if Asmtest::HwTrace::FN.nil?
        buf = Fiddle::Pointer.new(Fiddle.malloc(160), 160)
        buf[0, 160] = "\x00".b * 160
        Asmtest::HwTrace::FN[:ptrace_skip_reason].call(buf, 160)
        s = buf.to_s # up to first NUL
        Fiddle.free(buf.to_i)
        s
      end

      # Fork a tracee that calls +code+ (a NativeCode entry +code_base+ of length
      # +code_len+) with up to six integer +args+, single-step it out of process, and
      # fill +trace+ (a HwTrace). Returns the routine's return value (the child's RAX
      # at the ret). +args+ is packed as an array of C longs; nargs is its length.
      def self.ptrace_trace_call(code_base, code_len, args, trace)
        n = args.length
        argbuf = Fiddle::Pointer.new(Fiddle.malloc(8 * [n, 1].max), 8 * [n, 1].max)
        argbuf[0, 8 * [n, 1].max] = "\x00".b * (8 * [n, 1].max)
        argbuf[0, 8 * n] = args.pack("q*") if n > 0
        res = Fiddle::Pointer.new(Fiddle.malloc(8), 8)
        res[0, 8] = "\x00".b * 8
        rc = Asmtest::HwTrace::FN[:ptrace_trace_call].call(
          code_base, code_len, argbuf, n, res, trace.handle)
        result = res[0, 8].unpack1("q")
        Fiddle.free(argbuf.to_i)
        Fiddle.free(res.to_i)
        raise "asmtest_ptrace_trace_call failed: #{rc}" if rc != Asmtest::HwTrace::PTRACE_OK
        result
      end

      # Trace a region [+base+, +base+ + +len+) in a SEPARATE, already-ptrace-stopped
      # process +pid+ (the caller owns PTRACE_ATTACH/DETACH); the target's bytes are
      # read via process_vm_readv. Fills +trace+ and returns the target's RAX at the
      # ret. +pid+ is a C int.
      def self.ptrace_trace_attached(pid, base, len, trace)
        res = Fiddle::Pointer.new(Fiddle.malloc(8), 8)
        res[0, 8] = "\x00".b * 8
        base_p = Fiddle::Pointer.new(base)
        rc = Asmtest::HwTrace::FN[:ptrace_trace_attached].call(
          pid, base_p, len, res, trace.handle)
        result = res[0, 8].unpack1("q")
        Fiddle.free(res.to_i)
        raise "asmtest_ptrace_trace_attached failed: #{rc}" if rc != Asmtest::HwTrace::PTRACE_OK
        result
      end

      # Like ptrace_trace_attached, but decode the region against TIME-CORRECT bytes
      # from a CodeImage recorder (+img+) at logical timestamp +when+ (0 = latest)
      # instead of a single live process_vm_readv snapshot — the temporal
      # same-address-different-bytes fix for a JIT patched/freed/reused mid-trace.
      # +img+ must already be tracking a region covering [+base+, +base+ + +len+);
      # with +img+ nil this is exactly ptrace_trace_attached. +pid+ is a C int.
      def self.ptrace_trace_attached_versioned(pid, base, len, img, when_seq, trace)
        res = Fiddle::Pointer.new(Fiddle.malloc(8), 8)
        res[0, 8] = "\x00".b * 8
        base_p = Fiddle::Pointer.new(base)
        img_p  = img ? img.handle : Fiddle::NULL
        rc = Asmtest::HwTrace::FN[:ptrace_trace_attached_versioned].call(
          pid, base_p, len, img_p, when_seq, res, trace.handle)
        result = res[0, 8].unpack1("q")
        Fiddle.free(res.to_i)
        raise "asmtest_ptrace_trace_attached_versioned failed: #{rc}" if rc != Asmtest::HwTrace::PTRACE_OK
        result
      end

      # Run an already-attached, ptrace-stopped target forward until it reaches +addr+
      # (a software breakpoint that fires when the program itself next calls in),
      # leaving it stopped there ready for ptrace_trace_attached -- the step that makes a
      # resolved JIT method traceable when you don't control call timing. Returns the
      # status code (PTRACE_OK, or PTRACE_ENOENT if the target exited first). +pid+ is a
      # C int; +addr+ is an integer address. The caller owns PTRACE_ATTACH/DETACH.
      def self.ptrace_run_to(pid, addr)
        Asmtest::HwTrace::FN[:ptrace_run_to].call(pid, Fiddle::Pointer.new(addr))
      end

      # The executable mapping in /proc/<pid>/maps containing +addr+, as [base, len],
      # or nil if no executable mapping contains it (PTRACE_ENOENT). +pid+ is a C int;
      # +addr+ is an integer address. base/len come back via two void* out-params.
      def self.proc_region_by_addr(pid, addr)
        base_p = Fiddle::Pointer.new(Fiddle.malloc(8), 8)
        len_p  = Fiddle::Pointer.new(Fiddle.malloc(8), 8)
        base_p[0, 8] = "\x00".b * 8
        len_p[0, 8]  = "\x00".b * 8
        rc = Asmtest::HwTrace::FN[:proc_region_by_addr].call(
          pid, Fiddle::Pointer.new(addr), base_p, len_p)
        out = rc == Asmtest::HwTrace::PTRACE_OK ?
          [base_p[0, 8].unpack1("Q"), len_p[0, 8].unpack1("Q")] : nil
        Fiddle.free(base_p.to_i)
        Fiddle.free(len_p.to_i)
        out
      end

      # A JIT method by +name+ in /tmp/perf-<pid>.map, as [base, len], or nil
      # (PTRACE_ENOENT). +pid+ is a C int; +name+ is matched against the full symbol
      # text after the size field.
      def self.proc_perfmap_symbol(pid, name)
        base_p = Fiddle::Pointer.new(Fiddle.malloc(8), 8)
        len_p  = Fiddle::Pointer.new(Fiddle.malloc(8), 8)
        base_p[0, 8] = "\x00".b * 8
        len_p[0, 8]  = "\x00".b * 8
        rc = Asmtest::HwTrace::FN[:proc_perfmap_symbol].call(pid, name, base_p, len_p)
        out = rc == Asmtest::HwTrace::PTRACE_OK ?
          [base_p[0, 8].unpack1("Q"), len_p[0, 8].unpack1("Q")] : nil
        Fiddle.free(base_p.to_i)
        Fiddle.free(len_p.to_i)
        out
      end

      # A JIT method by +name+ from a jitdump (+path+, or /tmp/jit-<pid>.dump when
      # +path+ is nil) as a JitMethod carrying up to +want_bytes+ of recorded code, or
      # nil (PTRACE_ENOENT). The latest re-JIT body (highest timestamp) wins. The C
      # entry fills an asmtest_jitdump_entry_t (four packed u64) and, when want_bytes
      # > 0, copies up to want_bytes of code into a buffer and sets *bytes_len.
      def self.jitdump_find(path, name, pid: 0, want_bytes: 0)
        ent = Fiddle::Pointer.new(Fiddle.malloc(32), 32) # 4 u64
        ent[0, 32] = "\x00".b * 32
        buf = want_bytes > 0 ? Fiddle::Pointer.new(Fiddle.malloc(want_bytes), want_bytes) : nil
        blen = want_bytes > 0 ? Fiddle::Pointer.new(Fiddle.malloc(8), 8) : nil
        if want_bytes > 0
          buf[0, want_bytes] = "\x00".b * want_bytes
          blen[0, 8] = "\x00".b * 8
        end
        rc = Asmtest::HwTrace::FN[:jitdump_find].call(
          path, pid, name, ent,
          buf || Fiddle::NULL, want_bytes, blen || Fiddle::NULL)
        method = nil
        if rc == Asmtest::HwTrace::PTRACE_OK
          code_addr, code_size, timestamp, code_index = ent[0, 32].unpack("Q4")
          code = ""
          if want_bytes > 0
            n = blen[0, 8].unpack1("Q")
            code = buf[0, n] if n > 0
          end
          method = JitMethod.new(code_addr, code_size, timestamp, code_index, code)
        end
        Fiddle.free(ent.to_i)
        if want_bytes > 0
          Fiddle.free(buf.to_i)
          Fiddle.free(blen.to_i)
        end
        method
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

    # A time-aware code-image recorder (asmtest_codeimage.h) — a userspace
    # PERF_RECORD_TEXT_POKE. track() snapshots a region and arms write-protect;
    # refresh() re-snapshots only the pages that changed since the last arm,
    # appending a new version stamped with the next monotonic sequence; bytes_at
    # answers "what bytes were live at addr as of sequence +when+" — the bytes a
    # branch-trace decoder or the W2 block-normalizer needs to reconstruct a JIT
    # method whose address was reused mid-trace. Wraps the opaque
    # asmtest_codeimage_t handle. pid 0 == this process. Mirrors the Fiddle FFI
    # style of the HwTrace/NativeCode wrappers above.
    class CodeImage
      attr_reader :handle

      # True if the userspace recorder can detect page changes on this host
      # (PAGEMAP_SCAN, or the soft-dirty fallback). Folds a load failure (FN nil)
      # into a clean false so callers self-skip without a rescue.
      def self.available?
        return false if Asmtest::HwTrace::FN.nil?
        Asmtest::HwTrace::FN[:ci_available].call != 0
      end

      # Human-readable reason available? is false (or "available"). "" if the lib
      # failed to load.
      def self.skip_reason
        return "" if Asmtest::HwTrace::FN.nil?
        buf = Fiddle::Pointer.new(Fiddle.malloc(160), 160)
        buf[0, 160] = "\x00".b * 160
        Asmtest::HwTrace::FN[:ci_skip_reason].call(buf, 160)
        s = buf.to_s # up to first NUL
        Fiddle.free(buf.to_i)
        s
      end

      # True if the optional eBPF emission detector can load and attach here
      # (built with libbpf, kernel BTF present, sufficient privilege). Folds a load
      # failure into a clean false.
      def self.bpf_available?
        return false if Asmtest::HwTrace::FN.nil?
        Asmtest::HwTrace::FN[:ci_bpf_available].call != 0
      end

      # Human-readable reason bpf_available? is false (or "available"). "" if the
      # lib failed to load.
      def self.bpf_skip_reason
        return "" if Asmtest::HwTrace::FN.nil?
        buf = Fiddle::Pointer.new(Fiddle.malloc(160), 160)
        buf[0, 160] = "\x00".b * 160
        Asmtest::HwTrace::FN[:ci_bpf_skip_reason].call(buf, 160)
        s = buf.to_s # up to first NUL
        Fiddle.free(buf.to_i)
        s
      end

      # Create a timeline recording +pid+'s memory (+pid+ == 0 => this process).
      # +pid+ is a C int. Raises if allocation fails (the C side returns NULL).
      def initialize(pid = 0)
        @handle = Asmtest::HwTrace::FN[:ci_new].call(pid)
        raise "asmtest_codeimage_new failed" if @handle.null?
      end

      # Begin tracking [+base+, +base+ + +len+) in the target: snapshot version 0
      # now and arm write-protect on its pages. +base+ is an integer address.
      # Returns the C status (CI_OK on success).
      def track(base, len)
        Asmtest::HwTrace::FN[:ci_track].call(@handle, Fiddle::Pointer.new(base), len)
      end

      # Scan tracked ranges for pages changed since the last arm, re-snapshot each
      # as a new version, re-arm. Returns the number of new versions recorded
      # (>= 0), or a negative status.
      def refresh
        Asmtest::HwTrace::FN[:ci_refresh].call(@handle)
      end

      # The current capture sequence — a monotonic logical timestamp. Advances by
      # one per version recorded (track + each refresh change). 0 before anything
      # is tracked.
      def now
        Asmtest::HwTrace::FN[:ci_now].call(@handle)
      end

      # The bytes live at +addr+ as of capture sequence +when_seq+ (0 => latest),
      # as a binary String, or nil if +addr+ was never tracked / there is no
      # version at-or-before +when_seq+ (CI_ENOENT). +addr+ is an integer address.
      # On success the C side hands back borrowed bytes via *out / *out_len; we
      # copy them out into a fresh String (the borrow is owned by the timeline).
      def bytes_at(addr, when_seq = 0)
        out_p   = Fiddle::Pointer.new(Fiddle.malloc(8), 8) # const uint8_t **out
        len_p   = Fiddle::Pointer.new(Fiddle.malloc(8), 8) # size_t *out_len
        out_p[0, 8] = "\x00".b * 8
        len_p[0, 8] = "\x00".b * 8
        rc = Asmtest::HwTrace::FN[:ci_bytes_at].call(
          @handle, Fiddle::Pointer.new(addr), when_seq, out_p, len_p)
        bytes = nil
        if rc == Asmtest::HwTrace::CI_OK
          ptr = out_p[0, 8].unpack1("Q")
          len = len_p[0, 8].unpack1("Q")
          bytes = len > 0 ? Fiddle::Pointer.new(ptr)[0, len] : "".b
        end
        Fiddle.free(out_p.to_i)
        Fiddle.free(len_p.to_i)
        bytes
      end

      # Load the CO-RE eBPF program, filter it to this image's pid, and attach it.
      # Returns the C status (CI_OK on success; CI_ENOSYS/CI_EUNAVAIL/CI_ELOAD when
      # libbpf / BTF / privilege are missing).
      def watch_bpf
        Asmtest::HwTrace::FN[:ci_watch_bpf].call(@handle)
      end

      # Drain ready emission events from the BPF ring buffer into the internal
      # queue. +timeout_ms+ == 0 is a non-blocking drain; > 0 waits up to that
      # long. Returns the number of events queued (>= 0) or a negative status.
      def poll_bpf(timeout_ms = 0)
        Asmtest::HwTrace::FN[:ci_poll_bpf].call(@handle, timeout_ms)
      end

      # Pop one queued emission event as a CodeImageEvent, or nil if the queue is
      # empty. The C entry fills an asmtest_codeimage_event_t (u64 addr/len/
      # timestamp, u32 pid/tid/kind, i32 fd = 40 bytes) and returns 1 if an event
      # was returned, 0 if empty, or a negative status.
      def next_event
        ev = Fiddle::Pointer.new(Fiddle.malloc(40), 40)
        ev[0, 40] = "\x00".b * 40
        rc = Asmtest::HwTrace::FN[:ci_next].call(@handle, ev)
        out = nil
        if rc == 1
          addr, len, timestamp = ev[0, 24].unpack("Q3")
          pid, tid, kind = ev[24, 12].unpack("L3")
          fd = ev[36, 4].unpack1("l")
          out = CodeImageEvent.new(addr, len, timestamp, pid, tid, kind, fd)
        end
        Fiddle.free(ev.to_i)
        out
      end

      # Free the timeline and all recorded versions (detaches any eBPF watch).
      # NULL-safe and idempotent.
      def free
        return unless @handle
        Asmtest::HwTrace::FN[:ci_free].call(@handle)
        @handle = nil
      end
    end
  end
end
