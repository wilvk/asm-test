// asmtest_dataflow.hpp — C++ binding for the data-flow analysis tier (Phase 6).
//
// Thin, header-only, typed conveniences over the C API in asmtest_valtrace.h (which
// is extern "C"-guarded). This first increment mirrors the Python binding: the pure
// GC-move canonicalizer and the tiered-re-JIT method resolver. Link the pure analysis
// objects (dataflow_gcmove.o + dataflow_method.o) or libasmtest_dataflow.
#pragma once

#include "asmtest_valtrace.h"

#include <cstdint>
#include <set>
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

// A location: a register (LOC_REG, key = Capstone reg id) or absolute memory
// (LOC_MEM_ABS, key = address). Mirrors at_loc_kind_t.
struct Loc {
    at_loc_kind_t kind;
    std::uint64_t key;
};
inline Loc Reg(std::uint32_t id) { return {AT_LOC_REG, id}; }
inline Loc MemAbs(std::uint64_t addr) { return {AT_LOC_MEM_ABS, addr}; }

// A hand-built L0 value trace fed to the L1 def-use builder + L2 slicer — the RAII
// analog of the Python ValueTrace. step() records an instruction's read/write operand
// locations; forwardSlice/backwardSlice return the reached step-index set. Normally a
// producer (emulator / ptrace / DR) fills the trace.
class ValueTrace {
public:
    explicit ValueTrace(std::size_t stepsCap = 256, std::size_t recsCap = 2048)
        : v_(asmtest_valtrace_new(stepsCap, recsCap, 0)) {}
    ~ValueTrace() {
        if (g_) asmtest_defuse_free(g_);
        if (v_) asmtest_valtrace_free(v_);
    }
    ValueTrace(const ValueTrace&) = delete;
    ValueTrace& operator=(const ValueTrace&) = delete;

    ValueTrace& step(std::uint64_t off, const std::vector<Loc>& reads,
                     const std::vector<Loc>& writes) {
        std::vector<at_val_rec_t> recs;
        recs.reserve(reads.size() + writes.size());
        for (const auto& l : reads) recs.push_back(rec(l, false));
        for (const auto& l : writes) recs.push_back(rec(l, true));
        asmtest_valtrace_append(v_, off, recs.data(), recs.size());
        ++nSteps_;
        if (g_) { asmtest_defuse_free(g_); g_ = nullptr; }  // invalidate a stale graph
        return *this;
    }

    std::set<std::uint32_t> forwardSlice(std::uint32_t origin) { return slice(origin, true); }
    std::set<std::uint32_t> backwardSlice(std::uint32_t sink) { return slice(sink, false); }

private:
    static at_val_rec_t rec(const Loc& l, bool isWrite) {
        at_val_rec_t r{};
        r.kind = l.kind;
        if (l.kind == AT_LOC_REG)
            r.reg = static_cast<std::uint32_t>(l.key);
        else
            r.addr = l.key;
        r.is_write = isWrite;
        return r;
    }
    std::set<std::uint32_t> slice(std::uint32_t origin, bool forward) {
        if (!g_) g_ = asmtest_defuse_build(v_);
        at_val_rec_t seed{};
        seed.step = origin;
        asmtest_slice_t* s =
            forward ? asmtest_slice_forward(g_, seed) : asmtest_slice_backward(g_, seed);
        std::set<std::uint32_t> out;
        for (std::uint32_t i = 0; i < nSteps_; ++i)
            if (asmtest_slice_contains(s, i)) out.insert(i);
        asmtest_slice_free(s);
        return out;
    }

    asmtest_valtrace_t* v_;
    asmtest_defuse_t* g_ = nullptr;
    std::uint32_t nSteps_ = 0;
};

}  // namespace dataflow
}  // namespace asmtest
