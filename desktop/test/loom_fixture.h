// loom_fixture.h — the hand-filled `df_chain` value trace every loom test
// weaves (05-loom-day-one.md T1-T3). Header-only, so it adds no object to any
// link line and cannot drag a dependency into a test binary.
//
// examples/test_dataflow_emu.c:38 — args {7, 5}:
//   0x00 mov rax, rdi      0x03 mov [rsp-8], rax   0x08 mov rcx, [rsp-8]
//   0x0d lea rdx,[rcx+rsi] 0x11 mov rax, rdx       0x14 ret
//
// The records mirror what the emulator L0 producer records for it: the register
// read/write sets Capstone enumerates (including the implicit rsp on the store
// and the ret), the mem hook's absolute-address records, and the ONE deferred
// write value the run ends before filling (rip at the ret) — the hollow-thread
// case. Values are the real ones: rdi = 7, rsi = 5, so rdx = 12.
#ifndef ASMDESK_TEST_LOOM_FIXTURE_H
#define ASMDESK_TEST_LOOM_FIXTURE_H

#include <cstdio>
#include <string>
#include <vector>

#include "loom/fabric.h"

namespace loomfx {

enum : uint32_t {
    RAX = 35,
    RCX = 38,
    RDX = 40,
    RSI = 43,
    RDI = 39,
    RSP = 44,
    RIP = 41
};
inline constexpr uint64_t SP = 0x20fff8; // DF_STACK_BASE + DF_STACK_SIZE - 8
inline constexpr uint64_t SLOT = SP - 8; // [rsp-8]

inline at_val_rec_t reg(uint32_t step, uint32_t r, bool write, bool valid,
                        uint64_t value) {
    at_val_rec_t v{};
    v.kind = AT_LOC_REG;
    v.reg = r;
    v.size = 8;
    v.is_write = write;
    v.value_valid = valid;
    v.value = value;
    v.step = step;
    return v;
}

inline at_val_rec_t mem(uint32_t step, uint64_t addr, bool write, bool valid,
                        uint64_t value) {
    at_val_rec_t v{};
    v.kind = AT_LOC_MEM_ABS;
    v.addr = addr;
    v.size = 8;
    v.is_write = write;
    v.value_valid = valid;
    v.value = value;
    v.step = step;
    return v;
}

struct Fixture {
    std::vector<uint64_t> off{0x00, 0x03, 0x08, 0x0d, 0x11, 0x14};
    std::vector<at_val_rec_t> recs;
    std::vector<asmtest_defuse_edge_t> edges;
    asmtest_valtrace_t vt{};
    asmtest_defuse_t g{};

    Fixture() {
        recs = {
            reg(0, RDI, false, true, 7),
            reg(0, RAX, true, true, 7),
            reg(1, RAX, false, true, 7),
            reg(1, RSP, false, true, SP),
            mem(1, SLOT, true, true, 7),
            reg(2, RSP, false, true, SP),
            mem(2, SLOT, false, true, 7),
            reg(2, RCX, true, true, 7),
            reg(3, RCX, false, true, 7),
            reg(3, RSI, false, true, 5),
            reg(3, RDX, true, true, 12),
            reg(4, RDX, false, true, 12),
            reg(4, RAX, true, true, 12),
            reg(5, RSP, false, true, SP),
            mem(5, SP, false, true, 0xdeadbeef),
            reg(5, RSP, true, true, SP + 8),
            reg(5, RIP, true, false, 0), // deferred, never filled -> HOLLOW
        };
        edge(0, 1, reg(1, RAX, false, true, 7));
        edge(1, 2, mem(2, SLOT, false, true, 7));
        edge(2, 3, reg(3, RCX, false, true, 7));
        edge(3, 4, reg(4, RDX, false, true, 12));
        bind();
    }

    void edge(uint32_t f, uint32_t t, at_val_rec_t loc) {
        asmtest_defuse_edge_t e{};
        e.from_step = f;
        e.to_step = t;
        e.loc = loc;
        edges.push_back(e);
    }

    // Re-point the C view structs at the vectors. Call after any mutation.
    void bind() {
        vt = asmtest_valtrace_t{};
        vt.insn_off = off.data();
        vt.steps_cap = vt.steps_len = off.size();
        vt.steps_total = off.size();
        vt.recs = recs.data();
        vt.recs_cap = vt.recs_len = recs.size();
        vt.recs_total = recs.size();
        vt.mem_space = AT_LOC_MEM_ABS;
        g = asmtest_defuse_t{};
        g.edges = edges.data();
        g.n = edges.size();
        g.nsteps = off.size();
    }
};

inline asmdesk::loom_provenance_t prov() {
    asmdesk::loom_provenance_t p;
    p.producer = "dataflow-emu";
    p.exact = true;
    p.isolated_guest = true;
    p.steps_recorded = 6;
    p.steps_total = 6;
    p.disasm = {"mov rax, rdi",
                "mov qword ptr [rsp - 8], rax",
                "mov rcx, qword ptr [rsp - 8]",
                "lea rdx, [rcx + rsi]",
                "mov rax, rdx",
                "ret"};
    return p;
}

// Build or die loudly: a fixture that will not weave is a hard failure, never a
// skipped assertion.
inline asmdesk::loom_fabric_t build(const Fixture &fx,
                                    asmdesk::loom_provenance_t p = prov()) {
    asmdesk::loom_fabric_t f;
    std::string err;
    if (!asmdesk::loom_fabric_build(&fx.vt, &fx.g, p, &f, &err)) {
        std::fprintf(stderr, "FAIL fixture build: %s\n", err.c_str());
        std::exit(1);
    }
    return f;
}

inline int lane_named(const asmdesk::loom_fabric_t &f, const char *name) {
    for (size_t i = 0; i < f.lanes.size(); i++)
        if (f.lanes[i].name == name)
            return static_cast<int>(i);
    return -1;
}

inline int band_lane(const asmdesk::loom_fabric_t &f) {
    for (size_t i = 0; i < f.lanes.size(); i++)
        if (f.lanes[i].kind == asmdesk::loom_lane_kind::mem_band)
            return static_cast<int>(i);
    return -1;
}

} // namespace loomfx
#endif // ASMDESK_TEST_LOOM_FIXTURE_H
