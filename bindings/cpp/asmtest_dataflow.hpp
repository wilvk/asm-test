// asmtest_dataflow.hpp — C++ binding for the data-flow analysis tier (Phase 6).
//
// Thin, header-only, typed conveniences over the C API in asmtest_valtrace.h (which
// is extern "C"-guarded). This first increment mirrors the Python binding: the pure
// GC-move canonicalizer and the tiered-re-JIT method resolver. Link the pure analysis
// objects (dataflow_gcmove.o + dataflow_method.o) or libasmtest_dataflow.
#pragma once

#include "asmtest_valtrace.h"

#include <cstdint>
#include <vector>

namespace asmtest {
namespace dataflow {

// A GC move-range (the shape of an EventPipe GCBulkMovedObjectRanges entry):
// [old_base, old_base+len) was relocated to [new_base, new_base+len) at `step`.
struct GcMove {
    std::uint64_t old_base;
    std::uint64_t new_base;
    std::uint64_t len;
    std::uint32_t step;
};

// Map heap address `phys` observed at value-trace `step` to its canonical
// (final-resting) address across the compactions in `moves` (sorted ascending by
// step). See asmtest_gcmove_canon. Pure.
inline std::uint64_t gcmove_canon(const std::vector<GcMove>& moves,
                                  std::uint32_t step, std::uint64_t phys) {
    std::vector<asmtest_gcmove_t> c;
    c.reserve(moves.size());
    for (const auto& m : moves)
        c.push_back(asmtest_gcmove_t{m.old_base, m.new_base, m.len, m.step});
    return asmtest_gcmove_canon(c.empty() ? nullptr : c.data(), c.size(), step, phys);
}

// A method-map entry: [addr, addr+size) bounds the body (size 0 = point match on
// addr), a borrowed name, and the version / code_index re-JIT counter.
struct Method {
    std::uint64_t addr;
    std::uint64_t size;
    const char* name;
    std::uint64_t version;
};

// Resolve `pc` to the index of the owning method-map record, or -1. Newest version
// wins on an in-place tiered re-JIT collision. See asmtest_method_resolve_pc. Pure.
inline int method_resolve_pc(const std::vector<Method>& methods, std::uint64_t pc) {
    std::vector<asmtest_method_t> c;
    c.reserve(methods.size());
    for (const auto& m : methods)
        c.push_back(asmtest_method_t{m.addr, m.size, m.name, m.version});
    return asmtest_method_resolve_pc(c.empty() ? nullptr : c.data(), c.size(), pc);
}

}  // namespace dataflow
}  // namespace asmtest
