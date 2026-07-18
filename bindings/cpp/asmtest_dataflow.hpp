// asmtest_dataflow.hpp — C++ binding for the data-flow tier (Phase 6 + F7).
//
// Thin, header-only, typed conveniences over the C API in asmtest_valtrace.h (which
// is extern "C"-guarded): the pure GC-move canonicalizer, the tiered-re-JIT method
// resolver, the L0/L1/L2 ValueTrace pipeline, and — F7 — LIVE-ATTACH capture over a
// running pid. Link the pure analysis objects (dataflow_gcmove.o + dataflow_method.o),
// adding dataflow_ptrace.o + dataflow_operands.o + codeimage.o for the live attach,
// or just libasmtest_dataflow (which carries all of them).
#pragma once

#include "asmtest_codeimage.h"
#include "asmtest_valtrace.h"

#include <cstdint>
#include <set>
#include <sys/types.h>
#include <utility>
#include <vector>

// F7 — the scoped ptrace L0 producer's live-attach entry points. Declared HERE, not
// #included: a value-trace PRODUCER is a tier, not part of the shared sink API, so
// src/dataflow_ptrace.c ships no header and every caller re-declares what it uses —
// exactly as that tier's own C suite (examples/test_dataflow_ptrace.c) does. Keep in
// step with src/dataflow_ptrace.c. asmtest_codeimage_t itself comes from the real
// shipped header (#included above) rather than a hand forward-declaration.
extern "C" {
// Attach to LIVE `pid`, single-step [base, base+code_len), then DETACH leaving the
// target running. Steps the thread-group LEADER. `result` takes the region's return
// value; `vt` is filled with the captured trace. max_insns 0 = the tier's backstop.
int asmtest_dataflow_ptrace_attach_pid(pid_t pid, std::uint64_t base,
                                       std::size_t code_len, std::uint64_t max_insns,
                                       long* result, asmtest_valtrace_t* vt);
// As above, but SEIZE every thread and step whichever one first ENTERS the region
// (identified by its own tid, never assumed to be the leader); only_tid 0 = any, a
// nonzero value pins one thread. The entry managed methods need — they run on workers.
int asmtest_dataflow_ptrace_attach_pid_tid(pid_t pid, pid_t only_tid,
                                           std::uint64_t base, std::size_t code_len,
                                           std::uint64_t max_insns, long* result,
                                           asmtest_valtrace_t* vt);
// The JIT-aware entry: worker-targeting + a versioned code-image byte source
// (`img`/`when`; NULL = decode from a live snapshot) + an explicit `survived` report.
int asmtest_dataflow_ptrace_attach_jit(pid_t pid, pid_t only_tid, std::uint64_t base,
                                       std::size_t code_len, asmtest_codeimage_t* img,
                                       std::uint64_t when, std::uint64_t max_insns,
                                       long* result, int* survived,
                                       asmtest_valtrace_t* vt);
// The versioned-decode entry: like attach_pid, but decodes each step's operands
// from `img`'s bytes as of sequence `when` instead of a live snapshot. `img` may
// be NULL (degrades to exactly attach_pid).
int asmtest_dataflow_ptrace_attach_pid_versioned(pid_t pid, std::uint64_t base,
                                                 std::size_t code_len,
                                                 std::uint64_t max_insns,
                                                 asmtest_codeimage_t* img,
                                                 std::uint64_t when, long* result,
                                                 asmtest_valtrace_t* vt);
}

namespace asmtest {
namespace dataflow {

// The scoped ptrace producer's return codes (src/dataflow_ptrace.c). Re-declared for
// the same reason the entry points above are.
enum PtraceRc {
    kPtraceOk = 0,       // a complete scoped trace
    kPtraceFault = 1,    // the routine faulted; a partial trace is filled
    kPtraceEinval = -1,  // bad arguments
    kPtraceEnosys = -3,  // off Linux x86-64 / no Capstone: the tier is absent
    kPtraceEtrace = -4,  // ptrace/wait failure (seccomp/yama): the caller self-skips
};

// The outcome of a live attach: the producer's code, the region's return value, and
// (attachJit only) whether the detach left the target alive.
struct AttachResult {
    int rc;
    long result;
    int survived;
    bool ok() const { return rc == kPtraceOk; }
};

// True iff this build's live-attach tier is real (Linux x86-64 + Capstone) rather
// than the off-platform ENOSYS stub. PROBED, not guessed — an argument-rejecting
// call returns EINVAL from the real producer and ENOSYS from the stub, which is the
// only way to tell them apart when the symbol links either way. Attaches to nothing.
inline bool live_attach_available() {
    asmtest_valtrace_t* v = asmtest_valtrace_new(1, 1, 0);
    long out = 0;
    int rc = asmtest_dataflow_ptrace_attach_pid(0, 0, 0, 0, &out, v);
    asmtest_valtrace_free(v);
    return rc != kPtraceEnosys;
}

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

// A timestamped code-image timeline for one target process (asmtest_codeimage.h),
// RAII-owned. track() begins recording [base, base+len); bytesAt() answers "what
// bytes were live at addr as of sequence `when`" — the decode a mid-capture re-JIT
// needs. pid 0 (the default) records THIS process.
class CodeImage {
public:
    explicit CodeImage(pid_t pid = 0) : h_(asmtest_codeimage_new(pid)) {}
    ~CodeImage() { if (h_) asmtest_codeimage_free(h_); }
    CodeImage(const CodeImage&) = delete;
    CodeImage& operator=(const CodeImage&) = delete;

