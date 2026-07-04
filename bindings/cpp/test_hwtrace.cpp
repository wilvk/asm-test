/*
 * test_hwtrace.cpp — standalone live test for the single-step hardware-trace
 * backend via the C++ wrapper (asmtest_hwtrace.hpp). Mirrors the Python suite
 * bindings/python/tests/test_hwtrace.py.
 *
 * Unlike the DynamoRIO wrapper (which needs a DynamoRIO install) and the
 * PT/AMD/CoreSight backends (which need specific bare-metal hardware), the
 * SINGLESTEP backend runs on ANY x86-64 Linux — so this asserts a real, live trace
 * here and in CI/containers, self-skipping (prints "# SKIP <reason>", returns 0)
 * only off x86-64 Linux, without Capstone, or when the library is absent.
 *
 * Emits TAP-style "ok N - ..." / "not ok N - ..." lines and exits nonzero on any
 * failure. Build standalone (links only -ldl):
 *
 *   g++ -std=c++17 -I include bindings/cpp/test_hwtrace.cpp -ldl -o test_hwtrace
 *   ASMTEST_HWTRACE_LIB=$PWD/build/libasmtest_hwtrace.so ./test_hwtrace
 *
 * IMPORTANT: under SINGLESTEP, EFLAGS.TF is armed across begin..call..end — the
 * region scopes below keep that window tight (no I/O between begin and end).
 */
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "asmtest_hwtrace.hpp"

using namespace asmtest;

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
static const std::vector<std::uint8_t> ROUTINE = {
    0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
    0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3};

// mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  (19 back-edges > LBR's 16)
static const std::vector<std::uint8_t> LOOP = {
    0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x48,
    0x01, 0xF8, 0x48, 0xFF, 0xCE, 0x75, 0xF8, 0xC3};

static int g_test = 0;
static int g_failed = 0;

static void ok(bool cond, const char *msg) {
    ++g_test;
    if (cond) {
        std::printf("ok %d - %s\n", g_test, msg);
    } else {
        std::printf("not ok %d - %s\n", g_test, msg);
        ++g_failed;
    }
}

