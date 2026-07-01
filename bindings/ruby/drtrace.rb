# drtrace.rb — in-process native runtime tracing for Ruby, backed by DynamoRIO.
#
# This is the Ruby counterpart to bindings/python/asmtest/drtrace.py: the optional
# DynamoRIO native-trace tier (see include/asmtest_drtrace.h and
# docs/native-tracing.md). Where the emulator tier (Asmtest::Emu) traces isolated
# guest bytes, NativeTrace traces host-native code as it runs **inside this Ruby
# process**: bring DynamoRIO up once, materialize host-native machine code, mark a
# region, call into it, and read back basic-block coverage / the instruction stream.
#
# Like asmtest.rb it keeps all Fiddle FFI inside; unlike asmtest.rb it loads a
# *separate* shared object — libasmtest_drapp — which dlopen()s libdynamorio lazily
# on the C side once the client is configured, so nothing here links DynamoRIO.
#
# Advanced, Linux-x86-64-only, and opt-in. NativeTrace.available? reports whether the
# tier can run (built + libdynamorio resolvable) so callers self-skip cleanly. The
# lib path comes from env ASMTEST_DRAPP_LIB, else the gem-bundled slot
# native/<os>-<cpu>/libasmtest_drapp.<ext> next to this file, else
# <repo>/build/libasmtest_drapp.so; a load failure is caught so available? returns
# false rather than raising.
#
# Example:
#   require_relative "drtrace"
#   Asmtest::DrTrace::NativeTrace.start(client: "./build/libasmtest_drclient.so",
#                                       dynamorio_home: "/opt/DynamoRIO")
#   code = Asmtest::DrTrace::NativeCode.from_bytes(
#            [0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3].pack("C*"))  # mov rax,rdi; add; ret
#   tr = Asmtest::DrTrace::NativeTrace.create(blocks: 64, instructions: 0)
#   tr.register("add", code)
#   tr.region("add") { @r = code.call(20, 22) }
#   raise unless @r == 42 && tr.covered?(0)
#   Asmtest::DrTrace::NativeTrace.shutdown
require "fiddle"

