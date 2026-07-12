# frozen_string_literal: true

# asmtest/dataflow (Ruby, Fiddle) — binding for the data-flow analysis tier (Phase 6).
#
# Wraps libasmtest_dataflow (built with `make shared-dataflow`): this increment mirrors
# the Python/C++/Node bindings' pure helpers — the GC-move canonicalizer and the
# tiered-re-JIT method resolver. Fiddle has no high-level struct type, so the record
# arrays are packed into C-heap-stable buffers (Fiddle::Pointer.malloc — Ruby 3 GC
# compaction can move String data, so the buffers must not be plain Strings).
require "fiddle"

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
  end
end
