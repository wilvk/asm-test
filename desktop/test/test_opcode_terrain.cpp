// test_opcode_terrain.cpp — the opcode-class code terrain
// (56-fidelity-and-module-layers.md T4). Null harness: links space/
// opcode_terrain.o + space/mnemonic.o + the document model — no GL, no
// ImGui, no engine (D4).
//
// CodeCells are hand-built (bypassing build_terrain/projection) so this file
// tests build_opcode_terrain's own classification arithmetic in isolation —
// geometry/projection is test_terrain.cpp's job, not this one's. A cell is
// just "whichever steps I say share cell N"; the offsets/disasm text still
// come from a REAL Recording decoded through decode_streams, so the
// offset->text resolution this function actually does is exercised for real.
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

#include "doc/recording.h"
#include "space/opcode_terrain.h"

using namespace asmdesk;
using namespace asmdesk::space;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}
static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static Recording mk_rec(const std::string &ndjson) {
    std::istringstream in(ndjson);
    std::string err;
    auto rec = load_recording(in, err);
    if (!rec) {
        fail("load recording", err);
        return Recording{};
    }
    return *rec;
}

// A CodeCell naming exactly the given steps, at `cell`.
static TerrainModel::CodeCell mk_cell(uint32_t cell,
                                      std::vector<uint64_t> steps) {
    TerrainModel::CodeCell cc;
    cc.cell = cell;
    cc.steps = std::move(steps);
    return cc;
}

int main() {
    // Steps 0/1/2/3: mov, mov, add, movd(ambiguous). Step 4 has NO disasm.
    Recording rec = mk_rec(
        "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\","
        "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304,"
        "\"disasm\":\"mov rax, rbx\"}\n"       // step0, offset A
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194305,"
        "\"disasm\":\"mov rcx, rdx\"}\n"       // step1, offset B
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194306,"
        "\"disasm\":\"add rax, 1\"}\n"         // step2, offset C
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194307,"
        "\"disasm\":\"movd xmm0, eax\"}\n"     // step3, offset D (ambiguous)
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194308}\n" // step4, no disasm
        "{\"k\":\"end\",\"events\":6,\"truncated\":false,"
        "\"drops\":{\"lost\":0,\"throttled\":false}}\n");

    TerrainModel m;
    m.height_source = "trace";

    // --- cell 0: two mov offsets (A, B, hit 3x/1x) + one add (C, hit 1x) ----
    // Dominant should be Move (2 of 3 distinct offsets), purity 2/3, hottest
    // offset A (hit 3 times: steps 0,0,0 — repeat a step index to weight it).
    m.code.push_back(mk_cell(0, {0, 0, 0, 1, 2}));
    // --- cell 1: the lone ambiguous offset (D) ------------------------------
    m.code.push_back(mk_cell(1, {3}));
    // --- cell 2: the no-disasm offset (step 4) ------------------------------
    m.code.push_back(mk_cell(2, {4}));
    // --- cell 3: an out-of-range step (no offset stream can resolve it) -----
    m.code.push_back(mk_cell(3, {999}));

    std::vector<CellOpcode> out = build_opcode_terrain(m, rec, "x86");
    check("one CellOpcode per CodeCell", out.size() == 4,
          "got " + std::to_string(out.size()));

    const CellOpcode *c0 = nullptr, *c1 = nullptr, *c2 = nullptr, *c3 = nullptr;
    for (const CellOpcode &co : out) {
        if (co.cell == 0)
            c0 = &co;
        else if (co.cell == 1)
            c1 = &co;
        else if (co.cell == 2)
            c2 = &co;
        else if (co.cell == 3)
            c3 = &co;
    }
    check("cell 0 present", c0 != nullptr, "");
    check("cell 1 present", c1 != nullptr, "");
    check("cell 2 present", c2 != nullptr, "");
    check("cell 3 present", c3 != nullptr, "");

    if (c0) {
        check("cell 0: 3 distinct offsets (A, B, C)", c0->distinct_offsets == 3,
              "got " + std::to_string(c0->distinct_offsets));
        check("cell 0: dominant is Move (2 of 3 offsets)",
              c0->dominant == OpClass::Move,
              op_class_name(c0->dominant));
        check("cell 0: purity is 2/3 (by DISTINCT offset, not by hit count)",
              near(c0->purity, 2.0f / 3.0f),
              "got " + std::to_string(c0->purity));
        check("cell 0: not ambiguous (no ambiguous mnemonic here)",
              !c0->ambiguous, "");
        check("cell 0: hottest offset is A (0x400000, hit 3x via step 0)",
              c0->hottest_off == 4194304,
              "got 0x" + std::to_string(c0->hottest_off));
    }
    if (c1) {
        // mnemonic.cpp's own table: movd/movq classify VectorSIMD (the
        // vector-register-file side of the boundary it straddles), flagged
        // ambiguous — this test asserts the real table, not an assumption.
        check("cell 1: dominant is VectorSIMD (movd)",
              c1->dominant == OpClass::VectorSIMD, op_class_name(c1->dominant));
        check("cell 1: ambiguous (movd crosses GPR<->vector)", c1->ambiguous,
              "movd must be flagged ambiguous");
        check("cell 1: purity 1.0 (one offset, one class)",
              near(c1->purity, 1.0f), "got " + std::to_string(c1->purity));
    }
    if (c2) {
        check("cell 2: no disasm -> Unknown, never coerced",
              c2->dominant == OpClass::Unknown, op_class_name(c2->dominant));
        check("cell 2: still purity 1.0 (100% of its one offset is Unknown)",
              near(c2->purity, 1.0f), "got " + std::to_string(c2->purity));
    }
    if (c3) {
        check("cell 3: an unresolvable step yields distinct_offsets 0",
              c3->distinct_offsets == 0,
              "got " + std::to_string(c3->distinct_offsets));
        check("cell 3: Unknown with purity 0 (nothing resolved, not a guess)",
              c3->dominant == OpClass::Unknown && near(c3->purity, 0.0f),
              "an out-of-range step must not fabricate a classification");
    }

    // --- guest gating: an unrecognised guest abstains every cell -----------
    std::vector<CellOpcode> riscv = build_opcode_terrain(m, rec, "riscv");
    for (const CellOpcode &co : riscv)
        if (co.distinct_offsets > 0)
            check("unrecognised guest: cell " + std::to_string(co.cell) +
                      " is Unknown",
                  co.dominant == OpClass::Unknown,
                  "a guest this file has no vocabulary for must abstain, "
                  "never guess from another guest's words");

    // --- an empty model yields no cells -------------------------------------
    check("empty model yields no cells",
          build_opcode_terrain(TerrainModel{}, rec, "x86").empty(),
          "a model with no code cells must not synthesise one");

    if (failures) {
        std::fprintf(stderr, "%d test_opcode_terrain check(s) failed\n",
                     failures);
        return 1;
    }
    std::printf("test_opcode_terrain: all checks passed\n");
    return 0;
}
