// test_regsynth.cpp — 30 R3 T4: the Scrubber's register-history SYNTHESISER.
// FULL BUILD (links emu.o): it re-derives an emulator-replayable recording's
// per-step register file by re-running the recorded code under the emulator ring,
// and refuses honestly where it cannot. The pure Scrubber decision that OFFERS
// this (replayable vs not) is asserted engine-free in test_scrubber; this proves
// the re-run the offer promises.
#include <cstdio>
#include <string>

#include "analysis/stepindex.h"
#include "doc/recording.h"
#include "views/regsynth.h"

using namespace asmdesk;

static int failures;
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
        failures++;
    }
}

// A minimal emulator-replayable Recording carrying `hex` as its codeimage — the
// exact shape an Author run materialises (author_vm.cpp).
static Recording emu_recording(const std::string &hex, const std::string &arch) {
    Recording r;
    r.version = 1;
    r.arch = arch;
    r.producer.name = "asmtest-author";
    r.provenance.backend = "author-emulator";
    r.provenance.exact = true;
    nlohmann::json ev;
    ev["k"] = "codeimage";
    ev["base"] = 0x100000;
    ev["len"] = hex.size() / 2;
    ev["version"] = 0;
    ev["bytes"] = hex;
    r.by_kind["codeimage"].push_back(Event{"codeimage", ev, r.next_seq++});
    return r;
}

int main() {
    // Constant arithmetic, so the re-derived history is the same regardless of the
    // (absent) args — the seed is the emulator's deterministic zero-GP + SysV:
    //   mov rax, 5   ; 48 c7 c0 05 00 00 00
    //   add rax, 3   ; 48 83 c0 03
    //   ret          ; c3
    // step 0 (pre-mov) rax=0   step 1 (pre-add) rax=5   step 2 (pre-ret) rax=8
    const std::string code = "48c7c0050000004883c003c3";

    // --- the happy path: a re-derived, clearly-labelled history --------------
    {
        Recording r = emu_recording(code, "x86_64");
        StepIndex idx;
        std::string err;
        check("synthesis succeeds on an emulator-replayable recording",
              regsynth_synthesize(r, &idx, &err), err);
        check("the synthesised index is FLAGGED as a re-derivation (D6)",
              idx.synthesized, "must set synthesized");
        check("the synthesised index is present", idx.present(),
              "three held steps");
        check("three steps re-derived", idx.count() == 3,
              "got " + std::to_string(idx.count()));

        const RegFile *s0 = idx.at_step(0);
        const RegFile *s1 = idx.at_step(1);
        const RegFile *s2 = idx.at_step(2);
        check("step 0 seekable", s0 != nullptr, "");
        check("step 1 seekable", s1 != nullptr, "");
        check("step 2 seekable", s2 != nullptr, "");
        const RegField *r0 = s0 ? s0->find("rax") : nullptr;
        const RegField *r1 = s1 ? s1->find("rax") : nullptr;
        const RegField *r2 = s2 ? s2->find("rax") : nullptr;
        check("pre-mov rax is 0 (GP file zeroed at entry)",
              r0 && r0->value == 0, "");
        check("pre-add rax is 5 (mov rax,5 ran) and is highlighted",
              r1 && r1->value == 5 && r1->changed, "");
        check("pre-ret rax is 8 (add rax,3 ran) and is highlighted",
              r2 && r2->value == 8 && r2->changed, "");
        check("the deck carries the full 18-field GP file, descriptor order",
              s0 && s0->fields.size() == 18,
              s0 ? std::to_string(s0->fields.size()) : "null");
        check("the first held step has no diff baseline",
              s0 && !s0->has_prev, "step 0 cannot diff against a predecessor");
    }

    // --- honest refusal: no codeimage -> nothing to re-run -------------------
    {
        Recording r;
        r.arch = "x86_64";
        r.provenance.backend = "author-emulator";
        StepIndex idx;
        std::string err;
        check("a recording with no codeimage refuses",
              !regsynth_synthesize(r, &idx, &err), "it ran anyway");
        check("the refusal names the missing codeimage",
              err.find("codeimage") != std::string::npos, err);
    }

    // --- honest refusal: a non-x86-64 guest is outside the ring's scope -------
    {
        Recording r = emu_recording(code, "aarch64");
        StepIndex idx;
        std::string err;
        check("a non-x86-64 guest refuses",
              !regsynth_synthesize(r, &idx, &err), "it ran anyway");
        check("the refusal names x86-64",
              err.find("x86-64") != std::string::npos, err);
    }

    if (failures) {
        std::fprintf(stderr, "%d regsynth check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_regsynth: all checks passed\n");
    return 0;
}