    static bool available() { return asmtest_codeimage_available() != 0; }

    // The raw handle, for passing to ValueTrace::attachJit / attachPidVersioned.
    asmtest_codeimage_t* handle() const { return h_; }

    // Begin tracking [base, base+len): snapshot version 0 and arm change detection.
    // Returns ASMTEST_CI_OK (0) or a negative status.
    int track(std::uint64_t base, std::size_t len) {
        return asmtest_codeimage_track(h_, reinterpret_cast<const void*>(base), len);
    }

    // The current capture sequence (a monotonic logical timestamp).
    std::uint64_t now() const { return asmtest_codeimage_now(h_); }

    // The bytes live at `addr` as of sequence `when` (0 = latest), or an empty
    // optional-like pair {false, {}} if the address was never tracked / had no
    // version at-or-before `when`.
    std::pair<bool, std::vector<std::uint8_t>> bytesAt(std::uint64_t addr,
                                                        std::uint64_t when = 0) const {
        const std::uint8_t* out = nullptr;
        std::size_t outLen = 0;
        int rc = asmtest_codeimage_bytes_at(h_, reinterpret_cast<const void*>(addr),
                                            when, &out, &outLen);
        if (rc != ASMTEST_CI_OK || out == nullptr)
            return {false, {}};
        return {true, std::vector<std::uint8_t>(out, out + outLen)};
    }

private:
    asmtest_codeimage_t* h_;
};

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

    // --- F7: live-attach capture (fills THIS trace) --- //
    // The producer fills the very asmtest_valtrace_t this object owns, so a live
    // capture flows straight into forwardSlice/backwardSlice above — the point of
    // every tier sharing one L0 sink. Each resyncs the step count from the native
    // trace (the producer appends behind our back) and drops a stale def-use graph.

    // Attach to LIVE `pid` and single-step [base, base+codeLen); the target survives.
    AttachResult attachPid(pid_t pid, std::uint64_t base, std::size_t codeLen,
                           std::uint64_t maxInsns = 0) {
        AttachResult r{};
        r.rc = asmtest_dataflow_ptrace_attach_pid(pid, base, codeLen, maxInsns,
                                                  &r.result, v_);
        postAttach();
        return r;
    }
    // As attachPid, but step whichever THREAD enters the region (onlyTid 0 = any).
    AttachResult attachPidTid(pid_t pid, pid_t onlyTid, std::uint64_t base,
                              std::size_t codeLen, std::uint64_t maxInsns = 0) {
        AttachResult r{};
        r.rc = asmtest_dataflow_ptrace_attach_pid_tid(pid, onlyTid, base, codeLen,
                                                      maxInsns, &r.result, v_);
        postAttach();
        return r;
    }
    // The JIT-aware attach: worker-targeting plus a `survived` report. `img`/`when`
    // are the versioned code-image byte source (a CodeImage's raw handle, or
    // nullptr for live-snapshot decode — the default).
    AttachResult attachJit(pid_t pid, pid_t onlyTid, std::uint64_t base,
                           std::size_t codeLen, std::uint64_t maxInsns = 0,
                           asmtest_codeimage_t* img = nullptr, std::uint64_t when = 0) {
        AttachResult r{};
        r.rc = asmtest_dataflow_ptrace_attach_jit(pid, onlyTid, base, codeLen, img, when,
                                                  maxInsns, &r.result, &r.survived, v_);
        postAttach();
        return r;
    }
    // The versioned-decode attach: steps the thread-group LEADER (like attachPid)
    // but decodes each step's operands from `img`'s bytes as of sequence `when`
    // instead of a live snapshot — the right answer when a live JIT
    // patches/frees/reuses the region mid-capture. `img` may be nullptr (degrades
    // to exactly attachPid).
    AttachResult attachPidVersioned(pid_t pid, std::uint64_t base, std::size_t codeLen,
                                    asmtest_codeimage_t* img, std::uint64_t when,
                                    std::uint64_t maxInsns = 0) {
        AttachResult r{};
        r.rc = asmtest_dataflow_ptrace_attach_pid_versioned(
            pid, base, codeLen, maxInsns, img, when, &r.result, v_);
        postAttach();
        return r;
    }

    // Steps / records stored in the trace (a live producer's, or hand-built ones).
    std::size_t steps() const { return asmtest_valtrace_steps(v_); }
    std::size_t recs() const { return asmtest_valtrace_recs(v_); }

private:
    void postAttach() {
        nSteps_ = static_cast<std::uint32_t>(asmtest_valtrace_steps(v_));
        if (g_) { asmtest_defuse_free(g_); g_ = nullptr; }
    }

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
