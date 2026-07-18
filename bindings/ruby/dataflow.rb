# frozen_string_literal: true

# asmtest/dataflow (Ruby, Fiddle) — binding for the data-flow analysis tier (Phase 6).
#
# Wraps libasmtest_dataflow (built with `make shared-dataflow`): this increment mirrors
# the Python/C++/Node bindings' pure helpers — the GC-move canonicalizer and the
# tiered-re-JIT method resolver. Fiddle has no high-level struct type, so the record
# arrays are packed into C-heap-stable buffers (Fiddle::Pointer.malloc — Ruby 3 GC
# compaction can move String data, so the buffers must not be plain Strings).
require "fiddle"
require "set"

module Asmtest
  module DataFlow
    U64 = 0xFFFF_FFFF_FFFF_FFFF
    LL = Fiddle::TYPE_LONG_LONG
    INT = Fiddle::TYPE_INT
    SZ = Fiddle::TYPE_SIZE_T
    VOIDP = Fiddle::TYPE_VOIDP

    module_function

    def library_path
      return ENV["ASMTEST_DATAFLOW_LIB"] if ENV["ASMTEST_DATAFLOW_LIB"]

      bundled = File.join(__dir__, "_libs", "libasmtest_dataflow.so")
      return bundled if File.exist?(bundled)

      File.join(__dir__, "..", "..", "build", "libasmtest_dataflow.so")
    end

    LIB = Fiddle.dlopen(library_path)
    GCMOVE_CANON = Fiddle::Function.new(LIB["asmtest_gcmove_canon"], [VOIDP, SZ, INT, LL], LL)
    METHOD_RESOLVE = Fiddle::Function.new(LIB["asmtest_method_resolve_pc"], [VOIDP, SZ, LL], INT)

    # Copy `bytes` into a freshly malloc'd C buffer (stable across Ruby GC compaction).
    # The returned Fiddle::Pointer frees itself on GC; the caller keeps it alive across
    # the FFI call.
    def cbuf(bytes)
      p = Fiddle::Pointer.malloc(bytes.bytesize)
      p[0, bytes.bytesize] = bytes
      p
    end
    private_class_method :cbuf

    # at_loc_kind_t (include/asmtest_valtrace.h): the location space of an operand.
    LOC_REG = 0       # a register (key = Capstone reg id)
    LOC_MEM_ABS = 1   # memory at an absolute effective address (key = addr)
    LOC_MEM_OFF = 2   # memory at a routine offset

    # at_val_rec_t is 72 bytes (verified via offsetof on this Linux x86-64 build):
    # kind(l<,0) reg/base/index(L<x3,4/8/12) scale(l<,16) pad(x4,20) disp(q<,24)
    # addr(Q<,32) size(S<,40) is_write/value_valid/wide(C3,42) pad(x3,45)
    # wide_off(L<,48) pad(x4,52) value(Q<,56) step(L<,64) pad(x4,68).
    REC_PACK = "l<L<L<L<l<x4q<Q<S<C3x3L<x4Q<L<x4"

    # Pack one at_val_rec_t. The hand-built ValueTrace#step API only ever sets
    # kind/reg-or-addr/is_write/step; a live capture fills records natively and
    # never goes through this path.
    def pack_rec(kind:, is_write:, reg: 0, base: 0, index: 0, scale: 0, disp: 0,
                 addr: 0, size: 0, value: 0, step: 0)
      [kind, reg, base, index, scale, disp, addr, size,
       is_write ? 1 : 0, 0, 0, 0, value, step].pack(REC_PACK)
    end
    private_class_method :pack_rec

    # loc is (kind, key): key is a reg id for LOC_REG, else an absolute address.
    def mk_rec(loc, is_write)
      kind, key = loc
      if kind == LOC_REG
        pack_rec(kind: kind, reg: key, is_write: is_write)
      else
        pack_rec(kind: kind, addr: key, is_write: is_write)
      end
    end
    private_class_method :mk_rec

    # Map heap address `phys` observed at value-trace `step` to its canonical
    # (final-resting) address across the compactions in `moves` (each an
    # [old_base, new_base, len, step] array, sorted ascending by step).
    def gcmove_canon(moves, step, phys)
      if moves.empty?
        return GCMOVE_CANON.call(Fiddle::NULL, 0, step & 0xFFFF_FFFF, phys) & U64
      end

      # at_gcmove_t: uint64 old, new, len; uint32 step; 4 pad bytes = 32 bytes.
      packed = moves.map { |o, n, l, s| [o, n, l, s].pack("Q<Q<Q<L<x4") }.join
      buf = cbuf(packed)
      GCMOVE_CANON.call(buf, moves.length, step & 0xFFFF_FFFF, phys) & U64
    end

    # Resolve `pc` to the owning method-map record index, or -1. Each method is an
    # [addr, size, name, version] array (size 0 = point match on addr).
    def method_resolve_pc(methods, pc)
      return METHOD_RESOLVE.call(Fiddle::NULL, 0, pc) if methods.empty?

      # Each name lives in its own C-heap buffer (stable pointer); keep them alive.
      name_bufs = methods.map { |m| cbuf("#{m[2]}\0") }
      # at_method_t: uint64 addr, size, name-ptr, version = 32 bytes.
      packed = methods.each_with_index.map do |(addr, size, _name, ver), i|
        [addr, size, name_bufs[i].to_i, ver].pack("Q<Q<Q<Q<")
      end.join
      buf = cbuf(packed)
      result = METHOD_RESOLVE.call(buf, methods.length, pc)
      # Keep the name buffers (and buf) referenced until the call returns.
      name_bufs.clear
      result
    end

    # --- F7: live-attach data-flow capture -------------------------------- #
    # The scoped ptrace producer's return codes (src/dataflow_ptrace.c). The
    # producer ships NO header on purpose (a value-trace PRODUCER is a tier, not
    # part of the shared sink API), so — exactly as its own C suite does — this
    # binding re-declares them. Keep in step with that file.
    PTRACE_OK = 0       # a complete scoped trace
    PTRACE_FAULT = 1    # the routine faulted; a partial trace is filled
    PTRACE_EINVAL = -1  # bad arguments
    PTRACE_ENOSYS = -3  # off Linux x86-64 / no Capstone: the tier is absent
    PTRACE_ETRACE = -4  # ptrace/wait failure (seccomp/yama)

    VOID = Fiddle::TYPE_VOID
    VALTRACE_NEW = Fiddle::Function.new(LIB["asmtest_valtrace_new"], [SZ, SZ, SZ], VOIDP)
    VALTRACE_FREE = Fiddle::Function.new(LIB["asmtest_valtrace_free"], [VOIDP], VOID)
    VALTRACE_STEPS = Fiddle::Function.new(LIB["asmtest_valtrace_steps"], [VOIDP], SZ)
    VALTRACE_RECS = Fiddle::Function.new(LIB["asmtest_valtrace_recs"], [VOIDP], SZ)
    VALTRACE_APPEND = Fiddle::Function.new(
      LIB["asmtest_valtrace_append"], [VOIDP, LL, VOIDP, SZ], VOID
    )
    # L1 def-use graph + L2 slice (analysis pipeline, src/dataflow.c). The seed
    # crosses BY POINTER (asmtest_slice_forward_seed/_backward_seed) — Fiddle has
    # no by-value struct type, and a 72-byte at_val_rec_t is SysV MEMORY-class.
    DEFUSE_BUILD = Fiddle::Function.new(LIB["asmtest_defuse_build"], [VOIDP], VOIDP)
    DEFUSE_FREE = Fiddle::Function.new(LIB["asmtest_defuse_free"], [VOIDP], VOID)
    SLICE_FORWARD_SEED = Fiddle::Function.new(
      LIB["asmtest_slice_forward_seed"], [VOIDP, VOIDP], VOIDP
    )
    SLICE_BACKWARD_SEED = Fiddle::Function.new(
      LIB["asmtest_slice_backward_seed"], [VOIDP, VOIDP], VOIDP
    )
    SLICE_FREE = Fiddle::Function.new(LIB["asmtest_slice_free"], [VOIDP], VOID)
    SLICE_CONTAINS = Fiddle::Function.new(LIB["asmtest_slice_contains"], [VOIDP, INT], INT)
    # pid_t is int; base/max_insns/when are uint64; result is long* (8 bytes here);
    # survived is int*; img and vt are opaque pointers. No struct crosses by value.
    ATTACH_PID = Fiddle::Function.new(
      LIB["asmtest_dataflow_ptrace_attach_pid"], [INT, LL, SZ, LL, VOIDP, VOIDP], INT
    )
    ATTACH_PID_TID = Fiddle::Function.new(
      LIB["asmtest_dataflow_ptrace_attach_pid_tid"],
      [INT, INT, LL, SZ, LL, VOIDP, VOIDP], INT
    )
    ATTACH_JIT = Fiddle::Function.new(
      LIB["asmtest_dataflow_ptrace_attach_jit"],
      [INT, INT, LL, SZ, VOIDP, LL, LL, VOIDP, VOIDP, VOIDP], INT
    )

    # True iff this build's live-attach tier is real (Linux x86-64 + Capstone)
    # rather than the off-platform ENOSYS stub. PROBED, not guessed: an argument-
    # rejecting call returns EINVAL from the real producer and ENOSYS from the
    # stub, which is the only way to tell them apart — the symbol resolves either
    # way. Attaches to nothing.
    def live_attach_available?
      v = VALTRACE_NEW.call(1, 1, 0)
      begin
        out = Fiddle::Pointer.malloc(8)
        ATTACH_PID.call(0, 0, 0, 0, out, v) != PTRACE_ENOSYS
      ensure
        VALTRACE_FREE.call(v)
      end
    end

    # An L0 value trace handle (opaque) filled by the LIVE-ATTACH producer, or built
    # by hand via #step. The same asmtest_valtrace_t the pure analysis layers
    # consume, so either path feeds the same L1 def-use graph (#defuse) and L2
    # slicer (#forward_slice / #backward_slice) — the tier's whole design point.
    # The slice seed crosses BY POINTER (asmtest_slice_forward_seed/_backward_seed),
    # which is what makes this wrappable in Fiddle at all (a by-value at_val_rec_t
    # is SysV MEMORY-class and Fiddle has no type for it).
    class ValueTrace
      def initialize(steps_cap = 256, recs_cap = 2048)
        @v = VALTRACE_NEW.call(steps_cap, recs_cap, 0)
        raise "asmtest_valtrace_new failed" if @v.null?
        @g = nil
        @n_steps = 0
      end

      # Append one executed instruction at offset `off` reading `reads` and writing
      # `writes` (each a sequence of [kind, key] locations — kind is LOC_REG (key =
      # reg id) or LOC_MEM_ABS (key = address)). Read-set before write-set.
      def step(off, reads: [], writes: [])
        recs = reads.map { |loc| DataFlow.send(:mk_rec, loc, false) } +
               writes.map { |loc| DataFlow.send(:mk_rec, loc, true) }
        if recs.empty?
          VALTRACE_APPEND.call(@v, off, Fiddle::NULL, 0)
        else
          buf = DataFlow.send(:cbuf, recs.join)
          VALTRACE_APPEND.call(@v, off, buf, recs.length)
        end
        @n_steps += 1
        invalidate_defuse
        self
      end

      # Attach to LIVE `pid`, single-step [base, base+code_len), then DETACH leaving
      # the target running. Steps the thread-group LEADER. Returns [rc, result].
      def attach_pid(pid, base, code_len, max_insns = 0)
        out = Fiddle::Pointer.malloc(8)
        rc = ATTACH_PID.call(pid, base, code_len, max_insns, out, @v)
        post_attach
        [rc, out[0, 8].unpack1("q<")]
      end

      # As attach_pid, but SEIZE every thread and step whichever one first ENTERS
      # the region (by its own tid, never assumed to be the leader); only_tid 0 =
      # any. The entry managed methods need — they run on workers. [rc, result].
      def attach_pid_tid(pid, only_tid, base, code_len, max_insns = 0)
        out = Fiddle::Pointer.malloc(8)
        rc = ATTACH_PID_TID.call(pid, only_tid, base, code_len, max_insns, out, @v)
        post_attach
        [rc, out[0, 8].unpack1("q<")]
      end

      # The JIT-aware live attach: worker-targeting plus an explicit survival
      # report. Returns [rc, result, survived]; survived == 1 means the detach left
      # the target alive. The versioned-decode code-image (img/when) is NULL here —
      # operands decode from a live snapshot.
      def attach_jit(pid, only_tid, base, code_len, max_insns = 0, when_seq = 0)
        out = Fiddle::Pointer.malloc(8)
        survived = Fiddle::Pointer.malloc(4)
        rc = ATTACH_JIT.call(pid, only_tid, base, code_len, Fiddle::NULL, when_seq,
                             max_insns, out, survived, @v)
        post_attach
        [rc, out[0, 8].unpack1("q<"), survived[0, 4].unpack1("l<")]
      end

      def steps = VALTRACE_STEPS.call(@v)
      def recs = VALTRACE_RECS.call(@v)

      # The L1 last-writer def-use graph over this trace, built once and cached
      # until the next #step / attach invalidates it.
      def defuse
        @g ||= begin
          g = DEFUSE_BUILD.call(@v)
          raise "asmtest_defuse_build failed" if g.null?

          g
        end
      end

      # Steps influenced by the value defined at step `origin` (origin included).
      def forward_slice(origin)
        slice(origin, true)
      end

      # Steps that produced the value used at step `sink` (sink included).
      def backward_slice(sink)
        slice(sink, false)
      end

      def free
        return if @v.nil? || @v.null?

        invalidate_defuse
        VALTRACE_FREE.call(@v)
        @v = nil
      end

      private

      # A live producer appends behind our back (unlike #step, which counts as it
      # goes), so resync the step count and drop any stale def-use graph.
      def post_attach
        @n_steps = VALTRACE_STEPS.call(@v)
        invalidate_defuse
      end

      def invalidate_defuse
        return unless @g

        DEFUSE_FREE.call(@g)
        @g = nil
      end

      def slice(origin, forward)
        g = defuse
        seed = DataFlow.send(:cbuf, DataFlow.send(:pack_rec, kind: LOC_REG, is_write: false, step: origin))
        fn = forward ? SLICE_FORWARD_SEED : SLICE_BACKWARD_SEED
        s = fn.call(g, seed)
        begin
          out = Set.new
          (0...@n_steps).each { |i| out << i if SLICE_CONTAINS.call(s, i) != 0 }
          out
        ensure
          SLICE_FREE.call(s)
        end
      end
    end
  end
end