module Asmtest
  # The optional DynamoRIO native-trace tier. Namespaced apart from the core
  # Asmtest module so requiring it is explicit (it loads a different .so).
  module DrTrace
    # Status code shared by the lifecycle + registration calls (ASMTEST_DR_OK).
    OK = 0

    VOIDP = Fiddle::TYPE_VOIDP
    LONG  = Fiddle::TYPE_LONG
    INT   = Fiddle::TYPE_INT
    SZ    = Fiddle::TYPE_SIZE_T
    LL    = Fiddle::TYPE_LONG_LONG
    VOID  = Fiddle::TYPE_VOID

    # The gem-bundled native slot next to this file (how the gem ships payloads),
    # mirroring Asmtest.resolve_emu_lib in asmtest.rb: native/<os>-<cpu>/.
    def self.bundled_slot
      os  = RbConfig::CONFIG["host_os"] =~ /darwin/ ? "darwin" : "linux"
      cpu = RbConfig::CONFIG["host_cpu"] =~ /arm|aarch64/ ? "arm64" : "x86_64"
      ext = os == "darwin" ? "dylib" : "so"
      [os, cpu, ext]
    end

    # The absolute path libasmtest_drapp actually resolved to (set by
    # resolve_drapp_lib), for library_path below.
    @resolved_path = nil

    # Resolve libasmtest_drapp. Order: an explicit ASMTEST_DRAPP_LIB wins (dev /
    # custom build); else the gem-bundled slot native/<os>-<cpu>/ next to this file
    # (how the gem ships it); else <repo>/build/libasmtest_drapp.so (the in-tree
    # build artifact). The build/ fallback stays BELOW the bundled path so an
    # installed gem never prefers a leaked checkout.
    def self.resolve_drapp_lib
      env = ENV["ASMTEST_DRAPP_LIB"].to_s
      unless env.empty?
        @resolved_path = env
        return env
      end
      os, cpu, ext = bundled_slot
      bundled = File.expand_path("native/#{os}-#{cpu}/libasmtest_drapp.#{ext}", __dir__)
      if File.exist?(bundled)
        @resolved_path = bundled
        return bundled
      end
      repo = File.expand_path("../../build/libasmtest_drapp.so", __dir__)
      @resolved_path = repo
      repo
    end

    # The gem-bundled DR client shipped alongside libasmtest_drapp, if present:
    # $ASMTEST_DRCLIENT wins, else native/<os>-<cpu>/libasmtest_drclient.<ext> next
    # to this file, else the <repo>/build tree; nil when none exists (the C side
    # then takes its own $ASMTEST_DRCLIENT / repo-build fallback). Mirrors
    # drtrace.py's _default_client.
    def self.default_client
      env = ENV["ASMTEST_DRCLIENT"].to_s
      return env unless env.empty?
      os, cpu, ext = bundled_slot
      cands = [
        File.expand_path("native/#{os}-#{cpu}/libasmtest_drclient.#{ext}", __dir__),
        File.expand_path("../../build/libasmtest_drclient.so", __dir__),
      ]
      cands.find { |c| File.exist?(c) }
    end

    # The absolute path of the libasmtest_drapp this process resolved (resolving it
    # if it hasn't been yet). Lets a clean-room test assert the bundled tier — not a
    # leaked build/ tree — satisfied the load. Mirrors drtrace.py's library_path().
    def self.library_path
      resolve_drapp_lib if @resolved_path.nil?
      @resolved_path
    end

    def self.func(lib, name, args, ret)
      Fiddle::Function.new(lib[name], args, ret)
    end

    # Load the lib and bind every entry point, OR leave LIB/FN nil if it can't load
    # (the tier requires DynamoRIO and may be absent). available? folds both the
    # load failure and a runtime asmtest_dr_available()==0 into a clean false.
    begin
      LIB = Fiddle.dlopen(resolve_drapp_lib)
      FN = {
        # ---- process-wide lifecycle ----
        available:    func(LIB, "asmtest_dr_available", [], INT),
        init:         func(LIB, "asmtest_dr_init", [VOIDP], INT),
        start:        func(LIB, "asmtest_dr_start", [], INT),
        stop:         func(LIB, "asmtest_dr_stop", [], INT),
        shutdown:     func(LIB, "asmtest_dr_shutdown", [], VOID),
        marker_error: func(LIB, "asmtest_dr_marker_error", [], INT),
        # ---- region registration + markers ----
        register:     func(LIB, "asmtest_dr_register_region", [VOIDP, VOIDP, SZ, VOIDP], INT),
        register_symbol: func(LIB, "asmtest_dr_register_symbol", [VOIDP, SZ, VOIDP], INT),
        symbol_demo:  func(LIB, "asmtest_symbol_demo", [LONG, LONG], LONG),
        unregister:   func(LIB, "asmtest_dr_unregister_region", [VOIDP], INT),
        trace_begin:  func(LIB, "asmtest_trace_begin", [VOIDP], VOID),
        trace_end:    func(LIB, "asmtest_trace_end", [VOIDP], VOID),
        # ---- host-native executable code ----
        exec_alloc:   func(LIB, "asmtest_exec_alloc", [VOIDP, SZ, VOIDP], INT),
        exec_free:    func(LIB, "asmtest_exec_free", [VOIDP], VOID),
        # ---- the app-owned trace handle + accessors (from the shared trace.o) ----
        trace_new:    func(LIB, "asmtest_trace_new", [SZ, SZ], VOIDP),
        trace_free:   func(LIB, "asmtest_trace_free", [VOIDP], VOID),
        trace_covered: func(LIB, "asmtest_trace_covered", [VOIDP, LL], INT),
        blocks_len:   func(LIB, "asmtest_emu_trace_blocks_len", [VOIDP], LL),
        insns_total:  func(LIB, "asmtest_emu_trace_insns_total", [VOIDP], LL),
        insns_len:    func(LIB, "asmtest_emu_trace_insns_len", [VOIDP], LL),
        block_at:     func(LIB, "asmtest_emu_trace_block_at", [VOIDP, SZ], LL),
        insn_at:      func(LIB, "asmtest_emu_trace_insn_at", [VOIDP, SZ], LL),
      }.freeze
    rescue Fiddle::DLError
      LIB = nil
      FN = nil
    end

    # Host-native machine code in real executable (W^X) memory. Mirrors the C
    # asmtest_exec_code_t {void* base; size_t len;}: 16 bytes, base at offset 0.
    class NativeCode
      EXEC_CODE_SIZE = 16

      # Wrap an already-allocated out-struct pointer (a Fiddle::Pointer over the
      # 16-byte asmtest_exec_code_t the alloc wrote into).
      def initialize(ec_ptr)
        @ec = ec_ptr
        @base = @ec[0, 8].unpack1("Q")   # void* base  (offset 0)
        @len = @ec[8, 8].unpack1("Q")    # size_t len  (offset 8)
        @freed = false
      end

      # Map executable memory and copy +bytes+ (a binary String) into it, returning
      # a NativeCode whose #base is the live entry address.
      def self.from_bytes(bytes)
        ptr = Fiddle::Pointer.new(Fiddle.malloc(EXEC_CODE_SIZE), EXEC_CODE_SIZE)
        ptr[0, EXEC_CODE_SIZE] = "\x00".b * EXEC_CODE_SIZE
        rc = DrTrace::FN[:exec_alloc].call(bytes, bytes.bytesize, ptr)
        if rc != DrTrace::OK
          Fiddle.free(ptr.to_i)
          raise "asmtest_exec_alloc failed: #{rc}"
        end
        new(ptr)
      end

      # Entry address of the executable mapping (offset 0 = entry).
      def base
        @base
      end

      # Number of code bytes copied.
      def length
        @len
      end

      # Invoke the code through a function pointer with +a+ and +b+ as C longs,
      # reading the result back as a long (the SysV integer ABI). Fiddle::Function
      # accepts the raw entry address as its first argument.
      def call(a = 0, b = 0)
        Fiddle::Function.new(@base, [LONG, LONG], LONG).call(a, b)
      end

      # Unmap the executable memory. The caller MUST unregister the range first if it
      # was registered (see the C header) — NativeTrace#unregister does that.
      def free
        return if @freed
        DrTrace::FN[:exec_free].call(@ec)
        Fiddle.free(@ec.to_i)
        @freed = true
      end
    end

    # An app-owned coverage recorder for a registered native region. Wraps the opaque
    # asmtest_trace_t handle; instruction recording when instructions > 0, block
    # recording when blocks > 0.
    class NativeTrace
      attr_reader :handle

      def initialize(handle)
        @handle = handle
      end

      # ---- process-wide lifecycle (class-level; one DynamoRIO per process) ----

      # True if the tier can run: the lib loaded AND libdynamorio is resolvable.
      # Folds a load failure (FN nil) and a runtime asmtest_dr_available()==0 into
      # one clean false, so callers self-skip without a rescue.
      def self.available?
        return false if DrTrace::FN.nil?
        DrTrace::FN[:available].call != 0
      end

      # Bring DynamoRIO up in-process and take over (init then start). +client+ is the
      # path to libasmtest_drclient.so (else DrTrace.default_client: $ASMTEST_DRCLIENT,
      # then the gem-bundled slot / repo build; else the C side's own fallback);
      # +dynamorio_home+ lets the C side find libdynamorio (else $ASMTEST_DR_LIB /
      # rpath); +client_options+ are extra client options; +mode+ is the process-init
      # default recording mode. Raises on a nonzero rc.
      #
      # Named #start (not Ruby's reserved #initialize, and distinct from the instance
      # region begin) to read as a lifecycle verb at the class level.
      def self.start(client: nil, dynamorio_home: nil, client_options: nil, mode: 0)
        client ||= DrTrace.default_client # prefer the bundled DR client in a gem
        opts = DrTrace.build_options(client, dynamorio_home, client_options, mode)
        rc = DrTrace::FN[:init].call(opts)
        raise "asmtest_dr_init failed: #{rc}" if rc != DrTrace::OK
        rc = DrTrace::FN[:start].call
        raise "asmtest_dr_start failed: #{rc}" if rc != DrTrace::OK
      end

      # Stop DynamoRIO and clean up (dr_app_stop_and_cleanup), returning to UNINIT.
      def self.shutdown
        DrTrace::FN[:shutdown].call
      end

      # Count of illegal marker operations since init (0 == every marker balanced).
      def self.marker_error
        DrTrace::FN[:marker_error].call
      end

      # Call the exported asmtest_symbol_demo fixture (a*2+b) that symbol-mode
      # tests trace by name.
      def self.symbol_demo(a, b)
        DrTrace::FN[:symbol_demo].call(a, b)
      end

      # ---- per-trace ----

      # Allocate a trace handle. NOTE the C order: asmtest_trace_new(insns_cap,
      # blocks_cap) — instructions FIRST, blocks SECOND. Named #create (not #new)
      # because the handle comes from C, not a plain Ruby allocation.
      def self.create(blocks: 64, instructions: 0)
        h = DrTrace::FN[:trace_new].call(instructions, blocks)
        raise "asmtest_trace_new failed" if h.null?
        new(h)
      end

      # Register a non-overlapping native range under +name+, recording coverage into
      # this trace. The client resolves the range; +code+ is a NativeCode.
      def register(name, code)
        rc = DrTrace::FN[:register].call(name, code.base, code.length, @handle)
        raise "register_region(#{name.inspect}) failed: #{rc}" if rc != DrTrace::OK
        self
      end

      # Symbol mode: trace a named exported function with no begin/end markers —
      # recording is always-on over [entry, entry+max_len). The client resolves
      # +name+ across all loaded modules.
      def register_symbol(name, max_len = 256)
        rc = DrTrace::FN[:register_symbol].call(name, max_len, @handle)
        raise "register_symbol(#{name.inspect}) failed: #{rc}" if rc != DrTrace::OK
        self
      end

      # Drop the named region (the client drops its cached translation). Call this
      # before NativeCode#free per the C header's unregister-then-free ordering.
      def unregister(name)
        DrTrace::FN[:unregister].call(name)
      end

      # Open recording for +name+, run the block, then close it — markers stay
      # balanced even if the block raises (the ensure runs end). Returns the block's
      # value.
      def region(name)
        DrTrace::FN[:trace_begin].call(name)
        begin
          yield
        ensure
          DrTrace::FN[:trace_end].call(name)
        end
      end

      # True if the basic block at byte-offset +off+ (from the region entry) was entered.
      def covered?(off)
        DrTrace::FN[:trace_covered].call(@handle, off) != 0
      end

      # Number of distinct basic blocks recorded.
      def blocks_len
        DrTrace::FN[:blocks_len].call(@handle)
      end

      # Total instructions in the ordered instruction stream (instruction mode).
      def insns_total
        DrTrace::FN[:insns_total].call(@handle)
      end

      # The distinct basic-block start offsets recorded, in first-seen order.
      def block_offsets
        n = DrTrace::FN[:blocks_len].call(@handle)
        (0...n).map { |i| DrTrace::FN[:block_at].call(@handle, i) }
      end

      # The ordered instruction-offset stream actually stored — each executed
      # instruction's offset in execution order, up to the trace's insns capacity
      # (insns_len, not the possibly-larger insns_total).
      def insn_offsets
        n = DrTrace::FN[:insns_len].call(@handle)
        (0...n).map { |i| DrTrace::FN[:insn_at].call(@handle, i) }
      end

      def free
        return unless @handle
        DrTrace::FN[:trace_free].call(@handle)
        @handle = nil
      end
    end

    # Build the asmtest_drtrace_options_t the C side reads: three const char* then an
    # int mode. The layout is {ptr, ptr, ptr, int} = 3 * 8 + 4, padded to 32 bytes on
    # x86-64. A nil/empty string becomes a NULL pointer so the C side takes its env
    # fallback (e.g. $ASMTEST_DRCLIENT for the client). The string buffers and the
    # struct are kept alive on the returned pointer so the GC can't free them mid-call.
    def self.build_options(client, dynamorio_home, client_options, mode)
      opts = Fiddle::Pointer.new(Fiddle.malloc(32), 32)
      opts[0, 32] = "\x00".b * 32
      held = []
      write_strp = lambda do |off, s|
        next if s.nil? || s.empty?
        buf = Fiddle::Pointer[s + "\x00"]   # NUL-terminated C string, GC-pinned below
        held << buf
        opts[off, 8] = [buf.to_i].pack("Q")
      end
      write_strp.call(0, dynamorio_home) # const char* dynamorio_home
      write_strp.call(8, client)         # const char* client_path
      write_strp.call(16, client_options) # const char* client_options
      opts[24, 4] = [mode].pack("l")     # asmtest_drtrace_mode_t mode (int)
      opts.instance_variable_set(:@asmtest_held, held) # pin string buffers to opts
      opts
    end
  end
end