int main() {
    if (!HwTrace::available(SINGLESTEP)) {
        std::printf("# SKIP single-step backend unavailable: %s\n",
                    HwTrace::skip_reason(SINGLESTEP).c_str());
        return 0;
    }

    HwTrace::init(SINGLESTEP);

    // ---- ROUTINE: two-block branch fixture ----
    {
        NativeCode code = NativeCode::from_bytes(ROUTINE);
        HwTrace tr = HwTrace::create(/*blocks=*/64, /*instructions=*/64);
        tr.register_region("add2", code);

        long result;
        {
            auto scope = tr.region("add2");
            result = code.call(20, 22);  // 42 <= 100 -> jle taken, dec skipped
        }

        ok(result == 42, "ROUTINE: code.call(20, 22) == 42");
        // Byte-for-byte the Unicorn/DynamoRIO/PT/AMD result for this fixture.
        const std::vector<std::uint64_t> expect{0x0, 0x3, 0x6, 0xC, 0x11};
        ok(tr.insn_offsets() == expect,
           "ROUTINE: insn_offsets() == {0, 3, 6, 0xC, 0x11}");
        ok(tr.insns_total() == 5, "ROUTINE: insns_total() == 5");
        ok(tr.covered(0) && tr.covered(0x11),
           "ROUTINE: covered(0) && covered(0x11)");
        ok(tr.blocks_len() == 2, "ROUTINE: blocks_len() == 2");
        ok(!tr.truncated(), "ROUTINE: !truncated()");

        tr.free();
        code.free();
    }

    // ---- LOOP: tight loop past the LBR depth ceiling ----
    {
        NativeCode code = NativeCode::from_bytes(LOOP);
        HwTrace tr = HwTrace::create(/*blocks=*/64, /*instructions=*/256);
        tr.register_region("loop", code);

        long result;
        {
            auto scope = tr.region("loop");
            result = code.call(1, 20);
        }

        ok(result == 20, "LOOP: code.call(1, 20) == 20");
        ok(tr.insns_total() == 62,
           "LOOP: insns_total() == 62 (1 + 20*3 + 1)");
        ok(tr.covered(0) && tr.covered(0x7) && tr.covered(0xf),
           "LOOP: covered(0) && covered(0x7) && covered(0xf)");
        // 3 blocks {0,0x7,0xf}: the ret after the not-taken jnz is its own block.
        ok(tr.blocks_len() == 3, "LOOP: blocks_len() == 3");
        ok(!tr.truncated(), "LOOP: !truncated()");

        tr.free();
        code.free();
    }

    // ---- ScopedTrace: RAII scope with auto-name + render-on-close ----
    {
        NativeCode code = NativeCode::from_bytes(ROUTINE);
        std::string rendered;
        std::string nm;
        {
            asmtest::ScopedTrace t(code, &rendered, /*emit=*/false);
            code.call(20, 22);
            nm = t.name();
        } // dtor: end + render into `rendered`
        std::size_t lines = 0;
        for (char c : rendered)
            if (c == '\n')
                ++lines;
        ok(!rendered.empty(), "ScopedTrace: render-on-close produced text");
        ok(lines == 5, "ScopedTrace: 5 rendered instruction lines (matches insns)");
        ok(rendered.find("ret") != std::string::npos,
           "ScopedTrace: rendered listing includes the ret");
        ok(nm.rfind("test_hwtrace.cpp:", 0) == 0,
           "ScopedTrace: auto-name is basename:line from the call site");
        code.free();
    }

    HwTrace::shutdown();

    // ---- auto-select front-end: pick the most-faithful available backend ----
    // Mirrors examples/test_hwtrace.c's test_auto_resolve: selection invariants
    // plus a live traced call through whatever auto picks (single-step here).
    {
        std::vector<int> best = HwTrace::resolve(BEST);
        std::vector<int> cf = HwTrace::resolve(CEILING_FREE);

        bool ok_avail = true, ok_order = true;
        for (std::size_t i = 0; i < best.size(); ++i) {
            if (!HwTrace::available(best[i]))
                ok_avail = false;
            if (i && best[i] <= best[i - 1])
                ok_order = false;
        }
        ok(ok_avail, "auto: resolve(BEST) returns only available backends");
        ok(ok_order, "auto: resolve(BEST) ordered by descending fidelity, no dups");

        bool cf_no_amd = true, cf_subset = true;
        for (int b : cf) {
            if (b == AMD_LBR)
                cf_no_amd = false;
            bool in_best = false;
            for (int x : best)
                in_best = in_best || (x == b);
            cf_subset = cf_subset && in_best;
        }
        ok(cf_no_amd, "auto: CEILING_FREE never selects AMD LBR");
        ok(cf_subset, "auto: CEILING_FREE is a subset of BEST");

        int ab = HwTrace::auto_select(BEST);
        bool head_ok = best.empty() ? (ab == ASMTEST_HW_EUNAVAIL) : (ab == best[0]);
        ok(head_ok, "auto: auto_select(BEST) is the head of resolve(BEST)");

        // single-step keeps the cascade non-empty on this x86-64 Linux host.
        ok(!best.empty() && ab >= 0,
           "auto: resolves a backend (single-step floor)");

        if (ab >= 0) {
            HwTrace::init(ab);
            NativeCode code = NativeCode::from_bytes(ROUTINE);
            HwTrace tr = HwTrace::create(/*blocks=*/64, /*instructions=*/64);
            tr.register_region("auto", code);
            long result;
            {
                auto scope = tr.region("auto");
                result = code.call(20, 22);
            }
            ok(result == 42, "auto: auto-selected backend traces a live call (== 42)");
            ok(tr.covered(0), "auto: auto-selected backend covers block offset 0");
            if (ab == SINGLESTEP) {
                const std::vector<std::uint64_t> expect{0x0, 0x3, 0x6, 0xC, 0x11};
                ok(tr.insn_offsets() == expect,
                   "auto: single-step pick yields [0, 3, 6, 0xC, 0x11]");
            }
            tr.free();
            code.free();
            HwTrace::shutdown();
        }
    }

    // ---- cross-tier orchestrator: resolve over hwtrace + DynamoRIO + emulator ----
    // Mirrors test_hwtrace.py's test_cross_tier_resolve_invariants: structural
    // invariants of the full descending-fidelity cascade.
    {
        std::vector<TierChoice> best = HwTrace::resolveTiers(TRACE_BEST);
        std::vector<TierChoice> nat = HwTrace::resolveTiers(TRACE_NATIVE_ONLY);
        std::vector<TierChoice> cf = HwTrace::resolveTiers(TRACE_CEILING_FREE);

        // Every HW choice satisfies the hardware-tier probe; each choice's fidelity
        // is VIRTUAL iff it is the emulator tier, NATIVE otherwise.
        bool hw_avail = true, fidelity_ok = true;
        int emu_count = 0;
        for (const TierChoice &c : best) {
            if (c.tier == TIER_HWTRACE && !HwTrace::available(c.backend))
                hw_avail = false;
            int want = (c.tier == TIER_EMULATOR) ? FIDELITY_VIRTUAL
                                                 : FIDELITY_NATIVE;
            if (c.fidelity != want)
                fidelity_ok = false;
            if (c.tier == TIER_EMULATOR)
                ++emu_count;
        }
        ok(hw_avail, "cross-tier: every HW choice in resolveTiers(BEST) is available");
        ok(fidelity_ok,
           "cross-tier: fidelity is VIRTUAL iff emulator tier, else NATIVE");

        // The emulator floor is the single last entry under BEST.
        ok(!best.empty() && best.back().tier == TIER_EMULATOR,
           "cross-tier: emulator is the last entry under BEST");
        ok(emu_count == 1, "cross-tier: exactly one emulator floor under BEST");

        // NATIVE_ONLY forbids the native->emulator crossing: it is BEST minus floor.
        bool nat_no_emu = true;
        for (const TierChoice &c : nat)
            if (c.tier == TIER_EMULATOR)
                nat_no_emu = false;
        ok(nat_no_emu, "cross-tier: NATIVE_ONLY never selects the emulator tier");
        ok(nat.size() == best.size() - 1,
           "cross-tier: NATIVE_ONLY is BEST minus the emulator floor");

        // CEILING_FREE drops AMD LBR.
        bool cf_no_amd = true;
        for (const TierChoice &c : cf)
            if (c.tier == TIER_HWTRACE && c.backend == AMD_LBR)
                cf_no_amd = false;
        ok(cf_no_amd, "cross-tier: CEILING_FREE never selects AMD LBR");

        // autoTier(policy) is the head of resolveTiers(policy).
        std::optional<TierChoice> one = HwTrace::autoTier(TRACE_BEST);
        bool head_ok = one.has_value() && !best.empty() &&
                       one->tier == best[0].tier &&
                       one->backend == best[0].backend;
        ok(head_ok, "cross-tier: autoTier(BEST) is the head of resolveTiers(BEST)");
    }

    // ---- cross-tier NATIVE_ONLY on x86-64 Linux: single-step is a native floor ----
    // Mirrors test_cross_tier_native_only_resolves_on_linux_x86_64. We are past the
    // single-step availability guard at the top of main(), so the floor is present.
    {
        std::vector<TierChoice> nat = HwTrace::resolveTiers(TRACE_NATIVE_ONLY);
        std::optional<TierChoice> pick = HwTrace::autoTier(TRACE_NATIVE_ONLY);

        ok(!nat.empty() && pick.has_value() &&
               pick->fidelity == FIDELITY_NATIVE,
           "cross-tier: NATIVE_ONLY resolves a native pick on x86-64 Linux");

        bool has_singlestep = false;
        for (const TierChoice &c : nat)
            if (c.tier == TIER_HWTRACE && c.backend == SINGLESTEP)
                has_singlestep = true;
        ok(has_singlestep,
           "cross-tier: NATIVE_ONLY cascade includes the single-step floor");
    }

    // ---- Out-of-process / foreign-process toolkit (asmtest::Ptrace) ----
    // Mirrors test_hwtrace.py's tests after the same banner: a forked-tracee live
    // single-step (trace_call), region discovery from /proc/<pid>/maps, perf-map
    // and binary-jitdump symbol resolution. Guarded by a ptrace-available skip.
    // trace_attached and run_to have no live test (forking + ptrace of a foreign
    // process from the harness is impractical; the C suite covers them live), but the
    // symbols are wrapped (Ptrace::traceAttached / Ptrace::runTo) so the binding-surface
    // parity gate sees them, and we exercise runTo's FFI round-trip safely below.
    if (!Ptrace::available()) {
        std::printf("# SKIP ptrace backend unavailable: %s\n",
                    Ptrace::skipReason().c_str());
    } else {
        // trace_call: fork a tracee, single-step it out of process, same offsets.
        {
            NativeCode code = NativeCode::from_bytes(ROUTINE);
            HwTrace tr = HwTrace::create(/*blocks=*/64, /*instructions=*/64);
            long result = Ptrace::traceCall(code, {20, 22}, tr);
            ok(result == 42, "ptrace: trace_call(20, 22) == 42");
            const std::vector<std::uint64_t> expect{0x0, 0x3, 0x6, 0xC, 0x11};
            ok(tr.insn_offsets() == expect,
               "ptrace: trace_call insn_offsets() == {0, 3, 6, 0xC, 0x11}");
            ok(!tr.truncated(), "ptrace: trace_call !truncated()");
            tr.free();
            code.free();
        }

        // run_to: a live foreign attach is covered by the C suite; here exercise the
        // FFI round-trip safely — a NULL target address is rejected (EINVAL, non-OK)
        // before any ptrace call.
        ok(Ptrace::runTo(getpid(), nullptr) != 0,
           "ptrace: run_to(NULL addr) rejected (EINVAL) via the FFI round-trip");

        // region_by_addr: discover an executable region's extent from
        // /proc/<pid>/maps by an interior address (this process).
        {
            NativeCode code = NativeCode::from_bytes(ROUTINE);
            const std::uint8_t *p = static_cast<const std::uint8_t *>(code.base());
            auto region = Ptrace::regionByAddr(getpid(), p + 4);
            ok(region.has_value() && region->first == code.base() &&
                   region->second >= ROUTINE.size(),
               "ptrace: region_by_addr base==code base, len>=18");
            // Nothing maps address 1.
            ok(!Ptrace::regionByAddr(getpid(),
                                     reinterpret_cast<const void *>(0x1))
                    .has_value(),
               "ptrace: region_by_addr(addr 1) is empty");
            code.free();
        }

        // perfmap_symbol: parse /tmp/perf-<pid>.map and resolve a method by name.
        {
            int pid = getpid();
            std::string path = "/tmp/perf-" + std::to_string(pid) + ".map";
            FILE *f = std::fopen(path.c_str(), "w");
            std::fputs("400000 1a void demo(long, long)\n500000 8 other\n", f);
            std::fclose(f);

            auto hit = Ptrace::perfmapSymbol(pid, "void demo(long, long)");
            ok(hit.has_value() &&
                   hit->first == reinterpret_cast<void *>(0x400000) &&
                   hit->second == 0x1A,
               "ptrace: perfmap_symbol resolves to (0x400000, 0x1A)");
            ok(!Ptrace::perfmapSymbol(pid, "missing").has_value(),
               "ptrace: perfmap_symbol(missing) is empty");
            std::remove(path.c_str());
        }

        // jitdump_find: write a binary jitdump per the Python byte layout and
        // resolve a method to (addr, size, index, ts) + recorded bytes.
        {
            const std::string name = "void demo(long, long)";
            std::vector<std::uint8_t> img;
            auto u32 = [&img](std::uint32_t x) {
                for (int i = 0; i < 4; ++i)
                    img.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
            };
            auto u64 = [&img](std::uint64_t x) {
                for (int i = 0; i < 8; ++i)
                    img.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
            };
            // header: magic, version, total_size=40, elf_mach, pad1, pid, ts, flags
            u32(0x4A695444);  // "DTiJ" jitdump magic
            u32(1);
            u32(40);
            u32(62);
            u32(0);
            u32(0);
            u64(0);
            u64(0);
            // JIT_CODE_LOAD record: id=0, total_size, timestamp=5
            std::uint64_t total = 16 + 40 + (name.size() + 1) + ROUTINE.size();
            u32(0);
            u32(static_cast<std::uint32_t>(total));
            u64(5);
            // body: pid, tid, vma, code_addr, code_size, code_index
            u32(0);
            u32(0);
            u64(0x2000);
            u64(0x2000);
            u64(ROUTINE.size());
            u64(9);
            // name (NUL-terminated) + the recorded code bytes
            img.insert(img.end(), name.begin(), name.end());
            img.push_back(0);
            img.insert(img.end(), ROUTINE.begin(), ROUTINE.end());

            std::string path = "/tmp/asmtest-jit-" + std::to_string(getpid()) +
                               ".dump";
            FILE *f = std::fopen(path.c_str(), "wb");
            std::fwrite(img.data(), 1, img.size(), f);
            std::fclose(f);

            auto m = Ptrace::jitdumpFind(path, name, /*pid=*/0, /*wantBytes=*/64);
            ok(m.has_value() && m->code_addr == 0x2000 &&
                   m->code_size == ROUTINE.size() && m->code_index == 9 &&
                   m->timestamp == 5,
               "ptrace: jitdump_find resolves (addr 0x2000, size 18, index 9, ts 5)");
            ok(m.has_value() && m->code == ROUTINE,
               "ptrace: jitdump_find returns the recorded ROUTINE bytes");
            ok(!Ptrace::jitdumpFind(path, "missing").has_value(),
               "ptrace: jitdump_find(missing) is empty");
            std::remove(path.c_str());
        }

        // ---- call descent: edges (L1) + descended frames (L2) ----
        // Mirrors test_hwtrace.py::test_descent_edges_and_frames. Fixture:
        //   R@0:   mov rax,rdi; call S(+4); add rax,rsi; ret
        //   S@0xc: inc rax; ret
        // The traced region is R only (0xc bytes): S is a sibling BEYOND it in the
        // same allocation, so it is recorded as a call-out, not mis-attributed as
        // recursion. Passing the whole allocation would fold S into the region.
        {
            const std::vector<std::uint8_t> BLOB = {
                0x48, 0x89, 0xF8,              // R@0:   mov rax, rdi
                0xE8, 0x04, 0x00, 0x00, 0x00,  //        call +4 -> S@0xc
                0x48, 0x01, 0xF0,              //        add rax, rsi
                0xC3,                          //        ret
                0x48, 0xFF, 0xC0,              // S@0xc: inc rax
                0xC3};                         //        ret
            const std::size_t REGION = 0xC;    // trace R only; S stays outside
            const std::vector<std::uint64_t> F0{0x0, 0x3, 0x8, 0xB};

            // Level 1: RECORD_EDGES — R's body + one (call -> S) edge; S stepped over.
            {
                NativeCode code = NativeCode::from_bytes(BLOB);
                HwTrace tr = HwTrace::create(/*blocks=*/64, /*instructions=*/64);
                Descent d(DESCENT_RECORD_EDGES);
                long result = Ptrace::traceCallEx(code, {20, 22}, &tr, &d, REGION);
                ok(result == 43, "descent L1: trace_call_ex(20, 22) == 43");
                ok(d.frames_len() == 1, "descent L1: frames_len() == 1 (root only)");
                ok(d.frame_insns(0) == F0,
                   "descent L1: frame_insns(0) == {0, 3, 8, 0xB}");
                std::uint64_t s_addr =
                    reinterpret_cast<std::uint64_t>(code.base()) + 0xC;
                std::vector<Descent::Edge> edges = d.edges();
                ok(edges.size() == 1 && edges[0].site == 0x3 &&
                       edges[0].target == s_addr && edges[0].depth == 0,
                   "descent L1: one edge (site 0x3, target base+0xC, depth 0)");
                ok(!d.truncated(), "descent L1: !truncated()");
                d.free();
                tr.free();
                code.free();
            }

            // Level 2: DESCEND_KNOWN — S is in the allow-set, so it descends as a
            // nested frame; the edge becomes a real frame instead. The callee is
            // exposed through a NON-OWNING FrameView (never frees the handle).
            {
                NativeCode code = NativeCode::from_bytes(BLOB);
                HwTrace tr = HwTrace::create(/*blocks=*/64, /*instructions=*/64);
                Descent d(DESCENT_DESCEND_KNOWN);
                std::uint64_t s_addr =
                    reinterpret_cast<std::uint64_t>(code.base()) + 0xC;
                d.allow_region(reinterpret_cast<const void *>(s_addr), 4);
                long result = Ptrace::traceCallEx(code, {20, 22}, &tr, &d, REGION);
                ok(result == 43, "descent L2: trace_call_ex(20, 22) == 43");
                ok(d.frames_len() == 2,
                   "descent L2: frames_len() == 2 (root + descended callee)");
                ok(d.frame_insns(0) == F0,
                   "descent L2: frame_insns(0) == {0, 3, 8, 0xB}");
                FrameView s = d.frame(1);  // non-owning view of the callee frame
                ok(s.base() == s_addr, "descent L2: frame(1).base() == base+0xC");
                ok(s.depth() == 1, "descent L2: frame(1).depth() == 1");
                const std::vector<std::uint64_t> F1{0x0, 0x3};
                ok(s.insns() == F1, "descent L2: frame(1).insns() == {0, 3}");
                ok(d.edges().empty(),
                   "descent L2: edges() == [] (the call was descended)");
                d.free();
                tr.free();
                code.free();
            }
        }
    }

    // ---- Time-aware code-image recorder (asmtest::CodeImage) ----
    // The userspace PERF_RECORD_TEXT_POKE: track a region, advance the capture
    // sequence, and round-trip the live bytes back out through bytes_at(). Records
    // THIS process (pid 0). Self-skips off a host without PAGEMAP_SCAN / soft-dirty.
    {
        // mov rax,rdi; add rax,rsi; ret  (a tiny self-contained method body)
        const std::vector<std::uint8_t> CIBODY = {0x48, 0x89, 0xf8, 0x48,
                                                  0x01, 0xf0, 0xc3};
        if (!CodeImage::available()) {
            std::printf("# SKIP codeimage recorder unavailable: %s\n",
                        CodeImage::skip_reason().c_str());
        } else {
            NativeCode code = NativeCode::from_bytes(CIBODY);
            const void *base = code.base();

            CodeImage img(0);
            ok(img.track(base, CIBODY.size()) == 0,
               "codeimage: track(base, 7) == OK");
            // track() snapshots version 0, advancing the sequence to >= 1.
            ok(img.now() >= 1, "codeimage: now() >= 1 after track");
            // Nothing was written, so refresh records no new versions (>= 0).
            ok(img.refresh() >= 0, "codeimage: refresh() >= 0");

            // Round-trip: the bytes the recorder captured at version 0 are exactly
            // the body we wrote, and the latest (when == now()) version too.
            std::vector<std::uint8_t> at0 = img.bytes_at(base, 0);
            ok(at0.size() >= CIBODY.size() &&
                   std::vector<std::uint8_t>(at0.begin(),
                                             at0.begin() + CIBODY.size()) ==
                       CIBODY,
               "codeimage: bytes_at(base, 0) round-trips the 7 body bytes");
            std::vector<std::uint8_t> atnow = img.bytes_at(base, img.now());
            ok(atnow.size() >= CIBODY.size() &&
                   std::vector<std::uint8_t>(atnow.begin(),
                                             atnow.begin() + CIBODY.size()) ==
                       CIBODY,
               "codeimage: bytes_at(base, now()) round-trips the 7 body bytes");

            // An address never tracked yields no bytes (ASMTEST_CI_ENOENT -> empty).
            ok(img.bytes_at(reinterpret_cast<const void *>(0x1)).empty(),
               "codeimage: bytes_at(untracked addr) is empty");

            img.free();
            code.free();
        }
    }

    // ---- CodeImage optional eBPF emission detector probe ----
    // Sideband only (when/where code appears, never the instruction stream).
    // Self-skips without libbpf / CAP_BPF / kernel BTF; when available, the watch
    // loads and attaches (ASMTEST_CI_OK).
    {
        if (!CodeImage::bpf_available()) {
            std::printf("# SKIP codeimage eBPF detector unavailable: %s\n",
                        CodeImage::bpf_skip_reason().c_str());
        } else {
            CodeImage img(0);
            ok(img.watch_bpf() == 0, "codeimage: watch_bpf() == OK");
            img.free();
        }
    }

    std::printf("1..%d\n", g_test);
    return g_failed == 0 ? 0 : 1;
}
